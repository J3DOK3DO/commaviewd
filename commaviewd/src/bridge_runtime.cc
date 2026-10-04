#include "framing.h"
#include "msgq_ring_reader.h"
#include "process_hygiene.h"
#include "socket.h"
#include "policy.h"
#include "router.h"
#include "telemetry_policy.h"
#include "runtime_debug_config.h"
#include "runtime_video_send_accounting.h"
#include "ui_export_socket.h"
#include "video_chunk_protocol.h"
#include "video_transport_policy.h"
/**
 * CommaView Unified Bridge (C++)
 *
 * Streams encoded camera video (HEVC, or H.264 in livestream mode) and raw UI-export telemetry.
 * The telemetry-only port always carries telemetry. The road/wide video ports also carry it unless
 * the client's control frame turns it off: telemetryOnVideo defaults to on for transportVersion 1
 * (legacy) clients and to off for transportVersion >= 2 clients.
 *
 * Ports:
 *   8200 road, 8201 wide, 8202 driver, 8203 telemetry
 *
 * Framing:
 *   [4-byte big-endian length][payload]
 *   payload[0] = 0x06 (video chunk): chunked timestamp/geometry/header-length envelope + bytes
 *   payload[0] = 0x03 (control inbound): [type][json bytes]
 *   payload[0] = 0x04 (meta-raw): [type][version][service_idx][raw_len_be32][raw_event]
 * The legacy 0x02 JSON meta frame is no longer sent.
 */
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <optional>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <linux/sockios.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <atomic>
#include <cctype>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <array>
#include <sstream>
#include <fstream>
#include <utility>

#include <capnp/serialize.h>
#include "cereal/gen/cpp/log.capnp.h"
#include "cereal/messaging/messaging.h"
using commaview::telemetry::ServicePolicy;
using commaview::telemetry::service_policy_samples;
using commaview::runtime_debug::LoadedRuntimeDebugConfig;
using commaview::runtime_debug::effective_runtime_debug_config;
using commaview::runtime_debug::policy_for_service;
using commaview::runtime_debug::render_config_json;
using commaview::runtime_debug::runtime_debug_effective_path;
using commaview::runtime_debug::runtime_debug_stats_path;
using commaview::runtime_debug::service_mode_to_string;
using commaview::telemetry::telemetry_policy_allows_emit;
using commaview::telemetry::telemetry_policy_fetches_latest;

static constexpr uint8_t MSG_CONTROL = 0x03;
static constexpr uint8_t MSG_META_RAW = 0x04;
// Retired capnp envelope; the app accepts only V5. Kept because raw_only_runtime_contract_test.sh pins it.
[[maybe_unused]] static constexpr uint8_t RAW_META_ENVELOPE_V4 = 0x04;
static constexpr uint8_t RAW_META_ENVELOPE_V5 = 0x05;
static_assert(commaview::video::MSG_VIDEO_CHUNK == 0x06,
              "bridge runtime must send chunked video payloads");

static constexpr int PORT_ROAD = 8200;
static constexpr int PORT_WIDE = 8201;
static constexpr int PORT_DRIVER = 8202;
static constexpr int PORT_TELEMETRY = 8203;
static constexpr uint64_t VIDEO_SEND_BUDGET_MICROS = 16000ULL;
static constexpr uint64_t TELEMETRY_PARTIAL_SEND_RECOVERY_BUDGET_MICROS = 250000ULL;
// Video senders are isolated from msgq ingestion and other camera clients. Once bytes from
// a framed chunk enter TCP, allow that sender to finish the chunk rather than corrupt framing.
// Its bounded queue continues accepting only fresh work and resumes on a keyframe after drops.
static constexpr uint64_t VIDEO_PARTIAL_SEND_RECOVERY_BUDGET_MICROS = 2000000ULL;
static constexpr size_t VIDEO_FRAME_QUEUE_CAPACITY = 3;
// A new client's stream starts at its first keyframe. encoderd sends one every 15-30 frames
// (0.75-1.5 s at 20 fps); if this many pass without one being recognized, the stream starts anyway.
static constexpr size_t VIDEO_START_MAX_SKIPPED_FRAMES = 60;
// encoderd sets V4L2_BUF_FLAG_KEYFRAME in EncodeIndex.flags on keyframes (loggerd starts each
// recording on it), alongside the IDR NAL unit the bridge also looks for.
static constexpr uint32_t ENCODE_IDX_KEYFRAME_FLAG = 0x00000008;

// Wire protocol: the index of each name is the service_idx byte in 0x04 meta-raw frames and the
// UI export socket service id. Keep the order identical to telemetry::kDefaultServicePolicies,
// the Python exporters' *_SERVICE_INDEX constants, and the app's TELEMETRY_SERVICE_TYPES.
static constexpr std::array<const char*, 20> kTelemetryServices = {
  "uiStateOnroad",
  "selfdriveState",
  "carState",
  "controlsState",
  "onroadEvents",
  "driverMonitoringState",
  "driverStateV2",
  "modelV2",
  "radarState",
  "liveCalibration",
  "carOutput",
  "carControl",
  "liveParameters",
  "longitudinalPlan",
  "carParams",
  "deviceState",
  "roadCameraState",
  "pandaStatesSummary",
  "onroadProjection",
  "wideRoadCameraState",
};
static constexpr int NUM_TELEM = static_cast<int>(kTelemetryServices.size());
static constexpr int TELEMETRY_EMIT_MS = 50;  // 20 Hz base poll for PASS and SAMPLE modes

static constexpr bool telemetry_service_tables_match() {
  if (kTelemetryServices.size() != commaview::telemetry::kDefaultServicePolicyCount) return false;
  for (size_t i = 0; i < kTelemetryServices.size(); ++i) {
    const char* a = kTelemetryServices[i];
    const char* b = commaview::telemetry::kDefaultServicePolicies[i].service;
    while (*a != '\0' && *a == *b) {
      ++a;
      ++b;
    }
    if (*a != *b) return false;
  }
  return true;
}
static_assert(telemetry_service_tables_match(),
              "kTelemetryServices must list telemetry::kDefaultServicePolicies in the same order");
static_assert(NUM_TELEM == commaview::ui_export::kServiceCount,
              "UI export socket service ids must cover kTelemetryServices");

#if defined(COMMAVIEW_CURRENT_VIDEO_SCHEMA)
static const char* VIDEO_SERVICES_PROD[] = {
  "narrowRoadEncodeData", "wideRoadEncodeData", "cabinEncodeData"
};
static const char* VIDEO_SERVICES_LIVESTREAM[] = {
  "livestreamNarrowRoadEncodeData", "livestreamWideRoadEncodeData", "livestreamCabinEncodeData"
};
#else
static const char* VIDEO_SERVICES_PROD[] = {
  "roadEncodeData", "wideRoadEncodeData", "driverEncodeData"
};
static const char* VIDEO_SERVICES_LIVESTREAM[] = {
  "livestreamRoadEncodeData", "livestreamWideRoadEncodeData", "livestreamDriverEncodeData"
};
#endif

static std::atomic<bool> g_running{true};
static bool g_livestream_video_enabled = false;

static std::atomic<int> g_active_road{0};
static std::atomic<int> g_active_wide{0};
static std::atomic<int> g_active_driver{0};
static std::unique_ptr<commaview::ui_export::SocketServer> g_ui_export_socket;

static std::atomic<int>& active_counter_for_port(int port) {
  if (port == PORT_ROAD) return g_active_road;
  if (port == PORT_WIDE) return g_active_wide;
  return g_active_driver;
}

