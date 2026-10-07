#include "ui_export_socket.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

SocketServer::SocketServer(std::string socket_path, std::string recipe_dir)
    : socket_path_(std::move(socket_path)), recipe_dir_(std::move(recipe_dir)) {}

SocketServer::~SocketServer() {
  stop();
}

bool SocketServer::start() {
  if (running_.load()) return true;
  active_route_.clear();
  recipe_route_.clear();
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
    recipe_file_path_ = recipe_dir_ + "/ui-source-" + std::to_string(now_ms()) +
                        "-" + std::to_string(getpid()) + ".jsonl";
    if (ensure_parent_dirs(recipe_file_path_)) {
      chmod(recipe_dir_.c_str(), 0700);
      recipe_file_ = std::fopen(recipe_file_path_.c_str(), "a");
      if (recipe_file_ != nullptr) fchmod(fileno(recipe_file_), 0600);
    }
    if (recipe_file_ == nullptr) {
      const int failure = errno;
      std::fprintf(stderr, "[ui-export] source recipe disabled: cannot open %s errno=%d\n",
                   recipe_file_path_.c_str(), failure);
      recipe_file_path_.clear();
    } else {
      recipe_stopping_ = false;
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
    }
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
    result.recipe_active = recipe_file_ != nullptr;
    result.recipe_route = recipe_route_;
    result.snapshot_events = snapshot_events_;
    result.snapshot_dropped = snapshot_dropped_;
    result.snapshot_write_failures = snapshot_write_failures_;
    result.snapshot_missing_route = snapshot_missing_route_;
    result.snapshot_active = snapshot_active_;
    result.snapshot_route = snapshot_route_;
  }
  return result;
}

std::string SocketServer::recipe_file_path() const {
  std::lock_guard<std::mutex> lock(recipe_mutex_);
  return recipe_file_path_;
}

void SocketServer::offer_recipe_event(const LatestFrame& frame) {
  if (frame.service_index != kRecipeEventServiceIndex || recipe_file_ == nullptr ||
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
  recipe_queue_.push_back("{\"schemaVersion\":1,\"capturedWallMs\":" +
                          std::to_string(frame.updated_at_ms) + "," + json.substr(1) + "\n");
  recipe_condition_.notify_one();
}

void SocketServer::offer_snapshot(const LatestFrame& frame) {
  // These are copies of the comma UI's already-serialized telemetry. Capture
  // at most 10 Hz per service without disk IO in the receive callback.
  if (recipe_file_ == nullptr || frame.service_index >= kServiceCount ||
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
  snapshot_queue_.push_back({frame, sequence});
  recipe_condition_.notify_one();
}

void SocketServer::offer_recording_fault(const char* reason, uint64_t count, int error_number,
                                        uint64_t bytes, uint64_t limit, uint8_t service_index) {
  // Log the first failure and counts 2, 4, 8...; repeated bad frames never produce per-frame IO.
  // The eight tiny entries contain only technical data, never paths, routes or payloads.
  if (count == 0 || (count & (count - 1)) != 0 || recording_faults_.size() >= 8) return;
  recording_faults_.push_back({reason, count, error_number, bytes, limit, service_index});
  recipe_condition_.notify_one();
}

void SocketServer::recipe_writer_loop() {
  while (true) {
    std::string line;
    SnapshotRecord snapshot;
    RecordingFault fault;
    bool has_snapshot = false;
    {
      std::unique_lock<std::mutex> lock(recipe_mutex_);
      recipe_condition_.wait(lock, [this] {
        return recipe_stopping_ || !recipe_queue_.empty() || !snapshot_queue_.empty() || !recording_faults_.empty();
      });
      if (recipe_queue_.empty() && snapshot_queue_.empty() && recording_faults_.empty() && recipe_stopping_) break;
      if (!recipe_queue_.empty()) {
        line = std::move(recipe_queue_.front());
        recipe_queue_.pop_front();
      } else if (!recording_faults_.empty()) {
        fault = recording_faults_.front();
        recording_faults_.pop_front();
      } else {
        snapshot = std::move(snapshot_queue_.front());
        snapshot_queue_.pop_front();
        has_snapshot = true;
      }
    }
    if (fault.reason != nullptr) {
      std::fprintf(stderr, "[ui-export] recording_fault wall_ms=%llu reason=%s count=%llu errno=%d bytes=%llu limit=%llu service=%u\n",
                   static_cast<unsigned long long>(now_ms()), fault.reason,
                   static_cast<unsigned long long>(fault.count), fault.error_number,
                   static_cast<unsigned long long>(fault.bytes), static_cast<unsigned long long>(fault.limit),
                   static_cast<unsigned>(fault.service_index));
      continue;
    }
    const std::string route = current_route_id();
    if (route.empty()) continue;
    if (route != active_route_) {
      std::lock_guard<std::mutex> lock(recipe_mutex_);
      active_route_ = route;
      recipe_route_.clear();
      recipe_events_ = recipe_dropped_ = recipe_write_failures_ = recipe_missing_route_ = 0;
      snapshot_events_ = snapshot_dropped_ = snapshot_write_failures_ = snapshot_missing_route_ = 0;
      snapshot_active_ = false;
    }
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
        if (snapshot_file_ != nullptr) {
          // Final for this route: openpilot starts a new route each time the car goes onroad.
          sync_and_close(snapshot_file_);
          snapshot_file_ = nullptr;
          snapshot_route_.clear();
          std::lock_guard<std::mutex> lock(recipe_mutex_);
          snapshot_active_ = false;
        }
        continue;
      }
      if (snapshot_route_ != route || snapshot_file_ == nullptr) {
        if (snapshot_file_ != nullptr) sync_and_close(snapshot_file_);
        snapshot_route_ = route;
        const std::string path = recipe_dir_ + "/ui-snapshot-" + route + ".jsonl";
        snapshot_file_ = std::fopen(path.c_str(), "a");
        if (snapshot_file_ == nullptr) {
          const int failure = errno;
          std::lock_guard<std::mutex> lock(recipe_mutex_);
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
    line.insert(1, "\"routeId\":\"" + route + "\",");
    const bool written = std::fwrite(line.data(), 1, line.size(), recipe_file_) == line.size() &&
                         std::fflush(recipe_file_) == 0 &&
                         fdatasync(fileno(recipe_file_)) == 0;
    const int write_error = written ? 0 : errno;
    std::lock_guard<std::mutex> lock(recipe_mutex_);
    if (written) {
      ++recipe_events_;
      recipe_route_ = route;
    }
    else {
      ++recipe_write_failures_;
      offer_recording_fault("recipe_write_failed", recipe_write_failures_, write_error, line.size(), 0,
                            kRecipeEventServiceIndex);
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
