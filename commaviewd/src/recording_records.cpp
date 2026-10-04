#include "recording_records.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace commaview::runtime {
namespace {

constexpr const char* kListPath = "/commaview/recordings";
constexpr const char* kPrefix = "/commaview/recordings/";
constexpr const char* kJournalSuffix = ".json";
constexpr const char* kThumbSuffix = ".jpg";

commaview::api::HttpResponse json(int status, const std::string& body) {
  commaview::api::HttpResponse response;
  response.status = status;
  response.body = body;
  response.headers["Cache-Control"] = "no-store";
  return response;
}

commaview::api::HttpResponse error(int status, const std::string& message) {
  return json(status, "{\"ok\":false,\"error\":\"" + message + "\"}");
}

// A record's id is the app's session prefix (source_<ms>_<ns>): letters, digits, '_' and '-'.
bool safe_id(const std::string& id) {
  if (id.empty() || id.size() > 96) return false;
  for (char c : id) {
    if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') && c != '-' &&
        c != '_') {
      return false;
    }
  }
  return true;
}

// The one query this API takes: ?id=<id>. Anything else is refused.
bool query_id(const std::string& path, std::string* route, std::string* id) {
  const size_t q = path.find('?');
  *route = path.substr(0, q);
  if (q == std::string::npos) return false;
  const std::string query = path.substr(q + 1);
  if (query.rfind("id=", 0) != 0 || query.find('&') != std::string::npos) return false;
  *id = query.substr(3);
  return safe_id(*id);
}

bool read_capped(const std::string& path, size_t cap, std::string* out) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return false;
  struct stat st {};
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || static_cast<size_t>(st.st_size) > cap) {
    close(fd);
    return false;
  }
  out->assign(static_cast<size_t>(st.st_size), '\0');
  size_t done = 0;
  while (done < out->size()) {
    const ssize_t n = pread(fd, &(*out)[done], out->size() - done, static_cast<off_t>(done));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    done += static_cast<size_t>(n);
  }
  close(fd);
  out->resize(done);
  return done == static_cast<size_t>(st.st_size);
}

