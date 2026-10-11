#include "drive_ledger.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <utility>

namespace commaview::drives {
namespace {

// Just enough JSON for the files this module and the drive stats script write: objects, arrays,
// strings, numbers and the three literals, at most a few levels deep.
struct Json {
  enum class Kind { kNull, kBool, kNumber, kString, kArray, kObject };
  Kind kind = Kind::kNull;
  bool boolean = false;
  double number = 0.0;
  bool integral = false;  // written without a fraction or exponent, and fits int64
  int64_t integer = 0;
  std::string text;
  std::vector<Json> items;
  std::vector<std::pair<std::string, Json>> fields;

  const Json* field(const char* name) const {
    if (kind != Kind::kObject) return nullptr;
    for (const auto& [key, value] : fields) {
      if (key == name) return &value;
    }
    return nullptr;
  }
};

class Parser {
 public:
  explicit Parser(const std::string& text) : t_(text) {}

  bool parse(Json* out) {
    if (!value(out, 0)) return false;
    skip_space();
    return i_ == t_.size();
  }

 private:
  static constexpr int kMaxDepth = 8;

  void skip_space() {
    while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\t' || t_[i_] == '\n' || t_[i_] == '\r')) ++i_;
  }

  bool literal(const char* word) {
    const size_t n = std::strlen(word);
    if (t_.compare(i_, n, word) != 0) return false;
    i_ += n;
    return true;
  }

  bool string(std::string* out) {
    if (i_ >= t_.size() || t_[i_] != '"') return false;
    ++i_;
    out->clear();
    while (i_ < t_.size()) {
      const char c = t_[i_++];
      if (c == '"') return true;
      if (static_cast<unsigned char>(c) < 0x20) return false;
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (i_ >= t_.size()) return false;
      const char e = t_[i_++];
      switch (e) {
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/': out->push_back('/'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
          // Kept only for ASCII; nothing this reads needs more.
          if (i_ + 4 > t_.size()) return false;
          const std::string hex = t_.substr(i_, 4);
          char* end = nullptr;
          const long code = std::strtol(hex.c_str(), &end, 16);
          if (end != hex.c_str() + 4) return false;
          i_ += 4;
          out->push_back(code < 0x80 ? static_cast<char>(code) : '?');
          break;
        }
        default: return false;
      }
    }
    return false;
  }

  bool number(Json* out) {
    const size_t start = i_;
    if (i_ < t_.size() && t_[i_] == '-') ++i_;
    bool digits = false, fraction = false;
    while (i_ < t_.size()) {
      const char c = t_[i_];
      if (c >= '0' && c <= '9') {
        digits = true;
      } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || (c == '-' && i_ > start)) {
        fraction = true;
      } else {
        break;
      }
      ++i_;
    }
    if (!digits) return false;
    const std::string token = t_.substr(start, i_ - start);
    char* end = nullptr;
    const double value = std::strtod(token.c_str(), &end);
    if (end != token.c_str() + token.size() || !std::isfinite(value)) return false;
    out->kind = Json::Kind::kNumber;
    out->number = value;
    if (!fraction) {
      errno = 0;
      const long long whole = std::strtoll(token.c_str(), &end, 10);
      if (errno == 0 && end == token.c_str() + token.size()) {
        out->integral = true;
        out->integer = whole;
      }
    }
    return true;
  }

  bool value(Json* out, int depth) {
    if (depth > kMaxDepth) return false;
    skip_space();
    if (i_ >= t_.size()) return false;
    const char c = t_[i_];
    if (c == '{') {
      ++i_;
      out->kind = Json::Kind::kObject;
      skip_space();
      if (i_ < t_.size() && t_[i_] == '}') {
        ++i_;
        return true;
      }
      while (true) {
        skip_space();
        std::string key;
        if (!string(&key)) return false;
        skip_space();
        if (i_ >= t_.size() || t_[i_] != ':') return false;
        ++i_;
        Json item;
        if (!value(&item, depth + 1)) return false;
        out->fields.emplace_back(std::move(key), std::move(item));
        skip_space();
        if (i_ < t_.size() && t_[i_] == ',') {
          ++i_;
          continue;
        }
        if (i_ < t_.size() && t_[i_] == '}') {
          ++i_;
          return true;
        }
        return false;
      }
    }
    if (c == '[') {
      ++i_;
      out->kind = Json::Kind::kArray;
      skip_space();
      if (i_ < t_.size() && t_[i_] == ']') {
        ++i_;
        return true;
      }
      while (true) {
        Json item;
        if (!value(&item, depth + 1)) return false;
        out->items.push_back(std::move(item));
        skip_space();
        if (i_ < t_.size() && t_[i_] == ',') {
          ++i_;
          continue;
        }
        if (i_ < t_.size() && t_[i_] == ']') {
          ++i_;
          return true;
        }
        return false;
      }
    }
    if (c == '"') {
      out->kind = Json::Kind::kString;
      return string(&out->text);
    }
    if (literal("true")) {
      out->kind = Json::Kind::kBool;
      out->boolean = true;
      return true;
    }
    if (literal("false")) {
      out->kind = Json::Kind::kBool;
      return true;
    }
    if (literal("null")) {
      out->kind = Json::Kind::kNull;
      return true;
    }
    return number(out);
  }

