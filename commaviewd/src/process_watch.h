#pragma once

// The process watcher: a background loop in control mode that notices when one of openpilot's
// processes goes down, and when the kernel kills one for memory, and writes it to a log that
// survives a reboot (process_events.h). openpilot's manager never restarts a process that dies
// onroad, and its process table lives only in shared memory, so without this the evidence of a
// "Process Not Running" is gone after the reboot that clears it.
//
// What it costs the comma, by design:
//  - one tick every interval_ms (2 s by default): peek the newest managerState message the way
//    manager_state_peek.h does (mapped read-only, no subscriber, no reader slot, no signal to
//    anyone), decode it (a few dozen names and integers), compare it with the last one; and read
//    the few kernel log records written since the last tick from /dev/kmsg (non-blocking)
//  - nothing else unless something changed: then the context (onroad, deviceState.started, road
//    phase) is read and one line per event is appended to the log; no fsync, no lock held while
//    peeking or writing
//  - the thread runs at the lowest CPU and I/O priority (SCHED_IDLE, nice 19, I/O class idle)
//  - it never signals, opens for writing, or otherwise touches anything of openpilot's

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "gps_peek.h"
#include "manager_state_peek.h"
#include "process_events.h"

namespace commaview::process_events {

struct ProcessWatchConfig {
  std::string msgq_dir = "/dev/shm";
  std::string log_path = "/data/commaview/logs/process-events.jsonl";
  size_t rotate_bytes = 256 * 1024;
  std::string kmsg_path = "/dev/kmsg";  // empty: don't follow the kernel log
  std::string boot_id_path = "/proc/sys/kernel/random/boot_id";
  int interval_ms = 2000;
  // managerState comes twice a second; this long without one means manager is gone.
  uint64_t manager_silent_ns = 10ULL * 1000 * 1000 * 1000;
  size_t kmsg_records_per_tick = 256;
  size_t kmsg_lines_per_tick = 16;
  size_t events_per_tick = 32;
  // Read only when there is an event to write.
  std::function<std::optional<bool>()> onroad;
  std::function<std::string(bool onroad)> road_phase;
};

struct ProcessWatchSummary {
  bool watching = false;
  bool kmsg = false;             // following /dev/kmsg
  bool manager_seen = false;     // a managerState table has been read
  bool manager_silent = false;
  int interval_ms = 0;
  std::vector<std::string> down;  // processes down now (as of the newest table)
  uint64_t process_events = 0;    // process and manager events written since this start
  uint64_t kernel_events = 0;
  uint64_t suppressed = 0;        // events past the per-tick caps
  uint64_t write_failures = 0;
  std::string last_event;         // {"t":..,"event":..,"proc":..,"exitCode":..} or empty
};

// {"watching":true,"intervalMs":2000,"downCount":1,"down":["micd"],"events":3,...}
std::string process_watch_summary_json(const ProcessWatchSummary& summary);

// deviceState.started from one serialized cereal Event.
std::optional<bool> device_started_from_event(const uint8_t* data, size_t size);

class ProcessWatcher {
 public:
  explicit ProcessWatcher(ProcessWatchConfig config);

  // Opens /dev/kmsg (when it can) and prepares the watch-start line, written before the first event.
  void start();
  void tick();
  // tick() with the clocks given, for tests.
  void tick_at(uint64_t now_mono_ns, int64_t now_wall_ms);

  ProcessWatchSummary summary() const;
  // Tests hand in a record source instead of /dev/kmsg.
  KmsgTail& kmsg() { return kmsg_; }

 private:
  EventContext context(uint64_t now_mono_ns, int64_t now_wall_ms, uint64_t manager_age_ns);
  void write(const std::string& line, bool kernel, const std::string& last_event);

  ProcessWatchConfig config_;
  commaview::gps::QueuePeek manager_;
  KmsgTail kmsg_;
  std::string boot_id_;
  std::optional<ManagerStateSnapshot> prev_;
  std::vector<std::string> down_;  // down in prev_
  bool silent_ = false;
  std::string pending_start_line_;  // watch-start, written before the first event

  mutable std::mutex mutex_;  // guards summary_ only
  ProcessWatchSummary summary_;
};

// SCHED_IDLE, nice 19 and I/O class idle for the calling thread only.
void lower_current_thread_priority();

}  // namespace commaview::process_events