static void telemetry_loop(int client_fd,
                           const char* stream_name,
                           std::atomic<bool>* disconnect_requested,
                           std::mutex* send_mutex,
                           std::atomic<bool>* telemetry_enabled);

// Video chunks and telemetry frames share one client socket; send_mutex serializes whole frames.
// The send deadline starts only after the lock is held.
static commaview::net::SendResult send_frame_locked(
    int fd,
    const uint8_t* payload,
    size_t payload_len,
    std::mutex* send_mutex,
    uint64_t partial_recovery_budget_micros = VIDEO_PARTIAL_SEND_RECOVERY_BUDGET_MICROS) {
  const auto send = [&] {
    return commaview::net::send_frame_with_partial_recovery(
        fd,
        payload,
        payload_len,
        commaview::net::SendDeadline::after_micros(VIDEO_SEND_BUDGET_MICROS),
        partial_recovery_budget_micros);
  };
  if (send_mutex == nullptr) return send();
  std::lock_guard<std::mutex> send_lock(*send_mutex);
  return send();
}

static commaview::net::SendResult send_meta_bytes_bounded(int fd,
                                                           const uint8_t* bytes,
                                                           size_t bytes_len,
                                                           uint8_t msg_type,
                                                           std::mutex* send_mutex) {
  commaview::net::SendResult empty_ok{};
  if (bytes == nullptr || bytes_len == 0) return empty_ok;
  std::vector<uint8_t> framed_payload(1 + bytes_len);
  framed_payload[0] = msg_type;
  memcpy(&framed_payload[1], bytes, bytes_len);
  return send_frame_locked(fd,
                           framed_payload.data(),
                           framed_payload.size(),
                           send_mutex,
                           TELEMETRY_PARTIAL_SEND_RECOVERY_BUDGET_MICROS);
}

static commaview::net::SendResult send_meta_raw_frame(int fd,
                                                       uint8_t envelope_version,
                                                       uint8_t service_index,
                                                       const uint8_t* raw_data,
                                                       size_t raw_size,
                                                       std::mutex* send_mutex) {
  commaview::net::SendResult empty_ok{};
  if (raw_data == nullptr || raw_size == 0) return empty_ok;
  const uint32_t raw_len = static_cast<uint32_t>(raw_size);
  std::vector<uint8_t> payload(1 + 1 + 4 + raw_len);
  payload[0] = envelope_version;
  payload[1] = service_index;
  commaview::net::put_be32(&payload[2], raw_len);
  memcpy(&payload[6], raw_data, raw_len);
  return send_meta_bytes_bounded(fd, payload.data(), payload.size(), MSG_META_RAW, send_mutex);
}

struct RuntimeServiceStats {
  uint64_t active_subscribers = 0;
  uint64_t receive_count = 0;
  uint64_t receive_bytes = 0;
  uint64_t drain_count = 0;
  uint64_t max_drained_burst = 0;
  uint64_t sampled_emit_count = 0;
  uint64_t sampled_emit_bytes = 0;
  uint64_t emitted_count = 0;
  uint64_t emitted_bytes = 0;
  uint64_t last_receive_ms = 0;
  uint64_t last_emit_ms = 0;
  uint64_t send_failure_count = 0;
  uint64_t max_send_stall_micros = 0;
  uint64_t send_backpressure_count = 0;
};

struct RuntimeLoopStats {
  uint64_t iterations = 0;
  uint64_t total_micros = 0;
  uint64_t max_micros = 0;
  uint64_t over_budget = 0;
};

struct RuntimePeerDisconnectStats {
  uint64_t count = 0;
  std::string stream = "";
  std::string phase = "";
  std::string status = "ok";
  int error = 0;
  std::string error_name = "none";
  uint64_t bytes_sent = 0;
  uint64_t elapsed_micros = 0;
  uint64_t at_ms = 0;
};

// Encoder queue reads (msgq_ring_reader.h), summed over finished client sessions plus the gaps of
// sessions still running.
struct RuntimeVideoRingStats {
  uint64_t sessions = 0;
  uint64_t messages = 0;
  uint64_t discontinuities = 0;
  uint64_t lapped = 0;
  uint64_t invalid = 0;
  uint64_t resets = 0;
  uint64_t remaps = 0;
  uint64_t fell_behind = 0;
};

struct RuntimeState {
  LoadedRuntimeDebugConfig loaded_config = {};
  LoadedRuntimeDebugConfig effective_config = {};
  std::vector<RuntimeServiceStats> services = std::vector<RuntimeServiceStats>(NUM_TELEM);
  RuntimeLoopStats telemetry_loop = {};
  RuntimeLoopStats video_loop = {};
  commaview::runtime::RuntimeVideoSendStats video_send = {};
  RuntimePeerDisconnectStats peer_disconnect = {};
  std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();
  RuntimeVideoRingStats video_ring = {};
  uint64_t reconnect_count = 0;
  uint64_t config_reloads = 0;
  uint64_t last_config_reload_ms = 0;
  std::string last_restart_reason = "startup";
  bool initialized = false;
};

static std::mutex g_runtime_state_mutex;
static RuntimeState g_runtime_state;
// Bumped (under g_runtime_state_mutex) whenever effective_config changes, so telemetry loops copy
// the config only when it changed.
static std::atomic<uint64_t> g_config_generation{0};

static uint64_t runtime_now_ms() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

static std::string runtime_json_escape(const std::string& in) {
  return commaview::runtime_debug::json_escape(in);
}

static uint64_t runtime_loop_avg_micros(const RuntimeLoopStats& loop) {
  return loop.iterations == 0 ? 0 : (loop.total_micros / loop.iterations);
}

static void note_runtime_loop(RuntimeLoopStats* loop, uint64_t elapsed_micros, int budget_ms) {
  if (loop == nullptr) return;
  loop->iterations += 1;
  loop->total_micros += elapsed_micros;
  loop->max_micros = std::max(loop->max_micros, elapsed_micros);
  const uint64_t budget_micros = budget_ms > 0 ? static_cast<uint64_t>(budget_ms) * 1000ULL : 0ULL;
  if (budget_micros > 0 && elapsed_micros > budget_micros) loop->over_budget += 1;
}

static void write_text_file_best_effort(const std::string& path, const std::string& body) {
  // Stats/config flushes can be requested by different runtime threads. Never
  // expose a truncated or interleaved JSON document to the watchdog.
  static std::mutex file_write_mutex;
  std::lock_guard<std::mutex> lock(file_write_mutex);
  const std::string temporary_path = path + ".tmp";
  {
    std::ofstream out(temporary_path, std::ios::trunc);
    if (!out) return;
    out << body;
    out.close();
    if (!out) {
      unlink(temporary_path.c_str());
      return;
    }
  }
  if (std::rename(temporary_path.c_str(), path.c_str()) != 0) {
    unlink(temporary_path.c_str());
  }
}

static void append_runtime_run_event(const std::string& event,
                                     const std::string& stream = "",
                                     const std::string& peer = "",
                                     const std::string& phase = "",
                                     const std::string& status = "") {
  std::ofstream out("/data/commaview/logs/runtime-run-events.jsonl", std::ios::app);
  if (!out) return;
  std::string restart_reason;
  {
    std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
    restart_reason = g_runtime_state.last_restart_reason;
  }
  out << "{"
      << "\"tsMs\":" << runtime_now_ms() << ","
      << "\"event\":\"" << runtime_json_escape(event) << "\","
      << "\"pid\":" << getpid() << ","
      << "\"mode\":\"bridge\","
      << "\"restartReason\":\"" << runtime_json_escape(restart_reason) << "\"";
  if (!stream.empty()) out << ",\"stream\":\"" << runtime_json_escape(stream) << "\"";
  if (!peer.empty()) out << ",\"peer\":\"" << runtime_json_escape(peer) << "\"";
  if (!phase.empty()) out << ",\"phase\":\"" << runtime_json_escape(phase) << "\"";
  if (!status.empty()) out << ",\"status\":\"" << runtime_json_escape(status) << "\"";
  out << "}\n";
}

