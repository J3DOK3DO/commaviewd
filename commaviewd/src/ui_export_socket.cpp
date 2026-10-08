#include "ui_export_socket.h"

#include "recording_metadata_files.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <set>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace commaview::ui_export {
namespace {

constexpr uint32_t kMaxFrameBytes = 512 * 1024;
constexpr size_t kMaxRecipeQueue = 128;
constexpr size_t kMaxSnapshotQueue = 256;
constexpr size_t kMaxSnapshotPayload = 64 * 1024;
constexpr uint64_t kMaxSnapshotFileBytes = 512ULL * 1024 * 1024;
// receive_one_frame() accepts service indexes 0..kRecipeEventServiceIndex and indexes latest_
// with every index below it.
static_assert(kRecipeEventServiceIndex == kServiceCount,
              "the recipe event index must directly follow the telemetry service indexes");

uint64_t now_ms() {
  struct timespec ts = {};
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000ULL + static_cast<uint64_t>(ts.tv_nsec / 1000000ULL);
}

bool recv_all_exact(int fd, uint8_t* data, size_t len) {
  size_t received = 0;
  while (received < len) {
    const ssize_t rc = recv(fd, data + received, len - received, 0);
    if (rc == 0) return false;
    if (rc < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    received += static_cast<size_t>(rc);
  }
  return true;
}

// One JSONL-safe object: starts with '{', ends with '}', and has no line breaks.
bool is_single_line_json_object(const std::string& json) {
  return json.size() >= 2 && json.front() == '{' && json.back() == '}' &&
         json.find_first_of("\r\n") == std::string::npos;
}

void sync_and_close(FILE* file) {
  int failure = 0;
  if (std::fflush(file) != 0) failure = errno;
  if (fdatasync(fileno(file)) != 0 && failure == 0) failure = errno;
  if (std::fclose(file) != 0 && failure == 0) failure = errno;
  if (failure != 0) {
    std::fprintf(stderr, "[ui-export] recording_fault wall_ms=%llu reason=snapshot_close_failed errno=%d\n",
                 static_cast<unsigned long long>(now_ms()), failure);
  }
}

bool ensure_parent_dirs(const std::string& path) {
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos || slash == 0) return true;
  const std::string dir = path.substr(0, slash);
  std::string current;
  for (size_t i = 0; i < dir.size(); ++i) {
    current.push_back(dir[i]);
    if (dir[i] != '/' || current.size() == 1) continue;
    if (mkdir(current.c_str(), 0775) != 0 && errno != EEXIST) return false;
  }
  if (mkdir(dir.c_str(), 0775) != 0 && errno != EEXIST) return false;
  return true;
}

std::string current_route_path() {
  const char* override_path = std::getenv("COMMAVIEWD_CURRENT_ROUTE_FILE");
  return override_path && override_path[0] ? override_path : "/data/params/d/CurrentRoute";
}

std::string read_param_beside_route(const char* key) {
  std::string path = current_route_path();
  const size_t slash = path.find_last_of('/');
  path = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + key;
  FILE* file = std::fopen(path.c_str(), "r");
  if (file == nullptr) return {};
  char buffer[8] = {};
  const bool read = std::fgets(buffer, sizeof(buffer), file) != nullptr;
  std::fclose(file);
  if (!read) return {};
  std::string value(buffer);
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
  return value;
}

// Offroad as the comma says so (sunnypilot publishes IsOffroad, openpilot IsOnroad). An unknown
// state counts as onroad, so a drive is never missed.
bool comma_offroad() {
  const std::string offroad = read_param_beside_route("IsOffroad");
  if (offroad == "1") return true;
  if (offroad == "0") return false;
  return read_param_beside_route("IsOnroad") == "0";
}

std::string current_route_id() {
  const std::string path_string = current_route_path();
  const char* path = path_string.c_str();
  FILE* file = std::fopen(path, "r");
  if (file == nullptr) return {};
  char buffer[128] = {};
  const bool read = std::fgets(buffer, sizeof(buffer), file) != nullptr;
  std::fclose(file);
  if (!read) return {};
  std::string route(buffer);
  while (!route.empty() && std::isspace(static_cast<unsigned char>(route.back()))) {
    route.pop_back();
  }
  if (route.empty() || route.size() > 96 || route.find("--") == std::string::npos) return {};
  for (char c : route) {
    if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
        !(c >= '0' && c <= '9') && c != '-' && c != '_') return {};
  }
  return route;
}

std::string projection_camera(const std::string& json) {
  const size_t key = json.find("\"camera\"");
  if (key == std::string::npos) return {};
  size_t pos = json.find(':', key + 8);
  if (pos == std::string::npos) return {};
  ++pos;
  while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
  if (pos >= json.size() || json[pos++] != '"') return {};
  const size_t end = json.find('"', pos);
  if (end == std::string::npos) return {};
  const std::string camera = json.substr(pos, end - pos);
  return camera == "road" || camera == "wideRoad" ? camera : std::string{};
}

