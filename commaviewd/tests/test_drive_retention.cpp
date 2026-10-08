#include "drive_retention.h"
#include "openpilot_log_root.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using commaview::runtime::deletion_order;
using commaview::runtime::delete_below_bytes;
using commaview::runtime::DiskSpace;
using commaview::runtime::drive_retention_json;
using commaview::runtime::drive_retention_response;
using commaview::runtime::openpilot_log_root;
using commaview::runtime::SegmentDir;

namespace {

constexpr uint64_t GiB = 1024ULL * 1024 * 1024;
constexpr uint64_t MiB = 1024ULL * 1024;

SegmentDir dir(const std::string& name, uint64_t bytes, bool preserved = false) {
  SegmentDir d;
  d.name = name;
  d.bytes = bytes;
  d.preserved_attr = preserved;
  return d;
}

std::vector<std::string> names(const std::vector<SegmentDir>& dirs) {
  std::vector<std::string> out;
  for (const auto& d : dirs) out.push_back(d.name);
  return out;
}

}  // namespace

int main() {
  // 5 GiB, or 10% of a disk bigger than 50 GiB.
  assert(delete_below_bytes({0, 20 * GiB}) == 5 * GiB);
  assert(delete_below_bytes({0, 400 * GiB}) == 40 * GiB);

  // openpilot's order: old date-named routes, then by route and segment number (padded, so 10
  // comes after 9), with boot/crash and the bookmarked segments (and the two before each) last.
  const auto order = deletion_order({
      dir("crash", 1), dir("000000a1--aaaa--10", 1), dir("boot", 1), dir("000000a1--aaaa--9", 1),
      dir("2024-01-01--12-00-00--0", 1), dir("000000a2--bbbb--0", 1), dir("000000a2--bbbb--3", 1, true),
      dir("000000a2--bbbb--1", 1), dir("000000a2--bbbb--2", 1),
  });
  assert((names(order) == std::vector<std::string>{
      "2024-01-01--12-00-00--0", "000000a1--aaaa--9", "000000a1--aaaa--10", "000000a2--bbbb--0",
      "000000a2--bbbb--1", "000000a2--bbbb--2", "000000a2--bbbb--3", "boot", "crash"}));
  // Bookmarked: segment 3 and the two before it go after everything else that isn't boot/crash.
  const auto bookmarked = deletion_order({dir("000000a1--aaaa--0", 1), dir("000000a2--bbbb--5", 1, true),
                                          dir("000000a2--bbbb--4", 1), dir("000000a3--cccc--0", 1)});
  assert((names(bookmarked) == std::vector<std::string>{
      "000000a1--aaaa--0", "000000a3--cccc--0", "000000a2--bbbb--4", "000000a2--bbbb--5"}));

  // Three drives of 100 MiB segments; 6 GiB free on a 20 GiB disk (deletes below 5 GiB): 1 GiB of
  // headroom, so the oldest drive has ~10 minutes of driving left, the next 1 GiB + its 2 segments.
  std::vector<SegmentDir> dirs;
  for (int s = 0; s < 2; ++s) dirs.push_back(dir("000000a1--aaaa--" + std::to_string(s), 100 * MiB));
  for (int s = 0; s < 3; ++s) dirs.push_back(dir("000000a2--bbbb--" + std::to_string(s), 100 * MiB));
  dirs.push_back(dir("000000a3--cccc--0", 100 * MiB));
  const std::string body = drive_retention_json({"000000a1--aaaa", "000000a2--bbbb", "000000a9--gone"}, dirs,
                                                {6 * GiB, 20 * GiB});
  assert(body.find("\"deleteBelowBytes\":" + std::to_string(5 * GiB)) != std::string::npos);
  assert(body.find("\"bytesPerMinute\":" + std::to_string(100 * MiB)) != std::string::npos);
  assert(body.find("{\"route\":\"000000a1--aaaa\",\"present\":true,\"segments\":2,\"bytesBeforeDeletion\":" +
                   std::to_string(1 * GiB) + ",\"drivingMinutesLeft\":10}") != std::string::npos);
  assert(body.find("{\"route\":\"000000a2--bbbb\",\"present\":true,\"segments\":3,\"bytesBeforeDeletion\":" +
                   std::to_string(1 * GiB + 200 * MiB) + ",\"drivingMinutesLeft\":12}") != std::string::npos);
  assert(body.find("{\"route\":\"000000a9--gone\",\"present\":false}") != std::string::npos);

  // Already under the line (the deleter is at work): the oldest drive has no time left.
  const std::string low = drive_retention_json({"000000a1--aaaa"}, dirs, {4 * GiB, 20 * GiB});
  assert(low.find("\"bytesBeforeDeletion\":0,\"drivingMinutesLeft\":0") != std::string::npos);

  // No segments to measure a minute by: the minutes are unknown, not zero.
  const std::string unknown = drive_retention_json({"000000a1--aaaa"}, {dir("000000a1--aaaa--0", 0)}, {6 * GiB, 20 * GiB});
  assert(unknown.find("\"drivingMinutesLeft\":null") != std::string::npos);

  // Only the newest segments are measured: an unmeasured one counts as a minute's worth.
  std::vector<SegmentDir> sampled = {dir("000000a1--aaaa--0", 0), dir("000000a1--aaaa--1", 0),
                                     dir("000000a2--bbbb--0", 100 * MiB)};
  const std::string estimate = drive_retention_json({"000000a2--bbbb"}, sampled, {6 * GiB, 20 * GiB});
  assert(estimate.find("\"bytesBeforeDeletion\":" + std::to_string(1 * GiB + 200 * MiB)) != std::string::npos);

  // Requests it refuses.
  assert(drive_retention_response("/commaview/drive-retention").status == 400);
  assert(drive_retention_response("/commaview/drive-retention?routes=").status == 400);
  assert(drive_retention_response("/commaview/drive-retention?routes=../etc").status == 400);
  assert(drive_retention_response("/commaview/drive-retention?routes=a--b&x=1").status == 400);
  std::string many = "/commaview/drive-retention?routes=";
  for (int i = 0; i < 301; ++i) many += (i ? "," : "") + std::string("a--b");
  assert(drive_retention_response(many).status == 400);

  // StarPilot's High Bitrate Recording and Konik markers move the log root (hw.py), only under StarPilot.
  char tmpl[] = "/tmp/commaview-log-root-XXXXXX";
  const std::string tmp = mkdtemp(tmpl);
  const std::string op = tmp + "/openpilot", cache = tmp + "/cache", media = tmp + "/media";
  assert(mkdir(op.c_str(), 0755) == 0 && mkdir(cache.c_str(), 0755) == 0);
  std::ofstream(cache + "/use_konik").put('1');
  assert(openpilot_log_root(op, cache, media) == media + "/realdata");  // markers left by StarPilot
  assert(mkdir((op + "/starpilot").c_str(), 0755) == 0);
  assert(openpilot_log_root(op, cache, media) == media + "/realdata_konik");
  std::ofstream(cache + "/use_HD").put('1');
  assert(openpilot_log_root(op, cache, media) == media + "/realdata_HD");
  std::remove((cache + "/use_HD").c_str());
  std::remove((cache + "/use_konik").c_str());
  assert(openpilot_log_root(op, cache, media) == media + "/realdata");
  rmdir((op + "/starpilot").c_str());
  rmdir(op.c_str());
  rmdir(cache.c_str());
  rmdir(tmp.c_str());
  return 0;
}