static std::string build_runtime_stats_json_locked() {
  std::ostringstream out;
  const uint64_t uptime_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - g_runtime_state.started_at).count());
  out << "{";
  out << "\"uptimeMs\":" << uptime_ms << ",";
  out << "\"reconnectCount\":" << g_runtime_state.reconnect_count << ",";
  out << "\"configVersion\":" << g_runtime_state.effective_config.config_version << ",";
  out << "\"configHash\":\"" << runtime_json_escape(g_runtime_state.effective_config.config_hash) << "\",";
  out << "\"lastRestartReason\":\"" << runtime_json_escape(g_runtime_state.last_restart_reason) << "\",";
  out << "\"telemetryLoop\":{";
  out << "\"iterations\":" << g_runtime_state.telemetry_loop.iterations << ",";
  out << "\"avgMicros\":" << runtime_loop_avg_micros(g_runtime_state.telemetry_loop) << ",";
  out << "\"maxMicros\":" << g_runtime_state.telemetry_loop.max_micros << ",";
  out << "\"overBudget\":" << g_runtime_state.telemetry_loop.over_budget << "},";
  out << "\"videoLoop\":{";
  out << "\"iterations\":" << g_runtime_state.video_loop.iterations << ",";
  out << "\"avgMicros\":" << runtime_loop_avg_micros(g_runtime_state.video_loop) << ",";
  out << "\"maxMicros\":" << g_runtime_state.video_loop.max_micros << ",";
  out << "\"overBudget\":" << g_runtime_state.video_loop.over_budget << "},";
  out << "\"videoSend\":" << commaview::runtime::video_send_stats_json(g_runtime_state.video_send) << ",";
  out << "\"videoRing\":{"
      << "\"msgqReaderSlots\":0,"
      << "\"sessions\":" << g_runtime_state.video_ring.sessions << ","
      << "\"messages\":" << g_runtime_state.video_ring.messages << ","
      << "\"discontinuities\":" << g_runtime_state.video_ring.discontinuities << ","
      << "\"lapped\":" << g_runtime_state.video_ring.lapped << ","
      << "\"invalid\":" << g_runtime_state.video_ring.invalid << ","
      << "\"resets\":" << g_runtime_state.video_ring.resets << ","
      << "\"remaps\":" << g_runtime_state.video_ring.remaps << ","
      << "\"fellBehind\":" << g_runtime_state.video_ring.fell_behind << "},";
  out << "\"configReloads\":" << g_runtime_state.config_reloads << ",";
  out << "\"lastConfigReloadMs\":" << g_runtime_state.last_config_reload_ms << ",";
  out << "\"peerDisconnect\":{"
      << "\"count\":" << g_runtime_state.peer_disconnect.count << ","
      << "\"stream\":\"" << runtime_json_escape(g_runtime_state.peer_disconnect.stream) << "\","
      << "\"phase\":\"" << runtime_json_escape(g_runtime_state.peer_disconnect.phase) << "\","
      << "\"status\":\"" << runtime_json_escape(g_runtime_state.peer_disconnect.status) << "\","
      << "\"error\":" << g_runtime_state.peer_disconnect.error << ","
      << "\"errorName\":\"" << runtime_json_escape(g_runtime_state.peer_disconnect.error_name) << "\","
      << "\"bytesSent\":" << g_runtime_state.peer_disconnect.bytes_sent << ","
      << "\"elapsedMicros\":" << g_runtime_state.peer_disconnect.elapsed_micros << ","
      << "\"atMs\":" << g_runtime_state.peer_disconnect.at_ms << "},";
  out << "\"services\":{";
  for (int i = 0; i < NUM_TELEM; ++i) {
    if (i > 0) out << ",";
    const char* service_name = kTelemetryServices[static_cast<size_t>(i)];
    const ServicePolicy policy = policy_for_service(g_runtime_state.effective_config, service_name);
    const RuntimeServiceStats& stats = g_runtime_state.services[static_cast<size_t>(i)];
    out << "\"" << service_name << "\":{";
    out << "\"mode\":\"" << service_mode_to_string(policy.mode) << "\",";
    out << "\"subscribed\":" << (stats.active_subscribers > 0 ? "true" : "false") << ",";
    out << "\"sampleHz\":" << policy.sample_hz << ",";
    out << "\"receiveCount\":" << stats.receive_count << ",";
    out << "\"receiveBytes\":" << stats.receive_bytes << ",";
    out << "\"drainCount\":" << stats.drain_count << ",";
    out << "\"maxDrainedBurst\":" << stats.max_drained_burst << ",";
    out << "\"sampledEmitCount\":" << stats.sampled_emit_count << ",";
    out << "\"sampledEmitBytes\":" << stats.sampled_emit_bytes << ",";
    out << "\"emittedCount\":" << stats.emitted_count << ",";
    out << "\"emittedBytes\":" << stats.emitted_bytes << ",";
    out << "\"lastReceiveMs\":" << stats.last_receive_ms << ",";
    out << "\"lastEmitMs\":" << stats.last_emit_ms << ",";
    out << "\"sendFailureCount\":" << stats.send_failure_count << ",";
    out << "\"maxSendStallMicros\":" << stats.max_send_stall_micros << ",";
    out << "\"sendBackpressureCount\":" << stats.send_backpressure_count;
    out << "}";
  }
  out << "}";
  if (g_ui_export_socket != nullptr) {
    const auto socket_stats = g_ui_export_socket->stats();
    out << ",\"uiExportSocket\":{";
    out << "\"running\":" << (socket_stats.running ? "true" : "false") << ",";
    out << "\"connected\":" << (socket_stats.connected ? "true" : "false") << ",";
    out << "\"connectCount\":" << socket_stats.connect_count << ",";
    out << "\"acceptedCount\":" << socket_stats.accepted_count << ",";
    out << "\"malformedCount\":" << socket_stats.malformed_count << ",";
    out << "\"lastReceiveMs\":" << socket_stats.last_receive_ms << ",";
    out << "\"recipeActive\":" << (socket_stats.recipe_active ? "true" : "false") << ",";
    out << "\"recipeRoute\":\"" << socket_stats.recipe_route << "\",";
    out << "\"recipeEvents\":" << socket_stats.recipe_events << ",";
    out << "\"recipeDropped\":" << socket_stats.recipe_dropped << ",";
    out << "\"recipeMissingRoute\":" << socket_stats.recipe_missing_route << ",";
    out << "\"recipeWriteFailures\":" << socket_stats.recipe_write_failures;
    out << ",\"snapshotActive\":" << (socket_stats.snapshot_active ? "true" : "false");
    out << ",\"snapshotRoute\":\"" << socket_stats.snapshot_route << "\"";
    out << ",\"snapshotEvents\":" << socket_stats.snapshot_events;
    out << ",\"snapshotDropped\":" << socket_stats.snapshot_dropped;
    out << ",\"snapshotWriteFailures\":" << socket_stats.snapshot_write_failures;
    out << ",\"snapshotMissingRoute\":" << socket_stats.snapshot_missing_route;
    out << "}";
  }
  out << "}";
  return out.str();
}