uint64_t projection_uint(const std::string& json, const char* name) {
  const std::string key = std::string("\"") + name + "\"";
  const size_t found = json.find(key);
  if (found == std::string::npos) return 0;
  size_t pos = json.find(':', found + key.size());
  if (pos == std::string::npos) return 0;
  ++pos;
  while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
  if (pos >= json.size() || !std::isdigit(static_cast<unsigned char>(json[pos]))) return 0;
  uint64_t value = 0;
  while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) {
    const uint64_t digit = static_cast<unsigned char>(json[pos]) - '0';
    if (value > (UINT64_MAX - digit) / 10) return 0;
    value = value * 10 + digit;
    ++pos;
  }
  return value;
}

}  // namespace

std::string default_socket_path() {
  const char* env = std::getenv("COMMAVIEWD_UI_EXPORT_SOCKET");
  if (env != nullptr && env[0] != '\0') return env;
  return "/data/commaview/run/ui-export.sock";
}

std::string default_recipe_dir() {
  const char* env = std::getenv("COMMAVIEWD_RECIPE_DIR");
  if (env != nullptr && env[0] != '\0') return env;
  // Source metadata is prepared automatically while a stock route exists.
  // The writer is route-bound and capped; stock camera video is never copied.
  return "/data/commaview/recording-recipes";
}

size_t prune_recording_metadata(const std::string& recipe_dir, const std::string& archive_root,
                                const std::vector<std::string>& keep, bool scan_legacy) {
  // Drives with at least one segment folder (<route>--<segment>) left.
  std::set<std::string> drives;
  DIR* archive = ::opendir(archive_root.c_str());
  if (archive == nullptr) return 0;
  bool listed = true;
  while (true) {
    errno = 0;
    const dirent* entry = ::readdir(archive);
    if (entry == nullptr) {
      listed = errno == 0;
      break;
    }
    const std::string name = entry->d_name;
    const size_t dash = name.rfind("--");
    if (dash == std::string::npos || dash == 0 || dash + 2 == name.size() ||
        name.find_first_not_of("0123456789", dash + 2) != std::string::npos) continue;
    drives.insert(name.substr(0, dash));
  }
  ::closedir(archive);
  // An unreadable or empty archive (not mounted yet, or another layout) proves nothing.
  if (!listed || drives.empty()) return 0;
  const auto deleted = [&](const std::string& route) {
    return drives.count(route) == 0 && std::find(keep.begin(), keep.end(), route) == keep.end();
  };

  const int directory = ::open(recipe_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory < 0) return 0;
  const int listing_fd = ::dup(directory);
  DIR* listing = listing_fd >= 0 ? ::fdopendir(listing_fd) : nullptr;
  if (listing == nullptr) {
    if (listing_fd >= 0) ::close(listing_fd);
    ::close(directory);
    return 0;
  }
  std::vector<std::string> doomed;
  while (const dirent* entry = ::readdir(listing)) {
    const std::string name = entry->d_name;
    recording_files::RecipeFile recipe;
    if (recording_files::parse_recipe_file_name(name, &recipe)) {
      if (!recipe.legacy) {
        if (deleted(recipe.route)) doomed.push_back(name);
        continue;
      }
      if (!scan_legacy) continue;
      // An older runtime's file for every route of one process: removable once all are gone.
      const int file = ::openat(directory, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
      if (file < 0) continue;
      bool in_use = false;
      const auto scan = recording_files::scan_lines(file, recording_files::kMaxRecipeLineBytes,
          [&](const std::string& line, bool) {
            const std::string key = "\"routeId\":\"";
            const size_t at = line.find(key);
            if (at == std::string::npos) return true;
            const size_t end = line.find('"', at + key.size());
            in_use = end == std::string::npos || !deleted(line.substr(at + key.size(), end - at - key.size()));
            return !in_use;
          });
      ::close(file);
      if (scan == recording_files::LineScan::complete && !in_use) doomed.push_back(name);
      continue;
    }
    std::string route;
    if (name.rfind("ui-snapshot-", 0) == 0 && name.size() > 18 &&
        name.compare(name.size() - 6, 6, ".jsonl") == 0) {
      route = name.substr(12, name.size() - 18);
    } else if (name.rfind("ui-invalid-", 0) == 0) {
      route = name.substr(11);
    }
    if (recording_files::safe_route(route) && deleted(route)) doomed.push_back(name);
  }
  ::closedir(listing);
  size_t removed = 0;
  for (const std::string& name : doomed) {
    if (::unlinkat(directory, name.c_str(), 0) == 0) ++removed;
  }
  ::close(directory);
  return removed;
}

SocketServer::SocketServer(std::string socket_path, std::string recipe_dir)
    : socket_path_(std::move(socket_path)), recipe_dir_(std::move(recipe_dir)) {}

SocketServer::~SocketServer() {
  stop();
}

bool SocketServer::start() {
  if (running_.load()) return true;
  observe_route(current_route_id());
  if (!ensure_parent_dirs(socket_path_)) {
    std::fprintf(stderr, "[ui-export] failed to create parent dirs for %s\n", socket_path_.c_str());
    return false;
  }

  unlink(socket_path_.c_str());

  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    std::perror("socket(AF_UNIX)");
    return false;
  }

  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  if (socket_path_.size() >= sizeof(addr.sun_path)) {
    std::fprintf(stderr, "[ui-export] socket path too long: %s\n", socket_path_.c_str());
    close(fd);
    return false;
  }
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path_.c_str());

  if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::perror("bind(AF_UNIX)");
    close(fd);
    return false;
  }

  if (listen(fd, 1) != 0) {
    std::perror("listen(AF_UNIX)");
    close(fd);
    unlink(socket_path_.c_str());
    return false;
  }

  server_fd_ = fd;
  if (!recipe_dir_.empty()) {
    // Each route's recipe file opens when its first event is written.
    bool usable = ensure_parent_dirs(recipe_dir_ + "/ui-source");
    if (usable) {
      chmod(recipe_dir_.c_str(), 0700);
      usable = access(recipe_dir_.c_str(), W_OK | X_OK) == 0;
    }
    if (!usable) {
      const int failure = errno;
      std::fprintf(stderr, "[ui-export] source recipe disabled: cannot write %s errno=%d\n",
                   recipe_dir_.c_str(), failure);
    } else {
      {
        std::lock_guard<std::mutex> lock(recipe_mutex_);
        recipe_stamp_ = std::to_string(now_ms()) + "-" + std::to_string(getpid());
        recipe_enabled_ = true;
        recipe_stopping_ = false;
      }
      // Before the UI's frames are accepted: older runtimes' recipe files are read once here.
      prune_metadata(true);
      recipe_thread_ = std::thread(&SocketServer::recipe_writer_loop, this);
    }
  }
  running_.store(true);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.running = true;
  }
  accept_thread_ = std::thread(&SocketServer::accept_loop, this);
  return true;
}

