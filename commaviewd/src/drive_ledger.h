#pragma once

// The drives this comma has made, recorded while they happen.
//
// A comma often loses power the moment the car is switched off, so nothing about a drive may wait
// for "after the drive". The control service measures the distance live from carState.vEgo (read
// from its msgq queue without subscribing, as road_phase.h reads it) and keeps each drive in its own
// small file, data/drives/<startMs>.json, rewritten while the drive goes on: every
// kCheckpointInterval and whenever the car comes to a stop. A drive cut off by the power is already
// on disk up to its last write, and since a car stops before it's switched off, that is all of it.
//
// A drive's file: {"startMs", "endMs", "durationS", "distanceM", "route"?}, the fields the drive list
// always had (route only when loggerd named it). The list is the folder; files whose drive ended more
// than kKeepDays ago are deleted. data/drives-since.json ({"sinceMs"}) says when the comma started
// keeping them. The drive list from before (one data/drives.json the after-drive script rewrote) is
// moved into the folder once.
//
// Distance: the same rule the after-drive script applied to the qlogs: speed samples in order of
// logMonoTime, each pair at most kMaxSampleGapS apart joined by their average speed (never below 0),
// and gaps longer than that not counted at all.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace commaview::drives {

inline constexpr double kMaxSampleGapS = 1.0;
inline constexpr int kKeepDays = 62;
inline constexpr int64_t kDayMs = 86'400'000;
// 2024: before that the clock hasn't been set (no GPS or network time since boot).
inline constexpr int64_t kValidWallMs = 1'704'067'200'000LL;
// How often a drive's file is rewritten while it goes on (it's also rewritten at every stop).
inline constexpr int kCheckpointIntervalSec = 30;
// At least this long between two writes of one drive, however often the car stops.
inline constexpr int kMinCheckpointGapSec = 3;
// Slower than this is stopped.
inline constexpr float kStoppedMs = 0.3f;

bool valid_wall_ms(int64_t ms);

// Distance from carState.vEgo samples, in the order they arrive.
class DistanceMeter {
 public:
  // A sample at log_mono_ns (CLOCK_MONOTONIC ns) going v_ego m/s. One that isn't newer than the
  // last is ignored.
  void add(uint64_t mono_ns, float v_ego);
  // The next sample starts afresh (not joined to the last).
  void forget_last();
  void reset();
  double distance_m() const { return distance_m_; }

 private:
  double distance_m_ = 0.0;
  bool have_last_ = false;
  uint64_t last_mono_ns_ = 0;
  float last_speed_ = 0.0f;
};

struct DriveRecord {
  int64_t start_ms = 0;
  int64_t end_ms = 0;
  int64_t duration_s = 0;
  int64_t distance_m = 0;
  std::string route;  // empty when loggerd never named it
};

// {"startMs":..,"endMs":..,"durationS":..,"distanceM":..,"route":".."}, route left out when empty.
std::string record_json(const DriveRecord& record);
// A drive as its file (or the old drive list) holds it: start and end on a set clock, end not
// before start, counts not negative. Nothing for anything else.
std::optional<DriveRecord> parse_record(const std::string& json);
std::string record_file_name(int64_t start_ms);

struct StoredDrive {
  std::string path;
  DriveRecord record;
};

// The drives in dir (files named <startMs>.json holding a drive), oldest start first.
std::vector<StoredDrive> list_drives(const std::string& dir);
// The drive in dir with this route, if there is one (the newest, should there be more).
std::optional<StoredDrive> find_route(const std::string& dir, const std::string& route);
// Deletes the drives that ended more than keep_days before now_ms; how many. Nothing without a set clock.
int prune_drives(const std::string& dir, int64_t now_ms, int keep_days = kKeepDays);

// The drive list for the phone (/commaview/drive-stats "ledger"): {"version":1,"sinceMs":..,"drives":[..]}.
// skip_path (the drive going on) is left out. "null" when nothing has been kept yet.
std::string ledger_json(std::optional<int64_t> since_ms, const std::vector<StoredDrive>& drives,
                        const std::string& skip_path = "");

// What the folder holds, cheaply (each file's name, size, modification time and inode), so a change
// to any drive shows: "none" when there's no folder.
std::string folder_signature(const std::string& dir);

// The drive list from before the folder (data/drives.json): its sinceMs and drives.
struct LegacyLedger {
  std::optional<int64_t> since_ms;
  std::vector<DriveRecord> drives;
};
std::optional<LegacyLedger> parse_legacy_ledger(const std::string& json);

// {"sinceMs":..} as data/drives-since.json holds it.
std::optional<int64_t> parse_since(const std::string& json);
std::string since_json(int64_t since_ms);

// Owner-only, on disk before it replaces what was there: written beside path, flushed, renamed over
// it, and the folder flushed, so a power cut leaves the old file or the new one, never a torn one.
bool write_file_durable(const std::string& path, const std::string& content);
std::string read_small_file(const std::string& path, size_t cap_bytes = 64 * 1024);

}  // namespace commaview::drives
