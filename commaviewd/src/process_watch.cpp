#include "process_watch.h"

#include <capnp/serialize.h>
#include <kj/array.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

#include "cereal/gen/cpp/log.capnp.h"

namespace commaview::process_events {
namespace {

uint64_t clock_ns(clockid_t clock) {
  timespec ts {};
  if (clock_gettime(clock, &ts) != 0) return 0;
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

std::string read_boot_id(const std::string& path) {
  std::ifstream f(path);
  std::string id;
  if (f) std::getline(f, id);
  while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == ' ')) id.pop_back();
  return id.substr(0, 64);
}

std::string json_name(const std::string& in) {
  std::string out = "\"";
  for (size_t i = 0; i < in.size() && i < 64; i++) {
    const char c = in[i];
    const unsigned char uc = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else {
      out += (uc < 0x20 || uc == 0x7f) ? ' ' : c;
    }
  }
  return out + "\"";
}

std::string last_event_json(int64_t wall_ms, const char* event, const ProcessEvent* p) {
  std::ostringstream out;
  out << "{\"t\":" << wall_ms << ",\"event\":\"" << event << "\"";
  if (p != nullptr) out << ",\"proc\":" << json_name(p->name) << ",\"exitCode\":" << p->exit_code;
  out << "}";
  return out.str();
}

}  // namespace

std::string process_watch_summary_json(const ProcessWatchSummary& s) {
  std::ostringstream out;
  out << "{\"watching\":" << (s.watching ? "true" : "false");
  if (!s.watching) return out.str() + "}";
  out << ",\"intervalMs\":" << s.interval_ms << ",\"kernelLog\":" << (s.kmsg ? "true" : "false")
      << ",\"managerSeen\":" << (s.manager_seen ? "true" : "false")
      << ",\"managerSilent\":" << (s.manager_silent ? "true" : "false") << ",\"downCount\":" << s.down.size()
      << ",\"down\":[";
  for (size_t i = 0; i < s.down.size(); i++) out << (i > 0 ? "," : "") << json_name(s.down[i]);
  out << "],\"events\":" << s.process_events << ",\"kernelEvents\":" << s.kernel_events
      << ",\"suppressed\":" << s.suppressed << ",\"writeFailures\":" << s.write_failures
      << ",\"lastEvent\":" << (s.last_event.empty() ? "null" : s.last_event) << "}";
  return out.str();
}

std::optional<bool> device_started_from_event(const uint8_t* data, size_t size) {
  if (data == nullptr || size < sizeof(capnp::word) || size % sizeof(capnp::word) != 0) return std::nullopt;
  kj::Array<capnp::word> words = kj::heapArray<capnp::word>(size / sizeof(capnp::word));
  std::memcpy(words.begin(), data, size);
  try {
    capnp::ReaderOptions options;
    options.traversalLimitInWords = 1 << 16;
    capnp::FlatArrayMessageReader reader(words, options);
    auto event = reader.getRoot<cereal::Event>();
    if (!event.isDeviceState()) return std::nullopt;
    return event.getDeviceState().getStarted();
  } catch (...) {
    return std::nullopt;
  }
}

ProcessWatcher::ProcessWatcher(ProcessWatchConfig config)
    : config_(std::move(config)), manager_(config_.msgq_dir + "/msgq_managerState") {
  boot_id_ = read_boot_id(config_.boot_id_path);
  summary_.interval_ms = config_.interval_ms;
}

void ProcessWatcher::start() {
  const bool kmsg = kmsg_.active() || (!config_.kmsg_path.empty() && kmsg_.open(config_.kmsg_path));
  {
    std::lock_guard<std::mutex> lk(mutex_);
    summary_.watching = true;
    summary_.kmsg = kmsg;
  }
  EventContext ctx;
  ctx.wall_ms = static_cast<int64_t>(clock_ns(CLOCK_REALTIME) / 1000000ULL);
  ctx.mono_ms = clock_ns(CLOCK_MONOTONIC) / 1000000ULL;
  ctx.boot_id = boot_id_;
  // Written just before the first event, so a healthy comma's log stays empty.
  pending_start_line_ = format_watch_start_line(ctx, config_.interval_ms, kmsg);
}

void ProcessWatcher::tick() {
  tick_at(clock_ns(CLOCK_MONOTONIC), static_cast<int64_t>(clock_ns(CLOCK_REALTIME) / 1000000ULL));
}

