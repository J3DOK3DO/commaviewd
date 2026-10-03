#pragma once

// openpilot's process events, kept across reboots: what the process watcher (process_watch.h)
// notices in managerState and in the kernel log, as one JSON line per event in a small log under
// /data/commaview. openpilot's manager never restarts a process that dies onroad and keeps its
// process table only in shared memory, so after a reboot this log is the only record of which
// process went down, when, and with what exit code.
//
// Everything here is pure or touches only the file and descriptor it is given, so it can be
// tested on any machine.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "manager_state_peek.h"

namespace commaview::process_events {

using commaview::support::ManagerStateSnapshot;
using commaview::support::ProcessState;

// What changed for one process between two managerState tables. manager's own meaning of the
// fields: shouldBeRunning is "manager started it and hasn't stopped it", running is "it is alive",
// so a process manager stops on purpose (going offroad) has shouldBeRunning false and is no event.
enum class EventKind {
  kDied,             // was running, isn't, and manager never stopped it (a crash or a kill)
  kNotStarted,       // manager started it, and by its next table it was already dead
  kRestarted,        // running in both tables, under a new pid (a watchdog restart)
  kRecovered,        // was down while it should run, running again
  kDownAtWatchStart, // "down-first-seen": down while it should run in the first table the watcher
                     // compares (at its start, or after manager was silent and may have restarted)
};

const char* event_kind_name(EventKind kind);

struct ProcessEvent {
  EventKind kind = EventKind::kDied;
  std::string name;
  int32_t pid = 0;
  int32_t prev_pid = 0;
  int32_t exit_code = 0;
};

// The events between prev (nullptr: nothing to compare with) and cur, in cur's process order.
// Processes that left the table are ignored (manager's table only changes when manager restarts).
std::vector<ProcessEvent> detect_process_events(const ManagerStateSnapshot* prev, const ManagerStateSnapshot& cur);

// Names of the processes that should run but don't, in table order.
std::vector<std::string> down_processes(const ManagerStateSnapshot& snapshot);

// When and where an event happened, gathered only when there is something to write.
struct EventContext {
  int64_t wall_ms = 0;        // CLOCK_REALTIME; before the clock is set it is still written
  uint64_t mono_ms = 0;       // CLOCK_MONOTONIC
  std::string boot_id;        // /proc/sys/kernel/random/boot_id
  std::optional<bool> onroad;   // openpilot's IsOffroad, inverted
  std::optional<bool> started;  // deviceState.started
  std::string road_phase;       // offroad / parked / driving (road_phase.h), empty when unknown
  std::vector<std::string> down;  // every process down at that moment
  uint64_t manager_age_ms = 0;    // how old the managerState table was (0: unknown)
};

// One JSON object (no newline) for a process event:
// {"t":..,"monoMs":..,"boot":"..","event":"died","proc":"micd","pid":..,"exitCode":-9,"exitSignal":9,
//  "onroad":true,"started":true,"phase":"driving","down":["micd"]}
// Names are escaped and capped; nothing in a process table is private.
std::string format_process_event_line(const ProcessEvent& event, const EventContext& ctx);

// manager stopped publishing managerState (it died, or openpilot is restarting), or resumed.
std::string format_manager_event_line(bool silent, const EventContext& ctx);

// The watcher started (written once per start, just before its first event).
std::string format_watch_start_line(const EventContext& ctx, int interval_ms, bool kmsg);

// ---- Kernel log, as /dev/kmsg hands it out: one record per read, "pri,seq,usec,flags;text\n"
// followed by " KEY=value" continuation lines.

struct KmsgRecord {
  int level = 0;          // syslog level (the priority's low 3 bits)
  uint64_t seq = 0;
  uint64_t usec = 0;      // since boot
  std::string text;       // the message, continuation lines dropped
};

std::optional<KmsgRecord> parse_kmsg_record(const char* data, size_t size);

inline constexpr size_t kKernelLineCapBytes = 512;

// One JSON object for a kernel memory-pressure line, redacted (support_bundle.h) and capped.
std::string format_kernel_event_line(const KmsgRecord& record, const EventContext& ctx);

// Follows /dev/kmsg from where it ended when opened: only records written after that are read,
// never the whole ring, and reading moves nothing for anyone else (each open has its own position).
class KmsgTail {
 public:
  KmsgTail() = default;
  ~KmsgTail();
  KmsgTail(const KmsgTail&) = delete;
  KmsgTail& operator=(const KmsgTail&) = delete;

  // False (and nothing is read later) when the device can't be opened, e.g. dmesg_restrict.
  bool open(const std::string& path);
  // Takes a descriptor that hands out one record per read (tests use a SOCK_SEQPACKET pair).
  void adopt(int fd);
  bool active() const { return fd_ >= 0; }

  // Reads at most max_records new records without blocking; appends at most max_matches of those
  // matching kernel_line_matches (support_bundle.h) to *out.
  void poll(size_t max_records, size_t max_matches, std::vector<KmsgRecord>* out);

  uint64_t records_read() const { return records_read_; }
  uint64_t matched() const { return matched_; }
  uint64_t dropped() const { return dropped_; }   // matches past max_matches in a poll
  uint64_t overruns() const { return overruns_; } // records the kernel overwrote before we read them

 private:
  int fd_ = -1;
  uint64_t records_read_ = 0;
  uint64_t matched_ = 0;
  uint64_t dropped_ = 0;
  uint64_t overruns_ = 0;
};

// ---- The log file

// Appends line + '\n' to path (O_APPEND, created 0644, never through a symlink). When the file
// would grow past rotate_bytes it first becomes path.1 (replacing the previous one), so the log
// never takes more than about 2 * rotate_bytes. No fsync: an event costs one open, write, close.
bool append_event_line(const std::string& path, const std::string& line, size_t rotate_bytes);

}  // namespace commaview::process_events