void SocketServer::stop() {
  const bool was_running = running_.exchange(false);
  if (!was_running) return;

  if (server_fd_ >= 0) {
    // close() alone does not reliably wake a blocking accept() on Linux.
    shutdown(server_fd_, SHUT_RDWR);
    close(server_fd_);
    server_fd_ = -1;
  }
  {
    std::lock_guard<std::mutex> lock(client_mutex_);
    if (client_fd_ >= 0) shutdown(client_fd_, SHUT_RDWR);
  }
  if (accept_thread_.joinable()) accept_thread_.join();
  {
    std::lock_guard<std::mutex> lock(recipe_mutex_);
    recipe_stopping_ = true;
  }
  recipe_condition_.notify_all();
  if (recipe_thread_.joinable()) recipe_thread_.join();
  {
    std::lock_guard<std::mutex> lock(recipe_mutex_);
    if (snapshot_file_ != nullptr) {
      sync_and_close(snapshot_file_);
      snapshot_file_ = nullptr;
      snapshot_active_ = false;
    }
    if (recipe_file_ != nullptr) {
      std::fclose(recipe_file_);
      recipe_file_ = nullptr;
      recipe_file_route_.clear();
    }
    recipe_enabled_ = false;
  }
  unlink(socket_path_.c_str());

  std::lock_guard<std::mutex> lock(mutex_);
  stats_.running = false;
  stats_.connected = false;
}

bool SocketServer::latest_frame(uint8_t service_index, uint64_t fresh_within_ms, LatestFrame* out) const {
  if (service_index >= kServiceCount || out == nullptr) return false;
  const uint64_t now = now_ms();
  std::lock_guard<std::mutex> lock(mutex_);
  const LatestFrame& frame = latest_[service_index];
  if (!frame.available) return false;
  if (fresh_within_ms > 0 && (now < frame.updated_at_ms || (now - frame.updated_at_ms) > fresh_within_ms)) {
    return false;
  }
  *out = frame;
  return true;
}