struct RuntimeRenderedState {
  std::string effective_json;
  std::string stats_json;
};

static RuntimeRenderedState render_runtime_state_locked() {
  return RuntimeRenderedState{
      render_config_json(g_runtime_state.effective_config, true),
      build_runtime_stats_json_locked(),
  };
}

static void flush_runtime_state() {
  RuntimeRenderedState rendered;
  {
    std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
    rendered = render_runtime_state_locked();
  }
  write_text_file_best_effort(runtime_debug_effective_path(), rendered.effective_json);
  write_text_file_best_effort(runtime_debug_stats_path(), rendered.stats_json);
}

static void initialize_runtime_state_once() {
  {
    std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
    if (g_runtime_state.initialized) return;
    g_runtime_state.loaded_config = commaview::runtime_debug::load_runtime_debug_config();
    g_runtime_state.effective_config = effective_runtime_debug_config(g_runtime_state.loaded_config);
    g_runtime_state.started_at = std::chrono::steady_clock::now();
    const char* restart_reason = std::getenv("COMMAVIEWD_RESTART_REASON");
    if (restart_reason != nullptr && restart_reason[0] != '\0') {
      g_runtime_state.last_restart_reason = restart_reason;
    }
    g_runtime_state.initialized = true;
  }
  flush_runtime_state();
}

// Re-reads the runtime debug config in place (SIGHUP, sent by control mode's runtime-debug apply).
// Every setting in it is a per-service telemetry policy, read by each telemetry loop on its next
// pass, so nothing needs a restart.
static void reload_runtime_config(const char* reason) {
  LoadedRuntimeDebugConfig loaded = commaview::runtime_debug::load_runtime_debug_config();
  LoadedRuntimeDebugConfig effective = effective_runtime_debug_config(loaded);
  std::string hash;
  {
    std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
    g_runtime_state.loaded_config = std::move(loaded);
    g_runtime_state.effective_config = std::move(effective);
    g_runtime_state.config_reloads += 1;
    g_runtime_state.last_config_reload_ms = runtime_now_ms();
    g_config_generation.fetch_add(1);
    hash = g_runtime_state.effective_config.config_hash;
  }
  flush_runtime_state();
  append_runtime_run_event("config_reload", "", "", reason == nullptr ? "" : reason, "ok");
  printf("runtime debug config reloaded in place (%s) hash=%s\n", reason == nullptr ? "" : reason, hash.c_str());
  fflush(stdout);
}

// The telemetry loops' copy of the effective config, refreshed when it changed.
static void refresh_config_snapshot(LoadedRuntimeDebugConfig* config, uint64_t* generation) {
  const uint64_t current = g_config_generation.load();
  if (current == *generation) return;
  std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
  *config = g_runtime_state.effective_config;
  *generation = g_config_generation.load();
}

// SIGHUP is blocked in every thread (see commaview_bridge_main) and taken here, so a reload runs
// on an ordinary thread instead of in a signal handler, and costs nothing until one is asked for.
static void config_reload_signal_loop() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGHUP);
  while (g_running) {
    int sig = 0;
    if (sigwait(&set, &sig) != 0) continue;
    if (sig == SIGHUP && g_running) reload_runtime_config("sighup");
  }
}

static void note_video_ring_discontinuity() {
  std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
  g_runtime_state.video_ring.discontinuities += 1;
}

static void note_video_ring_session(const commaview::ipc::StreamReaderStats& stats) {
  {
    std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
    auto& ring = g_runtime_state.video_ring;
    ring.sessions += 1;
    ring.messages += stats.messages;
    ring.lapped += stats.lapped;
    ring.invalid += stats.invalid;
    ring.resets += stats.resets;
    ring.remaps += stats.remaps;
    ring.fell_behind += stats.fell_behind;
  }
  flush_runtime_state();
}

static void note_runtime_connect() {
  {
    std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
    g_runtime_state.reconnect_count += 1;
  }
  flush_runtime_state();
}

static constexpr uint64_t kTelemetryEmitBackpressureThresholdMicros = 5000ULL;

static void note_runtime_emit(int idx, size_t bytes, bool sampled, bool ok, uint64_t stall_micros) {
  if (idx < 0 || idx >= NUM_TELEM) return;
  std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
  RuntimeServiceStats& stats = g_runtime_state.services[static_cast<size_t>(idx)];
  stats.max_send_stall_micros = std::max(stats.max_send_stall_micros, stall_micros);
  if (stall_micros > kTelemetryEmitBackpressureThresholdMicros) {
    stats.send_backpressure_count += 1;
  }
  if (!ok) {
    stats.send_failure_count += 1;
    return;
  }
  stats.emitted_count += 1;
  stats.emitted_bytes += static_cast<uint64_t>(bytes);
  stats.last_emit_ms = runtime_now_ms();
  if (sampled) {
    stats.sampled_emit_count += 1;
    stats.sampled_emit_bytes += static_cast<uint64_t>(bytes);
  }
}

static void note_runtime_loop_sample(bool telemetry_loop, uint64_t elapsed_micros) {
  std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
  if (telemetry_loop) {
    note_runtime_loop(&g_runtime_state.telemetry_loop, elapsed_micros, TELEMETRY_EMIT_MS);
  } else {
    note_runtime_loop(&g_runtime_state.video_loop, elapsed_micros, 20);
  }
}

static void note_video_queue_deltas(uint64_t queue_drop_delta,
                                    uint64_t keyframe_wait_drop_delta,
                                    size_t queue_high_watermark) {
  if (queue_drop_delta == 0 && keyframe_wait_drop_delta == 0 && queue_high_watermark == 0) return;
  std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
  g_runtime_state.video_send.queue_drop_count += queue_drop_delta;
  g_runtime_state.video_send.keyframe_wait_drop_count += keyframe_wait_drop_delta;
  g_runtime_state.video_send.queue_high_watermark = std::max<uint64_t>(
      g_runtime_state.video_send.queue_high_watermark,
      static_cast<uint64_t>(queue_high_watermark));
}

static void note_video_zero_byte_backpressure_recovered() {
  std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
  g_runtime_state.video_send.zero_byte_backpressure_recovered_count += 1;
}

static void note_video_queued_frame_age(uint64_t age_ms) {
  std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
  g_runtime_state.video_send.max_queued_frame_age_ms = std::max(
      g_runtime_state.video_send.max_queued_frame_age_ms,
      age_ms);
}

static void note_video_chunk_send_result(const commaview::video::VideoChunk& chunk,
                                         const commaview::net::SendResult& result) {
  std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
  commaview::runtime::note_video_chunk_send_result(
      g_runtime_state.video_send,
      chunk,
      result,
      runtime_now_ms());
}

