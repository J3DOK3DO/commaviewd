// The process watcher: transitions found between two managerState tables, the event lines (escaped,
// capped, kernel lines redacted), the log's rotation, /dev/kmsg records followed and filtered, and a
// whole watcher run over fake msgq queues that leaves their headers untouched. Ends with what one
// tick costs on this machine.
#include "process_events.h"
#include "process_watch.h"

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "cereal/gen/cpp/log.capnp.h"
#include "msgq/msgq.h"

namespace {

using namespace commaview::process_events;

int failures = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    failures++;
  }
}

bool has(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

std::string read_text(const std::string& path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

size_t count_lines(const std::string& text) {
  size_t n = 0;
  for (char c : text) n += c == '\n';
  return n;
}

off_t file_size(const std::string& path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0 ? st.st_size : -1;
}

ProcessState proc(const char* name, int32_t pid, bool running, bool should, int32_t exit_code = 0) {
  ProcessState p;
  p.name = name;
  p.pid = pid;
  p.running = running;
  p.should_be_running = should;
  p.exit_code = exit_code;
  return p;
}

ManagerStateSnapshot table(std::vector<ProcessState> procs, uint64_t mono_ns = 1) {
  ManagerStateSnapshot s;
  s.log_mono_ns = mono_ns;
  s.processes = std::move(procs);
  return s;
}

// A queue file laid out as msgq lays it out, written the way msgq_msg_send writes it.
struct FakeQueue {
  std::string path;
  size_t size;
  uint32_t lap = 0;
  uint64_t offset = 0;

  FakeQueue(std::string p, size_t data_size) : path(std::move(p)), size(data_size) {
    std::vector<char> zeros(sizeof(msgq_header_t) + size, 0);
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(zeros.data(), 1, zeros.size(), f);
    std::fclose(f);
  }

  void write_at(uint64_t pos, const void* bytes, size_t n) {
    FILE* f = std::fopen(path.c_str(), "r+b");
    std::fseek(f, static_cast<long>(pos), SEEK_SET);
    std::fwrite(bytes, 1, n, f);
    std::fclose(f);
  }

  void publish(const std::vector<uint8_t>& message) {
    const uint64_t total = (message.size() + sizeof(int64_t) + 7) & ~uint64_t{7};
    const int64_t remaining = static_cast<int64_t>(size) - static_cast<int64_t>(offset) - static_cast<int64_t>(total) - 8;
    if (remaining <= 0) {
      const int64_t wrap = -1;
      write_at(sizeof(msgq_header_t) + offset, &wrap, sizeof(wrap));
      offset = 0;
      lap++;
    }
    const int64_t n = static_cast<int64_t>(message.size());
    write_at(sizeof(msgq_header_t) + offset, &n, sizeof(n));
    write_at(sizeof(msgq_header_t) + offset + sizeof(int64_t), message.data(), message.size());
    offset = (offset + sizeof(int64_t) + message.size() + 7) & ~uint64_t{7};
    const uint64_t pointer = (uint64_t{lap} << 32) | offset;
    write_at(offsetof(msgq_header_t, write_pointer), &pointer, sizeof(pointer));
  }

  std::vector<char> header() const {
    std::vector<char> out(sizeof(msgq_header_t));
    FILE* f = std::fopen(path.c_str(), "rb");
    size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    assert(got == out.size());
    return out;
  }
};

std::vector<uint8_t> to_bytes(capnp::MallocMessageBuilder& builder) {
  auto words = capnp::messageToFlatArray(builder);
  auto bytes = words.asBytes();
  return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

std::vector<uint8_t> manager_state_event(const std::vector<ProcessState>& procs, uint64_t mono_ns) {
  capnp::MallocMessageBuilder builder;
  auto event = builder.initRoot<cereal::Event>();
  event.setLogMonoTime(mono_ns);
  auto list = event.initManagerState().initProcesses(static_cast<unsigned>(procs.size()));
  for (size_t i = 0; i < procs.size(); i++) {
    list[i].setName(procs[i].name);
    list[i].setPid(procs[i].pid);
    list[i].setRunning(procs[i].running);
    list[i].setShouldBeRunning(procs[i].should_be_running);
    list[i].setExitCode(procs[i].exit_code);
  }
  return to_bytes(builder);
}

std::vector<uint8_t> device_state_event(bool started) {
  capnp::MallocMessageBuilder builder;
  builder.initRoot<cereal::Event>().initDeviceState().setStarted(started);
  return to_bytes(builder);
}

void transitions() {
  const auto base = table({proc("controlsd", 10, true, true), proc("micd", 11, true, true), proc("soundd", 12, true, true),
                           proc("ui", 13, true, true), proc("loggerd", 0, false, false)});

  check(detect_process_events(&base, base).empty(), "the same table: no events");

  auto first = table({proc("micd", 11, false, true, 1), proc("ui", 13, true, true), proc("camerad", 0, false, false)});
  auto seen = detect_process_events(nullptr, first);
  check(seen.size() == 1 && seen[0].kind == EventKind::kDownAtWatchStart && seen[0].name == "micd" &&
            seen[0].exit_code == 1,
        "first look: only what is down while it should run");

  // micd crashes (SIGSEGV), soundd is stopped on purpose (going offroad: shouldBeRunning false).
  auto crashed = base;
  crashed.processes[1] = proc("micd", 11, false, true, -11);
  crashed.processes[2] = proc("soundd", 12, false, false, 0);
  auto events = detect_process_events(&base, crashed);
  check(events.size() == 1 && events[0].kind == EventKind::kDied && events[0].name == "micd" && events[0].pid == 11 &&
            events[0].exit_code == -11,
        "a crash is an event, a stop on purpose is not");
  check(detect_process_events(&crashed, crashed).empty(), "still down: no new event");
  check(down_processes(crashed) == std::vector<std::string>{"micd"}, "down now: micd");

  // manager reports pid 0 for a dead process in some versions: the last live pid is kept.
  auto zero_pid = base;
  zero_pid.processes[1] = proc("micd", 0, false, true, -9);
  events = detect_process_events(&base, zero_pid);
  check(events.size() == 1 && events[0].pid == 11, "a dead process keeps the pid it had");

  // restart_if_crash: started again under a new pid and dead again before the next table.
  auto again = crashed;
  again.processes[1] = proc("micd", 21, false, true, -6);
  events = detect_process_events(&crashed, again);
  check(events.size() == 1 && events[0].kind == EventKind::kDied && events[0].pid == 21 && events[0].prev_pid == 11,
        "a crash loop is an event per new pid");

  // Started and already dead by the next table (going onroad).
  auto offroad = base;
  offroad.processes[4] = proc("loggerd", 0, false, false);
  auto onroad = base;
  onroad.processes[4] = proc("loggerd", 30, false, true, 1);
  events = detect_process_events(&offroad, onroad);
  check(events.size() == 1 && events[0].kind == EventKind::kNotStarted && events[0].name == "loggerd",
        "manager started it and it was dead at once");

  auto recovered = crashed;
  recovered.processes[1] = proc("micd", 40, true, true);
  events = detect_process_events(&crashed, recovered);
  check(events.size() == 1 && events[0].kind == EventKind::kRecovered && events[0].pid == 40 &&
            events[0].prev_pid == 11,
        "running again after being down");

  auto restarted = base;
  restarted.processes[0] = proc("controlsd", 99, true, true);
  events = detect_process_events(&base, restarted);
  check(events.size() == 1 && events[0].kind == EventKind::kRestarted && events[0].pid == 99 && events[0].prev_pid == 10,
        "a new pid while running is a restart");

  auto added = base;
  added.processes.push_back(proc("newd", 50, false, true, 2));
  events = detect_process_events(&base, added);
  check(events.size() == 1 && events[0].kind == EventKind::kNotStarted && events[0].name == "newd",
        "a process new to the table that is already down");

  auto removed = base;
  removed.processes.pop_back();
  check(detect_process_events(&base, removed).empty(), "a process gone from the table is no event");
}

void formatting() {
  EventContext ctx;
  ctx.wall_ms = 1700000000123;
  ctx.mono_ms = 4567;
  ctx.boot_id = "1b2c3d4e-0000-1111-2222-333344445555";
  ctx.onroad = true;
  ctx.started = true;
  ctx.road_phase = "driving";
  ctx.down = {"micd", "soundd"};
  ctx.manager_age_ms = 250;

  ProcessEvent died{EventKind::kDied, "micd", 1234, 1200, -9};
  const std::string line = format_process_event_line(died, ctx);
  check(line ==
            "{\"t\":1700000000123,\"monoMs\":4567,\"boot\":\"1b2c3d4e-0000-1111-2222-333344445555\",\"event\":\"died\","
            "\"proc\":\"micd\",\"pid\":1234,\"prevPid\":1200,\"exitCode\":-9,\"exitSignal\":9,\"onroad\":true,"
            "\"started\":true,\"phase\":\"driving\",\"managerAgeMs\":250,\"down\":[\"micd\",\"soundd\"]}",
        "the event line: " + line);
  check(line.find('\n') == std::string::npos, "one line");

  EventContext bare;
  ProcessEvent odd{EventKind::kNotStarted, std::string("we\"ird\\na\nme\x01") + std::string(100, 'x'), 7, 7, 3};
  const std::string escaped = format_process_event_line(odd, bare);
  check(has(escaped, "\"proc\":\"we\\\"ird\\\\na me ") && !has(escaped, "\n") && !has(escaped, "\x01") &&
            !has(escaped, std::string(100, 'x')) && !has(escaped, "prevPid") && !has(escaped, "exitSignal") &&
            !has(escaped, "onroad") && !has(escaped, "\"started\"") && !has(escaped, "phase") && has(escaped, "\"down\":[]"),
        "names escaped and capped, unknowns left out: " + escaped);

  check(has(format_manager_event_line(true, bare), "\"event\":\"manager-silent\"") &&
            has(format_manager_event_line(false, bare), "\"event\":\"manager-resumed\""),
        "manager lines");
  const std::string start = format_watch_start_line(ctx, 2000, false);
  check(has(start, "\"event\":\"watch-start\"") && has(start, "\"intervalMs\":2000") && has(start, "\"kmsg\":false"),
        "watch-start line: " + start);

  KmsgRecord oom;
  oom.level = 3;
  oom.seq = 812;
  oom.usec = 99000;
  oom.text = "Out of memory: Killed process 4242 (micd) from 8.8.8.8 at aa:bb:cc:dd:ee:ff " + std::string(900, 'y');
  const std::string kline = format_kernel_event_line(oom, ctx);
  check(has(kline, "\"event\":\"kernel\"") && has(kline, "\"kmsgSeq\":812") && has(kline, "\"kmsgUsec\":99000") &&
            has(kline, "Killed process 4242 (micd)") && !has(kline, "8.8.8.8") && !has(kline, "aa:bb:cc") &&
            kline.size() < 512 + 400,
        "kernel line redacted and capped: " + kline.substr(0, 300));
}

void kmsg() {
  const auto parse = [](const std::string& text) { return parse_kmsg_record(text.data(), text.size()); };
  auto r = parse("6,1234,5678901,-;usb 1-1: new device\n SUBSYSTEM=usb\n DEVICE=c189:1\n");
  check(r && r->level == 6 && r->seq == 1234 && r->usec == 5678901 && r->text == "usb 1-1: new device",
        "a record, continuation lines dropped");
  r = parse("11,7,42,c,extra;Out of memory");
  check(r && r->level == 3 && r->seq == 7 && r->usec == 42 && r->text == "Out of memory", "facility bits and extra fields");
  r = parse("3,9,10;no flags\n");
  check(r && r->usec == 10 && r->text == "no flags", "no flags field");
  check(!parse("garbage without header"), "not a record");
  check(!parse("a,b,c,-;text"), "non-numeric header");
  check(!parse("6,1;text"), "too few fields");
  check(!parse_kmsg_record(nullptr, 0), "nothing");

  KmsgTail missing;
  check(!missing.open("/nonexistent/kmsg") && !missing.active(), "an unreadable kmsg is skipped");
  std::vector<KmsgRecord> none;
  missing.poll(10, 10, &none);
  check(none.empty(), "an inactive tail reads nothing");

  int fds[2];
  check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fds) == 0, "socketpair");
  KmsgTail tail;
  tail.adopt(fds[0]);
  std::vector<KmsgRecord> out;
  tail.poll(10, 10, &out);
  check(out.empty() && tail.active(), "nothing new: no records, still active (EAGAIN)");

  const std::vector<std::string> records = {
      "6,100,1000,-;usb 1-1: new high-speed USB device\n",
      "4,101,1001,-;micd invoked oom-killer: gfp_mask=0x6200ca\n",
      "6,102,1002,-;wlan0: associated\n",
      "3,103,1003,-;Out of memory: Killed process 4242 (micd)\n",
      "6,104,1004,-;oom_reaper: reaped process 4242 (micd)\n",
      "6,105,1005,-;lowmemorykiller: Killing 'soundd' (77)\n",
  };
  for (const auto& rec : records) check(send(fds[1], rec.data(), rec.size(), 0) == static_cast<ssize_t>(rec.size()), "send");
  tail.poll(3, 10, &out);
  check(tail.records_read() == 3 && out.size() == 1 && out[0].seq == 101,
        "at most max_records per poll, only memory-pressure lines kept");
  out.clear();
  tail.poll(10, 1, &out);
  check(out.size() == 1 && out[0].seq == 103 && tail.dropped() == 2 && tail.matched() == 4 && tail.records_read() == 6,
        "at most max_matches per poll, the rest counted as dropped");
  out.clear();
  tail.poll(10, 10, &out);
  check(out.empty(), "drained");
  close(fds[1]);
}

void rotation(const std::string& root) {
  const std::string path = root + "/logs/process-events.jsonl";
  const std::string line(99, 'a');  // 100 bytes with its newline
  check(append_event_line(path, line, 1000), "the first line creates the log (and its directory)");
  for (int i = 1; i < 10; i++) append_event_line(path, line, 1000);
  check(file_size(path) == 1000 && file_size(path + ".1") == -1, "ten lines fit exactly");
  append_event_line(path, "b" + std::string(98, 'b'), 1000);
  check(file_size(path) == 100 && file_size(path + ".1") == 1000, "the next line rotates the log to .1");
  for (int i = 0; i < 25; i++) append_event_line(path, line, 1000);
  check(file_size(path) <= 1000 && file_size(path + ".1") <= 1000, "never more than two files of rotate_bytes");
  check(read_text(path + ".1").find('b') == std::string::npos, "only one previous file is kept");

  const std::string target = root + "/elsewhere.txt";
  std::ofstream(target) << "x\n";
  const std::string link = root + "/logs/linked.jsonl";
  check(symlink(target.c_str(), link.c_str()) == 0, "symlink made");
  check(!append_event_line(link, "nope", 1000) && read_text(target) == "x\n", "never written through a symlink");
}

void watcher(const std::string& root) {
  const std::string msgq = root + "/msgq";
  mkdir(msgq.c_str(), 0755);
  const std::string log = root + "/watch/process-events.jsonl";

  int onroad_calls = 0;
  ProcessWatchConfig config;
  config.msgq_dir = msgq;
  config.log_path = log;
  config.kmsg_path.clear();
  config.boot_id_path = root + "/boot_id";
  std::ofstream(config.boot_id_path) << "abcd-boot\n";
  config.onroad = [&onroad_calls]() -> std::optional<bool> {
    onroad_calls++;
    return true;
  };
  config.road_phase = [](bool onroad) { return std::string(onroad ? "driving" : "offroad"); };

  ProcessWatcher w(config);
  check(!w.summary().watching && has(process_watch_summary_json(w.summary()), "{\"watching\":false}"),
        "not watching before start");
  int fds[2];
  check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fds) == 0, "socketpair");
  w.kmsg().adopt(fds[0]);
  w.start();
  std::string text = read_text(log);
  check(text.empty() && file_size(log) == -1, "start writes nothing");

  const uint64_t s = 1000000000ULL;
  w.tick_at(100 * s, 1000);
  check(file_size(log) == -1 && !w.summary().manager_seen, "no queue yet: nothing written");

  FakeQueue manager(msgq + "/msgq_managerState", 1 << 16);
  FakeQueue device(msgq + "/msgq_deviceState", 1 << 14);
  device.publish(device_state_event(true));
  std::vector<ProcessState> procs = {proc("controlsd", 10, true, true), proc("micd", 11, true, true),
                                     proc("soundd", 12, true, true), proc("ui", 13, true, true)};
  manager.publish(manager_state_event(procs, 100 * s));
  const auto device_header = device.header();
  w.tick_at(100 * s + 200000000, 2000);
  check(file_size(log) == -1 && w.summary().manager_seen && w.summary().down.empty() && onroad_calls == 0,
        "all running: nothing written, no context read");

  procs[1] = proc("micd", 11, false, true, -9);
  manager.publish(manager_state_event(procs, 102 * s));
  w.tick_at(102 * s + 300000000, 4000);
  text = read_text(log);
  check(count_lines(text) == 2 && text.rfind("{\"t\":", 0) == 0 && has(text, "\"event\":\"watch-start\"") &&
            has(text, "\"boot\":\"abcd-boot\"") && has(text, "\"kmsg\":true") &&
            text.find("watch-start") < text.find("\"died\"") && has(text, "\"event\":\"died\",\"proc\":\"micd\",\"pid\":11,\"exitCode\":-9,\"exitSignal\":9") &&
            has(text, "\"onroad\":true,\"started\":true,\"phase\":\"driving\",\"managerAgeMs\":300,\"down\":[\"micd\"]") &&
            onroad_calls == 1,
        "micd died: the watch-start line, then one line with its context: " + text);
  w.tick_at(102 * s + 900000000, 4600);
  procs[2] = proc("soundd", 12, false, false);  // stopped on purpose
  manager.publish(manager_state_event(procs, 104 * s));
  w.tick_at(104 * s + 100000000, 6000);
  check(count_lines(read_text(log)) == 2, "the same table again, and a stop on purpose: nothing written");
  auto summary = w.summary();
  check(summary.down == std::vector<std::string>{"micd"} && summary.process_events == 1 &&
            has(summary.last_event, "\"event\":\"died\",\"proc\":\"micd\",\"exitCode\":-9"),
        "the summary: micd down, its death the last event");
  const std::string json = process_watch_summary_json(summary);
  check(has(json, "\"watching\":true") && has(json, "\"downCount\":1") && has(json, "\"down\":[\"micd\"]") &&
            has(json, "\"events\":1") && has(json, "\"lastEvent\":{\"t\":4000,\"event\":\"died\""),
        "summary json: " + json);

  const std::string oom = "3,500,9000,-;Out of memory: Killed process 11 (micd)\n";
  const std::string noise = "6,501,9001,-;usb 1-1: hello\n";
  send(fds[1], oom.data(), oom.size(), 0);
  send(fds[1], noise.data(), noise.size(), 0);
  w.tick_at(104 * s + 500000000, 6500);
  text = read_text(log);
  check(count_lines(text) == 3 && has(text, "\"event\":\"kernel\"") && has(text, "Killed process 11 (micd)") &&
            !has(text, "usb 1-1") && w.summary().kernel_events == 1,
        "an OOM kill from kmsg is written, other kernel lines aren't: " + text);

  // manager goes quiet (it died, or openpilot is restarting), then comes back with new pids.
  w.tick_at(120 * s, 20000);
  text = read_text(log);
  check(count_lines(text) == 4 && has(text, "\"event\":\"manager-silent\"") && w.summary().manager_silent,
        "manager silent: one line");
  w.tick_at(130 * s, 30000);
  check(count_lines(read_text(log)) == 4, "still silent: nothing more");
  std::vector<ProcessState> fresh = {proc("controlsd", 110, true, true), proc("micd", 111, true, true),
                                     proc("soundd", 112, true, true), proc("ui", 113, true, true)};
  manager.publish(manager_state_event(fresh, 131 * s));
  w.tick_at(131 * s + 100000000, 31000);
  text = read_text(log);
  check(count_lines(text) == 5 && has(text, "\"event\":\"manager-resumed\"") && !has(text, "\"restarted\"") &&
            w.summary().down.empty(),
        "manager back: one line, new pids after a restart are not restarts: " + text);

  const auto manager_header = manager.header();
  w.tick_at(131 * s + 500000000, 31500);
  check(manager.header() == manager_header && device.header() == device_header,
        "queue headers untouched: no reader slot, no read pointer");
  close(fds[1]);

  {
    // Started after manager went quiet: the last table still says what was down.
    const std::string stale_dir = root + "/stale";
    mkdir(stale_dir.c_str(), 0755);
    FakeQueue stale(stale_dir + "/msgq_managerState", 1 << 14);
    stale.publish(manager_state_event({proc("controlsd", 10, true, true), proc("modeld", 11, false, true, -9)}, 5 * s));
    ProcessWatchConfig late = config;
    late.msgq_dir = stale_dir;
    late.log_path = stale_dir + "/events.jsonl";
    ProcessWatcher w2(late);
    w2.start();
    w2.tick_at(500 * s, 9000);
    w2.tick_at(502 * s, 9002);
    const std::string late_text = read_text(late.log_path);
    check(count_lines(late_text) == 3 && has(late_text, "\"event\":\"down-first-seen\",\"proc\":\"modeld\"") &&
              has(late_text, "\"event\":\"manager-silent\"") && has(late_text, "\"kmsg\":false") &&
              w2.summary().down == std::vector<std::string>{"modeld"},
          "a stale first table: what was down, then manager silent: " + late_text);
  }

  // What a tick costs: a 40-process table, published fresh before each tick, compared and found
  // unchanged (the common case), on this machine.
  std::vector<ProcessState> big;
  for (int i = 0; i < 40; i++) {
    big.push_back(proc(("process" + std::to_string(i)).c_str(), 1000 + i, true, true));
  }
  const int kTicks = 2000;
  const auto msg = manager_state_event(big, 0);
  double total_us = 0;
  for (int i = 0; i < kTicks; i++) {
    manager.publish(manager_state_event(big, (200 + i) * s));
    const auto t0 = std::chrono::steady_clock::now();
    w.tick_at((200 + i) * s + 1000, 40000 + i);
    total_us += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
  }
  std::printf("process watcher tick: %.1f us on average over %d ticks (40-process table, %zu bytes)\n",
              total_us / kTicks, kTicks, msg.size());
  check(total_us / kTicks < 2000.0, "a tick takes well under 2 ms");
}

}  // namespace

int main() {
  char dir_template[] = "/tmp/commaview-process-events-XXXXXX";
  const std::string root = mkdtemp(dir_template);

  transitions();
  formatting();
  kmsg();
  rotation(root);
  watcher(root);

  const std::string cleanup = "rm -rf '" + root + "'";
  if (std::system(cleanup.c_str()) != 0) std::fprintf(stderr, "warning: could not remove %s\n", root.c_str());
  if (failures == 0) std::printf("PASS: process watcher transitions, event log, kmsg and rotation\n");
  return failures == 0 ? 0 : 1;
}