SocketStats SocketServer::stats() const {
  SocketStats result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    result = stats_;
  }
  {
    std::lock_guard<std::mutex> lock(recipe_mutex_);
    result.recipe_events = recipe_events_;
    result.recipe_dropped = recipe_dropped_;
    result.recipe_write_failures = recipe_write_failures_;
    result.recipe_missing_route = recipe_missing_route_;
    result.recipe_active = recipe_enabled_;
    result.recipe_route = recipe_route_;
    result.snapshot_events = snapshot_events_;
    result.snapshot_dropped = snapshot_dropped_;
    result.snapshot_write_failures = snapshot_write_failures_;
    result.snapshot_missing_route = snapshot_missing_route_;
    result.snapshot_active = snapshot_active_;
    result.snapshot_route = snapshot_route_;
    result.metadata_route = active_route_;
    result.route_generation = route_generation_;
    result.route_transition_discarded = route_transition_discarded_;
    result.recording_faults_suppressed = recording_faults_suppressed_;
    result.metadata_damaged = metadata_damaged_;
  }
  return result;
}

std::string SocketServer::recipe_file_path() const {
  std::lock_guard<std::mutex> lock(recipe_mutex_);
  return recipe_file_path_;
}

void SocketServer::offer_recipe_event(const LatestFrame& frame) {
  if (frame.service_index != kRecipeEventServiceIndex || !recipe_enabled_ ||
      frame.payload.size() > 4096) return;
  const std::string json(frame.payload.begin(), frame.payload.end());
  if (!is_single_line_json_object(json)) return;
  const bool switched = json.find("\"kind\":\"camera_switch\"") != std::string::npos;
  const bool anchor = json.find("\"kind\":\"anchor\"") != std::string::npos;
  if (!switched && !anchor) return;
  if (projection_uint(json, "sequence") == 0) return;
  const std::string camera = projection_camera(json);
  if (camera.empty()) return;
  if (projection_uint(json, "logMonoTime") == 0 ||
      projection_uint(json, camera == "road" ? "roadTimestampEof" : "wideTimestampEof") == 0) {
    return;
  }

  std::lock_guard<std::mutex> lock(recipe_mutex_);
  if (recipe_queue_.size() >= kMaxRecipeQueue) {
    ++recipe_dropped_;
    offer_recording_fault("recipe_queue_full", recipe_dropped_, 0, recipe_queue_.size(),
                          kMaxRecipeQueue, frame.service_index);
    return;
  }
  recipe_queue_.push_back({"{\"schemaVersion\":1,\"capturedWallMs\":" +
                          std::to_string(frame.updated_at_ms) + "," + json.substr(1) + "\n", active_epoch_});
  recipe_condition_.notify_one();
}

void SocketServer::offer_snapshot(const LatestFrame& frame) {
  // These are copies of the comma UI's already-serialized telemetry. Capture
  // at most 10 Hz per service without disk IO in the receive callback.
  if (!recipe_enabled_ || frame.service_index >= kServiceCount ||
      frame.payload.empty()) return;
  if (frame.payload.size() > kMaxSnapshotPayload) {
    std::lock_guard<std::mutex> lock(recipe_mutex_);
    ++snapshot_sequence_;
    ++snapshot_dropped_;
    offer_recording_fault("snapshot_payload_too_large", snapshot_dropped_, 0, frame.payload.size(),
                          kMaxSnapshotPayload, frame.service_index);
    return;
  }
  const std::string json(frame.payload.begin(), frame.payload.end());
  if (!is_single_line_json_object(json)) {
    std::lock_guard<std::mutex> lock(recipe_mutex_);
    ++snapshot_sequence_;
    ++snapshot_dropped_;
    offer_recording_fault("snapshot_json_invalid", snapshot_dropped_, 0, frame.payload.size(),
                          kMaxSnapshotPayload, frame.service_index);
    return;
  }
  std::lock_guard<std::mutex> lock(recipe_mutex_);
  const uint64_t previous = last_snapshot_offer_ms_[frame.service_index];
  if (previous && frame.updated_at_ms >= previous && frame.updated_at_ms - previous < 100) return;
  last_snapshot_offer_ms_[frame.service_index] = frame.updated_at_ms;
  const uint64_t sequence = ++snapshot_sequence_;
  if (snapshot_queue_.size() >= kMaxSnapshotQueue) {
    ++snapshot_dropped_;
    offer_recording_fault("snapshot_queue_full", snapshot_dropped_, 0, snapshot_queue_.size(),
                          kMaxSnapshotQueue, frame.service_index);
    return;
  }
  snapshot_queue_.push_back({frame, sequence, active_epoch_});
  recipe_condition_.notify_one();
}

