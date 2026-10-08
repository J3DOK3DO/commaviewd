#include "drive_retention.h"

#include "recording_metadata_files.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <set>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>

namespace commaview::runtime {
namespace {

constexpr uint64_t kMinBytes = 5ULL * 1024 * 1024 * 1024;  // deleter.py MIN_BYTES
constexpr uint64_t kMinPercent = 10;                       // deleter.py MIN_PERCENT
constexpr size_t kPreserveCount = 5;                       // deleter.py PRESERVE_COUNT
constexpr size_t kMaxRoutes = 300;
constexpr size_t kRateSegments = 30;  // the newest segments a minute's size is taken from
constexpr time_t kScanCacheSeconds = 5 * 60;

std::string archive_root() {
  return recording_files::default_source_archive_root();
}

commaview::api::HttpResponse json(int status, const std::string& body) {
  commaview::api::HttpResponse response;
  response.status = status;
  response.body = body;
  response.headers["Cache-Control"] = "no-store";
  return response;
}

bool safe_route(const std::string& value) {
  if (value.empty() || value.size() > 96 || value.find("--") == std::string::npos) return false;
  for (char c : value) {
    if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') && c != '-' && c != '_') {
      return false;
    }
  }
  return true;
}

// uploader.py get_directory_sort: old date-named folders (2024-...) first, then the route and the
// segment number, each left-padded to 10 characters with '0'.
std::vector<std::string> sort_key(const std::string& name) {
  std::vector<std::string> key{name.rfind("2024-", 0) == 0 ? "0" : "1"};
  const size_t split = name.rfind("--");
  auto pad = [](const std::string& part) {
    return part.size() >= 10 ? part : std::string(10 - part.size(), '0') + part;
  };
  if (split == std::string::npos) {
    key.push_back(pad(name));
  } else {
    key.push_back(pad(name.substr(0, split)));
    key.push_back(pad(name.substr(split + 2)));
  }
  return key;
}

// A segment folder's route and number; false for anything else (boot, crash).
bool segment_of(const std::string& name, std::string* route, long* segment) {
  const size_t split = name.rfind("--");
  if (split == std::string::npos || split == 0) return false;
  const std::string number = name.substr(split + 2);
  if (number.empty() || number.size() > 6 || number.find_first_not_of("0123456789") != std::string::npos) return false;
  *route = name.substr(0, split);
  *segment = std::strtol(number.c_str(), nullptr, 10);
  return true;
}

// deleter.py get_preserved_segments: the newest PRESERVE_COUNT bookmarked segments and the two
// before each.
std::set<std::string> preserved(const std::vector<SegmentDir>& by_creation) {
  std::set<std::string> kept;
  size_t found = 0;
  for (auto it = by_creation.rbegin(); it != by_creation.rend(); ++it) {
    if (!it->preserved_attr) continue;
    if (found++ == kPreserveCount) break;
    std::string route;
    long segment = 0;
    if (!segment_of(it->name, &route, &segment)) continue;
    for (long s = std::max(0L, segment - 2); s <= segment; ++s) kept.insert(route + "--" + std::to_string(s));
  }
  return kept;
}

uint64_t folder_bytes(int parent, const char* name) {
  const int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return 0;
  DIR* dir = fdopendir(fd);
  if (dir == nullptr) {
    close(fd);
    return 0;
  }
  uint64_t total = 0;
  while (dirent* entry = readdir(dir)) {
    struct stat st {};
    if (fstatat(dirfd(dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode)) {
      total += static_cast<uint64_t>(st.st_blocks) * 512;
    }
  }
  closedir(dir);
  return total;
}

// The comma's folders by name, as the deleter lists them, with only the newest kRateSegments
// measured (what a minute of driving takes); the others count as that much. No per-file walk of
// the whole drive folder, and no extended attributes: bookmarked segments (at most 15, kept last
// by the deleter) are counted where their name sorts, which can only make a drive look a few
// minutes safer than it is.
std::vector<SegmentDir> scan(const char* root) {
  std::vector<SegmentDir> dirs;
  const int fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return dirs;
  DIR* handle = fdopendir(fd);
  if (handle == nullptr) {
    close(fd);
    return dirs;
  }
  while (dirent* entry = readdir(handle)) {
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    if (entry->d_type != DT_DIR && entry->d_type != DT_UNKNOWN) continue;
    SegmentDir dir;
    dir.name = name;
    dirs.push_back(std::move(dir));
  }
  std::sort(dirs.begin(), dirs.end(),
            [](const SegmentDir& a, const SegmentDir& b) { return sort_key(a.name) < sort_key(b.name); });
  size_t measured = 0;
  for (auto it = dirs.rbegin(); it != dirs.rend() && measured < kRateSegments; ++it) {
    std::string route;
    long segment = 0;
    if (!segment_of(it->name, &route, &segment)) continue;
    it->bytes = folder_bytes(dirfd(handle), it->name.c_str());
    ++measured;
  }
  closedir(handle);
  return dirs;
}

std::vector<std::string> split_routes(const std::string& list) {
  std::vector<std::string> routes;
  size_t at = 0;
  while (at <= list.size()) {
    const size_t comma = list.find(',', at);
    const std::string route = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
    if (!route.empty()) routes.push_back(route);
    if (comma == std::string::npos) break;
    at = comma + 1;
  }
  return routes;
}

}  // namespace

std::vector<SegmentDir> deletion_order(std::vector<SegmentDir> dirs) {
  std::sort(dirs.begin(), dirs.end(),
            [](const SegmentDir& a, const SegmentDir& b) { return sort_key(a.name) < sort_key(b.name); });
  const std::set<std::string> kept = preserved(dirs);
  // deleter.py: sorted(dirs, key=lambda d: (d in DELETE_LAST, d in preserved_dirs)), a stable sort.
  std::stable_sort(dirs.begin(), dirs.end(), [&kept](const SegmentDir& a, const SegmentDir& b) {
    const bool a_last = a.name == "boot" || a.name == "crash";
    const bool b_last = b.name == "boot" || b.name == "crash";
    if (a_last != b_last) return !a_last;
    return kept.count(a.name) == 0 && kept.count(b.name) != 0;
  });
  return dirs;
}

uint64_t delete_below_bytes(const DiskSpace& disk) {
  return std::max(kMinBytes, disk.total_bytes / 100 * kMinPercent);
}

std::string drive_retention_json(const std::vector<std::string>& routes, const std::vector<SegmentDir>& dirs,
                                 const DiskSpace& disk) {
  const std::vector<SegmentDir> order = deletion_order(dirs);
  const uint64_t line = delete_below_bytes(disk);
  const uint64_t headroom = disk.available_bytes > line ? disk.available_bytes - line : 0;

  // What a minute of driving takes: the median of the newest segments (each is a minute).
  std::vector<SegmentDir> by_creation = dirs;
  std::sort(by_creation.begin(), by_creation.end(),
            [](const SegmentDir& a, const SegmentDir& b) { return sort_key(a.name) < sort_key(b.name); });
  std::vector<uint64_t> recent;
  for (auto it = by_creation.rbegin(); it != by_creation.rend() && recent.size() < kRateSegments; ++it) {
    std::string route;
    long segment = 0;
    if (segment_of(it->name, &route, &segment) && it->bytes > 0) recent.push_back(it->bytes);
  }
  uint64_t per_minute = 0;
  if (!recent.empty()) {
    std::sort(recent.begin(), recent.end());
    per_minute = recent[recent.size() / 2];
  }

  // Each route's first folder in deletion order, the bytes deleted before it, and its segment count.
  std::map<std::string, std::pair<uint64_t, size_t>> first_at;  // route -> (bytes before, segments)
  uint64_t before = 0;
  for (const SegmentDir& dir : order) {
    std::string route;
    long segment = 0;
    const bool is_segment = segment_of(dir.name, &route, &segment);
    if (is_segment) {
      auto [it, added] = first_at.emplace(route, std::make_pair(before, 0));
      it->second.second++;
      (void)added;
    }
    // A segment that wasn't measured counts as a minute's worth.
    before += dir.bytes > 0 || !is_segment ? dir.bytes : per_minute;
  }

  std::string body = "{\"ok\":true,\"availableBytes\":" + std::to_string(disk.available_bytes) +
                     ",\"deleteBelowBytes\":" + std::to_string(line) + ",\"bytesPerMinute\":" +
                     std::to_string(per_minute) + ",\"routes\":[";
  bool first = true;
  for (const std::string& route : routes) {
    body += first ? "" : ",";
    first = false;
    body += "{\"route\":\"" + route + "\"";
    auto it = first_at.find(route);
    if (it == first_at.end()) {
      body += ",\"present\":false}";
      continue;
    }
    const uint64_t budget = headroom + it->second.first;
    body += ",\"present\":true,\"segments\":" + std::to_string(it->second.second) +
            ",\"bytesBeforeDeletion\":" + std::to_string(budget) + ",\"drivingMinutesLeft\":" +
            (per_minute > 0 ? std::to_string(budget / per_minute) : std::string("null")) + "}";
  }
  body += "]}";
  return body;
}

commaview::api::HttpResponse drive_retention_response(const std::string& request_path) {
  const size_t q = request_path.find('?');
  const std::string query = q == std::string::npos ? "" : request_path.substr(q + 1);
  if (query.rfind("routes=", 0) != 0 || query.find('&') != std::string::npos) {
    return json(400, "{\"ok\":false,\"error\":\"routes required\"}");
  }
  const std::vector<std::string> routes = split_routes(query.substr(7));
  if (routes.empty() || routes.size() > kMaxRoutes) {
    return json(400, "{\"ok\":false,\"error\":\"between 1 and 300 routes\"}");
  }
  for (const std::string& route : routes) {
    if (!safe_route(route)) return json(400, "{\"ok\":false,\"error\":\"bad route\"}");
  }
  const std::string root = archive_root();
  struct statvfs vfs {};
  if (statvfs(root.c_str(), &vfs) != 0) return json(503, "{\"ok\":false,\"error\":\"drives unavailable\"}");
  DiskSpace disk;
  disk.available_bytes = static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
  disk.total_bytes = static_cast<uint64_t>(vfs.f_blocks) * vfs.f_frsize;
  // The folder listing is kept for a few minutes: drives change by a minute's segment at a time.
  static std::vector<SegmentDir> cached;
  static std::string cached_root;
  static time_t cached_at = 0;
  const time_t now = time(nullptr);
  if (cached_root != root || now - cached_at > kScanCacheSeconds || now < cached_at) {
    cached = scan(root.c_str());
    cached_root = root;
    cached_at = now;
  }
  return json(200, drive_retention_json(routes, cached, disk));
}

}  // namespace commaview::runtime
