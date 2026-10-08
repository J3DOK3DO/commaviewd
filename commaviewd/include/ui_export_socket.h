#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace commaview::ui_export {

inline constexpr uint8_t kFrameVersion = 1;
inline constexpr uint8_t kServiceCount = 20;
// Internal source-side recording event. Never forwarded as Android telemetry.
inline constexpr uint8_t kRecipeEventServiceIndex = 20;
inline constexpr uint64_t kFreshFrameWindowMs = 750;

struct LatestFrame {
  bool available = false;
  uint8_t service_index = 0;
  uint64_t updated_at_ms = 0;
  std::vector<uint8_t> payload = {};
};

struct SocketStats {
  bool running = false;
  bool connected = false;
  uint64_t connect_count = 0;
  uint64_t accepted_count = 0;
  uint64_t malformed_count = 0;
  uint64_t last_receive_ms = 0;
  uint64_t recipe_events = 0;
  uint64_t recipe_dropped = 0;
  uint64_t recipe_write_failures = 0;
  uint64_t recipe_missing_route = 0;
  bool recipe_active = false;
  std::string recipe_route;
  uint64_t snapshot_events = 0;
  uint64_t snapshot_dropped = 0;
  uint64_t snapshot_write_failures = 0;
  uint64_t snapshot_missing_route = 0;
  bool snapshot_active = false;
  std::string snapshot_route;
  std::string metadata_route;
  uint64_t route_generation = 0;
  uint64_t route_transition_discarded = 0;
  uint64_t recording_faults_suppressed = 0;
  bool metadata_damaged = false;
};

std::string default_socket_path();
std::string default_recipe_dir();

// Removes the UI recording metadata (recipe files, snapshot, damage marker) of drives whose source
// segments are gone from archive_root: openpilot deleted them, so nothing can be replayed. Routes
// in `keep` stay. Nothing is removed while the archive can't be listed or holds no drive at all.
// Older runtimes' per-process recipe files are read for their routes only with scan_legacy.
// Returns the number of files removed.
size_t prune_recording_metadata(const std::string& recipe_dir, const std::string& archive_root,
                                const std::vector<std::string>& keep, bool scan_legacy);

class SocketServer {
 public:
  explicit SocketServer(std::string socket_path = default_socket_path(),
                        std::string recipe_dir = default_recipe_dir());
  ~SocketServer();

  bool start();
  void stop();

  bool latest_frame(uint8_t service_index, uint64_t fresh_within_ms, LatestFrame* out) const;
  SocketStats stats() const;
  std::string recipe_file_path() const;

 private:
  friend struct RecordingMetadataTestAccess;
  struct RouteEpoch {
    std::string route;
    uint64_t generation = 0;
    bool damage_recorded = false; // Only the writer reads/writes this flag.
    uint64_t damage_attempts = 0;
    const char* damage_reason = nullptr;
    // Only the writer reads/writes the drive lifecycle flags. onroad_observed is set once this
    // process records onroad data for the route (an onroad snapshot or a recipe event). Only such
    // an epoch is finalized offroad, or damaged when its queued records are lost: a route this
    // process merely found in CurrentRoute (a restart offroad, or the previous boot's value)
    // recorded nothing here that could be incomplete.
    bool onroad_observed = false;
    bool finalized = false; // Offroad after onroad; later UI traffic is outside this drive.
  };
  struct RecipeRecord {
    std::string line;
    std::shared_ptr<RouteEpoch> epoch;
  };
  struct SnapshotRecord {
    LatestFrame frame;
    uint64_t sequence = 0;
    std::shared_ptr<RouteEpoch> epoch;
  };
  struct RecordingFault {
    const char* reason = nullptr;
    uint64_t count = 0;
    int error_number = 0;
    uint64_t bytes = 0;
    uint64_t limit = 0;
    uint8_t service_index = 0;
    uint64_t wall_ms = 0;
  };
  struct FaultCounter {
    const char* reason = nullptr;
    int error_number = 0;
    uint64_t count = 0;
  };
  void accept_loop();
  bool receive_one_frame(int client_fd);
  void mark_client_connected(bool connected);
  void offer_recipe_event(const LatestFrame& frame);
  void offer_snapshot(const LatestFrame& frame);
  // Called with recipe_mutex_ held; only the writer thread emits these messages.
  void offer_recording_fault(const char* reason, uint64_t count, int error_number,
                             uint64_t bytes, uint64_t limit, uint8_t service_index);
  void recipe_writer_loop();
  // Writer/startup only: parameter IO never runs in a frame callback. True for a new route.
  bool observe_route(const std::string& route);
  void mark_route_damage(const std::shared_ptr<RouteEpoch>& epoch, const char* reason);
  // Writer only: this process's recipe file for the route, opened on first use. Null with errno
  // set when it can't be opened.
  FILE* recipe_file_for(const std::string& route);
  // Writer/startup only.
  void prune_metadata(bool scan_legacy);

  std::string socket_path_;
  std::string recipe_dir_;
  // Recording metadata is written at all (the recipe directory is usable). Set before the
  // threads start and cleared after they stop.
  bool recipe_enabled_ = false;
  // "<start ms>-<pid>": names this process's recipe file for each route.
  std::string recipe_stamp_;
  std::string recipe_file_path_;
  // Writer only: the open recipe file and its route. Each route has its own file, so one
  // route's recipe never makes another's larger.
  FILE* recipe_file_ = nullptr;
  std::string recipe_file_route_;
  std::thread recipe_thread_;
  mutable std::mutex recipe_mutex_;
  std::condition_variable recipe_condition_;
  std::deque<RecipeRecord> recipe_queue_;
  std::deque<SnapshotRecord> snapshot_queue_;
  std::deque<RecordingFault> recording_faults_;
  std::array<FaultCounter, 32> recording_fault_counts_ = {};
  uint64_t recording_faults_suppressed_ = 0;
  uint64_t reported_faults_suppressed_ = 0;
  uint64_t route_generation_ = 0;
  std::shared_ptr<RouteEpoch> active_epoch_;
  bool metadata_damaged_ = false;
  uint64_t route_transition_discarded_ = 0;
  uint64_t snapshot_sequence_ = 0;
  std::array<uint64_t, kServiceCount> last_snapshot_offer_ms_ = {};
  FILE* snapshot_file_ = nullptr;
  bool snapshot_active_ = false;
  std::string snapshot_route_;
  uint64_t snapshot_bytes_ = 0;
  // Whether the comma is offroad, read at most once a second by the writer thread.
  bool offroad_ = false;
  std::chrono::steady_clock::time_point offroad_checked_at_{};
  std::string active_route_;
  std::string recipe_route_;
  bool recipe_stopping_ = false;
  uint64_t recipe_dropped_ = 0;
  uint64_t recipe_events_ = 0;
  uint64_t recipe_write_failures_ = 0;
  uint64_t recipe_missing_route_ = 0;
  uint64_t snapshot_events_ = 0;
  uint64_t snapshot_dropped_ = 0;
  uint64_t snapshot_write_failures_ = 0;
  uint64_t snapshot_missing_route_ = 0;
  std::atomic<bool> running_{false};
  int server_fd_ = -1;
  int client_fd_ = -1;
  mutable std::mutex client_mutex_;
  std::thread accept_thread_;

  mutable std::mutex mutex_;
  std::array<LatestFrame, kServiceCount> latest_ = {};
  SocketStats stats_ = {};
};

}  // namespace commaview::ui_export
