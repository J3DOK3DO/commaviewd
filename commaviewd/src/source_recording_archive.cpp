#include "source_recording_archive.h"

#include "openpilot_log_root.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace commaview::runtime {
namespace {

using commaview::api::HttpResponse;

constexpr uint64_t kMaxChunk = 256 * 1024;
constexpr size_t kMaxRecipeBytes = 2 * 1024 * 1024;

HttpResponse error(int status, const char* detail) {
  HttpResponse result;
  result.status = status;
  result.body = std::string("{\"ok\":false,\"error\":\"") + detail + "\"}";
  return result;
}

bool parse_query(const std::string& path, std::map<std::string, std::string>* values) {
  const size_t question = path.find('?');
  if (question == std::string::npos || question + 1 == path.size()) return false;
  size_t at = question + 1;
  while (at < path.size()) {
    const size_t end = path.find('&', at);
    const std::string pair = path.substr(at, end == std::string::npos ? end : end - at);
    const size_t equals = pair.find('=');
    if (equals == std::string::npos || equals == 0 || equals + 1 == pair.size()) return false;
    const std::string key = pair.substr(0, equals);
    if (!values->emplace(key, pair.substr(equals + 1)).second) return false;
    if (end == std::string::npos) break;
    at = end + 1;
  }
  return true;
}

bool safe_route(const std::string& value) {
  if (value.empty() || value.size() > 96 || value.find("--") == std::string::npos) return false;
  for (char c : value) {
    if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
        !(c >= '0' && c <= '9') && c != '-' && c != '_') return false;
  }
  return true;
}