void SocketServer::offer_recording_fault(const char* reason, uint64_t count, int error_number,
                                        uint64_t bytes, uint64_t limit, uint8_t service_index) {
  // Counts are per reason/errno, not a shared integrity counter: another reason's
  // third failure must still preserve this cause's first occurrence.
  (void)count;
  FaultCounter* counter = nullptr;
  for (auto& entry : recording_fault_counts_) {
    if (entry.reason != nullptr && entry.error_number == error_number && std::strcmp(entry.reason, reason) == 0) {
      counter = &entry;
      break;
    }
  }
  if (counter == nullptr) {
    for (auto& entry : recording_fault_counts_) {
      if (entry.reason == nullptr) { entry.reason = reason; entry.error_number = error_number; counter = &entry; break; }
    }
  }
  if (counter == nullptr) {
    ++recording_faults_suppressed_;
    recipe_condition_.notify_one();
    return;
  }
  const uint64_t occurrence = ++counter->count;
  if ((occurrence & (occurrence - 1)) != 0) return;
  if (recording_faults_.size() >= 8) {
    // Prefer a new first cause over a queued repeat. All-first saturation is
    // accounted for and reported by the writer, never silently ignored.
    auto repeat = recording_faults_.end();
    if (occurrence == 1) {
      for (auto it = recording_faults_.begin(); it != recording_faults_.end(); ++it) {
        if (it->count > 1) { repeat = it; break; }
      }
    }
    ++recording_faults_suppressed_;
    if (repeat == recording_faults_.end()) { recipe_condition_.notify_one(); return; }
    recording_faults_.erase(repeat);
  }
  recording_faults_.push_back({reason, occurrence, error_number, bytes, limit, service_index, now_ms()});
  recipe_condition_.notify_one();
}

bool SocketServer::observe_route(const std::string& route) {
  // Once per transition/startup, off the frame path. Restore known route damage.
  if (route == active_route_) return false;
  struct stat marker = {};
  const bool damaged = !route.empty() &&
      ::lstat((recipe_dir_ + "/ui-invalid-" + route).c_str(), &marker) == 0;
  std::lock_guard<std::mutex> lock(recipe_mutex_);
  if (route == active_route_) return false;
  active_route_ = route;
  ++route_generation_;
  active_epoch_ = std::make_shared<RouteEpoch>(RouteEpoch{route, route_generation_, false});
  metadata_damaged_ = damaged;
  recipe_route_.clear();
  snapshot_route_.clear();
  recipe_events_ = recipe_dropped_ = recipe_write_failures_ = recipe_missing_route_ = 0;
  snapshot_events_ = snapshot_dropped_ = snapshot_write_failures_ = snapshot_missing_route_ = 0;
  snapshot_active_ = false;
  offroad_checked_at_ = {}; // An old drive's cached road state cannot finalize the new epoch.
  last_snapshot_offer_ms_.fill(0);
  recording_fault_counts_.fill({});
  return true;
}

FILE* SocketServer::recipe_file_for(const std::string& route) {
  if (recipe_file_ != nullptr && recipe_file_route_ == route) return recipe_file_;
  if (recipe_file_ != nullptr) std::fclose(recipe_file_);  // Every line was synced when written.
  recipe_file_ = nullptr;
  recipe_file_route_.clear();
  const std::string path = recipe_dir_ + "/" + recording_files::recipe_file_name(route, recipe_stamp_);
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return nullptr;
  FILE* file = ::fdopen(fd, "a");
  if (file == nullptr) {
    const int failure = errno;
    ::close(fd);
    errno = failure;
    return nullptr;
  }
  recipe_file_ = file;
  recipe_file_route_ = route;
  std::lock_guard<std::mutex> lock(recipe_mutex_);
  recipe_file_path_ = path;
  return file;
}

void SocketServer::prune_metadata(bool scan_legacy) {
  std::vector<std::string> keep;
  {
    std::lock_guard<std::mutex> lock(recipe_mutex_);
    keep = {active_route_, snapshot_route_};
  }
  keep.push_back(recipe_file_route_);
  const size_t removed = prune_recording_metadata(recipe_dir_, recording_files::default_source_archive_root(),
                                                  keep, scan_legacy);
  // Counts only: support exports must not carry recording identifiers.
  if (removed > 0) {
    std::fprintf(stderr, "[ui-export] removed %zu recording metadata files of deleted drives\n", removed);
  }
}