bool write_atomic(const std::string& path, const std::string& value) {
  const std::string tmp = path + ".tmp";
  const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return false;
  size_t done = 0;
  while (done < value.size()) {
    const ssize_t n = write(fd, value.data() + done, value.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    done += static_cast<size_t>(n);
  }
  const bool ok = done == value.size() && fsync(fd) == 0;
  close(fd);
  if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

bool ensure_dir(const std::string& dir) {
  // The records' parent (/data/commaview/data) holds other private files: owner only.
  const size_t slash = dir.find_last_of('/');
  if (slash != std::string::npos && slash > 0) {
    const std::string parent = dir.substr(0, slash);
    if (mkdir(parent.c_str(), 0700) != 0 && errno != EEXIST) return false;
  }
  if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return false;
  struct stat st {};
  return lstat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool ends_with(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

struct Record {
  std::string id;
  int64_t saved_ms = 0;
  bool thumb = false;
};

// Every record in `dir`, newest first.
std::vector<Record> records(const std::string& dir) {
  std::map<std::string, Record> by_id;
  std::vector<std::string> thumbs;
  DIR* handle = opendir(dir.c_str());
  if (handle == nullptr) return {};
  while (dirent* entry = readdir(handle)) {
    const std::string name = entry->d_name;
    std::string id;
    bool journal = false;
    if (ends_with(name, kJournalSuffix)) {
      id = name.substr(0, name.size() - 5);
      journal = true;
    } else if (ends_with(name, kThumbSuffix)) {
      id = name.substr(0, name.size() - 4);
    } else {
      continue;
    }
    if (!safe_id(id)) continue;
    struct stat st {};
    if (fstatat(dirfd(handle), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode)) continue;
    if (journal) {
      Record& record = by_id[id];
      record.id = id;
      record.saved_ms = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1000000;
    } else {
      thumbs.push_back(id);
    }
  }
  closedir(handle);
  for (const auto& id : thumbs) {
    auto it = by_id.find(id);
    if (it != by_id.end()) it->second.thumb = true;
  }
  std::vector<Record> out;
  out.reserve(by_id.size());
  for (auto& [id, record] : by_id) out.push_back(record);
  std::sort(out.begin(), out.end(), [](const Record& a, const Record& b) {
    return a.saved_ms != b.saved_ms ? a.saved_ms > b.saved_ms : a.id > b.id;
  });
  return out;
}

void remove_record(const std::string& dir, const std::string& id) {
  unlink((dir + "/" + id + kJournalSuffix).c_str());
  unlink((dir + "/" + id + kThumbSuffix).c_str());
}

// Keeps the newest `max_records`; a thumbnail whose record never came is cleared after a day.
void prune(const std::string& dir, size_t max_records) {
  const std::vector<Record> all = records(dir);
  for (size_t i = max_records; i < all.size(); ++i) remove_record(dir, all[i].id);
  DIR* handle = opendir(dir.c_str());
  if (handle == nullptr) return;
  const time_t now = time(nullptr);
  std::vector<std::string> orphans;
  while (dirent* entry = readdir(handle)) {
    const std::string name = entry->d_name;
    if (!ends_with(name, kThumbSuffix)) continue;
    const std::string id = name.substr(0, name.size() - 4);
    struct stat st {};
    if (!safe_id(id) || fstatat(dirfd(handle), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
    struct stat journal {};
    const bool has_journal = fstatat(dirfd(handle), (id + kJournalSuffix).c_str(), &journal, AT_SYMLINK_NOFOLLOW) == 0;
    if (!has_journal && now - st.st_mtim.tv_sec > 24 * 60 * 60) orphans.push_back(name);
  }
  closedir(handle);
  for (const auto& name : orphans) unlink((dir + "/" + name).c_str());
}

commaview::api::HttpResponse list_response(const std::string& dir, const RecordingRecordLimits& limits) {
  std::string body = "{\"ok\":true,\"recordings\":[";
  bool first = true;
  bool truncated = false;
  for (const Record& record : records(dir)) {
    std::string journal;
    if (!read_capped(dir + "/" + record.id + kJournalSuffix, limits.max_journal_bytes, &journal)) continue;
    if (journal.empty() || journal.front() != '{' || journal.find_last_not_of(" \t\r\n") == std::string::npos ||
        journal[journal.find_last_not_of(" \t\r\n")] != '}') {
      continue;
    }
    std::string item = std::string(first ? "" : ",") + "{\"id\":\"" + record.id + "\",\"savedAtMs\":" +
                       std::to_string(record.saved_ms) + ",\"thumb\":" + (record.thumb ? "true" : "false") +
                       ",\"journal\":" + journal.substr(0, journal.find_last_not_of(" \t\r\n") + 1) + "}";
    if (body.size() + item.size() + 32 > limits.max_listing_bytes) {
      truncated = true;
      break;
    }
    body += item;
    first = false;
  }
  body += std::string("],\"truncated\":") + (truncated ? "true" : "false") + "}";
  return json(200, body);
}

commaview::api::HttpResponse thumb_response(const std::string& dir, const std::string& id,
                                            const RecordingRecordLimits& limits) {
  std::string data;
  if (!read_capped(dir + "/" + id + kThumbSuffix, limits.max_thumb_bytes, &data)) {
    return error(404, "no thumbnail");
  }
  commaview::api::HttpResponse response;
  response.status = 200;
  response.content_type = "image/jpeg";
  response.body = std::move(data);
  response.headers["Cache-Control"] = "no-store";
  return response;
}

bool json_object(const std::string& body) {
  const size_t first = body.find_first_not_of(" \t\r\n");
  const size_t last = body.find_last_not_of(" \t\r\n");
  if (first == std::string::npos || body[first] != '{' || body[last] != '}') return false;
  // Embedded verbatim in the listing: no control characters that could end or split it.
  for (unsigned char c : body) {
    if (c < 0x20 && c != '\t' && c != '\r' && c != '\n') return false;
  }
  return true;
}

bool jpeg(const std::string& body) {
  return body.size() >= 4 && static_cast<unsigned char>(body[0]) == 0xFF &&
         static_cast<unsigned char>(body[1]) == 0xD8 && static_cast<unsigned char>(body[body.size() - 2]) == 0xFF &&
         static_cast<unsigned char>(body[body.size() - 1]) == 0xD9;
}

}  // namespace

bool is_recording_records_path(const std::string& request_path) {
  const std::string route = request_path.substr(0, request_path.find('?'));
  return route == kListPath || route.rfind(kPrefix, 0) == 0;
}

commaview::api::HttpResponse recording_records_response(const std::string& method,
                                                        const std::string& request_path,
                                                        const std::string& body,
                                                        const std::string& dir,
                                                        const RecordingRecordLimits& limits) {
  if (method == "GET" && request_path == kListPath) return list_response(dir, limits);
  std::string route;
  std::string id;
  if (!query_id(request_path, &route, &id)) {
    return route == kListPath || route.rfind(kPrefix, 0) == 0 ? error(400, "id required") : error(404, "not found");
  }
  if (method == "GET" && route == "/commaview/recordings/thumb") return thumb_response(dir, id, limits);
  if (method != "POST") return error(404, "not found");
  if (route == "/commaview/recordings/delete") {
    remove_record(dir, id);
    return json(200, "{\"ok\":true}");
  }
  const bool journal = route == "/commaview/recordings/journal";
  if (!journal && route != "/commaview/recordings/thumb") return error(404, "not found");
  if (body.size() > (journal ? limits.max_journal_bytes : limits.max_thumb_bytes)) return error(413, "too large");
  if (journal ? !json_object(body) : !jpeg(body)) {
    return error(400, journal ? "a record is a JSON object" : "a thumbnail is a JPEG");
  }
  if (!ensure_dir(dir)) return error(500, "could not make the recordings folder");
  if (!write_atomic(dir + "/" + id + (journal ? kJournalSuffix : kThumbSuffix), body)) {
    return error(500, "could not save");
  }
  if (journal) prune(dir, limits.max_records);
  return json(200, "{\"ok\":true}");
}

}  // namespace commaview::runtime