EventContext ProcessWatcher::context(uint64_t now_mono_ns, int64_t now_wall_ms, uint64_t manager_age_ns) {
  EventContext ctx;
  ctx.wall_ms = now_wall_ms;
  ctx.mono_ms = now_mono_ns / 1000000ULL;
  ctx.boot_id = boot_id_;
  ctx.manager_age_ms = manager_age_ns / 1000000ULL;
  if (config_.onroad) ctx.onroad = config_.onroad();
  if (config_.road_phase && ctx.onroad) ctx.road_phase = config_.road_phase(*ctx.onroad);
  {
    // Only now, with something to write: deviceState is mapped, read and unmapped again.
    commaview::gps::QueuePeek device(config_.msgq_dir + "/msgq_deviceState");
    if (const auto message = device.newest()) ctx.started = device_started_from_event(message->data(), message->size());
  }
  if (prev_) ctx.down = down_processes(*prev_);  // the table just read
  return ctx;
}

void ProcessWatcher::write(const std::string& line, bool kernel, const std::string& last_event) {
  bool ok = true;
  if (!pending_start_line_.empty()) {
    ok = append_event_line(config_.log_path, pending_start_line_, config_.rotate_bytes);
    pending_start_line_.clear();
  }
  ok = append_event_line(config_.log_path, line, config_.rotate_bytes) && ok;
  std::lock_guard<std::mutex> lk(mutex_);
  if (!ok) summary_.write_failures++;
  if (kernel) {
    summary_.kernel_events++;
  } else if (!last_event.empty()) {
    summary_.process_events++;
    summary_.last_event = last_event;
  }
}

void ProcessWatcher::tick_at(uint64_t now_mono_ns, int64_t now_wall_ms) {
  std::optional<ManagerStateSnapshot> table;
  if (const auto message = manager_.newest()) table = commaview::support::manager_state_from_event(message->data(), message->size());

  if (table) {
    const uint64_t age_ns = now_mono_ns > table->log_mono_ns ? now_mono_ns - table->log_mono_ns : 0;
    const bool silent = table->log_mono_ns != 0 && age_ns > config_.manager_silent_ns;
    const bool new_table = !prev_ || table->log_mono_ns != prev_->log_mono_ns;
    // A stale table is still worth one look when it is the first: what was down when manager stopped.
    if (new_table && (!silent || !prev_)) {
      // After manager was silent it may have restarted: every pid is new, so compare with nothing.
      const bool compare = prev_ && !silent_;
      const auto events = detect_process_events(compare ? &*prev_ : nullptr, *table);
      std::vector<std::string> down = down_processes(*table);
      const bool down_changed = !prev_ || down != down_;
      prev_ = std::move(*table);
      if (!events.empty()) {
        const EventContext ctx = context(now_mono_ns, now_wall_ms, age_ns);
        size_t written = 0;
        for (const auto& e : events) {
          if (written >= config_.events_per_tick) {
            std::lock_guard<std::mutex> lk(mutex_);
            summary_.suppressed++;
            continue;
          }
          write(format_process_event_line(e, ctx), false, last_event_json(now_wall_ms, event_kind_name(e.kind), &e));
          written++;
        }
      }
      std::lock_guard<std::mutex> lk(mutex_);
      summary_.manager_seen = true;
      if (down_changed) {
        down_ = std::move(down);
        summary_.down = down_;
      }
    }
    if (silent != silent_) {
      silent_ = silent;
      const EventContext ctx = context(now_mono_ns, now_wall_ms, age_ns);
      const char* name = silent ? "manager-silent" : "manager-resumed";
      write(format_manager_event_line(silent, ctx), false, last_event_json(now_wall_ms, name, nullptr));
      std::lock_guard<std::mutex> lk(mutex_);
      summary_.manager_silent = silent;
    }
  }

  if (kmsg_.active()) {
    std::vector<KmsgRecord> records;
    const uint64_t dropped_before = kmsg_.dropped();
    kmsg_.poll(config_.kmsg_records_per_tick, config_.kmsg_lines_per_tick, &records);
    if (!records.empty()) {
      const EventContext ctx = context(now_mono_ns, now_wall_ms, 0);
      for (const auto& r : records) write(format_kernel_event_line(r, ctx), true, "");
    }
    const uint64_t dropped = kmsg_.dropped() - dropped_before;
    std::lock_guard<std::mutex> lk(mutex_);
    summary_.suppressed += dropped;
    summary_.kmsg = kmsg_.active();
  }
}

ProcessWatchSummary ProcessWatcher::summary() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return summary_;
}

void lower_current_thread_priority() {
  const pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
  // Each is best effort: a thread that can't lower one priority still lowers the others.
  sched_param param {};
  param.sched_priority = 0;
  (void)pthread_setschedparam(pthread_self(), SCHED_IDLE, &param);
  (void)setpriority(PRIO_PROCESS, static_cast<id_t>(tid), 19);
#if defined(SYS_ioprio_set)
  constexpr int kIoprioWhoProcess = 1;
  constexpr int kIoprioClassIdle = 3;
  constexpr int kIoprioClassShift = 13;
  (void)syscall(SYS_ioprio_set, kIoprioWhoProcess, tid, kIoprioClassIdle << kIoprioClassShift);
#endif
}

}  // namespace commaview::process_events