void SocketServer::mark_route_damage(const std::shared_ptr<RouteEpoch>& epoch, const char* reason) {
  if (!epoch || epoch->route.empty() || epoch->damage_recorded || !recipe_enabled_) return;
  // Failed storage must not turn damage-marker retries into per-frame IO.
  const uint64_t attempt = ++epoch->damage_attempts;
  if ((attempt & (attempt - 1)) != 0) return;
  if (epoch->damage_reason == nullptr) epoch->damage_reason = reason;
  reason = epoch->damage_reason;
  // Once per damaged route, on the writer only. The paired archive retains this
  // marker across restarts; old clients also fail closed on sequence zero.
  {
    std::lock_guard<std::mutex> lock(recipe_mutex_);
    if (epoch == active_epoch_) metadata_damaged_ = true;
  }
  // A tiny atomic sidecar restores the guard after restart. Sequence zero in
  // the recipe below also makes older clients fail closed.
  const std::string path = recipe_dir_ + "/ui-invalid-" + epoch->route;
  const std::string temporary = path + ".tmp-" + std::to_string(getpid());
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  const char damaged[] = "recording metadata incomplete\n";
  bool persisted = fd >= 0;
  int persist_error = persisted ? 0 : errno;
  if (persisted) {
    const ssize_t count = ::write(fd, damaged, sizeof(damaged) - 1);
    if (count != static_cast<ssize_t>(sizeof(damaged) - 1)) {
      persisted = false; persist_error = count < 0 ? errno : EIO;
    } else if (fdatasync(fd) != 0) {
      persisted = false; persist_error = errno;
    }
  }
  if (fd >= 0 && ::close(fd) != 0 && persisted) { persisted = false; persist_error = errno; }
  if (persisted && ::rename(temporary.c_str(), path.c_str()) != 0) { persisted = false; persist_error = errno; }
  if (persisted) {
    const int parent = ::open(recipe_dir_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent < 0 || ::fsync(parent) != 0) { persisted = false; persist_error = errno; }
    if (parent >= 0) ::close(parent);
  }
  ::unlink(temporary.c_str());
  const std::string marker = "\n{\"schemaVersion\":1,\"routeId\":\"" + epoch->route +
      "\",\"kind\":\"recording_fault\",\"sequence\":0,\"reason\":\"" + reason + "\"}\n";
  // The fault row goes in that route's own recipe file, where its recipe is read from.
  errno = 0;
  FILE* recipe = recipe_file_for(epoch->route);
  const bool written = recipe != nullptr &&
      std::fwrite(marker.data(), 1, marker.size(), recipe) == marker.size() &&
      std::fflush(recipe) == 0 && fdatasync(fileno(recipe)) == 0;
  const int failure = written ? 0 : (errno ? errno : EIO);
  epoch->damage_recorded = written && persisted;
  if (!written || !persisted) {
    std::lock_guard<std::mutex> lock(recipe_mutex_);
    // The writer may still be retiring an old epoch after observe_route has
    // reset the counters for the next drive. Keep its fault evidence without
    // making that healthy drive permanently fail recording preflight.
    if (epoch == active_epoch_) ++recipe_write_failures_;
    offer_recording_fault("route_damage_marker_write_failed", attempt, !written ? failure : persist_error, marker.size(), 0, kRecipeEventServiceIndex);
  }
}

