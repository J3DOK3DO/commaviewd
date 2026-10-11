// The drive list recorded while each drive happens (src/drive_ledger.h): distance from carState.vEgo
// by the after-drive script's old rule, one file per drive rewritten safely, the old drives.json
// moved in, and the phone's ledger built from the folder.
#include "drive_ledger.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace commaview::drives;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    failures++;
  }
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

constexpr uint64_t kMs = 1'000'000ULL;
constexpr int64_t kT0 = 1'780'000'000'000LL;  // 2026, a set clock

void test_meter() {
  DistanceMeter meter;
  // 10 Hz at 10 m/s for one second: 10 m.
  for (int i = 0; i <= 10; ++i) meter.add(1000 * kMs + i * 100 * kMs, 10.0f);
  check(near(meter.distance_m(), 10.0), "10 m/s for 1 s at 10 Hz is 10 m");

  // Speeding up: the average of each pair.
  meter.reset();
  meter.add(0 + kMs, 0.0f);
  meter.add(kMs + 500 * kMs, 10.0f);
  check(near(meter.distance_m(), 2.5), "0 to 10 m/s over 0.5 s is 2.5 m");

  // A gap over a second is never counted; exactly a second is.
  meter.reset();
  meter.add(kMs, 20.0f);
  meter.add(kMs + 1001 * kMs, 20.0f);
  check(near(meter.distance_m(), 0.0), "samples 1.001 s apart aren't joined");
  meter.add(kMs + 2001 * kMs, 20.0f);
  check(near(meter.distance_m(), 20.0), "samples 1 s apart are");

  // Out of order or repeated samples are ignored; negative speed never subtracts.
  meter.add(kMs + 1500 * kMs, 100.0f);
  meter.add(kMs + 2001 * kMs, 100.0f);
  check(near(meter.distance_m(), 20.0), "a sample no newer than the last is ignored");
  meter.add(kMs + 2501 * kMs, -30.0f);
  check(near(meter.distance_m(), 20.0), "reversing counts as nothing, never less");
  meter.add(kMs + 3001 * kMs, NAN);
  check(near(meter.distance_m(), 20.0), "a speed that isn't a number is ignored");

  // A new drive starts afresh.
  meter.forget_last();
  meter.add(kMs + 3100 * kMs, 50.0f);
  check(near(meter.distance_m(), 20.0), "after forget_last the next sample isn't joined to the last");
}

void test_records() {
  DriveRecord record{kT0, kT0 + 600'000, 600, 12'345, "0000002a--8f1e2d3c4b"};
  const std::string json = record_json(record);
  check(json == "{\"startMs\":1780000000000,\"endMs\":1780000600000,\"durationS\":600,\"distanceM\":12345,"
                "\"route\":\"0000002a--8f1e2d3c4b\"}", "a drive's file holds the drive list's fields");
  const auto back = parse_record(json);
  check(back && back->start_ms == kT0 && back->end_ms == kT0 + 600'000 && back->duration_s == 600 &&
            back->distance_m == 12'345 && back->route == "0000002a--8f1e2d3c4b",
        "a drive's file reads back as written");
  record.route.clear();
  check(record_json(record).find("route") == std::string::npos, "no route, no route field");
  record.route = "a\"b\\c";
  const auto escaped = parse_record(record_json(record));
  check(escaped && escaped->route == "a\"b\\c", "a route is escaped and read back");

  // As the old drive list's Python wrote them, with or without spaces, distance a float.
  const auto spaced = parse_record("{ \"startMs\": 1780000000000, \"endMs\": 1780000600000, \"durationS\": 600, "
                                   "\"distanceM\": 9000.6 }");
  check(spaced && spaced->distance_m == 9001, "spaces and a fractional distance are read like the script read them");
  check(!parse_record("{\"startMs\":1600000000000,\"endMs\":1780000600000}"), "a start before the clock was set isn't a drive");
  check(!parse_record("{\"startMs\":1780000000000.5,\"endMs\":1780000600000}"), "a fractional start isn't a drive");
  check(!parse_record("{\"endMs\":1780000600000}"), "no start, no drive");
  check(!parse_record("{\"startMs\":1780000000000,\"endMs\":1780000600000"), "torn JSON isn't a drive");
  check(!parse_record("[1,2]"), "an array isn't a drive");
  const auto negative = parse_record("{\"startMs\":1780000000000,\"endMs\":1780000600000,\"durationS\":-5,\"distanceM\":-1}");
  check(negative && negative->duration_s == 0 && negative->distance_m == 0, "counts are never below 0");
  check(record_file_name(kT0) == "1780000000000.json", "a drive's file is named by its start");
}

std::string temp_dir() {
  char dir_template[] = "/tmp/commaview-drive-ledger-XXXXXX";
  return mkdtemp(dir_template);
}