  const std::string& t_;
  size_t i_ = 0;
};

std::optional<Json> parse_json(const std::string& text) {
  Json root;
  Parser parser(text);
  if (!parser.parse(&root)) return std::nullopt;
  return root;
}

// A wall-clock time as the drive list keeps it: a whole number of ms on a set clock.
std::optional<int64_t> wall_field(const Json& object, const char* name) {
  const Json* value = object.field(name);
  if (value == nullptr || value->kind != Json::Kind::kNumber || !value->integral) return std::nullopt;
  if (!valid_wall_ms(value->integer)) return std::nullopt;
  return value->integer;
}

// A count as the drive stats script took one (its whole()): a finite number, rounded, never below 0.
int64_t count_field(const Json& object, const char* name) {
  const Json* value = object.field(name);
  if (value == nullptr || value->kind != Json::Kind::kNumber) return 0;
  if (value->integral) return std::max<int64_t>(0, value->integer);
  return std::max<int64_t>(0, static_cast<int64_t>(std::llround(value->number)));
}

std::optional<DriveRecord> record_from(const Json& object) {
  if (object.kind != Json::Kind::kObject) return std::nullopt;
  const auto start = wall_field(object, "startMs");
  const auto end = wall_field(object, "endMs");
  if (!start || !end) return std::nullopt;
  DriveRecord record;
  record.start_ms = *start;
  record.end_ms = *end;
  record.duration_s = count_field(object, "durationS");
  record.distance_m = count_field(object, "distanceM");
  if (const Json* route = object.field("route"); route != nullptr && route->kind == Json::Kind::kString) {
    record.route = route->text;
  }
  return record;
}

std::string json_string(const std::string& value) {
  std::string out = "\"";
  for (const char c : value) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char escaped[8];
          std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned char>(c));
          out += escaped;
        } else {
          out.push_back(c);
        }
    }
  }
  return out + "\"";
}

bool drive_file_name(const char* name) {
  const size_t n = std::strlen(name);
  if (n <= 5 || std::strcmp(name + n - 5, ".json") != 0) return false;
  for (size_t i = 0; i < n - 5; ++i) {
    if (name[i] < '0' || name[i] > '9') return false;
  }
  return true;
}

std::string parent_dir(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) return ".";
  return slash == 0 ? "/" : path.substr(0, slash);
}

uint64_t fnv1a(uint64_t hash, const std::string& text) {
  for (const unsigned char c : text) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return hash;
}

}  // namespace

bool valid_wall_ms(int64_t ms) { return ms >= kValidWallMs; }

void DistanceMeter::add(uint64_t mono_ns, float v_ego) {
  if (!std::isfinite(v_ego)) return;
  if (have_last_ && mono_ns <= last_mono_ns_) return;
  if (have_last_) {
    const double dt = static_cast<double>(mono_ns - last_mono_ns_) / 1e9;
    if (dt > 0.0 && dt <= kMaxSampleGapS) {
      distance_m_ += std::max(0.0, (static_cast<double>(last_speed_) + v_ego) / 2.0) * dt;
    }
  }
  have_last_ = true;
  last_mono_ns_ = mono_ns;
  last_speed_ = v_ego;
}

void DistanceMeter::forget_last() { have_last_ = false; }

void DistanceMeter::reset() {
  distance_m_ = 0.0;
  have_last_ = false;
  last_mono_ns_ = 0;
  last_speed_ = 0.0f;
}

std::string record_json(const DriveRecord& record) {
  std::ostringstream out;
  out << "{\"startMs\":" << record.start_ms << ",\"endMs\":" << record.end_ms
      << ",\"durationS\":" << std::max<int64_t>(0, record.duration_s)
      << ",\"distanceM\":" << std::max<int64_t>(0, record.distance_m);
  if (!record.route.empty()) out << ",\"route\":" << json_string(record.route);
  out << "}";
  return out.str();
}

std::optional<DriveRecord> parse_record(const std::string& json) {
  const auto root = parse_json(json);
  if (!root) return std::nullopt;
  return record_from(*root);
}

std::string record_file_name(int64_t start_ms) { return std::to_string(start_ms) + ".json"; }

std::vector<StoredDrive> list_drives(const std::string& dir) {
  std::vector<StoredDrive> drives;
  DIR* d = opendir(dir.c_str());
  if (d == nullptr) return drives;
  while (const dirent* entry = readdir(d)) {
    if (!drive_file_name(entry->d_name)) continue;
    const std::string path = dir + "/" + entry->d_name;
    if (auto record = parse_record(read_small_file(path, 4096))) drives.push_back({path, std::move(*record)});
  }
  closedir(d);
  std::sort(drives.begin(), drives.end(), [](const StoredDrive& a, const StoredDrive& b) {
    return a.record.start_ms != b.record.start_ms ? a.record.start_ms < b.record.start_ms : a.path < b.path;
  });
  return drives;
}