bool decimal(const std::string& text, uint64_t* value) {
  if (text.empty() || text.size() > 20) return false;
  uint64_t result = 0;
  for (char c : text) {
    if (c < '0' || c > '9') return false;
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (result > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
    result = result * 10 + digit;
  }
  *value = result;
  return true;
}

// Every archive/recipe open refuses symlinks and never leaks into children.
int open_directory(const char* path) {
  return ::open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
}

int open_directory_at(int dir, const char* name) {
  return ::openat(dir, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
}

int open_file_at(int dir, const char* name) {
  return ::openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
}

std::string archive_root() {
  const char* override_root = std::getenv("COMMAVIEWD_SOURCE_ARCHIVE_ROOT");
  return override_root && override_root[0] ? override_root : openpilot_log_root();
}

const char* recipe_root() {
  const char* override_root = std::getenv("COMMAVIEWD_RECIPE_DIR");
  return override_root && override_root[0] ? override_root : "/data/commaview/recording-recipes";
}

HttpResponse range_response(const std::map<std::string, std::string>& q) {
  if (q.size() != 5 || !q.count("route") || !q.count("segment") ||
      !q.count("kind") || !q.count("offset") || !q.count("length") ||
      !safe_route(q.at("route"))) return error(400, "invalid query");
  uint64_t segment = 0, offset = 0, length = 0;
  if (!decimal(q.at("segment"), &segment) || segment > 99999 ||
      !decimal(q.at("offset"), &offset) ||
      !decimal(q.at("length"), &length) || length == 0 || length > kMaxChunk) {
    return error(400, "invalid range");
  }
  const std::string kind = q.at("kind");
  const char* filename = kind == "road" ? "fcamera.hevc" :
                         kind == "wide" ? "ecamera.hevc" :
                         kind == "rlog" ? "rlog.zst" :
                         kind == "snapshot" ? "" : nullptr;
  if (!filename || (kind == "snapshot" && segment != 0)) return error(400, "invalid kind");
  int file = -1;
  if (kind == "snapshot") {
    const int root = open_directory(recipe_root());
    if (root < 0) return error(404, "snapshot unavailable");
    const std::string name = "ui-snapshot-" + q.at("route") + ".jsonl";
    file = open_file_at(root, name.c_str());
    ::close(root);
  } else {
    const std::string segment_name = q.at("route") + "--" + std::to_string(segment);
    const int root = open_directory(archive_root().c_str());
    if (root < 0) return error(404, "archive unavailable");
    const int dir = open_directory_at(root, segment_name.c_str());
    ::close(root);
    if (dir < 0) return error(404, "segment unavailable");
    file = open_file_at(dir, filename);
    ::close(dir);
  }
  if (file < 0) return error(404, "media unavailable");
  struct stat info = {};
  if (::fstat(file, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0) {
    ::close(file);
    return error(404, "media unavailable");
  }
  const uint64_t size = static_cast<uint64_t>(info.st_size);
  if (offset > size || length > size - offset ||
      offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
    ::close(file);
    return error(400, "range outside media");
  }
  HttpResponse response;
  response.content_type = "application/octet-stream";
  response.headers["Cache-Control"] = "no-store";
  response.body.resize(static_cast<size_t>(length));
  size_t read = 0;
  while (read < response.body.size()) {
    const ssize_t n = ::pread(file, response.body.data() + read,
                              response.body.size() - read, static_cast<off_t>(offset + read));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      ::close(file);
      return error(500, "archive read failed");
    }
    read += static_cast<size_t>(n);
  }
  ::close(file);
  response.headers["X-Source-Size"] = std::to_string(size);
  response.headers["X-Source-Offset"] = std::to_string(offset);
  response.headers["X-Source-Modified-Ns"] =
      std::to_string(info.st_mtim.tv_sec) + std::to_string(info.st_mtim.tv_nsec + 1000000000L).substr(1);
  return response;
}

HttpResponse recipe_response(const std::map<std::string, std::string>& q) {
  if (q.size() != 1 || !q.count("route") || !safe_route(q.at("route"))) {
    return error(400, "invalid route");
  }
  const int root = open_directory(recipe_root());
  if (root < 0) return error(404, "recipe unavailable");
  DIR* dir = ::fdopendir(root);
  if (!dir) {
    ::close(root);
    return error(404, "recipe unavailable");
  }
  std::string body;
  bool overflow = false;
  const std::string match = "\"routeId\":\"" + q.at("route") + "\"";
  std::vector<std::string> names;
  while (const dirent* entry = ::readdir(dir)) {
    const std::string name = entry->d_name;
    if (name.rfind("ui-source-", 0) != 0 || name.size() < 16 ||
        name.substr(name.size() - 6) != ".jsonl") continue;
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  for (const std::string& name : names) {
    const int file = open_file_at(root, name.c_str());
    if (file < 0) continue;
    struct stat info = {};
    if (::fstat(file, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 0 || static_cast<uint64_t>(info.st_size) > kMaxRecipeBytes) {
      ::close(file);
      overflow = true;
      break;
    }
    std::string source;
    source.resize(static_cast<size_t>(info.st_size));
    size_t have = 0;
    while (have < source.size()) {
      const ssize_t n = ::read(file, source.data() + have, source.size() - have);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) break;
      have += static_cast<size_t>(n);
    }
    ::close(file);
    if (have != source.size()) {
      overflow = true;
      break;
    }
    size_t at = 0;
    while (at < source.size()) {
      const size_t end = source.find('\n', at);
      if (end == std::string::npos) break;  // Ignore incomplete last event.
      const std::string line = source.substr(at, end - at + 1);
      if (line.find(match) != std::string::npos) {
        if (body.size() + line.size() > kMaxRecipeBytes) {
          overflow = true;
          break;
        }
        body += line;
      }
      at = end + 1;
    }
    if (overflow) break;
  }
  ::closedir(dir);
  if (overflow) return error(413, "recipe too large");
  if (body.empty()) return error(404, "recipe unavailable");
  HttpResponse response;
  response.content_type = "application/x-ndjson";
  response.headers["Cache-Control"] = "no-store";
  response.body = std::move(body);
  return response;
}

HttpResponse manifest_response(const std::map<std::string, std::string>& q) {
  if (q.size() != 1 || !q.count("route") || !safe_route(q.at("route"))) {
    return error(400, "invalid route");
  }
  const int root = open_directory(archive_root().c_str());
  if (root < 0) return error(404, "archive unavailable");
  DIR* listing = ::fdopendir(root);
  if (!listing) {
    ::close(root);
    return error(404, "archive unavailable");
  }
  const std::string prefix = q.at("route") + "--";
  std::vector<uint64_t> segments;
  std::map<uint64_t, std::vector<std::string>> incomplete;
  while (const dirent* entry = ::readdir(listing)) {
    const std::string name = entry->d_name;
    if (name.rfind(prefix, 0) != 0) continue;
    uint64_t number = 0;
    if (!decimal(name.substr(prefix.size()), &number) || number > 99999 ||
        name != prefix + std::to_string(number)) continue;
    const int dir = open_directory_at(root, name.c_str());
    if (dir < 0) continue;
    std::vector<std::string> missing;
    size_t kind = 0;
    const char* kinds[] = {"road", "wide", "rlog"};
    for (const char* file_name : {"fcamera.hevc", "ecamera.hevc", "rlog.zst"}) {
      const int file = open_file_at(dir, file_name);
      struct stat info = {};
      if (file < 0 || ::fstat(file, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0) {
        missing.emplace_back(kinds[kind]);
      }
      if (file >= 0) ::close(file);
      ++kind;
    }
    ::close(dir);
    if (missing.empty()) segments.push_back(number);
    else incomplete.emplace(number, std::move(missing));
    if (segments.size() + incomplete.size() > 1000) break;
  }
  ::closedir(listing);
  if (segments.size() + incomplete.size() > 1000) return error(413, "route too large");
  if (segments.empty() && incomplete.empty()) return error(404, "route archive unavailable");
  std::sort(segments.begin(), segments.end());
  std::string body = "{\"routeId\":\"" + q.at("route") + "\",\"segments\":[";
  for (size_t i = 0; i < segments.size(); ++i) {
    if (i) body += ',';
    body += std::to_string(segments[i]);
  }
  // Existing clients still receive only complete segments in `segments`. An existing route
  // with unfinished/missing files is different from one whose directories are absent.
  body += "],\"incompleteSegments\":[";
  bool first = true;
  for (const auto& entry : incomplete) {
    if (!first) body += ',';
    first = false;
    body += "{\"segment\":" + std::to_string(entry.first) + ",\"missing\":[";
    for (size_t i = 0; i < entry.second.size(); ++i) {
      if (i) body += ',';
      body += "\"" + entry.second[i] + "\"";
    }
    body += "]}";
  }
  body += "]}";
  HttpResponse response;
  response.body = std::move(body);
  response.headers["Cache-Control"] = "no-store";
  return response;
}

}  // namespace

commaview::api::HttpResponse source_recording_archive_response(const std::string& request_path) {
  const size_t question = request_path.find('?');
  const std::string endpoint = request_path.substr(0, question);
  std::map<std::string, std::string> query;
  if (!parse_query(request_path, &query)) return error(400, "invalid query");
  if (query.count("route") && safe_route(query.at("route"))) {
    const int root = open_directory(recipe_root());
    if (root >= 0) {
      struct stat marker = {};
      const bool damaged = ::fstatat(root, ("ui-invalid-" + query.at("route")).c_str(), &marker, AT_SYMLINK_NOFOLLOW) == 0;
      ::close(root);
      if (damaged) return error(409, "recording metadata damaged");
    }
  }
  if (endpoint == "/commaview/source-recording/range") return range_response(query);
  if (endpoint == "/commaview/source-recording/recipe") return recipe_response(query);
  if (endpoint == "/commaview/source-recording/manifest") return manifest_response(query);
  return error(404, "not found");
}

commaview::api::HttpResponse source_recording_current_response(const std::string& route_id) {
  if (!safe_route(route_id)) return error(404, "route unavailable");
  HttpResponse response;
  response.body = "{\"routeId\":\"" + route_id + "\"}";
  response.headers["Cache-Control"] = "no-store";
  return response;
}

commaview::api::HttpResponse source_recording_preflight_response(
    const std::string& route_before, const std::string& status_json,
    const std::string& route_after, bool onroad_after) {
  auto response = !onroad_after ? error(403, "onroad required") :
      route_before != route_after ? error(404, "route unavailable") : source_recording_current_response(route_before);
  response.body.pop_back();
  response.body += ",\"recordingStatus\":" + status_json +
      ",\"routeChanged\":" + (route_before != route_after ? "true" : "false") + "}";
  response.headers["Cache-Control"] = "no-store";
  return response;
}

}  // namespace commaview::runtime