// Snapshot at the first sign of delivery pressure, while the socket is still
// open. Call sites bound this to a few samples per client, not one per frame.
static void note_video_tcp_pressure_snapshot(int fd, const char* stream, const char* reason) {
  struct tcp_info info {};
  socklen_t info_len = sizeof(info);
  const bool valid = getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &info_len) == 0;
  int send_queue_bytes = -1;
  int not_sent_bytes = -1;
  if (ioctl(fd, SIOCOUTQ, &send_queue_bytes) != 0) send_queue_bytes = -1;
  if (ioctl(fd, SIOCOUTQNSD, &not_sent_bytes) != 0) not_sent_bytes = -1;
  {
    std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
    auto& stats = g_runtime_state.video_send;
    stats.last_tcp_snapshot_valid = valid;
    stats.last_tcp_stream = stream == nullptr ? "" : stream;
    stats.last_tcp_reason = reason == nullptr ? "" : reason;
    stats.last_tcp_snapshot_at_ms = runtime_now_ms();
    stats.last_tcp_rtt_micros = valid ? info.tcpi_rtt : 0;
    stats.last_tcp_total_retrans = valid ? info.tcpi_total_retrans : 0;
    stats.last_tcp_unacked = valid ? info.tcpi_unacked : 0;
    stats.last_tcp_cwnd = valid ? info.tcpi_snd_cwnd : 0;
    stats.last_tcp_send_queue_bytes = send_queue_bytes;
    stats.last_tcp_not_sent_bytes = not_sent_bytes;
  }
  printf("[%s] tcp_pressure reason=%s rtt_us=%u retrans=%u unacked=%u cwnd=%u sendq=%d notsent=%d\n",
         stream == nullptr ? "unknown" : stream,
         reason == nullptr ? "unknown" : reason,
         valid ? info.tcpi_rtt : 0,
         valid ? info.tcpi_total_retrans : 0,
         valid ? info.tcpi_unacked : 0,
         valid ? info.tcpi_snd_cwnd : 0,
         send_queue_bytes,
         not_sent_bytes);
  fflush(stdout);
  flush_runtime_state();
}

static void note_video_frame_abandoned(const char* stream, uint64_t sequence, uint16_t chunk_index) {
  {
    std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
    g_runtime_state.video_send.frame_abandon_count += 1;
  }
  printf("[%s] abandoned video frame seq=%llu at chunk=%u\n",
         stream == nullptr ? "unknown" : stream,
         static_cast<unsigned long long>(sequence),
         static_cast<unsigned int>(chunk_index));
  fflush(stdout);
}

static uint32_t chunk_wire_sequence(uint64_t queue_sequence) {
  // Chunk protocol carries a 32-bit frame sequence; the long-running queue sequence
  // intentionally wraps modulo 2^32 on the wire.
  return static_cast<uint32_t>(queue_sequence);
}

static commaview::net::SendResult peer_closed_result() {
  commaview::net::SendResult result{};
  result.status = commaview::net::SendStatus::Disconnected;
  return result;
}

static void note_runtime_peer_disconnect(const char* stream,
                                         const char* phase,
                                         const commaview::net::SendResult& result) {
  {
    std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
    g_runtime_state.peer_disconnect.count += 1;
    g_runtime_state.peer_disconnect.stream = stream == nullptr ? "" : stream;
    g_runtime_state.peer_disconnect.phase = phase == nullptr ? "" : phase;
    g_runtime_state.peer_disconnect.status = commaview::net::send_status_name(result.status);
    g_runtime_state.peer_disconnect.error = result.error;
    g_runtime_state.peer_disconnect.error_name = commaview::net::send_error_name(result.error);
    g_runtime_state.peer_disconnect.bytes_sent = static_cast<uint64_t>(result.bytes_sent);
    g_runtime_state.peer_disconnect.elapsed_micros = result.elapsed_micros;
    g_runtime_state.peer_disconnect.at_ms = runtime_now_ms();
  }
  append_runtime_run_event("peer_disconnect",
                           stream == nullptr ? "unknown" : stream,
                           "",
                           phase == nullptr ? "unknown" : phase,
                           commaview::net::send_status_name(result.status));
  printf("[%s] peer disconnect phase=%s status=%s errno=%s(%d) bytes=%zu elapsed_us=%llu\n",
         stream == nullptr ? "unknown" : stream,
         phase == nullptr ? "unknown" : phase,
         commaview::net::send_status_name(result.status),
         commaview::net::send_error_name(result.error).c_str(),
         result.error,
         result.bytes_sent,
         static_cast<unsigned long long>(result.elapsed_micros));
  fflush(stdout);
  flush_runtime_state();
}

using ClientControlState = commaview::control::ClientControlState;

static void consume_client_control_frames(int client_fd,
                                          ClientControlState* state,
                                          const char* video_service) {
  commaview::control::consume_client_control_frames(client_fd, state, video_service, MSG_CONTROL);
}

