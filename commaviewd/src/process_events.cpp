#include "process_events.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <unordered_map>

#include "support_bundle.h"

namespace commaview::process_events {
namespace {

constexpr size_t kMaxNameBytes = 64;
constexpr size_t kMaxBootIdBytes = 64;
// A kmsg record is at most a few KiB (the kernel's own limit for /dev/kmsg is 8 KiB).
constexpr size_t kKmsgReadBytes = 8192;

// JSON string with control characters (and DEL) turned into spaces; capped at cap bytes.
std::string json_string(const std::string& in, size_t cap) {
  std::string out = "\"";
  const size_t n = in.size() < cap ? in.size() : cap;
  for (size_t i = 0; i < n; i++) {
    const char c = in[i];
    const unsigned char uc = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (uc < 0x20 || uc == 0x7f) {
      out += ' ';
    } else {
      out += c;
    }
  }
  out += '"';
  return out;
}

void context_head(std::ostringstream& out, const EventContext& ctx, const char* event) {
  out << "{\"t\":" << ctx.wall_ms << ",\"monoMs\":" << ctx.mono_ms
      << ",\"boot\":" << json_string(ctx.boot_id, kMaxBootIdBytes) << ",\"event\":\"" << event << "\"";
}

void context_tail(std::ostringstream& out, const EventContext& ctx) {
  if (ctx.onroad) out << ",\"onroad\":" << (*ctx.onroad ? "true" : "false");
  if (ctx.started) out << ",\"started\":" << (*ctx.started ? "true" : "false");
  if (!ctx.road_phase.empty()) out << ",\"phase\":" << json_string(ctx.road_phase, 32);
  if (ctx.manager_age_ms > 0) out << ",\"managerAgeMs\":" << ctx.manager_age_ms;
  out << ",\"down\":[";
  for (size_t i = 0; i < ctx.down.size(); i++) {
    if (i > 0) out << ",";
    out << json_string(ctx.down[i], kMaxNameBytes);
  }
  out << "]}";
}

bool parse_u64(const char* begin, const char* end, uint64_t* value) {
  if (begin >= end) return false;
  uint64_t v = 0;
  for (const char* p = begin; p < end; p++) {
    if (*p < '0' || *p > '9') return false;
    v = v * 10 + static_cast<uint64_t>(*p - '0');
  }
  *value = v;
  return true;
}

}  // namespace

const char* event_kind_name(EventKind kind) {
  switch (kind) {
    case EventKind::kDied: return "died";
    case EventKind::kNotStarted: return "not-started";
    case EventKind::kRestarted: return "restarted";
    case EventKind::kRecovered: return "recovered";
    case EventKind::kDownAtWatchStart: return "down-first-seen";
  }
  return "died";
}

std::vector<ProcessEvent> detect_process_events(const ManagerStateSnapshot* prev, const ManagerStateSnapshot& cur) {
  std::vector<ProcessEvent> events;
  std::unordered_map<std::string, const ProcessState*> before;
  if (prev != nullptr) {
    before.reserve(prev->processes.size());
    for (const auto& q : prev->processes) before.emplace(q.name, &q);
  }
  const auto add = [&events](EventKind kind, const ProcessState& p, int32_t pid, int32_t prev_pid) {
    events.push_back(ProcessEvent{kind, p.name, pid, prev_pid, p.exit_code});
  };
  for (const auto& p : cur.processes) {
    const bool down = p.should_be_running && !p.running;
    if (prev == nullptr) {
      if (down) add(EventKind::kDownAtWatchStart, p, p.pid, 0);
      continue;
    }
    const auto it = before.find(p.name);
    if (it == before.end()) {
      if (down) add(EventKind::kNotStarted, p, p.pid, 0);
      continue;
    }
    const ProcessState& q = *it->second;
    if (down) {
      if (q.running) {
        // manager keeps the dead process's pid; fall back to the one it had while alive.
        add(EventKind::kDied, p, p.pid != 0 ? p.pid : q.pid, q.pid);
      } else if (!q.should_be_running) {
        add(EventKind::kNotStarted, p, p.pid, 0);
      } else if (p.pid != 0 && p.pid != q.pid) {
        // Started again (restart_if_crash) and dead again before the next table.
        add(EventKind::kDied, p, p.pid, q.pid);
      }
      continue;
    }
    if (!p.running) continue;
    if (q.should_be_running && !q.running) {
      add(EventKind::kRecovered, p, p.pid, q.pid);
    } else if (q.running && q.pid != 0 && p.pid != 0 && q.pid != p.pid) {
      add(EventKind::kRestarted, p, p.pid, q.pid);
    }
  }
  return events;
}

std::vector<std::string> down_processes(const ManagerStateSnapshot& snapshot) {
  std::vector<std::string> names;
  for (const auto& p : snapshot.processes) {
    if (p.should_be_running && !p.running) names.push_back(p.name);
  }
  return names;
}

std::string format_process_event_line(const ProcessEvent& event, const EventContext& ctx) {
  std::ostringstream out;
  context_head(out, ctx, event_kind_name(event.kind));
  out << ",\"proc\":" << json_string(event.name, kMaxNameBytes) << ",\"pid\":" << event.pid;
  if (event.prev_pid != 0 && event.prev_pid != event.pid) out << ",\"prevPid\":" << event.prev_pid;
  out << ",\"exitCode\":" << event.exit_code;
  // A negative exit code is the signal that ended it (-9: SIGKILL, often the OOM killer).
  if (event.exit_code < 0) out << ",\"exitSignal\":" << -static_cast<int64_t>(event.exit_code);
  context_tail(out, ctx);
  return out.str();
}

std::string format_manager_event_line(bool silent, const EventContext& ctx) {
  std::ostringstream out;
  context_head(out, ctx, silent ? "manager-silent" : "manager-resumed");
  context_tail(out, ctx);
  return out.str();
}

std::string format_watch_start_line(const EventContext& ctx, int interval_ms, bool kmsg) {
  std::ostringstream out;
  context_head(out, ctx, "watch-start");
  out << ",\"intervalMs\":" << interval_ms << ",\"kmsg\":" << (kmsg ? "true" : "false");
  context_tail(out, ctx);
  return out.str();
}

std::optional<KmsgRecord> parse_kmsg_record(const char* data, size_t size) {
  if (data == nullptr || size == 0) return std::nullopt;
  const char* end = data + size;
  const char* nl = static_cast<const char*>(std::memchr(data, '\n', size));
  const char* line_end = nl != nullptr ? nl : end;
  const char* semi = static_cast<const char*>(std::memchr(data, ';', static_cast<size_t>(line_end - data)));
  if (semi == nullptr) return std::nullopt;
  // pri,seq,usec,flags[,more...]
  uint64_t fields[3] = {0, 0, 0};
  const char* p = data;
  for (int i = 0; i < 3; i++) {
    const char* comma = p;
    while (comma < semi && *comma != ',') comma++;
    if (!parse_u64(p, comma, &fields[i])) return std::nullopt;
    if (comma >= semi) {
      if (i < 2) return std::nullopt;
      p = semi;
    } else {
      p = comma + 1;
    }
  }
  KmsgRecord record;
  record.level = static_cast<int>(fields[0] & 7);
  record.seq = fields[1];
  record.usec = fields[2];
  record.text.assign(semi + 1, line_end);
  return record;
}

std::string format_kernel_event_line(const KmsgRecord& record, const EventContext& ctx) {
  std::string text = commaview::support::redact_support_text(record.text, {});
  if (text.size() > kKernelLineCapBytes) text.resize(kKernelLineCapBytes);
  std::ostringstream out;
  context_head(out, ctx, "kernel");
  out << ",\"level\":" << record.level << ",\"kmsgSeq\":" << record.seq << ",\"kmsgUsec\":" << record.usec
      << ",\"line\":" << json_string(text, kKernelLineCapBytes) << "}";
  return out.str();
}

KmsgTail::~KmsgTail() {
  if (fd_ >= 0) ::close(fd_);
}

bool KmsgTail::open(const std::string& path) {
  if (fd_ >= 0) ::close(fd_);
  fd_ = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd_ < 0) return false;
  // Start after the newest record: the ring's past is the support bundle's (dmesg) business.
  (void)::lseek(fd_, 0, SEEK_END);
  return true;
}