void SocketServer::recipe_writer_loop() {
  const auto suppression_due = [this] {
    return recording_faults_suppressed_ > reported_faults_suppressed_ &&
        (reported_faults_suppressed_ == 0 || recording_faults_suppressed_ / 2 >= reported_faults_suppressed_ || recipe_stopping_);
  };
  while (true) {
    std::string line;
    SnapshotRecord snapshot;
    RecordingFault fault;
    bool has_snapshot = false;
    std::shared_ptr<RouteEpoch> epoch;
    {
      std::unique_lock<std::mutex> lock(recipe_mutex_);
      recipe_condition_.wait(lock, [this, &suppression_due] {
        return recipe_stopping_ || !recipe_queue_.empty() || !snapshot_queue_.empty() || !recording_faults_.empty() ||
            suppression_due();
      });
      if (recipe_queue_.empty() && snapshot_queue_.empty() && recording_faults_.empty() &&
          recording_faults_suppressed_ == reported_faults_suppressed_ && recipe_stopping_) break;
      if (!recipe_queue_.empty()) {
        epoch = recipe_queue_.front().epoch;
        line = std::move(recipe_queue_.front().line);
        recipe_queue_.pop_front();
      } else if (!recording_faults_.empty()) {
        fault = recording_faults_.front();
        recording_faults_.pop_front();
      } else if (suppression_due()) {
        reported_faults_suppressed_ = recording_faults_suppressed_;
        fault = {"recording_fault_queue_overflow", reported_faults_suppressed_, 0, 0, 8, 0, now_ms()};
      } else {
        snapshot = std::move(snapshot_queue_.front());
        snapshot_queue_.pop_front();
        has_snapshot = true;
        epoch = snapshot.epoch;
      }
    }
    if (fault.reason != nullptr) {
      std::fprintf(stderr, "[ui-export] recording_fault wall_ms=%llu reason=%s count=%llu errno=%d bytes=%llu limit=%llu service=%u\n",
                   static_cast<unsigned long long>(fault.wall_ms), fault.reason,
                   static_cast<unsigned long long>(fault.count), fault.error_number,
                   static_cast<unsigned long long>(fault.bytes), static_cast<unsigned long long>(fault.limit),
                   static_cast<unsigned>(fault.service_index));
      continue;
    }
    const std::string route = current_route_id();
    // A new drive: drop the metadata of drives openpilot has deleted since.
    if (!route.empty() && observe_route(route)) prune_metadata(false);
    // Offroad UI traffic cannot extend a finalized drive or turn it into a
    // damaged archive at the next ignition. Still observe the new route first,
    // so future offers acquire its fresh epoch instead of remaining on the old one.
    if (epoch && epoch->finalized) continue;
    // Losing a record only damages a drive this process was recording. A bridge that starts
    // offroad (a runtime update, or a boot that reads the previous drive's CurrentRoute before
    // openpilot clears it) holds an epoch for a finished route: its queued offroad traffic is
    // never part of that drive, so the next ignition must not mark the finished drive damaged.
    const bool recording = epoch && epoch->onroad_observed;
    if (route.empty()) {
      if (recording) mark_route_damage(epoch, "route_unavailable");
      // Startup has no owner yet. A transient missing parameter during an
      // established drive must not clear that drive's integrity failures.
      std::lock_guard<std::mutex> lock(recipe_mutex_);
      if (!active_route_.empty()) {
        auto& missing = has_snapshot ? snapshot_missing_route_ : recipe_missing_route_;
        ++missing;
        offer_recording_fault(has_snapshot ? "snapshot_route_unavailable" : "recipe_route_unavailable",
                              missing, 0, 0, 0, has_snapshot ? snapshot.frame.service_index : kRecipeEventServiceIndex);
      }
      continue;
    }
    if (!epoch || epoch->generation != route_generation_) {
      if (recording) mark_route_damage(epoch, "route_transition_discarded");
      std::lock_guard<std::mutex> lock(recipe_mutex_);
      ++route_transition_discarded_;
      offer_recording_fault("metadata_route_transition", route_transition_discarded_, 0, 0, 0,
                            has_snapshot ? snapshot.frame.service_index : kRecipeEventServiceIndex);
      continue;
    }
    if (epoch->damage_attempts != 0 && !epoch->damage_recorded) mark_route_damage(epoch, epoch->damage_reason);
    if (has_snapshot) {
      // CurrentRoute stays set after the drive, and the comma's UI keeps exporting while offroad.
      // A finished route's snapshot must stop growing, or the app's download of it (which checks
      // that the file doesn't change mid-transfer) fails and the drive can't be rendered.
      const auto now = std::chrono::steady_clock::now();
      if (now - offroad_checked_at_ >= std::chrono::seconds(1)) {
        offroad_checked_at_ = now;
        offroad_ = comma_offroad();
      }
      if (offroad_) {
        // CurrentRoute can change before IsOffroad at startup. Only finalize
        // an observed drive after any known damage is durable; pending marker
        // writes must retain their bounded retries when storage recovers offroad.
        epoch->finalized = epoch->onroad_observed &&
            (epoch->damage_attempts == 0 || epoch->damage_recorded);
        if (snapshot_file_ != nullptr) {
          // Final for this route: openpilot starts a new route each time the car goes onroad.
          sync_and_close(snapshot_file_);
          snapshot_file_ = nullptr;
          std::lock_guard<std::mutex> lock(recipe_mutex_);
          snapshot_route_.clear();
          snapshot_active_ = false;
        }
        continue;
      }
      epoch->onroad_observed = true;
      if (snapshot_route_ != route || snapshot_file_ == nullptr) {
        if (snapshot_file_ != nullptr) sync_and_close(snapshot_file_);
        const std::string path = recipe_dir_ + "/ui-snapshot-" + route + ".jsonl";
        snapshot_file_ = std::fopen(path.c_str(), "a");
        if (snapshot_file_ == nullptr) {
          const int failure = errno;
          std::lock_guard<std::mutex> lock(recipe_mutex_);
          snapshot_route_ = route;
          snapshot_active_ = false;
          ++snapshot_write_failures_;
          offer_recording_fault("snapshot_open_failed", snapshot_write_failures_, failure, 0, 0,
                                snapshot.frame.service_index);
          continue;
        }
        if (snapshot_file_ != nullptr) {
          fchmod(fileno(snapshot_file_), 0600);
          struct stat info = {};
          snapshot_bytes_ = fstat(fileno(snapshot_file_), &info) == 0 && info.st_size >= 0 ?
              static_cast<uint64_t>(info.st_size) : kMaxSnapshotFileBytes;
        }
        {
          std::lock_guard<std::mutex> lock(recipe_mutex_);
          snapshot_route_ = route;
          snapshot_active_ = snapshot_file_ != nullptr;
        }
      }
      const std::string payload(snapshot.frame.payload.begin(), snapshot.frame.payload.end());
      const std::string record = "{\"schemaVersion\":1,\"routeId\":\"" + route +
          "\",\"sequence\":" + std::to_string(snapshot.sequence) +
          ",\"serviceIndex\":" + std::to_string(snapshot.frame.service_index) +
          ",\"capturedWallMs\":" + std::to_string(snapshot.frame.updated_at_ms) +
          ",\"payload\":" + payload + "}\n";
      if (snapshot_bytes_ > kMaxSnapshotFileBytes - record.size()) {
        std::lock_guard<std::mutex> lock(recipe_mutex_);
        ++snapshot_dropped_;
        offer_recording_fault("snapshot_file_limit", snapshot_dropped_, 0, snapshot_bytes_,
                              kMaxSnapshotFileBytes, snapshot.frame.service_index);
        continue;
      }
      const bool written = snapshot_file_ != nullptr &&
          std::fwrite(record.data(), 1, record.size(), snapshot_file_) == record.size() &&
          std::fflush(snapshot_file_) == 0 &&
          ((snapshot_events_ + 1) % 20 != 0 || fdatasync(fileno(snapshot_file_)) == 0);
      const int write_error = written ? 0 : errno;
      std::lock_guard<std::mutex> lock(recipe_mutex_);
      if (written) {
        ++snapshot_events_;
        snapshot_bytes_ += record.size();
      }
      else {
        ++snapshot_write_failures_;
        offer_recording_fault("snapshot_write_failed", snapshot_write_failures_, write_error,
                              record.size(), 0, snapshot.frame.service_index);
      }
      continue;
    }
    // Recipe events carry camera frame timestamps: the UI only sends them onroad.
    epoch->onroad_observed = true;
    line.insert(1, "\"routeId\":\"" + route + "\",");
    errno = 0;
    FILE* recipe = recipe_file_for(route);
    const bool written = recipe != nullptr &&
                         std::fwrite(line.data(), 1, line.size(), recipe) == line.size() &&
                         std::fflush(recipe) == 0 &&
                         fdatasync(fileno(recipe)) == 0;
    const int write_error = written ? 0 : (errno ? errno : EIO);
    std::lock_guard<std::mutex> lock(recipe_mutex_);
    if (written) {
      ++recipe_events_;
      recipe_route_ = route;
    }
    else {
      ++recipe_write_failures_;
      offer_recording_fault(recipe == nullptr ? "recipe_open_failed" : "recipe_write_failed",
                            recipe_write_failures_, write_error, line.size(), 0, kRecipeEventServiceIndex);
    }
  }
}