static void handle_video_client(int client_fd, const char* video_service, int port) {
  char addr_str[64] = {};
  struct sockaddr_in peer = {};
  socklen_t peer_len = sizeof(peer);
  if (getpeername(client_fd, reinterpret_cast<struct sockaddr*>(&peer), &peer_len) == 0) {
    inet_ntop(AF_INET, &peer.sin_addr, addr_str, sizeof(addr_str));
  }

  append_runtime_run_event("client_connected", video_service, addr_str);
  printf("[%s] client connected: %s (fd=%d)\n", video_service, addr_str, client_fd);
  fflush(stdout);

  int opt = 1;
  setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
  int sndbuf = 524288;
  setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

  auto& active_counter = active_counter_for_port(port);
  int previous = active_counter.fetch_add(1);
  if (previous > 0) {
    active_counter.fetch_sub(1);
    close(client_fd);
    return;
  }

  // Reads the encoder queue without a msgq reader slot (msgq_ring_reader.h): any number of client
  // reconnects and bridge restarts leave openpilot's queue, and loggerd's place in it, untouched.
  // The session starts at the newest message.
  commaview::ipc::QueueStreamReader video_reader(commaview::ipc::msgq_queue_path(video_service));
  std::vector<uint8_t> raw_msg;
  uint64_t ring_discontinuities = 0;

  note_runtime_connect();

  const bool include_telemetry = (port == PORT_ROAD || port == PORT_WIDE);

  std::atomic<bool> telemetry_disconnect_requested{false};
  std::atomic<bool> telemetry_enabled_for_client{include_telemetry};
  std::thread telemetry_thread;
  std::mutex send_mutex;
  if (include_telemetry) {
    telemetry_thread = std::thread(telemetry_loop,
                                   client_fd,
                                   video_service,
                                   &telemetry_disconnect_requested,
                                   &send_mutex,
                                   &telemetry_enabled_for_client);
  }

  uint64_t frame_count = 0;
  uint64_t parsed_frame_count = 0;
  uint64_t parse_error_count = 0;
  uint64_t wrong_union_count = 0;
  uint64_t suppressed_video_count = 0;
  commaview::video::VideoFrameQueue video_queue(VIDEO_FRAME_QUEUE_CAPACITY);
  commaview::video::KeyframeStartGate start_gate(VIDEO_START_MAX_SKIPPED_FRAMES);
  std::mutex video_queue_mutex;
  std::condition_variable video_queue_cv;
  std::atomic<bool> video_sender_disconnect_requested{false};
  std::atomic<bool> queue_drop_tcp_snapshot_taken{false};
  std::atomic<bool> slow_send_tcp_snapshot_taken{false};
  std::atomic<bool> zero_byte_tcp_snapshot_taken{false};
  uint64_t last_queue_drop_count = 0;
  uint64_t last_keyframe_wait_drop_count = 0;
  auto t0 = std::chrono::steady_clock::now();
  AlignedBuffer aligned_buf;
  ClientControlState control_state;

  // Keep msgq ingestion independent from socket backpressure. Each connected camera owns
  // this bounded queue and sender thread, so a slow cabin client cannot stop road/wide
  // subscribers from consuming their latest conflated encoder frames.
  std::thread video_sender_thread([&] {
    while (g_running && !video_sender_disconnect_requested.load()) {
      std::optional<commaview::video::PendingVideoFrame> queued;
      {
        std::unique_lock<std::mutex> queue_lock(video_queue_mutex);
        video_queue_cv.wait_for(queue_lock, std::chrono::milliseconds(50), [&] {
          return video_sender_disconnect_requested.load() || video_queue.size() > 0;
        });
        if (video_sender_disconnect_requested.load()) break;
        queued = video_queue.pop_next();
        note_video_queue_deltas(
            video_queue.drop_count() - last_queue_drop_count,
            video_queue.keyframe_wait_drop_count() - last_keyframe_wait_drop_count,
            video_queue.high_watermark());
        last_queue_drop_count = video_queue.drop_count();
        last_keyframe_wait_drop_count = video_queue.keyframe_wait_drop_count();
      }
      if (!queued.has_value()) continue;

      note_video_queued_frame_age(runtime_now_ms() - queued->created_at_ms);
      const uint32_t queued_header_len = static_cast<uint32_t>(queued->codec_header.size());
      commaview::video::VideoFrameForChunking frame;
      frame.sequence = chunk_wire_sequence(queued->sequence);
      frame.timestamp_ns = queued->timestamp_ns;
      frame.width = queued->width;
      frame.height = queued->height;
      frame.is_keyframe = queued->is_keyframe;
      frame.codec_header = queued->codec_header;
      frame.data = queued->data;

      const auto chunks = commaview::video::plan_video_chunks(
          frame,
          commaview::video::DEFAULT_VIDEO_CHUNK_BYTES);

      bool frame_sent_successfully = true;
      for (const auto& chunk : chunks) {
        const auto payload = commaview::video::encode_video_chunk_payload(chunk);
        const auto send_result = send_frame_locked(client_fd, payload.data(), payload.size(), &send_mutex);
        note_video_chunk_send_result(chunk, send_result);
        if (send_result.status == commaview::net::SendStatus::Ok &&
            send_result.elapsed_micros >= 100000 &&
            !slow_send_tcp_snapshot_taken.exchange(true)) {
          note_video_tcp_pressure_snapshot(client_fd, video_service, "slow_send");
        }
        if (send_result.status == commaview::net::SendStatus::Backpressure &&
            send_result.bytes_sent == 0 &&
            !zero_byte_tcp_snapshot_taken.exchange(true)) {
          note_video_tcp_pressure_snapshot(client_fd, video_service, "zero_byte_backpressure");
        }
        if (send_result.status == commaview::net::SendStatus::Backpressure && send_result.bytes_sent == 0) {
          {
            std::lock_guard<std::mutex> queue_lock(video_queue_mutex);
            video_queue.note_backpressure_without_partial_send();
          }
          note_video_zero_byte_backpressure_recovered();
          note_video_frame_abandoned(video_service, queued->sequence, chunk.chunk_index);
          frame_sent_successfully = false;
          break;
        }
        if (send_result.status != commaview::net::SendStatus::Ok) {
          if (send_result.bytes_sent > 0) {
            note_video_tcp_pressure_snapshot(client_fd, video_service, "partial_send_failure");
          }
          note_runtime_peer_disconnect(video_service, "video_chunk_send", send_result);
          video_sender_disconnect_requested.store(true);
          telemetry_disconnect_requested.store(true);
          shutdown(client_fd, SHUT_RDWR);
          frame_sent_successfully = false;
          break;
        }
      }

      if (!frame_sent_successfully) continue;

      frame_count++;
      if (frame_count <= 5 || (frame_count % 200) == 0) {
        auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - t0).count();
        const double fps = elapsed > 0 ? frame_count / elapsed : 0.0;
        printf("[%s] frames=%llu fps=%.1f header=%u data=%zu key=%d\n",
               video_service,
               static_cast<unsigned long long>(frame_count),
               fps,
               queued_header_len,
               queued->data.size(),
               queued->is_keyframe ? 1 : 0);
        fflush(stdout);
      }
    }
  });

  // Parses one encoder message from the port's reader and queues its frame for the sender thread.
  const auto ingest_video_message = [&](const std::vector<uint8_t>& raw_msg) {
    const size_t raw_size = raw_msg.size();

    try {
      capnp::ReaderOptions options;
      options.traversalLimitInWords = kj::maxValue;

      capnp::FlatArrayMessageReader reader(
          aligned_buf.align(reinterpret_cast<const char*>(raw_msg.data()), raw_size), options);
      auto event = reader.getRoot<cereal::Event>();

      const auto which = event.which();
      const auto expected = commaview::video::expected_video_which_for_port(port, g_livestream_video_enabled);
      if (which != expected) {
        wrong_union_count++;
        if (wrong_union_count <= 20 || (wrong_union_count % 100) == 0) {
          printf("[%s] union mismatch #%llu: got=%d expected=%d raw=%zu\n",
                 video_service,
                 static_cast<unsigned long long>(wrong_union_count),
                 static_cast<int>(which),
                 static_cast<int>(expected),
                 raw_size);
          fflush(stdout);
        }
        return;
      }

      auto ed = commaview::video::read_encode_data(event, port, g_livestream_video_enabled);
      auto header = ed.getHeader();
      auto data = ed.getData();

      const uint32_t header_len = header.size();
      const size_t data_len = data.size();
      const uint64_t timestamp_ns = ed.getUnixTimestampNanos();
      const uint32_t video_width = ed.getWidth();
      const uint32_t video_height = ed.getHeight();
      if (data_len == 0) return;

      bool suppress_video = false;
      bool session_policy = false;
      if (commaview::control::get_session_policy(control_state.bound_session_id, &session_policy)) {
        suppress_video = session_policy;
      }

      if (suppress_video) {
        suppressed_video_count++;
        if (suppressed_video_count <= 3 || (suppressed_video_count % 500) == 0) {
          printf("[%s] suppress-video drop=%llu session=%s header=%u data=%zu\n",
                 video_service,
                 static_cast<unsigned long long>(suppressed_video_count),
                 control_state.bound_session_id.empty() ? "<legacy>" : control_state.bound_session_id.c_str(),
                 header_len,
                 data_len);
          fflush(stdout);
        }
        return;
      }

      const auto contains_idr = g_livestream_video_enabled
          ? commaview::video::contains_h264_idr
          : commaview::video::contains_hevc_idr;
      const bool is_keyframe =
          (ed.getIdx().getFlags() & ENCODE_IDX_KEYFRAME_FLAG) != 0 ||
          contains_idr(header.begin(), header_len) ||
          contains_idr(data.begin(), data_len);
      const bool stream_was_started = start_gate.started();
      if (!start_gate.admit(is_keyframe)) return;
      if (!stream_was_started) {
        printf("[%s] stream starts on a %s after skipping %llu frames\n",
               video_service,
               is_keyframe ? "keyframe" : "non-keyframe (no keyframe recognized)",
               static_cast<unsigned long long>(start_gate.skipped()));
        fflush(stdout);
      }
      commaview::video::PendingVideoFrame pending;
      pending.sequence = ++parsed_frame_count;
      pending.is_keyframe = is_keyframe;
      pending.created_at_ms = runtime_now_ms();
      pending.timestamp_ns = timestamp_ns;
      pending.width = video_width;
      pending.height = video_height;
      pending.codec_header.assign(header.begin(), header.begin() + header_len);
      pending.data.assign(data.begin(), data.begin() + data_len);
      uint64_t queue_drop_delta = 0;
      {
        std::lock_guard<std::mutex> queue_lock(video_queue_mutex);
        video_queue.push(std::move(pending));
        queue_drop_delta = video_queue.drop_count() - last_queue_drop_count;
        note_video_queue_deltas(
            queue_drop_delta,
            video_queue.keyframe_wait_drop_count() - last_keyframe_wait_drop_count,
            video_queue.high_watermark());
        last_queue_drop_count = video_queue.drop_count();
        last_keyframe_wait_drop_count = video_queue.keyframe_wait_drop_count();
      }
      if (queue_drop_delta > 0 && !queue_drop_tcp_snapshot_taken.exchange(true)) {
        note_video_tcp_pressure_snapshot(client_fd, video_service, "queue_drop");
      }
      video_queue_cv.notify_one();
    } catch (const std::exception& e) {
      parse_error_count++;
      printf("[%s] parse exception #%llu: %s (raw=%zu)\n",
             video_service,
             static_cast<unsigned long long>(parse_error_count),
             e.what(),
             raw_size);
      fflush(stdout);
    } catch (...) {
      parse_error_count++;
      printf("[%s] parse unknown exception #%llu (raw=%zu)\n",
             video_service,
             static_cast<unsigned long long>(parse_error_count),
             raw_size);
      fflush(stdout);
    }
  };

  while (g_running) {
    const auto loop_started = std::chrono::steady_clock::now();
    if (!commaview::net::client_socket_alive(client_fd)) {
      note_runtime_peer_disconnect(video_service, "client_socket_alive", peer_closed_result());
      break;
    }

    consume_client_control_frames(client_fd, &control_state, video_service);
    if (include_telemetry) {
      telemetry_enabled_for_client.store(control_state.telemetry_on_video);
    }

    if (video_sender_disconnect_requested.load() ||
        (include_telemetry && telemetry_disconnect_requested.load())) break;

    const int video_poll_ms = 20;
    bool discontinuity = false;
    if (video_reader.next(video_poll_ms, &raw_msg, &discontinuity)) {
      if (discontinuity) {
        // The encoder overtook us (or the queue started over): frames are missing, so the stream
        // resumes on the next keyframe, as a new client's does.
        ring_discontinuities += 1;
        start_gate = commaview::video::KeyframeStartGate(VIDEO_START_MAX_SKIPPED_FRAMES);
        note_video_ring_discontinuity();
        if (ring_discontinuities <= 5 || (ring_discontinuities % 100) == 0) {
          printf("[%s] encoder queue gap #%llu: waiting for the next keyframe\n",
                 video_service,
                 static_cast<unsigned long long>(ring_discontinuities));
          fflush(stdout);
        }
      }
      ingest_video_message(raw_msg);
    }

    const auto video_loop_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - loop_started);
    note_runtime_loop_sample(false, static_cast<uint64_t>(video_loop_elapsed.count()));

    if (video_sender_disconnect_requested.load() ||
        (include_telemetry && telemetry_disconnect_requested.load())) {
      break;
    }
  }

  append_runtime_run_event("client_disconnected", video_service, addr_str);
  printf("[%s] client disconnected: %s\n", video_service, addr_str);
  fflush(stdout);

  note_video_ring_session(video_reader.stats());
  telemetry_disconnect_requested.store(true);
  video_sender_disconnect_requested.store(true);
  video_queue_cv.notify_all();
  shutdown(client_fd, SHUT_RDWR);
  if (video_sender_thread.joinable()) video_sender_thread.join();
  if (telemetry_thread.joinable()) telemetry_thread.join();
  active_counter.fetch_sub(1);
  close(client_fd);
}