std::optional<StoredDrive> find_route(const std::string& dir, const std::string& route) {
  if (route.empty()) return std::nullopt;
  std::optional<StoredDrive> found;
  for (auto& drive : list_drives(dir)) {
    if (drive.record.route == route) found = std::move(drive);
  }
  return found;
}

int prune_drives(const std::string& dir, int64_t now_ms, int keep_days) {
  if (!valid_wall_ms(now_ms)) return 0;
  const int64_t oldest = now_ms - static_cast<int64_t>(keep_days) * kDayMs;
  int removed = 0;
  for (const auto& drive : list_drives(dir)) {
    if (drive.record.end_ms < oldest && unlink(drive.path.c_str()) == 0) ++removed;
  }
  return removed;
}

std::string ledger_json(std::optional<int64_t> since_ms, const std::vector<StoredDrive>& drives,
                        const std::string& skip_path) {
  std::vector<const StoredDrive*> shown;
  for (const auto& drive : drives) {
    if (skip_path.empty() || drive.path != skip_path) shown.push_back(&drive);
  }
  if (since_ms && !valid_wall_ms(*since_ms)) since_ms.reset();
  if (!since_ms && !drives.empty()) since_ms = drives.front().record.start_ms;
  if (!since_ms) return "null";
  std::string out = "{\"version\":1,\"sinceMs\":" + std::to_string(*since_ms) + ",\"drives\":[";
  for (size_t i = 0; i < shown.size(); ++i) {
    if (i) out += ",";
    out += record_json(shown[i]->record);
  }
  return out + "]}";
}

std::string folder_signature(const std::string& dir) {
  DIR* d = opendir(dir.c_str());
  if (d == nullptr) return "none";
  std::vector<std::string> entries;
  while (const dirent* entry = readdir(d)) {
    if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
    struct stat st {};
    const std::string path = dir + "/" + entry->d_name;
    if (stat(path.c_str(), &st) != 0) continue;
    std::ostringstream line;
    line << entry->d_name << ':' << st.st_size << ':' << st.st_mtim.tv_sec << '.' << st.st_mtim.tv_nsec << ':'
         << st.st_ino;
    entries.push_back(line.str());
  }
  closedir(d);
  std::sort(entries.begin(), entries.end());
  uint64_t hash = 14695981039346656037ULL;
  for (const auto& entry : entries) hash = fnv1a(fnv1a(hash, entry), "\n");
  char text[48];
  std::snprintf(text, sizeof(text), "%zu:%016llx", entries.size(), static_cast<unsigned long long>(hash));
  return text;
}

std::optional<LegacyLedger> parse_legacy_ledger(const std::string& json) {
  const auto root = parse_json(json);
  if (!root || root->kind != Json::Kind::kObject) return std::nullopt;
  LegacyLedger ledger;
  ledger.since_ms = wall_field(*root, "sinceMs");
  if (const Json* drives = root->field("drives"); drives != nullptr && drives->kind == Json::Kind::kArray) {
    for (const auto& item : drives->items) {
      if (auto record = record_from(item)) ledger.drives.push_back(std::move(*record));
    }
  }
  return ledger;
}

std::optional<int64_t> parse_since(const std::string& json) {
  const auto root = parse_json(json);
  if (!root || root->kind != Json::Kind::kObject) return std::nullopt;
  return wall_field(*root, "sinceMs");
}

std::string since_json(int64_t since_ms) { return "{\"sinceMs\":" + std::to_string(since_ms) + "}"; }

bool write_file_durable(const std::string& path, const std::string& content) {
  const std::string dir = parent_dir(path);
  if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return false;
  const std::string tmp = path + ".tmp";
  const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return false;
  bool ok = fchmod(fd, 0600) == 0;
  size_t written = 0;
  while (ok && written < content.size()) {
    const ssize_t n = write(fd, content.data() + written, content.size() - written);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      ok = false;
      break;
    }
    written += static_cast<size_t>(n);
  }
  ok = ok && fsync(fd) == 0;
  ok = close(fd) == 0 && ok;
  if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
    unlink(tmp.c_str());
    return false;
  }
  const int dir_fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir_fd >= 0) {
    fsync(dir_fd);
    close(dir_fd);
  }
  return true;
}

std::string read_small_file(const std::string& path, size_t cap_bytes) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return "";
  std::string out;
  char buf[4096];
  while (out.size() <= cap_bytes) {
    const ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    out.append(buf, static_cast<size_t>(n));
  }
  close(fd);
  return out.size() > cap_bytes ? "" : out;
}

}  // namespace commaview::drives
