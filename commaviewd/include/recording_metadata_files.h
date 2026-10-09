#pragma once

// UI recording metadata files under the recipe directory, shared by the bridge's writer
// (ui_export_socket.cpp) and the source archive endpoints (source_recording_archive.cpp).

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <utility>

#include "openpilot_log_root.h"

namespace commaview::recording_files {

using commaview::runtime::openpilot_log_root;

// Longest recipe line either side handles. Writers produce lines under 5 KiB (a 4 KiB event plus
// the route and capture time), so anything longer is not a recipe event.
inline constexpr size_t kMaxRecipeLineBytes = 16 * 1024;

// Where the drives are: loggerd's log root (openpilot_log_root.h), which StarPilot can move.
inline std::string default_source_archive_root() {
  const char* override_root = std::getenv("COMMAVIEWD_SOURCE_ARCHIVE_ROOT");
  return override_root && override_root[0] ? override_root : openpilot_log_root();
}

inline bool safe_route(const std::string& value) {
  if (value.empty() || value.size() > 96 || value.find("--") == std::string::npos) return false;
  for (char c : value) {
    if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
        !(c >= '0' && c <= '9') && c != '-' && c != '_') return false;
  }
  return true;
}

// One recipe file holds one route's events from one bridge process:
//   ui-source-<route>.<process start ms>-<pid>.jsonl
// Route identifiers never contain '.', which separates the route from the stamp. Runtimes up to
// v0.0.58-alpha.1 wrote a single file per process for every route instead
// (ui-source-<start ms>-<pid>.jsonl); readers still accept those "legacy" files.
inline std::string recipe_file_name(const std::string& route, const std::string& stamp) {
  return "ui-source-" + route + "." + stamp + ".jsonl";
}

struct RecipeFile {
  bool legacy = false;      // Every route's events from one older bridge process.
  std::string route;        // Empty for a legacy file.
  uint64_t started_ms = 0;  // The writing process's start: orders one route's files.
  uint64_t pid = 0;
};

namespace detail {
inline bool parse_decimal(const std::string& text, uint64_t* value) {
  if (text.empty() || text.size() > 19) return false;
  uint64_t result = 0;
  for (char c : text) {
    if (c < '0' || c > '9') return false;
    result = result * 10 + static_cast<uint64_t>(c - '0');
  }
  *value = result;
  return true;
}

// "<ms>-<pid>" (or "<ms>").
inline bool parse_stamp(const std::string& stamp, uint64_t* started_ms, uint64_t* pid) {
  const size_t dash = stamp.find('-');
  *pid = 0;
  if (dash == std::string::npos) return parse_decimal(stamp, started_ms);
  return parse_decimal(stamp.substr(0, dash), started_ms) && parse_decimal(stamp.substr(dash + 1), pid);
}
}  // namespace detail

// False for a name that is not a recipe file.
inline bool parse_recipe_file_name(const std::string& name, RecipeFile* out) {
  static const std::string prefix = "ui-source-", suffix = ".jsonl";
  if (name.size() <= prefix.size() + suffix.size() || name.compare(0, prefix.size(), prefix) != 0 ||
      name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
  const std::string stem = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
  RecipeFile file;
  const size_t dot = stem.find('.');
  if (dot == std::string::npos) {
    // Older writers' names; one that doesn't parse still counts, ordered first.
    file.legacy = true;
    if (!detail::parse_stamp(stem, &file.started_ms, &file.pid)) file.started_ms = file.pid = 0;
  } else {
    file.route = stem.substr(0, dot);
    if (!safe_route(file.route) || !detail::parse_stamp(stem.substr(dot + 1), &file.started_ms, &file.pid)) {
      return false;
    }
  }
  *out = std::move(file);
  return true;
}

enum class LineScan { complete, stopped, read_failed };

// Reads fd to its end and calls on_line(line, truncated) for each newline-terminated line, without
// the newline. Memory stays bounded whatever the file's size: a line longer than max_line arrives
// cut to its first max_line bytes, with truncated set. An unterminated last line is still being
// written and is skipped. on_line returns false to stop early.
template <typename OnLine>
LineScan scan_lines(int fd, size_t max_line, OnLine&& on_line) {
  std::string line;
  bool truncated = false;
  char buffer[16 * 1024];
  while (true) {
    const ssize_t count = ::read(fd, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) return LineScan::read_failed;
    if (count == 0) return LineScan::complete;
    const size_t size = static_cast<size_t>(count);
    size_t at = 0;
    while (at < size) {
      const void* newline = std::memchr(buffer + at, '\n', size - at);
      const size_t end = newline ? static_cast<size_t>(static_cast<const char*>(newline) - buffer) : size;
      const size_t room = line.size() < max_line ? max_line - line.size() : 0;
      const size_t take = std::min(room, end - at);
      line.append(buffer + at, take);
      truncated = truncated || take < end - at;
      if (newline == nullptr) break;
      if (!on_line(static_cast<const std::string&>(line), truncated)) return LineScan::stopped;
      line.clear();
      truncated = false;
      at = end + 1;
    }
  }
}

}  // namespace commaview::recording_files