static void telemetry_loop(int client_fd,
                           const char* stream_name,
                           std::atomic<bool>* disconnect_requested,
                           std::mutex* send_mutex,
                           std::atomic<bool>* telemetry_enabled) {
  if (disconnect_requested == nullptr) return;
  uint64_t telem_raw_count = 0;
  auto next_telem_poll = std::chrono::steady_clock::now();
  auto next_stats_flush = next_telem_poll + std::chrono::milliseconds(1000);
  std::array<uint64_t, static_cast<size_t>(NUM_TELEM)> last_ui_emit_ms = {};
  std::array<uint64_t, static_cast<size_t>(NUM_TELEM)> last_ui_emit_wall_ms = {};
  LoadedRuntimeDebugConfig config;
  uint64_t config_generation = ~uint64_t{0};

  while (g_running && !disconnect_requested->load()) {
    if (!commaview::net::client_socket_alive(client_fd)) {
      note_runtime_peer_disconnect(stream_name, "telemetry_socket_alive", peer_closed_result());
      disconnect_requested->store(true);
      break;
    }

    if (telemetry_enabled != nullptr && !telemetry_enabled->load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(TELEMETRY_EMIT_MS));
      continue;
    }

    const auto telemetry_started = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (now < next_telem_poll) {
      const auto sleep_for_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(next_telem_poll - now).count();
      if (sleep_for_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_for_ms));
      }
      now = std::chrono::steady_clock::now();
    }

    do {
      next_telem_poll += std::chrono::milliseconds(TELEMETRY_EMIT_MS);
    } while (next_telem_poll <= now);

    refresh_config_snapshot(&config, &config_generation);
    if (g_ui_export_socket != nullptr) {
      for (int i = 0; i < NUM_TELEM; ++i) {
        const char* service_name = kTelemetryServices[static_cast<size_t>(i)];
        const ServicePolicy policy = policy_for_service(config, service_name);
        if (!telemetry_policy_fetches_latest(policy)) {
          continue;
        }

        commaview::ui_export::LatestFrame frame;
        const uint64_t fresh_within_ms = commaview::telemetry::telemetry_service_is_latched(service_name)
            ? 0
            : commaview::ui_export::kFreshFrameWindowMs;
        if (!g_ui_export_socket->latest_frame(static_cast<uint8_t>(i), fresh_within_ms, &frame)) {
          continue;
        }
        const uint64_t now_ms = runtime_now_ms();
        if (!telemetry_policy_allows_emit(policy,
                                          frame.updated_at_ms,
                                          now_ms,
                                          last_ui_emit_ms[static_cast<size_t>(i)],
                                          last_ui_emit_wall_ms[static_cast<size_t>(i)])) {
          continue;
        }

        const auto send_started = std::chrono::steady_clock::now();
        auto send_result = send_meta_raw_frame(client_fd,
                                               RAW_META_ENVELOPE_V5,
                                               static_cast<uint8_t>(i),
                                               frame.payload.data(),
                                               frame.payload.size(),
                                               send_mutex);
        const auto send_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - send_started);
        send_result.elapsed_micros = std::max(send_result.elapsed_micros,
                                              static_cast<uint64_t>(send_elapsed.count()));
        const bool sampled = service_policy_samples(policy);
        const bool sent = send_result.status == commaview::net::SendStatus::Ok;
        note_runtime_emit(i,
                          frame.payload.size(),
                          sampled,
                          sent,
                          send_result.elapsed_micros);
        if (!sent) {
          if (commaview::telemetry::telemetry_send_failure_is_droppable(send_result)) {
            append_runtime_run_event("telemetry_drop",
                                     stream_name,
                                     "",
                                     "telemetry_send",
                                     commaview::net::send_status_name(send_result.status));
            last_ui_emit_ms[static_cast<size_t>(i)] = frame.updated_at_ms;
            last_ui_emit_wall_ms[static_cast<size_t>(i)] = now_ms;
            continue;
          }
          note_runtime_peer_disconnect(stream_name, "telemetry_send", send_result);
          disconnect_requested->store(true);
          shutdown(client_fd, SHUT_RDWR);
          break;
        }
        last_ui_emit_ms[static_cast<size_t>(i)] = frame.updated_at_ms;
        last_ui_emit_wall_ms[static_cast<size_t>(i)] = now_ms;
        telem_raw_count++;
      }
    }

    if (disconnect_requested->load()) break;

    if (telem_raw_count > 0 && (telem_raw_count <= 3 || (telem_raw_count % 200) == 0)) {
      printf("[%s] telem_raw=%llu [DIRECT_UI_SOCKET_ONLY] (read+send throttled %dms)\n",
             stream_name,
             static_cast<unsigned long long>(telem_raw_count),
             TELEMETRY_EMIT_MS);
      fflush(stdout);
    }

    now = std::chrono::steady_clock::now();
    if (now >= next_stats_flush) {
      next_stats_flush = now + std::chrono::milliseconds(1000);
      flush_runtime_state();
    }

    const auto telemetry_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - telemetry_started);
    note_runtime_loop_sample(true, static_cast<uint64_t>(telemetry_elapsed.count()));
  }
}

