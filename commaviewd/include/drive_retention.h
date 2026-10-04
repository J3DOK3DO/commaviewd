#pragma once

#include "http_server.h"

#include <cstdint>
#include <string>
#include <vector>

namespace commaview::runtime {

// How long each drive has before openpilot deletes it to make room, so the app can warn about a
// recording whose replay isn't made yet, and say plainly when its drive is gone.
//
// openpilot's deleter (system/loggerd/deleter.py) removes one segment folder at a time while free
// space is under 5 GiB or 10% of the disk, earliest first in its own order (listdir_by_creation),
// with 'boot' and 'crash' and the newest bookmarked (preserved) segments last. A drive's first
// segment is deleted once the comma has written, past the free space above that line, everything
// that comes before it in that order: dividing that by what the comma records a minute (its recent
// segments' size) gives the minutes of driving left.
//
//   GET /commaview/drive-retention?routes=<route>,<route>,...   (at most 300)
//
// Cheap on the comma: one listing of the drive folder's names (as the deleter itself does), the
// newest 30 segments measured, the result kept for 5 minutes. The caller must authenticate first
// and refuses it while the car is being driven.

struct DiskSpace {
  uint64_t available_bytes = 0;  // statvfs f_bavail * f_frsize, as the deleter reads it
  uint64_t total_bytes = 0;      // f_blocks * f_frsize
};

struct SegmentDir {
  std::string name;   // <route>--<segment>, or another folder (boot, crash)
  uint64_t bytes = 0; // what deleting it frees
  bool preserved_attr = false;  // user.preserve == 1 (bookmarked); the scan doesn't read it
};

// The folders in openpilot's deletion order: what the deleter removes first comes first.
std::vector<SegmentDir> deletion_order(std::vector<SegmentDir> dirs);

// The bytes below which openpilot starts deleting: 5 GiB, or 10% of the disk if that's more.
uint64_t delete_below_bytes(const DiskSpace& disk);

// The response for `routes` given the comma's folders and disk.
std::string drive_retention_json(const std::vector<std::string>& routes, const std::vector<SegmentDir>& dirs,
                                 const DiskSpace& disk);

commaview::api::HttpResponse drive_retention_response(const std::string& request_path);

}  // namespace commaview::runtime