void test_folder() {
  const std::string root = temp_dir();
  const std::string dir = root + "/drives";
  check(list_drives(dir).empty(), "no folder, no drives");
  check(folder_signature(dir) == "none", "no folder signs as none");
  check(ledger_json(std::nullopt, {}) == "null", "nothing kept yet is a null ledger");

  DriveRecord a{kT0, kT0 + 600'000, 600, 9000, "route-a"};
  DriveRecord b{kT0 + 7'200'000, kT0 + 7'500'000, 300, 3000, ""};
  check(write_file_durable(dir + "/" + record_file_name(b.start_ms), record_json(b)), "the folder is made and a drive written");
  check(write_file_durable(dir + "/" + record_file_name(a.start_ms), record_json(a)), "a second drive is written");
  struct stat st {};
  check(stat(dir.c_str(), &st) == 0 && (st.st_mode & 0777) == 0700, "the folder is owner-only");
  check(stat((dir + "/" + record_file_name(a.start_ms)).c_str(), &st) == 0 && (st.st_mode & 0777) == 0600,
        "a drive's file is owner-only");
  check(access((dir + "/" + record_file_name(a.start_ms) + ".tmp").c_str(), F_OK) != 0, "nothing is left beside it");

  // Files that aren't drives are ignored.
  write_file_durable(dir + "/notes.json", "{}");
  write_file_durable(dir + "/123.json", "not json");
  const auto drives = list_drives(dir);
  check(drives.size() == 2 && drives[0].record.route == "route-a" && drives[1].record.start_ms == b.start_ms,
        "the drives, oldest first, and nothing else");

  const auto found = find_route(dir, "route-a");
  check(found && found->record.distance_m == 9000, "a drive is found by its route");
  check(!find_route(dir, "route-z") && !find_route(dir, ""), "no route, no drive");

  const std::string ledger = ledger_json(kT0 - 1000, drives);
  check(ledger == "{\"version\":1,\"sinceMs\":1779999999000,\"drives\":[" + record_json(a) + "," + record_json(b) + "]}",
        "the phone's ledger lists every drive since when the comma started keeping them");
  check(ledger_json(std::nullopt, drives).find("\"sinceMs\":1780000000000") != std::string::npos,
        "with no since, the first drive's start");
  const std::string without = ledger_json(kT0 - 1000, drives, drives[1].path);
  check(without.find("route-a") != std::string::npos && without.find("\"startMs\":1780007200000") == std::string::npos,
        "the drive going on is left out");

  // Rewriting a drive changes the folder's signature, even at the same size.
  const std::string before = folder_signature(dir);
  check(before == folder_signature(dir), "an unchanged folder signs the same");
  DriveRecord a2 = a;
  a2.distance_m = 9001;
  write_file_durable(dir + "/" + record_file_name(a.start_ms), record_json(a2));
  check(folder_signature(dir) != before, "a drive rewritten changes the signature");

  // Pruning: only drives that ended more than 62 days ago, and only on a set clock.
  check(prune_drives(dir, 1000) == 0 && list_drives(dir).size() == 2, "no set clock, nothing pruned");
  check(prune_drives(dir, kT0 + 62 * kDayMs) == 0, "a drive that ended 62 days ago stays");
  check(prune_drives(dir, b.end_ms + 62 * kDayMs) == 1, "a drive that ended earlier goes");
  const auto left = list_drives(dir);
  check(left.size() == 1 && left[0].record.start_ms == b.start_ms, "the newer drive is kept");

  std::string cleanup = "rm -rf '" + root + "'";
  if (std::system(cleanup.c_str()) != 0) std::fprintf(stderr, "warning: could not remove %s\n", root.c_str());
}

void test_legacy() {
  const auto ledger = parse_legacy_ledger(
      "{\"version\":1,\"sinceMs\":1780000000000,\"drives\":["
      "{\"startMs\":1780000000000,\"endMs\":1780000600000,\"durationS\":600,\"distanceM\":12000,\"route\":\"r1\"},"
      "{\"startMs\":0,\"endMs\":1780000600000,\"durationS\":1,\"distanceM\":1},"
      "{\"startMs\":1780001000000,\"endMs\":1780001300000,\"durationS\":300,\"distanceM\":0}]}");
  check(ledger && ledger->since_ms == kT0 && ledger->drives.size() == 2, "the old list's since and its usable drives");
  check(ledger && ledger->drives[0].route == "r1" && ledger->drives[1].distance_m == 0, "each drive as it was");
  const auto empty = parse_legacy_ledger("{\"version\":1,\"sinceMs\":null,\"drives\":[]}");
  check(empty && !empty->since_ms && empty->drives.empty(), "an empty old list");
  check(!parse_legacy_ledger("[not an object]"), "not an object, not a list");

  check(parse_since(since_json(kT0)) == kT0, "since reads back");
  check(!parse_since("{\"sinceMs\":5}"), "a since before the clock was set is none");
}

}  // namespace

int main() {
  test_meter();
  test_records();
  test_folder();
  test_legacy();
  if (failures == 0) std::printf("PASS: drive list recorded while driving, one file per drive\n");
  return failures == 0 ? 0 : 1;
}