void KmsgTail::adopt(int fd) {
  if (fd_ >= 0) ::close(fd_);
  fd_ = fd;
}

void KmsgTail::poll(size_t max_records, size_t max_matches, std::vector<KmsgRecord>* out) {
  if (fd_ < 0) return;
  char buf[kKmsgReadBytes];
  size_t kept = 0;
  for (size_t i = 0; i < max_records; i++) {
    const ssize_t n = ::read(fd_, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      if (errno == EINTR) continue;
      if (errno == EPIPE) {
        // The kernel overwrote records before we got to them; the next read goes on after them.
        overruns_++;
        continue;
      }
      ::close(fd_);
      fd_ = -1;
      return;
    }
    if (n == 0) return;
    records_read_++;
    const auto record = parse_kmsg_record(buf, static_cast<size_t>(n));
    if (!record || !commaview::support::kernel_line_matches(record->text)) continue;
    matched_++;
    if (kept >= max_matches || out == nullptr) {
      dropped_++;
      continue;
    }
    out->push_back(*record);
    kept++;
  }
}

bool append_event_line(const std::string& path, const std::string& line, size_t rotate_bytes) {
  const int flags = O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW;
  int fd = ::open(path.c_str(), flags, 0644);
  if (fd < 0 && errno == ENOENT) {
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0) ::mkdir(path.substr(0, slash).c_str(), 0755);
    fd = ::open(path.c_str(), flags, 0644);
  }
  if (fd < 0) return false;
  struct stat st {};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    return false;
  }
  const size_t size = st.st_size > 0 ? static_cast<size_t>(st.st_size) : 0;
  if (size > 0 && size + line.size() + 1 > rotate_bytes) {
    ::close(fd);
    if (::rename(path.c_str(), (path + ".1").c_str()) != 0) return false;
    fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0) return false;
  }
  std::string out = line;
  out += '\n';
  size_t done = 0;
  while (done < out.size()) {
    const ssize_t w = ::write(fd, out.data() + done, out.size() - done);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) break;
    done += static_cast<size_t>(w);
  }
  ::close(fd);
  return done == out.size();
}

}  // namespace commaview::process_events