static void handle_telemetry_client(int client_fd) {
  const char* stream_name = "telemetry";
  char addr_str[64] = {};
  struct sockaddr_in peer = {};
  socklen_t peer_len = sizeof(peer);
  if (getpeername(client_fd, reinterpret_cast<struct sockaddr*>(&peer), &peer_len) == 0) {
    inet_ntop(AF_INET, &peer.sin_addr, addr_str, sizeof(addr_str));
  }

  append_runtime_run_event("client_connected", stream_name, addr_str);
  printf("[%s] client connected: %s (fd=%d)\n", stream_name, addr_str, client_fd);
  fflush(stdout);

  int opt = 1;
  setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
  int sndbuf = 524288;
  setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

  note_runtime_connect();

  std::atomic<bool> disconnect_requested{false};
  std::mutex send_mutex;
  std::thread telemetry_thread(telemetry_loop,
                               client_fd,
                               stream_name,
                               &disconnect_requested,
                               &send_mutex,
                               nullptr);  // telemetry-only port: always enabled
  ClientControlState control_state;

  while (g_running && !disconnect_requested.load()) {
    if (!commaview::net::client_socket_alive(client_fd)) {
      note_runtime_peer_disconnect(stream_name, "client_socket_alive", peer_closed_result());
      break;
    }
    consume_client_control_frames(client_fd, &control_state, stream_name);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  append_runtime_run_event("client_disconnected", stream_name, addr_str);
  printf("[%s] client disconnected: %s\n", stream_name, addr_str);
  fflush(stdout);

  disconnect_requested.store(true);
  shutdown(client_fd, SHUT_RDWR);
  if (telemetry_thread.joinable()) telemetry_thread.join();
  close(client_fd);
}

static void accept_loop(int server_fd, const char* service_name, int port) {
  printf("[%s] listening on :%d\n", service_name, port);
  fflush(stdout);

  while (g_running) {
    struct sockaddr_in client_addr = {};
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept4(server_fd, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len, SOCK_CLOEXEC);
    if (client_fd < 0) {
      if (g_running) perror("accept");
      continue;
    }
    if (port == PORT_TELEMETRY) {
      std::thread(handle_telemetry_client, client_fd).detach();
    } else {
      std::thread(handle_video_client, client_fd, service_name, port).detach();
    }
  }
}

int commaview_bridge_main(int argc, char* argv[]) {
  signal(SIGPIPE, SIG_IGN);
  // Before any thread starts, so every thread inherits the mask: SIGHUP is taken by the reload
  // thread, SIGINT and SIGTERM by this one (sigwait below), never by a thread blocked in accept().
  // A handler would interrupt only whichever thread the kernel picked, and accept() restarts after
  // it, so stop.sh used to wait out its 5 seconds and SIGKILL the bridge.
  sigset_t reload_signals;
  sigemptyset(&reload_signals);
  sigaddset(&reload_signals, SIGHUP);
  sigset_t stop_signals;
  sigemptyset(&stop_signals);
  sigaddset(&stop_signals, SIGINT);
  sigaddset(&stop_signals, SIGTERM);
  sigset_t blocked = reload_signals;
  sigaddset(&blocked, SIGINT);
  sigaddset(&blocked, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &blocked, nullptr);

  (void)argc;
  (void)argv;
  const char* source = std::getenv("COMMAVIEW_VIDEO_SOURCE");
  if (source != nullptr && source[0] != '\0' && std::strcmp(source, "full") != 0 &&
      std::strcmp(source, "livestream") != 0) {
    fprintf(stderr, "unsupported COMMAVIEW_VIDEO_SOURCE: expected full or livestream\n");
    return 2;
  }
  g_livestream_video_enabled = source != nullptr && std::strcmp(source, "livestream") == 0;
  // A second bridge would unlink the running one's UI export socket before its own bind failed.
  if (!commaview::runtime::hold_instance_lock(commaview::ui_export::default_socket_path() + ".lock")) {
    fprintf(stderr, "another CommaView bridge is running; exiting\n");
    return 3;
  }
  initialize_runtime_state_once();
  append_runtime_run_event("process_start");
  std::thread(config_reload_signal_loop).detach();

  g_ui_export_socket = std::make_unique<commaview::ui_export::SocketServer>();
  const bool ui_export_socket_ready = g_ui_export_socket->start();

  const char** video_services = g_livestream_video_enabled ? VIDEO_SERVICES_LIVESTREAM : VIDEO_SERVICES_PROD;
  printf("CommaView Bridge v3.3.8-safe-bundle (C++) [VIDEO+TELEMETRY][VIDEO_SOURCE=%s][RAW_ONLY_DEFAULT][DIRECT_V2_UI_EXPORT_DEFAULT][UI_SOCKET_PREFERRED=%s][META_MODE=raw-only][EMIT_MS=%d]\n",
         g_livestream_video_enabled ? "livestream-h264" : "full-hevc",
         ui_export_socket_ready ? "on" : "off",
         TELEMETRY_EMIT_MS);
  fflush(stdout);

  std::vector<std::pair<int, const char*>> streams;
  streams.push_back({PORT_ROAD, video_services[0]});
  streams.push_back({PORT_WIDE, video_services[1]});
  streams.push_back({PORT_DRIVER, video_services[2]});
  streams.push_back({PORT_TELEMETRY, "telemetry"});

  std::vector<std::thread> threads;
  std::vector<int> server_fds;

  for (auto& s : streams) {
    int fd = commaview::net::create_server(s.first);
    if (fd < 0) {
      fprintf(stderr, "failed on port %d\n", s.first);
      return 1;
    }
    server_fds.push_back(fd);
    threads.emplace_back(accept_loop, fd, s.second, s.first);
  }

  append_runtime_run_event("ready");
  printf("ready. waiting for connections...\n");
  fflush(stdout);

  int stop_signal = 0;
  while (sigwait(&stop_signals, &stop_signal) != 0) {
  }
  g_running = false;
  // shutdown() wakes a thread blocked in accept(); close() alone may not.
  for (int fd : server_fds) shutdown(fd, SHUT_RDWR);
  for (auto& t : threads) t.join();
  for (int fd : server_fds) close(fd);
  if (g_ui_export_socket != nullptr) g_ui_export_socket->stop();

  append_runtime_run_event("process_stop");
  printf("CommaView Bridge stopped (signal %d).\n", stop_signal);
  fflush(nullptr);
  // Client threads are detached and may still be finishing: leave without running static
  // destructors under them. 128 + signal, as a shell reports a process stopped by it.
  std::_Exit(128 + stop_signal);
}
#ifndef COMMAVIEW_BRIDGE_NO_MAIN
int main(int argc, char* argv[]) {
  return commaview_bridge_main(argc, argv);
}
#endif