void SocketServer::mark_client_connected(bool connected) {
  std::lock_guard<std::mutex> lock(mutex_);
  stats_.connected = connected;
  if (connected) stats_.connect_count += 1;
}

void SocketServer::accept_loop() {
  while (running_.load()) {
    const int client_fd = accept4(server_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (client_fd < 0) {
      if (running_.load() && errno != EINTR) std::perror("accept(AF_UNIX)");
      continue;
    }

    {
      std::lock_guard<std::mutex> lock(client_mutex_);
      client_fd_ = client_fd;
    }
    mark_client_connected(true);
    while (running_.load() && receive_one_frame(client_fd)) {
    }
    {
      std::lock_guard<std::mutex> lock(client_mutex_);
      client_fd_ = -1;
      close(client_fd);
    }
    mark_client_connected(false);
  }
}

bool SocketServer::receive_one_frame(int client_fd) {
  uint8_t len_buf[4] = {};
  if (!recv_all_exact(client_fd, len_buf, sizeof(len_buf))) return false;

  uint32_t payload_len_be = 0;
  std::memcpy(&payload_len_be, len_buf, sizeof(payload_len_be));
  const uint32_t payload_len = ntohl(payload_len_be);
  if (payload_len < 3 || payload_len > kMaxFrameBytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.malformed_count += 1;
    return false;
  }

  std::vector<uint8_t> payload(payload_len);
  if (!recv_all_exact(client_fd, payload.data(), payload.size())) return false;

  const uint8_t version = payload[0];
  const uint8_t service_index = payload[1];
  if (version != kFrameVersion || service_index > kRecipeEventServiceIndex) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.malformed_count += 1;
    return false;
  }

  LatestFrame frame;
  frame.available = true;
  frame.service_index = service_index;
  frame.updated_at_ms = now_ms();
  frame.payload.assign(payload.begin() + 2, payload.end());

  if (service_index == kRecipeEventServiceIndex) {
    offer_recipe_event(frame);
    return true;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_[service_index] = frame;
    stats_.accepted_count += 1;
    stats_.last_receive_ms = frame.updated_at_ms;
  }
  offer_snapshot(frame);
  return true;
}

}  // namespace commaview::ui_export
