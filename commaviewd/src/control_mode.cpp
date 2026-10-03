#include "control_mode.h"
#include "gps_peek.h"
#include "http_server.h"
#include "manager_state_peek.h"
#include "process_watch.h"
#include "road_phase.h"
#include "runtime_debug_config.h"
#include "source_recording_archive.h"
#include "support_bundle.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <csignal>
#include <iomanip>
#include <cmath>
#include <fcntl.h>
#include <poll.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <mutex>
#include <optional>
#include <ctime>
#include <thread>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

extern char** environ;

namespace commaview::runtime {
namespace {

// Same helpers the runtime-debug renderer uses (identical implementations).
using commaview::runtime_debug::json_escape;
using commaview::runtime_debug::trim_copy;

constexpr const char* kInstallDir = "/data/commaview";
constexpr const char* kParamsDir = "/data/params/d";
constexpr int kDefaultApiPort = 5002;
constexpr int kDiscoveryPort = 5004;
constexpr const char* kDiscoveryQuery = "COMMAVIEW_DISCOVER_V1";
constexpr const char* kVersionFile = "/data/commaview/VERSION";
constexpr const char* kPairingScheme = "commaview://pair";
constexpr int kPairingCodeTtlSec = 300;
constexpr const char* kOnroadUiExportStatusFile = "/data/commaview/run/onroad-ui-export-status.json";
constexpr const char* kOnroadUiExportApplyScript = "/data/commaview/scripts/apply_onroad_ui_export_patch.sh";
constexpr const char* kOnroadUiExportVerifyScript = "/data/commaview/scripts/verify_onroad_ui_export_patch.sh";
constexpr size_t kSupportLogPerFileCapBytes = 512 * 1024;
constexpr size_t kSupportLogTotalCapBytes = 2 * 1024 * 1024;
// The kernel ring buffer is usually 256 KiB-1 MiB; only its OOM/kill lines are kept.
constexpr size_t kSupportKernelLogReadCapBytes = 2 * 1024 * 1024;

// Where a support bundle entry comes from. Everything is read only while answering
// GET /commaview/support/logs, which the app sends only when the user taps Share.
enum class SupportLogSource {
  kFile,
  kManagerState,  // openpilot's process table, peeked from its msgq ring (no subscriber)
  kSwaglog,       // openpilot's swaglog lines about stopped/killed processes
  kKernelLog,     // dmesg lines about out-of-memory kills
};

struct SupportLogFileSpec {
  std::string entry_name;
  std::string path;
  bool rotated;
  SupportLogSource source = SupportLogSource::kFile;
};

std::string msgq_dir();
std::string swaglog_dir();
std::string process_events_log_path();
std::string process_watch_status_json();
std::string leaderboard_seed_for_redaction();

std::vector<SupportLogFileSpec> support_log_files() {
  // Put the small structured snapshots first. Large rolling logs can consume the
  // total response cap, but a support bundle must always retain the current
  // bounded counters and effective configuration needed to diagnose the run.
  // Nothing from data/: the drive list is private, and data/leaderboard-key.json is
  // this comma's private key (its seed is also redacted wherever it might appear).
  std::vector<SupportLogFileSpec> files = {
      {"telemetry-stats.json", "/data/commaview/run/telemetry-stats.json", false},
      {"runtime-debug-effective.json", "/data/commaview/run/runtime-debug-effective.json", false},
      {"onroad-ui-export-status.json", "/data/commaview/run/onroad-ui-export-status.json", false},
      {"last-restart-reason.txt", "/data/commaview/run/last-restart-reason.txt", false},
      // openpilot's side, each capped small: why a process openpilot needs isn't running.
      {"openpilot-manager-state.json", msgq_dir() + "/msgq_managerState", false, SupportLogSource::kManagerState},
      // What the process watcher wrote down as it happened, kept across reboots (process_watch.h).
      {"process-events.jsonl", process_events_log_path(), false},
      {"openpilot-process-events.log", swaglog_dir() + "/swaglog.*", false, SupportLogSource::kSwaglog},
      {"kernel-oom-events.log", "dmesg", false, SupportLogSource::kKernelLog},
      {"process-events.jsonl.1", process_events_log_path() + ".1", true},
      {"runtime-debug-apply.log", "/data/commaview/logs/runtime-debug-apply.log", false},
      {"runtime-run-events.jsonl", "/data/commaview/logs/runtime-run-events.jsonl", false},
      {"commaviewd-bridge.log", "/data/commaview/logs/commaviewd-bridge.log", false},
      {"commaviewd-control.log", "/data/commaview/logs/commaviewd-control.log", false},
      {"onroad-ui-export-startup.log", "/data/commaview/logs/onroad-ui-export-startup.log", false},
      // The drive stats script's own log never holds positions.
      {"commaview-drive-stats.log", "/data/commaview/logs/commaview-drive-stats.log", false},
  };
  const std::array<std::string, 5> rotated = {{
      "commaviewd-bridge.log",
      "commaviewd-control.log",
      "onroad-ui-export-startup.log",
      "commaview-drive-stats.log",
      "runtime-run-events.jsonl",
  }};
  for (const auto& name : rotated) {
    for (int idx = 1; idx <= 14; ++idx) {
      const std::string rotated_name = name + "." + std::to_string(idx);
      const std::string rotated_path = "/data/commaview/logs/" + rotated_name;
      files.push_back({rotated_name, rotated_path, true});
    }
  }
  return files;
}

struct PairingGrant {
  std::string code;
  std::time_t expires_at = 0;
  bool used = true;
};

std::mutex g_pairing_mutex;
PairingGrant g_pairing_grant;
bool run_command(const std::vector<std::string>& args, int* exit_code, std::string* stdout_text, std::string* stderr_text);
bool run_command_with_optional_sudo(const std::vector<std::string>& args, int* exit_code, std::string* stdout_text,
                                    std::string* stderr_text);
bool is_onroad();
bool being_driven();
std::string deferred_maintenance_json();

std::string normalize_code(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (char c : in) {
    unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc)) out.push_back(static_cast<char>(std::toupper(uc)));
  }
  return out;
}

bool codes_equal(const std::string& a, const std::string& b) {
  return normalize_code(a) == normalize_code(b);
}

std::string random_pair_code() {
  static const char* alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  std::array<unsigned char, 8> bytes{};
  size_t filled = 0;
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd >= 0) {
    ssize_t n = read(fd, bytes.data(), bytes.size());
    close(fd);
    filled = n < 0 ? 0 : static_cast<size_t>(n);
  }
  if (filled < bytes.size()) {
    // /dev/urandom missing or short: fill the remaining bytes from rand().
    std::srand(static_cast<unsigned int>(std::time(nullptr) ^ getpid()));
    for (size_t i = filled; i < bytes.size(); ++i) {
      bytes[i] = static_cast<unsigned char>(std::rand() & 0xFF);
    }
  }

  std::string raw;
  raw.reserve(8);
  for (size_t i = 0; i < 8; ++i) raw.push_back(alphabet[bytes[i] % 32]);
  return raw.substr(0, 4) + "-" + raw.substr(4, 4);
}


bool file_executable(const char* path) {
  return path != nullptr && access(path, X_OK) == 0;
}

std::string read_file_raw(const std::string& path) {
  std::ifstream f(path);
  if (!f) return "";
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string read_file_trimmed(const std::string& path) {
  return trim_copy(read_file_raw(path));
}

std::string runtime_version() {
  const std::string v = read_file_trimmed(kVersionFile);
  return v.empty() ? "unknown" : v;
}

std::string telemetry_mode() {
  return "direct-v2-ui-export";
}

std::string device_dongle_id() {
  const std::string dongle_id = read_file_trimmed(std::string(kParamsDir) + "/DongleId");
  if (dongle_id.empty() || dongle_id == "UnregisteredDevice") return "";
  return dongle_id;
}

std::string device_hardware_serial() {
  return read_file_trimmed(std::string(kParamsDir) + "/HardwareSerial");
}

std::string device_model() {
  const std::string model = read_file_trimmed(std::string(kParamsDir) + "/HardwareModel");
  return model.empty() ? "comma" : model;
}

std::string discovery_device_name() {
  const std::string dongle_id = device_dongle_id();
  if (!dongle_id.empty()) return std::string("comma-") + dongle_id;
  const std::string serial = device_hardware_serial();
  if (!serial.empty()) return std::string("comma-") + serial;
  char hostname[128] = {0};
  if (gethostname(hostname, sizeof(hostname) - 1) == 0 && hostname[0] != '\0') {
    return hostname;
  }
  return "comma-device";
}

bool write_file(const std::string& path, const std::string& value, mode_t mode = 0644) {
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (fd < 0) return false;
  const ssize_t want = static_cast<ssize_t>(value.size());
  const ssize_t wrote = write(fd, value.data(), static_cast<size_t>(want));
  close(fd);
  return wrote == want;
}

void ensure_runtime_debug_dirs() {
  mkdir("/data/commaview/config", 0755);
  mkdir("/data/commaview/run", 0755);
}

uint64_t now_ms() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string read_file_tail_capped(const std::string& path, size_t cap, bool* exists, bool* truncated) {
  if (exists) *exists = false;
  if (truncated) *truncated = false;

  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return "";
  if (exists) *exists = true;

  struct stat st{};
  if (fstat(fd, &st) != 0 || st.st_size <= 0) {
    close(fd);
    return "";
  }

  off_t start = 0;
  if (static_cast<uint64_t>(st.st_size) > cap) {
    start = st.st_size - static_cast<off_t>(cap);
    if (truncated) *truncated = true;
  }
  if (lseek(fd, start, SEEK_SET) < 0) {
    close(fd);
    return "";
  }

  std::string out;
  out.resize(static_cast<size_t>(st.st_size - start));
  const ssize_t n = read(fd, out.data(), out.size());
  close(fd);
  if (n < 0) return "";
  out.resize(static_cast<size_t>(n));
  return out;
}

// openpilot's own log (swaglog), kept by its logmessaged as /data/log/swaglog.<index>.
std::string swaglog_dir() {
#if !defined(__aarch64__)
  if (const char* test_dir = std::getenv("COMMAVIEWD_TEST_SWAGLOG_DIR")) {
    if (*test_dir) return test_dir;
  }
#endif
  return "/data/log";
}

// The kernel log as plain `dmesg` prints it: read-only, the ring buffer is never cleared.
std::string kernel_log_text(std::string* source, std::string* error) {
#if !defined(__aarch64__)
  // Host tests feed a fixed kernel log instead of the build machine's.
  if (const char* test_file = std::getenv("COMMAVIEWD_TEST_KERNEL_LOG_FILE")) {
    if (*test_file) {
      bool exists = false;
      bool truncated = false;
      *source = "test";
      std::string text = read_file_tail_capped(test_file, kSupportKernelLogReadCapBytes, &exists, &truncated);
      if (!exists) *error = "kernel log unavailable";
      return text;
    }
  }
#endif
  std::string klog_error;
  std::string text = commaview::support::read_kernel_log(kSupportKernelLogReadCapBytes, &klog_error);
  if (klog_error.empty()) {
    *source = "klogctl";
    return text;
  }
  // kernel.dmesg_restrict: the same read through plain `dmesg`, with sudo -n when not root.
  int rc = 0;
  std::string out;
  std::string err;
  if (run_command_with_optional_sudo({"dmesg"}, &rc, &out, &err) && rc == 0) {
    *source = "dmesg";
    if (out.size() > kSupportKernelLogReadCapBytes) out.erase(0, out.size() - kSupportKernelLogReadCapBytes);
    return out;
  }
  *error = "kernel log unreadable: " + klog_error;
  return "";
}

struct SupportLogEntry {
  bool exists = false;
  bool truncated = false;
  std::string content;
};

SupportLogEntry support_log_file_entry(const SupportLogFileSpec& spec, const commaview::support::RedactionSecrets& secrets) {
  SupportLogEntry entry;
  std::string body = read_file_tail_capped(spec.path, kSupportLogPerFileCapBytes, &entry.exists, &entry.truncated);
  if (entry.truncated) {
    // Start at a whole line, so nothing private is cut in half and missed by redaction.
    const size_t nl = body.find('\n');
    body.erase(0, nl == std::string::npos ? body.size() : nl + 1);
  }
  entry.content = commaview::support::redact_support_text(body, secrets);
  return entry;
}

SupportLogEntry support_manager_state_entry() {
  SupportLogEntry entry;
  const auto peek = commaview::support::peek_manager_state(msgq_dir());
  entry.exists = peek.queue_exists;
  // Process names, pids and exit codes only: nothing in it to redact.
  entry.content = peek.json + "\n";
  return entry;
}

SupportLogEntry support_swaglog_entry(const commaview::support::RedactionSecrets& secrets) {
  SupportLogEntry entry;
  const std::string dir = swaglog_dir();
  const auto scan = commaview::support::scan_swaglog(dir, secrets);
  entry.exists = scan.dir_exists;
  entry.truncated = scan.truncated;
  std::ostringstream out;
  out << "# openpilot swaglog lines about processes stopping, starting, retrying or failing"
         " (process_not_running, manager, retries, micd/soundd errors)\n";
  if (!scan.dir_exists) {
    out << "# unavailable: no swaglog directory at " << dir << "\n";
  } else {
    out << "# scanned the newest " << scan.files_scanned << " of " << scan.files_recent
        << " swaglog files modified in the last 24 h (" << scan.files_seen << " in all; " << scan.bytes_scanned
        << " bytes); " << scan.lines_matched << " matching lines" << (scan.truncated ? ", capped" : "") << "\n";
  }
  entry.content = out.str() + scan.text;
  return entry;
}

SupportLogEntry support_kernel_log_entry(const commaview::support::RedactionSecrets& secrets) {
  SupportLogEntry entry;
  std::string source;
  std::string error;
  const std::string log = kernel_log_text(&source, &error);
  entry.exists = error.empty();
  std::ostringstream out;
  out << "# kernel log (dmesg) lines about out-of-memory kills\n";
  if (!error.empty()) {
    out << "# unavailable: " << error << "\n";
    entry.content = out.str();
    return entry;
  }
  size_t matched = 0;
  const std::string lines = commaview::support::filter_kernel_log(
      log, secrets, commaview::support::KernelLogLimits(), &matched, &entry.truncated);
  out << "# read " << log.size() << " bytes via " << source << "; " << matched << " matching lines"
      << (entry.truncated ? ", capped" : "") << "\n";
  entry.content = out.str() + lines;
  return entry;
}

const char* support_log_source_name(SupportLogSource source) {
  switch (source) {
    case SupportLogSource::kManagerState: return "openpilot-manager-state";
    case SupportLogSource::kSwaglog: return "openpilot-swaglog";
    case SupportLogSource::kKernelLog: return "kernel-log";
    case SupportLogSource::kFile: break;
  }
  return "file";
}

// The support bundle, built only while answering the request: every entry is read now, capped,
// and redacted (GPS, VIN, dongle id and serial, Wi-Fi SSIDs, public IPs, tokens) before it leaves
// the comma.
std::string support_logs_response_json(const std::string& api_token) {
  const commaview::support::RedactionSecrets secrets{api_token, device_dongle_id(), device_hardware_serial(),
                                                     leaderboard_seed_for_redaction()};
  std::ostringstream out;
  size_t total = 0;
  out << "{\"ok\":true,";
  out << "\"generatedAtMs\":" << now_ms() << ",";
  out << "\"perFileCapBytes\":" << kSupportLogPerFileCapBytes << ",";
  out << "\"totalCapBytes\":" << kSupportLogTotalCapBytes << ",";
  out << "\"redacted\":[\"gps\",\"vin\",\"dongle-id\",\"serial\",\"wifi-ssid\",\"mac\",\"public-ip\",\"token\"],";
  out << "\"files\":[";

  bool first = true;
  const std::vector<SupportLogFileSpec> log_files = support_log_files();
  for (const auto& spec : log_files) {
    SupportLogEntry entry;
    switch (spec.source) {
      case SupportLogSource::kManagerState: entry = support_manager_state_entry(); break;
      case SupportLogSource::kSwaglog: entry = support_swaglog_entry(secrets); break;
      case SupportLogSource::kKernelLog: entry = support_kernel_log_entry(secrets); break;
      case SupportLogSource::kFile: entry = support_log_file_entry(spec, secrets); break;
    }
    std::string& body = entry.content;
    if (total + body.size() > kSupportLogTotalCapBytes) {
      entry.truncated = true;
      const size_t remaining = total >= kSupportLogTotalCapBytes ? 0 : kSupportLogTotalCapBytes - total;
      body.resize(std::min(body.size(), remaining));
    }
    total += body.size();

    if (!first) out << ",";
    first = false;
    out << "{";
    out << "\"name\":\"" << json_escape(spec.entry_name) << "\",";
    out << "\"path\":\"" << json_escape(spec.path) << "\",";
    out << "\"source\":\"" << support_log_source_name(spec.source) << "\",";
    out << "\"exists\":" << (entry.exists ? "true" : "false") << ",";
    out << "\"truncated\":" << (entry.truncated ? "true" : "false") << ",";
    out << "\"rotated\":" << (spec.rotated ? "true" : "false") << ",";
    out << "\"encoding\":\"text\",";
    out << "\"content\":\"" << json_escape(body) << "\"";
    out << "}";

    if (total >= kSupportLogTotalCapBytes) break;
  }

  out << "],\"totalBytes\":" << total << "}";
  return out.str();
}

std::string runtime_restart_reason() {
  const char* from_env = std::getenv("COMMAVIEWD_RESTART_REASON");
  if (from_env != nullptr) {
    const std::string value = trim_copy(from_env);
    if (!value.empty()) return value;
  }
  const std::string from_file = read_file_trimmed("/data/commaview/run/last-restart-reason.txt");
  return from_file.empty() ? "startup" : from_file;
}

commaview::runtime_debug::LoadedRuntimeDebugConfig load_persisted_runtime_debug_config() {
  return commaview::runtime_debug::load_runtime_debug_config();
}

commaview::runtime_debug::LoadedRuntimeDebugConfig load_effective_runtime_debug_config_state() {
  auto persisted = load_persisted_runtime_debug_config();
  auto effective = commaview::runtime_debug::effective_runtime_debug_config(persisted);
  const std::string effective_path = commaview::runtime_debug::runtime_debug_effective_path();
  const std::string raw = read_file_raw(effective_path);
  if (!trim_copy(raw).empty()) {
    auto parsed = commaview::runtime_debug::parse_runtime_debug_config_body(raw, true, effective_path);
    if (parsed.valid) {
      effective = parsed;
      effective.exists = true;
      effective.valid = true;
      effective.safe_fallback = persisted.safe_fallback;
      effective.warnings = persisted.warnings;
    }
  }
  return effective;
}

std::string default_runtime_stats_json(const commaview::runtime_debug::LoadedRuntimeDebugConfig& effective) {
  std::ostringstream out;
  out << "{";
  out << "\"uptimeMs\":0,";
  out << "\"reconnectCount\":0,";
  out << "\"configVersion\":" << effective.config_version << ",";
  out << "\"configHash\":\"" << json_escape(effective.config_hash) << "\",";
  out << "\"lastRestartReason\":\"" << json_escape(runtime_restart_reason()) << "\",";
  out << "\"telemetryLoop\":{\"iterations\":0,\"avgMicros\":0,\"maxMicros\":0,\"overBudget\":0},";
  out << "\"videoLoop\":{\"iterations\":0,\"avgMicros\":0,\"maxMicros\":0,\"overBudget\":0},";
  out << "\"services\":{}";
  out << "}";
  return out.str();
}

std::string load_runtime_stats_json(const commaview::runtime_debug::LoadedRuntimeDebugConfig& effective) {
  const std::string raw = read_file_trimmed(commaview::runtime_debug::runtime_debug_stats_path());
  return raw.empty() ? default_runtime_stats_json(effective) : raw;
}

std::string runtime_debug_state_json() {
  const auto persisted = load_persisted_runtime_debug_config();
  const auto effective = load_effective_runtime_debug_config_state();
  const std::vector<std::string> warnings = !effective.warnings.empty() ? effective.warnings : persisted.warnings;
  std::ostringstream out;
  out << "{";
  out << "\"persistedConfig\":" << commaview::runtime_debug::render_config_json(persisted, true) << ",";
  out << "\"effectiveConfig\":" << commaview::runtime_debug::render_config_json(effective, true) << ",";
  out << "\"runtimeStats\":" << load_runtime_stats_json(effective) << ",";
  out << "\"warnings\":" << commaview::runtime_debug::warnings_json(warnings) << ",";
  out << "\"safeFallback\":" << ((persisted.safe_fallback || effective.safe_fallback) ? "true" : "false");
  out << "}";
  return out.str();
}

bool json_field_true(const std::string& body, const char* key) {
  if (key == nullptr) return false;
  return body.find(std::string("\"") + key + "\":true") != std::string::npos;
}

std::string onroad_ui_export_status_error_json(const std::string& state, const std::string& reason) {
  return std::string("{\"healthy\":false,\"patchVerified\":false,\"statusScope\":\"patch-installation\",\"repairNeeded\":true,\"state\":\"") +
         json_escape(state.empty() ? "error" : state) + "\",\"reason\":\"" +
         json_escape(reason.empty() ? "onroad UI export verify failed" : reason) + "\"}";
}

std::string load_onroad_ui_export_status_json() {
  const std::string raw = read_file_trimmed(kOnroadUiExportStatusFile);
  return raw.empty() ? onroad_ui_export_status_error_json("missing", "onroad UI export status unavailable") : raw;
}

std::string run_onroad_ui_export_verify_json(int* rc_out, std::string* err_out) {
  if (rc_out) *rc_out = 0;
  if (err_out) err_out->clear();
  if (!file_executable(kOnroadUiExportVerifyScript)) {
    return load_onroad_ui_export_status_json();
  }

  int rc = 0;
  std::string out;
  std::string err;
  if (!run_command({kOnroadUiExportVerifyScript, "--json"}, &rc, &out, &err)) {
    return load_onroad_ui_export_status_json();
  }

  if (rc_out) *rc_out = rc;
  if (err_out) *err_out = trim_copy(err);

  const std::string body = trim_copy(out);
  if (!body.empty()) return body;
  if (rc == 0) return load_onroad_ui_export_status_json();
  return onroad_ui_export_status_error_json("error", trim_copy(err));
}

std::string run_onroad_ui_export_apply_status_json(int* rc_out, std::string* err_out) {
  if (rc_out) *rc_out = 1;
  if (err_out) err_out->clear();
  if (being_driven()) {
    if (err_out) *err_out = "repair blocked while driving";
    return onroad_ui_export_status_error_json("onroad-blocked", "repair blocked while driving");
  }
  if (!file_executable(kOnroadUiExportApplyScript)) {
    if (err_out) *err_out = "onroad UI export repair helper missing";
    return onroad_ui_export_status_error_json("missing-helper", "onroad UI export repair helper missing");
  }

  int rc = 0;
  std::string out;
  std::string err;
  std::vector<std::string> args{kOnroadUiExportApplyScript};
  const bool ran = run_command(args, &rc, &out, &err);
  const std::string trimmed_err = trim_copy(err);
  const std::string failure = trimmed_err.empty() ? "onroad UI export repair failed" : trimmed_err;
  if (!ran) {
    if (err_out) *err_out = failure;
    return onroad_ui_export_status_error_json("error", failure);
  }

  if (rc_out) *rc_out = rc;
  if (err_out) *err_out = trimmed_err;

  const std::string body = trim_copy(out);
  if (!body.empty()) return body;
  if (rc == 0) return load_onroad_ui_export_status_json();
  return onroad_ui_export_status_error_json("error", failure);
}

std::string live_onroad_ui_export_status_json(bool allow_self_heal) {
  int verify_rc = 0;
  std::string verify_err;
  std::string status = run_onroad_ui_export_verify_json(&verify_rc, &verify_err);

  if (!allow_self_heal) return status;
  if (is_onroad()) return status;
  if (!file_executable(kOnroadUiExportApplyScript)) return status;
  if (verify_rc == 0 && !json_field_true(status, "repairNeeded")) return status;

  int repair_rc = 0;
  std::string repair_err;
  const std::string repaired = run_onroad_ui_export_apply_status_json(&repair_rc, &repair_err);
  if (repair_rc == 0 || json_field_true(repaired, "patchVerified")) return repaired;
  return status;
}

std::string discovery_response_json() {
  const std::string version = runtime_version();
  const std::string dongleId = device_dongle_id();
  const std::string hardwareSerial = device_hardware_serial();
  const std::string model = device_model();
  std::ostringstream out;
  out << "{";
  out << "\"type\":\"commaview.discovery.v1\",";
  out << "\"name\":\"" << json_escape(discovery_device_name()) << "\",";
  out << "\"version\":\"" << json_escape(version) << "\",";
  out << "\"runtimeVersion\":\"" << json_escape(version) << "\",";
  out << "\"dongleId\":\"" << json_escape(dongleId) << "\",";
  out << "\"dongle_id\":\"" << json_escape(dongleId) << "\",";
  out << "\"deviceModel\":\"" << json_escape(model) << "\",";
  out << "\"device\":\"" << json_escape(model) << "\",";
  out << "\"hardwareSerial\":\"" << json_escape(hardwareSerial) << "\",";
  out << "\"apiPort\":" << kDefaultApiPort << ",";
  out << "\"api_port\":" << kDefaultApiPort << ",";
  out << "\"videoPorts\":{\"road\":8200,\"wide\":8201,\"driver\":8202}";
  out << "}";
  return out.str();
}

void discovery_responder_loop() {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    std::perror("commaviewd discovery socket");
    return;
  }

  int opt = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(kDiscoveryPort));
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::perror("commaviewd discovery bind");
    ::close(fd);
    return;
  }

  std::printf("commaviewd discovery: listening on udp :%d\n", kDiscoveryPort);
  std::fflush(stdout);

  std::array<char, 512> buf{};
  while (true) {
    sockaddr_in peer{};
    socklen_t peer_len = sizeof(peer);
    const ssize_t n = ::recvfrom(fd, buf.data(), buf.size() - 1, 0, reinterpret_cast<sockaddr*>(&peer), &peer_len);
    if (n <= 0) continue;
    std::string query(buf.data(), static_cast<size_t>(n));
    query = trim_copy(query);
    if (query != kDiscoveryQuery) continue;

    const std::string response = discovery_response_json();
    ::sendto(fd, response.data(), response.size(), 0, reinterpret_cast<sockaddr*>(&peer), peer_len);
  }
}

void start_discovery_responder() {
  std::thread(discovery_responder_loop).detach();
}

std::string runtime_status_json() {
  const std::string version = runtime_version();
  const std::string telemetryMode = telemetry_mode();
  const std::string dongleId = device_dongle_id();
  const std::string hardwareSerial = device_hardware_serial();
  const bool onroad = is_onroad();
  const auto persisted = load_persisted_runtime_debug_config();
  const auto effective = load_effective_runtime_debug_config_state();
  const std::vector<std::string> warnings = !effective.warnings.empty() ? effective.warnings : persisted.warnings;
  std::ostringstream out;
  out << "{";
  out << "\"version\":\"" << json_escape(version) << "\",";
  out << "\"runtimeVersion\":\"" << json_escape(version) << "\",";
  out << "\"dongleId\":\"" << json_escape(dongleId) << "\",";
  out << "\"dongle_id\":\"" << json_escape(dongleId) << "\",";
  out << "\"hardwareSerial\":\"" << json_escape(hardwareSerial) << "\",";
  out << "\"api_port\":" << kDefaultApiPort << ",";
  out << "\"telemetryMode\":\"" << json_escape(telemetryMode) << "\",";
  out << "\"roadState\":\"" << (onroad ? "onroad" : "offroad") << "\",";
  out << "\"isOnroad\":" << (onroad ? "true" : "false") << ",";
  out << "\"onroad\":" << (onroad ? "true" : "false") << ",";
  // roadState stays onroad/offroad (openpilot's own IsOffroad). roadPhase splits onroad into
  // parked (in Park, at a standstill, not engaged) and driving; anything uncertain is driving.
  const auto phase = commaview::road::read_road_phase(onroad, msgq_dir());
  out << "\"roadPhase\":\"" << commaview::road::road_phase_name(phase.phase) << "\",";
  out << "\"roadPhaseReason\":\"" << json_escape(phase.reason) << "\",";
  // An install, uninstall or repair asked for while driving, waiting for Park (or how it ended).
  out << "\"deferredMaintenance\":" << deferred_maintenance_json() << ",";
  // openpilot processes down now, and the last one the process watcher saw go down.
  out << "\"processEvents\":" << process_watch_status_json() << ",";
  // What this runtime can do for a paired phone without SSH.
  out << "\"capabilities\":[\"runtime-update\",\"runtime-uninstall\"],";
  out << "\"onroadUiExport\":" << live_onroad_ui_export_status_json(false) << ",";
  out << "\"persistedConfig\":" << commaview::runtime_debug::render_config_json(persisted, true) << ",";
  out << "\"effectiveConfig\":" << commaview::runtime_debug::render_config_json(effective, true) << ",";
  out << "\"runtimeStats\":" << load_runtime_stats_json(effective) << ",";
  out << "\"configVersion\":" << effective.config_version << ",";
  out << "\"configHash\":\"" << json_escape(effective.config_hash) << "\",";
  out << "\"warnings\":" << commaview::runtime_debug::warnings_json(warnings) << ",";
  out << "\"safeFallback\":" << ((persisted.safe_fallback || effective.safe_fallback) ? "true" : "false");
  out << "}";
  return out.str();
}

bool write_runtime_debug_config_json(const std::string& body,
                                     commaview::runtime_debug::LoadedRuntimeDebugConfig* parsed_out,
                                     std::string* error_out) {
  auto parsed = commaview::runtime_debug::parse_runtime_debug_config_body(
      body,
      true,
      commaview::runtime_debug::runtime_debug_config_path());
  if (!parsed.valid) {
    if (error_out != nullptr) {
      *error_out = parsed.error.empty() ? "invalid runtime debug config" : parsed.error;
    }
    return false;
  }
  ensure_runtime_debug_dirs();
  const std::string canonical = commaview::runtime_debug::canonical_config_contents_json(parsed);
  if (!write_file(commaview::runtime_debug::runtime_debug_config_path(), canonical, 0644)) {
    if (error_out != nullptr) *error_out = "failed to write runtime debug config";
    return false;
  }
  parsed.exists = true;
  parsed.valid = true;
  parsed.safe_fallback = false;
  parsed.warnings.clear();
  if (parsed_out != nullptr) *parsed_out = parsed;
  return true;
}

std::string runtime_debug_write_response(bool ok,
                                         const commaview::runtime_debug::LoadedRuntimeDebugConfig& persisted,
                                         const std::string& error = "") {
  auto effective = commaview::runtime_debug::effective_runtime_debug_config(persisted);
  std::ostringstream out;
  out << "{";
  out << "\"ok\":" << (ok ? "true" : "false") << ",";
  out << "\"persistedConfig\":" << commaview::runtime_debug::render_config_json(persisted, true) << ",";
  out << "\"effectiveConfig\":" << commaview::runtime_debug::render_config_json(effective, true);
  if (!error.empty()) out << ",\"error\":\"" << json_escape(error) << "\"";
  out << "}";
  return out.str();
}

std::string runtime_debug_restore_defaults_response() {
  commaview::runtime_debug::LoadedRuntimeDebugConfig parsed;
  std::string error;
  if (!write_runtime_debug_config_json(commaview::runtime_debug::default_runtime_debug_config_json(), &parsed, &error)) {
    return runtime_debug_write_response(false, load_persisted_runtime_debug_config(), error);
  }
  return runtime_debug_write_response(true, parsed);
}

// The run directory start.sh writes pid files to: where the bridge writes its effective config.
std::string runtime_run_dir() {
  const std::string effective = commaview::runtime_debug::runtime_debug_effective_path();
  const size_t slash = effective.find_last_of('/');
  return slash == std::string::npos ? std::string(".") : effective.substr(0, slash);
}

// The running bridge's pid, or 0. A pid file outlives its process (and reboots) and the pid may
// since belong to an openpilot process, so it counts only while /proc says it is our bridge.
pid_t running_bridge_pid() {
  const std::string text = read_file_trimmed(runtime_run_dir() + "/bridge.pid");
  if (text.empty() || text.size() > 9 || text.find_first_not_of("0123456789") != std::string::npos) return 0;
  const pid_t pid = static_cast<pid_t>(std::atoi(text.c_str()));
  if (pid <= 1) return 0;
  const std::string cmdline = read_file_raw("/proc/" + std::to_string(pid) + "/cmdline");
  std::vector<std::string> args;
  size_t start = 0;
  while (start < cmdline.size()) {
    size_t end = cmdline.find('\0', start);
    if (end == std::string::npos) end = cmdline.size();
    args.push_back(cmdline.substr(start, end - start));
    start = end + 1;
  }
  if (args.size() < 2 || args[1] != "bridge") return 0;
  const std::string& exe = args[0];
  const std::string base = exe.substr(exe.find_last_of('/') == std::string::npos ? 0 : exe.find_last_of('/') + 1);
  if (base.rfind("commaviewd", 0) != 0) return 0;
  return pid;
}

// How many times the running bridge has reloaded its config (its stats file's configReloads).
uint64_t bridge_config_reload_count() {
  const std::string stats = read_file_raw(commaview::runtime_debug::runtime_debug_stats_path());
  const std::string key = "\"configReloads\":";
  const size_t pos = stats.find(key);
  if (pos == std::string::npos) return 0;
  return std::strtoull(stats.c_str() + pos + key.size(), nullptr, 10);
}

std::string runtime_debug_apply_response() {
  const auto persisted = load_persisted_runtime_debug_config();
  if (!persisted.valid) {
    return runtime_debug_write_response(false, persisted, persisted.error.empty() ? "invalid runtime debug config" : persisted.error);
  }
  ensure_runtime_debug_dirs();
  // Applied in place: the running bridge re-reads the config on SIGHUP. Nothing restarts, so it is
  // safe while driving. Every setting in the config is a per-service telemetry policy that the
  // bridge's telemetry loops pick up on their next pass, so nothing waits for the car to be parked
  // (appliesWhenParked stays empty). Control mode itself reads the config on every request.
  const uint64_t reloads_before = bridge_config_reload_count();
  const pid_t bridge = running_bridge_pid();
  const bool signalled = bridge > 0 && ::kill(bridge, SIGHUP) == 0;
  bool applied = false;
  if (signalled) {
    for (int i = 0; i < 40 && !applied; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      applied = bridge_config_reload_count() > reloads_before;
    }
  }
  // With no bridge running, the next one starts with the saved config.
  const bool ok = !signalled || applied;
  auto effective = commaview::runtime_debug::effective_runtime_debug_config(persisted);
  std::ostringstream out;
  out << "{";
  out << "\"ok\":" << (ok ? "true" : "false") << ",";
  out << "\"applied\":" << (applied ? "true" : "false") << ",";
  out << "\"appliedLive\":" << (applied ? "true" : "false") << ",";
  out << "\"bridgeRunning\":" << (bridge > 0 ? "true" : "false") << ",";
  out << "\"appliesOnNextStart\":" << (bridge > 0 ? "false" : "true") << ",";
  out << "\"appliesWhenParked\":[],";
  out << "\"restartScheduled\":false,";
  out << "\"persistedConfig\":" << commaview::runtime_debug::render_config_json(persisted, true) << ",";
  out << "\"effectiveConfig\":" << commaview::runtime_debug::render_config_json(effective, true);
  if (!ok) out << ",\"error\":\"the bridge did not confirm the config reload\"";
  out << "}";
  return out.str();
}
std::string read_param(const char* key) {
  std::string root = kParamsDir;
#if !defined(__aarch64__)
  // Host integration tests need an isolated params tree; device builds never
  // permit overriding the vehicle road-state source.
  if (const char* test_root = std::getenv("COMMAVIEWD_TEST_PARAMS_DIR")) {
    if (*test_root) root = test_root;
  }
#endif
  return read_file_trimmed(root + "/" + key);
}

bool is_onroad() {
  // Sunnypilot publishes IsOffroad, not IsOnroad. Prefer the actual device
  // state so the status API and offroad-only mutations agree with the car.
  const std::string offroad = read_param("IsOffroad");
  if (offroad == "0") return true;
  if (offroad == "1") return false;
  const std::string onroad = read_param("IsOnroad");
  if (onroad == "1") return true;
  if (onroad == "0") return false;
  // An unknown state must not authorize offroad-only changes.
  return true;
}

// Maintenance (install, update, uninstall, repair) never runs while the car is being driven: it
// may run offroad, or parked (in Park, at a standstill, not engaged). Anything uncertain is driving.
bool being_driven() {
  const bool onroad = is_onroad();
  if (!onroad) return false;
  return commaview::road::read_road_phase(onroad, msgq_dir()).phase == commaview::road::RoadPhase::kDriving;
}

// Raw text between the quotes after the first `"key":` - no escape decoding,
// and an empty value counts as missing (returns false, *value_out == "").
// Deliberately differs from commaview::runtime_debug::extract_string_field.
bool extract_raw_string_field(const std::string& body, const char* key, std::string* value_out) {
  if (key == nullptr || value_out == nullptr) return false;
  const std::string needle = std::string("\"") + key + "\"";
  size_t pos = body.find(needle);
  if (pos == std::string::npos) return false;
  pos = body.find(':', pos + needle.size());
  if (pos == std::string::npos) return false;
  pos++;
  while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) pos++;
  if (pos >= body.size() || body[pos] != '"') return false;
  pos++;
  size_t end = body.find('"', pos);
  if (end == std::string::npos) return false;
  *value_out = body.substr(pos, end - pos);
  return !value_out->empty();
}

std::string load_api_token() {
  const char* direct = std::getenv("COMMAVIEWD_API_TOKEN");
  if (direct != nullptr) {
    std::string token = trim_copy(direct);
    if (!token.empty()) return token;
  }

  const char* token_file_env = std::getenv("COMMAVIEWD_API_TOKEN_FILE");
  std::string token_file = token_file_env ? token_file_env : std::string(kInstallDir) + "/api/auth.token";
  return read_file_trimmed(token_file);
}

bool run_command(const std::vector<std::string>& args,
                 int* exit_code,
                 std::string* stdout_text,
                 std::string* stderr_text) {
  if (args.empty()) return false;

  int out_pipe[2];
  int err_pipe[2];
  if (pipe(out_pipe) != 0) return false;
  if (pipe(err_pipe) != 0) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    return false;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(out_pipe[0]); close(out_pipe[1]);
    close(err_pipe[0]); close(err_pipe[1]);
    return false;
  }

  if (pid == 0) {
    dup2(out_pipe[1], STDOUT_FILENO);
    dup2(err_pipe[1], STDERR_FILENO);

    close(out_pipe[0]); close(out_pipe[1]);
    close(err_pipe[0]); close(err_pipe[1]);

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    execvp(argv[0], argv.data());
    _exit(127);
  }

  close(out_pipe[1]);
  close(err_pipe[1]);

  auto read_all = [](int fd) {
    std::string out;
    std::array<char, 4096> buf{};
    while (true) {
      ssize_t n = read(fd, buf.data(), buf.size());
      if (n <= 0) break;
      out.append(buf.data(), static_cast<size_t>(n));
    }
    return out;
  };

  std::string out = read_all(out_pipe[0]);
  std::string err = read_all(err_pipe[0]);
  close(out_pipe[0]);
  close(err_pipe[0]);

  int status = 0;
  waitpid(pid, &status, 0);

  if (stdout_text) *stdout_text = out;
  if (stderr_text) *stderr_text = err;

  if (exit_code) {
    if (WIFEXITED(status)) *exit_code = WEXITSTATUS(status);
    else *exit_code = 128;
  }

  return true;
}

bool run_command_with_optional_sudo(const std::vector<std::string>& args,
                                    int* exit_code,
                                    std::string* stdout_text,
                                    std::string* stderr_text) {
  int local_rc = 0;
  int* rc = exit_code ? exit_code : &local_rc;

  if (run_command(args, rc, stdout_text, stderr_text) && *rc == 0) {
    return true;
  }
  if (geteuid() == 0) {
    return false;
  }

  std::vector<std::string> sudo_args = {"sudo", "-n"};
  sudo_args.insert(sudo_args.end(), args.begin(), args.end());
  if (!run_command(sudo_args, rc, stdout_text, stderr_text)) {
    return false;
  }
  return *rc == 0;
}

// Change only the active Wi-Fi connection. Never edit NetworkManager's global
// default or a saved profile that is not currently connected.
bool active_wifi_profile(std::string* profile, std::string* error_json) {
  int rc = 0;
  std::string ignored;
  if (!run_command({"nmcli", "-g", "GENERAL.CONNECTION", "device", "show", "wlan0"},
                   &rc, profile, &ignored) || rc != 0) {
    *error_json = "{\"ok\":false,\"error\":\"Wi-Fi connection unavailable\"}";
    return false;
  }
  *profile = trim_copy(*profile);
  if (profile->empty() || *profile == "--") {
    *error_json = "{\"ok\":false,\"error\":\"Wi-Fi not connected\"}";
    return false;
  }
  return true;
}

std::string wifi_power_save_status_response() {
  std::string profile, error_json;
  if (!active_wifi_profile(&profile, &error_json)) return error_json;
  int rc = 0;
  std::string ignored, configured;
  if (!run_command({"nmcli", "-g", "802-11-wireless.powersave", "connection", "show", profile},
                   &rc, &configured, &ignored) || rc != 0) {
    return "{\"ok\":false,\"error\":\"Wi-Fi power-save state unavailable\"}";
  }
  configured = trim_copy(configured);
  if (configured != "enable" && configured != "disable" && configured != "default") {
    return "{\"ok\":false,\"error\":\"Unknown Wi-Fi power-save state\"}";
  }
  return std::string("{\"ok\":true,\"mode\":\"") + configured + "\",\"enabled\":" +
         (configured == "default" ? "null" : configured == "enable" ? "true" : "false") + "}";
}

std::string wifi_power_save_set_response(const std::string& body) {
  std::string mode;
  if (!extract_raw_string_field(body, "mode", &mode) || (mode != "on" && mode != "off")) {
    return "{\"ok\":false,\"error\":\"mode must be on or off\"}";
  }
  // Any time, driving included: this only saves the active profile's setting. The live link is
  // never touched; the choice takes effect at the next normal Wi-Fi reconnect (reconnectRequired).
  std::string profile, error_json;
  if (!active_wifi_profile(&profile, &error_json)) return error_json;
  int rc = 0;
  std::string ignored;
  const std::string value = mode == "on" ? "3" : "2";
  if (!run_command_with_optional_sudo({"nmcli", "connection", "modify", "id", profile,
                                      "802-11-wireless.powersave", value},
                                      &rc, &ignored, &ignored)) {
    return "{\"ok\":false,\"error\":\"Could not save Wi-Fi setting\"}";
  }
  // This hardware cannot reapply power saving to an active Wi-Fi link. Do not
  // bounce the network under the tablet: the saved choice takes effect after
  // the next normal reconnect.
  return std::string("{\"ok\":true,\"mode\":\"") + (mode == "on" ? "enable" : "disable") +
         "\",\"enabled\":" + (mode == "on" ? "true" : "false") + ",\"reconnectRequired\":true}";
}

// ---- Drive stats and location, from what the comma already keeps: its onroad flag, its drive
// logs, and the positions sunnypilot saves. Nothing here subscribes to openpilot's messages. A short
// script (commaview_drive_stats.py) runs after each drive to read the drive's logs and comma's
// totals; the API only reads files, and deletes the location files the moment location is turned off.

std::string drivelog_root() {
#if !defined(__aarch64__)
  // Host integration tests need an isolated tree; device builds always use the install dir.
  if (const char* test_root = std::getenv("COMMAVIEWD_TEST_DATA_ROOT")) {
    if (*test_root) return test_root;
  }
#endif
  return kInstallDir;
}

// The position openpilot's last finished log minute gives, kept in memory (tmpfs) only.
std::string drivelog_live_dir() {
#if !defined(__aarch64__)
  if (const char* test_dir = std::getenv("COMMAVIEWD_TEST_LIVE_DIR")) {
    if (*test_dir) return test_dir;
  }
#endif
  return "/dev/shm/commaview";
}

// sunnypilot keeps the current position in memory params while it drives (its map helper).
std::string memory_params_dir() {
#if !defined(__aarch64__)
  if (const char* test_dir = std::getenv("COMMAVIEWD_TEST_MEM_PARAMS_DIR")) {
    if (*test_dir) return test_dir;
  }
#endif
  return "/dev/shm/params/d";
}

// openpilot's msgq rings, read without subscribing (see gps_peek.h).
std::string msgq_dir() {
#if !defined(__aarch64__)
  if (const char* test_dir = std::getenv("COMMAVIEWD_TEST_MSGQ_DIR")) {
    if (*test_dir) return test_dir;
  }
#endif
  return "/dev/shm";
}

// The process watcher's log (process_watch.h): openpilot's processes going down, and the kernel's
// memory-pressure kills, as they happen, kept across reboots.
std::string process_events_log_path() {
#if !defined(__aarch64__)
  if (const char* test_path = std::getenv("COMMAVIEWD_TEST_PROCESS_EVENTS_LOG")) {
    if (*test_path) return test_path;
  }
#endif
  return std::string(kInstallDir) + "/logs/process-events.jsonl";
}

// COMMAVIEWD_PROCESS_WATCH_MS: how often the watcher looks (500-60000 ms, default 2000); 0 turns it off.
int process_watch_interval_ms() {
  constexpr int kDefaultMs = 2000;
  const char* env = std::getenv("COMMAVIEWD_PROCESS_WATCH_MS");
  if (env == nullptr || *env == '\0') return kDefaultMs;
  char* end = nullptr;
  const long ms = std::strtol(env, &end, 10);
  if (end == env || *end != '\0' || ms < 0) return kDefaultMs;
  if (ms == 0) return 0;
  return static_cast<int>(std::min(60000L, std::max(500L, ms)));
}

// Set once, before the watcher's thread starts; never freed (it lives as long as control mode).
std::atomic<commaview::process_events::ProcessWatcher*> g_process_watcher{nullptr};

std::string process_watch_status_json() {
  const auto* watcher = g_process_watcher.load();
  if (watcher == nullptr) return "{\"watching\":false}";
  return commaview::process_events::process_watch_summary_json(watcher->summary());
}

void start_process_watcher() {
  const int interval_ms = process_watch_interval_ms();
  if (interval_ms <= 0) return;
  commaview::process_events::ProcessWatchConfig config;
  config.msgq_dir = msgq_dir();
  config.log_path = process_events_log_path();
  config.interval_ms = interval_ms;
#if !defined(__aarch64__)
  // Host tests never follow the build machine's kernel log, and write only where they say.
  if (std::getenv("COMMAVIEWD_TEST_MSGQ_DIR") != nullptr) {
    if (std::getenv("COMMAVIEWD_TEST_PROCESS_EVENTS_LOG") == nullptr) return;
    config.kmsg_path.clear();
  }
#endif
  config.onroad = []() -> std::optional<bool> { return is_onroad(); };
  config.road_phase = [](bool onroad) {
    return std::string(commaview::road::road_phase_name(commaview::road::read_road_phase(onroad, msgq_dir()).phase));
  };
  auto* watcher = new commaview::process_events::ProcessWatcher(std::move(config));
  g_process_watcher.store(watcher);
  std::thread([interval_ms, watcher] {
    commaview::process_events::lower_current_thread_priority();
    watcher->start();
    while (true) {
      watcher->tick();
      std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
  }).detach();
}

std::string drivelog_path(const std::string& relative) {
  return drivelog_root() + "/" + relative;
}

std::string params_path(const std::string& key) {
  std::string root = kParamsDir;
#if !defined(__aarch64__)
  if (const char* test_root = std::getenv("COMMAVIEWD_TEST_PARAMS_DIR")) {
    if (*test_root) root = test_root;
  }
#endif
  return root + "/" + key;
}

constexpr size_t kDrivelogFileCapBytes = 256 * 1024;
constexpr int64_t kValidWallMs = 1704067200000LL;  // 2024: before that the clock hasn't been set
constexpr int kLivePositionMaxAgeSec = 30;
constexpr int kTotalsStaleSec = 30 * 60;
constexpr int kTotalsRefreshSec = 6 * 60 * 60;
constexpr int kTotalsRequestGapSec = 5 * 60;
constexpr int kPositionRunGapSec = 60;
constexpr int kAfterDriveDelaySec = 30;  // lets loggerd close the drive's last log first
constexpr int kMinDriveSec = 10;
constexpr int kDriveScriptTimeoutSec = 10 * 60;

int64_t wall_ms_now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t file_mtime_ms(const std::string& path) {
  struct stat st {};
  if (stat(path.c_str(), &st) != 0) return 0;
  return static_cast<int64_t>(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1000000;
}

// A file the drive stats script wrote, embedded as it is, or null when it's missing, too big or not an object.
std::string embedded_json_object(const std::string& path) {
  struct stat st {};
  if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
      static_cast<size_t>(st.st_size) > kDrivelogFileCapBytes) {
    return "null";
  }
  const std::string raw = read_file_trimmed(path);
  if (raw.size() < 2 || raw.front() != '{' || raw.back() != '}') return "null";
  return raw;
}

// The value after the first `"key":`, spaces allowed: a JSON boolean or a number.
size_t json_value_start(const std::string& body, const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t pos = body.find(needle);
  if (pos == std::string::npos) return std::string::npos;
  pos = body.find_first_not_of(" \t\r\n", pos + needle.size());
  if (pos == std::string::npos || body[pos] != ':') return std::string::npos;
  return body.find_first_not_of(" \t\r\n", pos + 1);
}

bool extract_bool_field(const std::string& body, const char* key, bool* value_out) {
  if (key == nullptr || value_out == nullptr) return false;
  const size_t pos = json_value_start(body, key);
  if (pos == std::string::npos) return false;
  if (body.compare(pos, 4, "true") == 0) {
    *value_out = true;
    return true;
  }
  if (body.compare(pos, 5, "false") == 0) {
    *value_out = false;
    return true;
  }
  return false;
}

bool extract_number_field(const std::string& body, const char* key, double* value_out) {
  if (key == nullptr || value_out == nullptr) return false;
  const size_t pos = json_value_start(body, key);
  if (pos == std::string::npos) return false;
  const char* begin = body.c_str() + pos;
  char* end = nullptr;
  const double value = std::strtod(begin, &end);
  if (end == begin || !std::isfinite(value)) return false;
  *value_out = value;
  return true;
}

// Owner-only: these are where and how this comma drives.
bool write_file_atomic(const std::string& path, const std::string& value) {
  const std::string tmp = path + ".tmp";
  if (!write_file(tmp, value, 0600)) {
    unlink(tmp.c_str());
    return false;
  }
  return rename(tmp.c_str(), path.c_str()) == 0;
}

bool location_sharing_enabled() {
  bool enabled = false;
  return extract_bool_field(read_file_raw(drivelog_path("config/location.json")), "enabled", &enabled) && enabled;
}

// A position sunnypilot saved ({"latitude": .., "longitude": .., "bearing": ..}) as this API's
// position, dated by when it was saved; null when it isn't a real position.
std::string sunnypilot_position_json(const std::string& path, int max_age_sec) {
  const int64_t saved_ms = file_mtime_ms(path);
  if (saved_ms <= 0 || (max_age_sec > 0 && wall_ms_now() - saved_ms > static_cast<int64_t>(max_age_sec) * 1000)) {
    return "null";
  }
  const std::string raw = read_file_raw(path);
  double lat = 0.0, lon = 0.0, bearing = 0.0;
  if (!extract_number_field(raw, "latitude", &lat) || !extract_number_field(raw, "longitude", &lon) ||
      lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0 || (std::fabs(lat) < 1e-6 && std::fabs(lon) < 1e-6)) {
    return "null";
  }
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out << "{\"lat\":" << std::setprecision(6) << lat << ",\"lon\":" << lon;
  if (extract_number_field(raw, "bearing", &bearing)) out << ",\"bearingDeg\":" << std::setprecision(1) << bearing;
  out << ",\"fixMs\":" << saved_ms << ",\"source\":\"sunnypilot\"}";
  return out.str();
}

std::string comma_fix_json(const commaview::gps::Fix& fix, const std::string& extra = "") {
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out << "{\"lat\":" << std::setprecision(6) << fix.lat << ",\"lon\":" << fix.lon
      << ",\"accuracyM\":" << std::setprecision(1) << fix.accuracy_m
      << ",\"bearingDeg\":" << fix.bearing_deg
      << ",\"speedMs\":" << std::setprecision(2) << fix.speed_ms
      << ",\"fixMs\":" << fix.fix_ms << extra << ",\"source\":\"comma\"}";
  return out.str();
}

enum class RoadState { kOnroad, kOffroad, kUnknown };

RoadState road_state() {
  const std::string offroad = read_param("IsOffroad");
  if (offroad == "0") return RoadState::kOnroad;
  if (offroad == "1") return RoadState::kOffroad;
  const std::string onroad = read_param("IsOnroad");
  if (onroad == "1") return RoadState::kOnroad;
  if (onroad == "0") return RoadState::kOffroad;
  return RoadState::kUnknown;
}

// ---- The drive watcher: notices each drive from the onroad flag and runs the script afterwards.

using SteadyClock = std::chrono::steady_clock;

struct DriveScriptRun {
  std::vector<std::string> args;
  SteadyClock::time_point not_before;
};

struct DriveWatch {
  bool driving = false;
  SteadyClock::time_point started_at;
  int64_t started_wall_ms = 0;
  std::string route;
  std::string last_route;  // the previous drive's, so a stale CurrentRoute is never taken for this one
  pid_t script_pid = -1;
  SteadyClock::time_point script_started;
  std::vector<DriveScriptRun> queued;
  SteadyClock::time_point next_totals;
  SteadyClock::time_point last_totals_request;
  SteadyClock::time_point last_position_run;
  std::optional<commaview::gps::Fix> last_fix;  // the comma's newest fix this drive, while location is on
  bool started = false;
};

std::mutex g_drive_mutex;
DriveWatch g_drive;

// Caller holds g_drive_mutex.
commaview::gps::GpsPeek& gps_peek_locked() {
  static commaview::gps::GpsPeek peek(msgq_dir());
  return peek;
}

int drive_poll_ms() {
#if !defined(__aarch64__)
  if (const char* test_ms = std::getenv("COMMAVIEWD_TEST_DRIVE_POLL_MS")) {
    const int ms = std::atoi(test_ms);
    if (ms > 0) return ms;
  }
#endif
  return 5000;
}

int after_drive_delay_sec() {
#if !defined(__aarch64__)
  if (std::getenv("COMMAVIEWD_TEST_DRIVE_SCRIPT") != nullptr) return 0;
#endif
  return kAfterDriveDelaySec;
}

// Where python3 is, searched now rather than in the child after fork.
std::string python_path() {
  if (access("/usr/local/venv/bin/python3", X_OK) == 0) return "/usr/local/venv/bin/python3";
  const char* path_env = std::getenv("PATH");
  std::stringstream dirs(path_env != nullptr ? path_env : "/usr/local/bin:/usr/bin:/bin");
  std::string dir;
  while (std::getline(dirs, dir, ':')) {
    const std::string candidate = (dir.empty() ? "." : dir) + "/python3";
    if (access(candidate.c_str(), X_OK) == 0) return candidate;
  }
  return "/usr/bin/python3";
}

// The script's command line, environment, log and working directory, all prepared before fork: the
// child of this threaded process only makes async-signal-safe calls before exec.
struct DriveScriptCommand {
  std::vector<std::string> argv;
  std::vector<std::string> env;
  std::vector<char*> argv_ptrs;
  std::vector<char*> env_ptrs;
  std::string log_path;
  const char* workdir = nullptr;
};

// openpilot's own Python, so the script reads the fork's logs and identity.
void prepare_drive_script(const std::vector<std::string>& args, DriveScriptCommand* cmd) {
  bool test_script = false;
#if !defined(__aarch64__)
  if (const char* script = std::getenv("COMMAVIEWD_TEST_DRIVE_SCRIPT")) {
    cmd->argv.push_back(script);
    test_script = true;
  }
#endif
  if (!test_script) {
    cmd->argv.push_back(python_path());
    cmd->argv.push_back(std::string(kInstallDir) + "/src/commaview_drive_stats.py");
  }
  cmd->argv.insert(cmd->argv.end(), args.begin(), args.end());
  for (auto& a : cmd->argv) cmd->argv_ptrs.push_back(const_cast<char*>(a.c_str()));
  cmd->argv_ptrs.push_back(nullptr);

  for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
    if (!test_script && std::strncmp(*e, "PYTHONPATH=", 11) == 0) continue;
    cmd->env.emplace_back(*e);
  }
  if (!test_script) cmd->env.emplace_back("PYTHONPATH=/data/openpilot");
  for (auto& e : cmd->env) cmd->env_ptrs.push_back(const_cast<char*>(e.c_str()));
  cmd->env_ptrs.push_back(nullptr);

  cmd->log_path = drivelog_path("logs/commaview-drive-stats.log");
  cmd->workdir = test_script ? nullptr : "/data/openpilot";
}

// In the child, after its own stdout is set up: the script's log, its directory, the lowest
// priority, then the script. Never returns.
[[noreturn]] void exec_drive_script(const DriveScriptCommand& cmd, bool log_stdout) {
  const int log_fd = open(cmd.log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (log_fd >= 0) {
    if (log_stdout) dup2(log_fd, STDOUT_FILENO);
    dup2(log_fd, STDERR_FILENO);
    close(log_fd);
  }
  if (cmd.workdir != nullptr && chdir(cmd.workdir) != 0) _exit(126);
  if (nice(19) == -1) {
    // Normal priority still beats not running.
  }
  execve(cmd.argv_ptrs[0], cmd.argv_ptrs.data(), cmd.env_ptrs.data());
  _exit(127);
}

// A run in the background (after a drive, comma's totals, the position fallback).
pid_t spawn_drive_script(const std::vector<std::string>& args) {
  DriveScriptCommand cmd;
  prepare_drive_script(args, &cmd);
  const pid_t pid = fork();
  if (pid != 0) return pid;  // the parent, or -1
  setsid();
  exec_drive_script(cmd, true);
}

// A run the caller waits for, at most timeout_sec, reading its one-line answer from stdout (its log
// lines still go to its log). False when it couldn't start or ran too long (it's killed then).
bool run_drive_script_for_answer(const std::vector<std::string>& args, int timeout_sec, int* exit_code,
                                 std::string* answer) {
  constexpr size_t kAnswerCapBytes = 64 * 1024;
  DriveScriptCommand cmd;
  prepare_drive_script(args, &cmd);
  int out_pipe[2];
  if (pipe2(out_pipe, O_CLOEXEC) != 0) return false;
  const pid_t pid = fork();
  if (pid < 0) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    return false;
  }
  if (pid == 0) {
    dup2(out_pipe[1], STDOUT_FILENO);  // the copy isn't close-on-exec
    exec_drive_script(cmd, false);
  }
  close(out_pipe[1]);

  const auto deadline = SteadyClock::now() + std::chrono::seconds(timeout_sec);
  const auto ms_left = [&deadline] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - SteadyClock::now()).count();
  };
  bool timed_out = false;
  std::array<char, 4096> buf{};
  answer->clear();
  while (true) {
    const auto left = ms_left();
    if (left <= 0) {
      timed_out = true;
      break;
    }
    pollfd pfd{out_pipe[0], POLLIN, 0};
    const int ready = poll(&pfd, 1, static_cast<int>(left));
    if (ready < 0 && errno == EINTR) continue;
    if (ready == 0) {
      timed_out = true;
      break;
    }
    if (ready < 0) break;
    const ssize_t n = read(out_pipe[0], buf.data(), buf.size());
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;  // the script closed its stdout: done
    answer->append(buf.data(), std::min(static_cast<size_t>(n), kAnswerCapBytes - std::min(kAnswerCapBytes, answer->size())));
  }
  close(out_pipe[0]);

  int status = 0;
  pid_t done = 0;
  while (!timed_out && (done = waitpid(pid, &status, WNOHANG)) == 0) {
    if (ms_left() <= 0) {
      timed_out = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (timed_out) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return false;
  }
  if (done < 0) return false;
  *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
  return true;
}

// Caller holds g_drive_mutex.
void queue_drive_script_locked(std::vector<std::string> args, int delay_sec = 0) {
  for (const auto& run : g_drive.queued) {
    if (run.args == args) return;
  }
  g_drive.queued.push_back({std::move(args), SteadyClock::now() + std::chrono::seconds(delay_sec)});
}

void drive_watch_tick() {
  std::lock_guard<std::mutex> lk(g_drive_mutex);
  const auto now = SteadyClock::now();
  if (!g_drive.started) {
    g_drive.started = true;
    g_drive.next_totals = now + std::chrono::seconds(90);
  }
  if (g_drive.script_pid > 0) {
    int status = 0;
    if (waitpid(g_drive.script_pid, &status, WNOHANG) != 0) {
      g_drive.script_pid = -1;
    } else if (now - g_drive.script_started > std::chrono::seconds(kDriveScriptTimeoutSec)) {
      // A run stuck on the network (say) never holds up the next ones.
      kill(g_drive.script_pid, SIGKILL);
      waitpid(g_drive.script_pid, &status, 0);
      g_drive.script_pid = -1;
    }
  }

  const RoadState road = road_state();
  if (road == RoadState::kOnroad) {
    if (!g_drive.driving) {
      g_drive.driving = true;
      g_drive.started_at = now;
      const int64_t wall = wall_ms_now();
      g_drive.started_wall_ms = wall >= kValidWallMs ? wall : 0;
      g_drive.route.clear();
    }
    // loggerd names the route a moment after the drive starts; the latest new name is this drive's.
    const std::string route = read_param("CurrentRoute");
    if (!route.empty() && route != g_drive.last_route) g_drive.route = route;
    if (location_sharing_enabled()) {
      if (auto fix = gps_peek_locked().latest()) {
        if (g_drive.started_wall_ms == 0 || fix->fix_ms >= g_drive.started_wall_ms - 60000) g_drive.last_fix = fix;
      }
    } else {
      g_drive.last_fix.reset();
    }
  } else if (road == RoadState::kOffroad && g_drive.driving) {
    g_drive.driving = false;
    const int64_t duration_sec = std::chrono::duration_cast<std::chrono::seconds>(now - g_drive.started_at).count();
    // Where this drive ended: the comma's last fix, kept only while location is on.
    if (g_drive.last_fix && location_sharing_enabled()) {
      mkdir(drivelog_path("data").c_str(), 0700);
      write_file_atomic(drivelog_path("data/location-last.json"),
                        comma_fix_json(*g_drive.last_fix, ",\"endedMs\":" + std::to_string(wall_ms_now())) + "\n");
    }
    g_drive.last_fix.reset();
    if (duration_sec >= kMinDriveSec) {
      const int64_t end_wall = wall_ms_now();
      queue_drive_script_locked({"after-drive", "--route", g_drive.route,
                                 "--start-ms", std::to_string(g_drive.started_wall_ms),
                                 "--end-ms", std::to_string(end_wall >= kValidWallMs ? end_wall : 0),
                                 "--duration-s", std::to_string(duration_sec)},
                                after_drive_delay_sec());
      g_drive.next_totals = now + std::chrono::seconds(kTotalsRefreshSec);
    }
    if (!g_drive.route.empty()) g_drive.last_route = g_drive.route;
  }

  if (road == RoadState::kOffroad && now >= g_drive.next_totals) {
    queue_drive_script_locked({"totals"});
    g_drive.next_totals = now + std::chrono::seconds(kTotalsRefreshSec);
  }

  // One run at a time, in order. While the car is on only the position fallback runs, so reading a
  // drive's logs or asking comma never competes with driving; parked, a leftover position run is dropped.
  for (auto it = g_drive.queued.begin(); g_drive.script_pid <= 0 && it != g_drive.queued.end();) {
    const bool position_run = !it->args.empty() && it->args.front() == "position";
    if (road == RoadState::kOffroad && position_run) {
      it = g_drive.queued.erase(it);
      continue;
    }
    if (now < it->not_before || (road != RoadState::kOffroad && !position_run)) {
      ++it;
      continue;
    }
    const DriveScriptRun run = *it;
    g_drive.queued.erase(it);
    g_drive.script_pid = spawn_drive_script(run.args);
    g_drive.script_started = now;
    break;
  }
}

void start_drive_watcher() {
  std::thread([] {
    while (true) {
      drive_watch_tick();
      std::this_thread::sleep_for(std::chrono::milliseconds(drive_poll_ms()));
    }
  }).detach();
}

// A phone looking at stats that are half an hour old asks for comma's totals again (while parked).
void request_totals_if_stale() {
  if (road_state() != RoadState::kOffroad) return;
  const int64_t updated_ms = file_mtime_ms(drivelog_path("data/drive-stats.json"));
  if (updated_ms > 0 && wall_ms_now() - updated_ms < static_cast<int64_t>(kTotalsStaleSec) * 1000) return;
  std::lock_guard<std::mutex> lk(g_drive_mutex);
  const auto now = SteadyClock::now();
  if (g_drive.last_totals_request.time_since_epoch().count() != 0 &&
      now - g_drive.last_totals_request < std::chrono::seconds(kTotalsRequestGapSec)) {
    return;
  }
  g_drive.last_totals_request = now;
  queue_drive_script_locked({"totals"});
}

std::string drive_stats_response_json() {
  request_totals_if_stale();
  std::string current = "null";
  {
    std::lock_guard<std::mutex> lk(g_drive_mutex);
    if (g_drive.driving) {
      const int64_t duration_sec = std::chrono::duration_cast<std::chrono::seconds>(SteadyClock::now() - g_drive.started_at).count();
      const int64_t wall = wall_ms_now();
      const int64_t start_ms = g_drive.started_wall_ms > 0 ? g_drive.started_wall_ms : (wall >= kValidWallMs ? wall - duration_sec * 1000 : 0);
      if (start_ms > 0) {
        current = "{\"startMs\":" + std::to_string(start_ms) + ",\"durationS\":" + std::to_string(duration_sec) + "}";
      }
    }
  }
  std::ostringstream out;
  out << "{\"ok\":true,\"onroad\":" << (road_state() == RoadState::kOnroad ? "true" : "false")
      << ",\"current\":" << current
      << ",\"ledger\":" << embedded_json_object(drivelog_path("data/drives.json"))
      << ",\"totals\":" << embedded_json_object(drivelog_path("data/drive-stats.json")) << "}";
  return out.str();
}

// While driving: sunnypilot's current position, else the comma's own newest GPS fix read from
// openpilot's msgq ring without subscribing. If neither answers (say msgq's layout changed), the
// position at the end of the drive's last finished log minute, asked for about once a minute.
std::string live_location_json() {
  const std::string sunnypilot = sunnypilot_position_json(memory_params_dir() + "/LastGPSPosition", kLivePositionMaxAgeSec);
  if (sunnypilot != "null") return sunnypilot;
  {
    std::lock_guard<std::mutex> lk(g_drive_mutex);
    if (auto fix = gps_peek_locked().latest()) {
      const int64_t age_ms = wall_ms_now() - fix->fix_ms;
      if (age_ms >= -5000 && age_ms <= static_cast<int64_t>(kLivePositionMaxAgeSec) * 1000) return comma_fix_json(*fix);
    }
    const auto now = SteadyClock::now();
    if (g_drive.driving && !g_drive.route.empty() &&
        (g_drive.last_position_run.time_since_epoch().count() == 0 ||
         now - g_drive.last_position_run >= std::chrono::seconds(kPositionRunGapSec))) {
      g_drive.last_position_run = now;
      queue_drive_script_locked({"position", "--route", g_drive.route});
    }
  }
  return embedded_json_object(drivelog_live_dir() + "/location-log.json");
}

// Where the last drive ended: from its log, else the position sunnypilot saves once a minute.
std::string last_location_json() {
  const std::string from_log = embedded_json_object(drivelog_path("data/location-last.json"));
  if (from_log != "null") return from_log;
  return sunnypilot_position_json(params_path("LastGPSPositionLLK"), 0);
}

std::string location_response_json() {
  const bool enabled = location_sharing_enabled();
  const bool onroad = road_state() == RoadState::kOnroad;
  std::ostringstream out;
  out << "{\"ok\":true,\"enabled\":" << (enabled ? "true" : "false")
      << ",\"onroad\":" << (onroad ? "true" : "false")
      << ",\"live\":" << (enabled && onroad ? live_location_json() : "null")
      << ",\"last\":" << (enabled ? last_location_json() : "null") << "}";
  return out.str();
}

std::string location_set_response(const std::string& body) {
  bool enabled = false;
  if (!extract_bool_field(body, "enabled", &enabled)) {
    return "{\"ok\":false,\"error\":\"enabled must be true or false\"}";
  }
  mkdir(drivelog_path("config").c_str(), 0755);
  if (!write_file_atomic(drivelog_path("config/location.json"),
                         enabled ? "{\"enabled\":true}\n" : "{\"enabled\":false}\n")) {
    return "{\"ok\":false,\"error\":\"Could not save the location setting\"}";
  }
  if (!enabled) {
    unlink((drivelog_live_dir() + "/location-log.json").c_str());
    unlink(drivelog_path("data/location-last.json").c_str());
  }
  return std::string("{\"ok\":true,\"enabled\":") + (enabled ? "true" : "false") + "}";
}

// ---- The leaderboard (docs/plans/leaderboard.md in RhynoTech/commaview-web, "The contract"): this
// comma's own Ed25519 key, kept by the drive stats script in data/leaderboard-key.json (0600), signs a
// registration or a statement of its drives by UTC day for the paired phone to relay. It signs only
// when asked here: nothing runs in the background. The key never passes through this process except
// to be redacted from support bundles.
//
// Phones ask for a statement whenever they reach the comma (at most every 15 minutes each), driving
// or not. While the drive list and the key are as they were, the last statement is handed back
// unchanged from data/leaderboard-statement.json (0600, beside the counter), checked with two stat()
// calls: no Python, no new seq. The account service answers the same statement again as a duplicate,
// a success. The drive list only changes when a drive has ended (the after-drive script), so a
// statement never covers a drive in progress, and a day's total in it never goes down.

constexpr const char* kLeaderboardKeyFile = "data/leaderboard-key.json";
constexpr const char* kLeaderboardDriveListFile = "data/drives.json";
constexpr const char* kLeaderboardStatementCacheFile = "data/leaderboard-statement.json";
constexpr int kLeaderboardScriptTimeoutSec = 20;
// The script's exit codes (commaview_drive_stats.py, EXIT_*).
constexpr int kLeaderboardExitBadRequest = 3;
constexpr int kLeaderboardExitNoKey = 4;
constexpr int kLeaderboardExitNoCrypto = 5;
constexpr const char* kLeaderboardChallengeRequiredJson = "{\"ok\":false,\"error\":\"challenge required\"}";
constexpr const char* kLeaderboardNoKeyJson = "{\"ok\":false,\"error\":\"no key\"}";
constexpr const char* kLeaderboardNoCryptoJson = "{\"ok\":false,\"error\":\"crypto unavailable\"}";
constexpr const char* kLeaderboardFailedJson = "{\"ok\":false,\"error\":\"leaderboard failed\"}";

std::mutex g_leaderboard_mutex;

commaview::api::HttpResponse make_json(int code, const std::string& body);

// The account service's challenge: 32 bytes as base64url without padding (43 characters).
bool leaderboard_challenge_valid(const std::string& challenge) {
  if (challenge.size() != 43) return false;
  for (char c : challenge) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') return false;
  }
  return true;
}

// The key's seed, read only so a support bundle can take it out wherever it might appear.
std::string leaderboard_seed_for_redaction() {
  std::string seed;
  extract_raw_string_field(read_file_raw(drivelog_path(kLeaderboardKeyFile)), "seed", &seed);
  return seed;
}

// One line of JSON that says ok: what the script prints when it signed.
bool leaderboard_answer_ok(const std::string& answer) {
  return answer.rfind("{\"ok\":true,", 0) == 0 && answer.back() == '}' && answer.find('\n') == std::string::npos;
}

// What a statement is made from, cheaply: the drive list's and the key's size, modification time and
// inode (both are replaced whole when they change), or "none" for one that's missing.
std::string leaderboard_statement_inputs() {
  std::ostringstream out;
  out << "v1";
  for (const char* relative : {kLeaderboardDriveListFile, kLeaderboardKeyFile}) {
    struct stat st {};
    if (stat(drivelog_path(relative).c_str(), &st) != 0) {
      out << " none";
      continue;
    }
    out << ' ' << st.st_size << ':' << st.st_mtim.tv_sec << '.' << st.st_mtim.tv_nsec << ':' << st.st_ino;
  }
  return out.str();
}

// The last statement, when it was made from these same inputs; empty otherwise.
std::string leaderboard_cached_statement(const std::string& inputs) {
  const std::string cached = read_file_raw(drivelog_path(kLeaderboardStatementCacheFile));
  const size_t newline = cached.find('\n');
  if (newline == std::string::npos || cached.compare(0, newline, inputs) != 0) return "";
  const std::string answer = trim_copy(cached.substr(newline + 1));
  return leaderboard_answer_ok(answer) ? answer : "";
}

// Runs the script's leaderboard mode and answers with what it signed, or the contract's errors. The
// caller holds g_leaderboard_mutex.
commaview::api::HttpResponse leaderboard_script_response_locked(const std::vector<std::string>& args) {
  int rc = 0;
  std::string answer;
  if (!run_drive_script_for_answer(args, kLeaderboardScriptTimeoutSec, &rc, &answer)) {
    return make_json(500, kLeaderboardFailedJson);
  }
  answer = trim_copy(answer);
  if (rc == 0 && leaderboard_answer_ok(answer)) {
    return make_json(200, answer);
  }
  switch (rc) {
    case kLeaderboardExitBadRequest: return make_json(400, kLeaderboardChallengeRequiredJson);
    case kLeaderboardExitNoKey: return make_json(409, kLeaderboardNoKeyJson);
    case kLeaderboardExitNoCrypto:
    case 126:  // openpilot's directory is missing
    case 127:  // no python3 to run it
      return make_json(503, kLeaderboardNoCryptoJson);
    default: return make_json(500, kLeaderboardFailedJson);
  }
}

commaview::api::HttpResponse leaderboard_script_response(const std::vector<std::string>& args) {
  std::lock_guard<std::mutex> lk(g_leaderboard_mutex);
  return leaderboard_script_response_locked(args);
}

// POST /commaview/leaderboard/register {challenge, rotate?} -> {ok, registration, keyId}
commaview::api::HttpResponse leaderboard_register_response(const std::string& body) {
  std::string challenge;
  if (!extract_raw_string_field(body, "challenge", &challenge) || !leaderboard_challenge_valid(challenge)) {
    return make_json(400, kLeaderboardChallengeRequiredJson);
  }
  bool rotate = false;
  extract_bool_field(body, "rotate", &rotate);
  // One argument with '=': a challenge may start with '-'.
  std::vector<std::string> args = {"leaderboard-register", "--challenge=" + challenge};
  if (rotate) args.emplace_back("--rotate");
  return leaderboard_script_response(args);
}

// POST /commaview/leaderboard/statement {} -> {ok, statement, keyId, seq}: the last one again while
// the drive list and the key haven't changed, else a new one (and that one kept for next time).
commaview::api::HttpResponse leaderboard_statement_response() {
  std::lock_guard<std::mutex> lk(g_leaderboard_mutex);
  // Taken before signing: a drive list that changes meanwhile makes the next request sign again.
  const std::string inputs = leaderboard_statement_inputs();
  const std::string cached = leaderboard_cached_statement(inputs);
  if (!cached.empty()) return make_json(200, cached);
  commaview::api::HttpResponse response = leaderboard_script_response_locked({"leaderboard-statement"});
  const std::string cache_path = drivelog_path(kLeaderboardStatementCacheFile);
  if (response.status == 200) {
    if (write_file_atomic(cache_path, inputs + "\n" + response.body + "\n")) chmod(cache_path.c_str(), 0600);
  } else {
    unlink(cache_path.c_str());  // no key any more, or no way to sign: nothing to hand back
  }
  return response;
}

bool extract_pair_code(const std::string& body, std::string* code_out) {
  return extract_raw_string_field(body, "pairCode", code_out) ||
         extract_raw_string_field(body, "code", code_out);
}

std::string pairing_create(const std::string& api_token) {
  if (api_token.empty()) {
    return "{\"ok\":false,\"error\":\"api token unavailable\"}";
  }
  const std::string code = random_pair_code();
  {
    std::lock_guard<std::mutex> lk(g_pairing_mutex);
    g_pairing_grant.code = code;
    g_pairing_grant.expires_at = std::time(nullptr) + kPairingCodeTtlSec;
    g_pairing_grant.used = false;
  }

  std::ostringstream out;
  out << "{\"ok\":true,\"pairCode\":\"" << code
      << "\",\"expiresInSec\":" << kPairingCodeTtlSec
      << ",\"pairingUri\":\"" << kPairingScheme << "?code=" << code << "\"}";
  return out.str();
}

std::string pairing_redeem(const std::string& code, const std::string& api_token) {
  if (api_token.empty()) {
    return "{\"ok\":false,\"error\":\"api token unavailable\"}";
  }
  if (code.empty()) {
    return "{\"ok\":false,\"error\":\"pairCode required\"}";
  }

  std::lock_guard<std::mutex> lk(g_pairing_mutex);
  const std::time_t now = std::time(nullptr);
  if (g_pairing_grant.code.empty() || g_pairing_grant.used) {
    return "{\"ok\":false,\"error\":\"pairing code unavailable\"}";
  }
  if (now > g_pairing_grant.expires_at) {
    g_pairing_grant.used = true;
    return "{\"ok\":false,\"error\":\"pairing code expired\"}";
  }
  if (!codes_equal(code, g_pairing_grant.code)) {
    return "{\"ok\":false,\"error\":\"invalid pairing code\"}";
  }

  g_pairing_grant.used = true;
  return std::string("{\"ok\":true,\"apiToken\":\"") + json_escape(api_token) + "\"}";
}

bool is_authorized(const commaview::api::HttpRequest& req, const std::string& token) {
  if (token.empty()) return true;
  auto it = req.headers.find("x-commaview-token");
  if (it == req.headers.end()) return false;
  return it->second == token;
}

commaview::api::HttpResponse make_json(int code, const std::string& body) {
  commaview::api::HttpResponse resp;
  resp.status = code;
  resp.body = body;
  return resp;
}

std::string onroad_ui_export_status_response() {
  return live_onroad_ui_export_status_json(false);
}

// Maintenance asked for while driving waits in comma/scripts/run_when_offroad.sh's queue, outside
// the install directory, until the car is parked or openpilot is offroad.
std::string deferred_maintenance_dir() {
#if !defined(__aarch64__)
  if (const char* test_dir = std::getenv("COMMAVIEWD_DEFERRED_DIR")) {
    if (*test_dir) return test_dir;
  }
#endif
  return "/data/commaview-deferred";
}

std::string deferred_runner_path() {
#if !defined(__aarch64__)
  if (const char* test_runner = std::getenv("COMMAVIEWD_TEST_DEFERRED_RUNNER")) {
    if (*test_runner) return test_runner;
  }
#endif
  return std::string(kInstallDir) + "/scripts/run_when_offroad.sh";
}

// The queue's status JSON: {"state":"none"} when nothing was ever queued, else state
// waiting / running / done / failed / cancelled, with action, queuedAtMs, startedAtMs,
// finishedAtMs, attempts and exitStatus.
std::string deferred_maintenance_json() {
  const std::string raw = read_file_trimmed(deferred_maintenance_dir() + "/status.json");
  if (raw.size() < 2 || raw.front() != '{' || raw.back() != '}') return "{\"state\":\"none\"}";
  return raw;
}

// A repair asked for while driving ("Safe Repair"): queued to run once the car is parked.
// Nothing asks openpilot to go offroad.
std::string onroad_ui_export_repair_deferred_response() {
  int rc = 1;
  std::string out;
  std::string err;
  const std::string runner = deferred_runner_path();
  const bool queued = file_executable(runner.c_str()) &&
                      run_command({"/usr/bin/env", "COMMAVIEWD_DEFERRED_DIR=" + deferred_maintenance_dir(), "bash", runner,
                                   "queue", "repair", "--", "bash", kOnroadUiExportApplyScript},
                                  &rc, &out, &err) &&
                      rc == 0;
  const std::string reason = queued ? "repair queued until the car is in Park with openpilot disengaged, or offroad"
                                    : "repair blocked while driving and could not be queued";
  std::ostringstream resp;
  resp << "{\"ok\":false,\"deferred\":" << (queued ? "true" : "false") << ",\"repairNeeded\":true,\"status\":"
       << onroad_ui_export_status_error_json(queued ? "deferred-until-offroad" : "onroad-blocked", reason)
       << ",\"deferredMaintenance\":" << deferred_maintenance_json()
       << ",\"error\":\"" << json_escape(reason) << "\"}";
  return resp.str();
}

std::string onroad_ui_export_repair_response(const std::string& request_body) {
  const bool force_offroad = json_field_true(request_body, "forceOffroad");
  // Repair rewrites openpilot's UI files: offroad or parked only, and never forced. With
  // forceOffroad (the app's Safe Repair) a repair asked for while driving is queued instead.
  if (force_offroad && being_driven()) return onroad_ui_export_repair_deferred_response();
  int rc = 0;
  std::string err;
  const std::string status = run_onroad_ui_export_apply_status_json(&rc, &err);
  std::ostringstream resp;
  resp << "{\"ok\":" << (rc == 0 ? "true" : "false") << ",\"repairNeeded\":" << (rc == 0 ? "false" : "true") << ",\"status\":" << status;
  if (rc != 0) {
    resp << ",\"error\":\"" << json_escape(err.empty() ? "onroad UI export repair failed" : err) << "\"";
  }
  resp << "}";
  return resp.str();
}

std::string installed_install_script() {
#if !defined(__aarch64__)
  if (const char* test_script = std::getenv("COMMAVIEWD_TEST_INSTALL_SCRIPT")) {
    if (*test_script) return test_script;
  }
#endif
  return std::string(kInstallDir) + "/install.sh";
}

std::string installed_uninstall_script() {
#if !defined(__aarch64__)
  if (const char* test_script = std::getenv("COMMAVIEWD_TEST_UNINSTALL_SCRIPT")) {
    if (*test_script) return test_script;
  }
#endif
  return std::string(kInstallDir) + "/uninstall.sh";
}

// Queues ACTION (install or uninstall) in run_when_offroad.sh with a copy of SCRIPT and its
// arguments; the waiter runs outside this process, which the job stops. 202 with the queue's
// status, 409 while another job runs, 500 when it couldn't be queued.
commaview::api::HttpResponse queue_runtime_job_response(const std::string& action,
                                                        const std::string& script,
                                                        const std::vector<std::string>& script_args,
                                                        const std::string& extra_json) {
  int rc = 1;
  std::string out;
  std::string err;
  std::vector<std::string> cmd{"/usr/bin/env", "COMMAVIEWD_DEFERRED_DIR=" + deferred_maintenance_dir(), "bash",
                               deferred_runner_path(), "queue", action, "--file", script, "--", "bash",
                               "@JOB@/" + script.substr(script.find_last_of('/') + 1)};
  cmd.insert(cmd.end(), script_args.begin(), script_args.end());
  const bool ran = run_command(cmd, &rc, &out, &err);
  if (!ran || rc != 0) {
    const std::string why = trim_copy(err).empty() ? "the " + action + " could not be queued" : trim_copy(err);
    std::ostringstream resp;
    resp << "{\"ok\":false,\"error\":\"" << json_escape(why) << "\",\"deferredMaintenance\":"
         << deferred_maintenance_json() << "}";
    // 3: a queued job is running right now.
    return make_json(rc == 3 ? 409 : 500, resp.str());
  }
  std::ostringstream resp;
  resp << "{\"ok\":true,\"queued\":true," << extra_json << "\"deferredMaintenance\":" << deferred_maintenance_json()
       << "}";
  return make_json(202, resp.str());
}

bool regular_file(const std::string& path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// A release tag as commaviewd's releases name them: v1.2.3 or v1.2.3-alpha.4. Nothing else reaches
// the installer, which builds the GitHub release URL from it.
bool valid_release_tag(const std::string& tag) {
  if (tag.size() < 6 || tag.size() > 64 || tag[0] != 'v') return false;
  size_t i = 1;
  for (int part = 0; part < 3; part++) {
    const size_t start = i;
    while (i < tag.size() && std::isdigit(static_cast<unsigned char>(tag[i]))) i++;
    if (i == start) return false;
    if (part < 2) {
      if (i >= tag.size() || tag[i] != '.') return false;
      i++;
    }
  }
  if (i == tag.size()) return true;
  if (tag[i] != '-' || i + 1 == tag.size()) return false;
  for (i++; i < tag.size(); i++) {
    const char c = tag[i];
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-') return false;
  }
  return true;
}

// POST /commaview/runtime/update {"tag":"v0.0.57-alpha.1"}: the paired phone's runtime update, no
// SSH. The installed install.sh is queued in run_when_offroad.sh with the tag; once the car is
// parked or offroad it fetches that release from GitHub, checks its sha256, hands over to the
// release's own installer, and rolls back if the new runtime doesn't come up. Always queued, even
// when parked now: the waiter runs outside this process, which the install stops. Answers 202 with
// the queue's status; the app follows deferredMaintenance and /commaview/version from there.
commaview::api::HttpResponse runtime_update_response(const std::string& request_body) {
  std::string tag;
  if (!extract_raw_string_field(request_body, "tag", &tag) || !valid_release_tag(tag)) {
    return make_json(400, "{\"ok\":false,\"error\":\"a release tag (v1.2.3 or v1.2.3-alpha.4) is required\"}");
  }
  const std::string installer = installed_install_script();
  if (!regular_file(installer) || !file_executable(deferred_runner_path().c_str())) {
    return make_json(501, "{\"ok\":false,\"error\":\"this runtime can't update itself; update it over SSH\"}");
  }
  return queue_runtime_job_response("install", installer, {"--tag", tag}, "\"tag\":\"" + json_escape(tag) + "\",");
}

// POST /commaview/runtime/uninstall: the paired phone's uninstall, no SSH. The installed
// uninstall.sh is queued like an update: once the car is parked or offroad it reverts the onroad
// UI export, stops CommaView and removes /data/commaview (this API, its token and the queue
// included), so the app takes the API going away for good as the uninstall having run.
commaview::api::HttpResponse runtime_uninstall_response() {
  const std::string uninstaller = installed_uninstall_script();
  if (!regular_file(uninstaller) || !file_executable(deferred_runner_path().c_str())) {
    return make_json(501, "{\"ok\":false,\"error\":\"this runtime can't uninstall itself; uninstall it over SSH\"}");
  }
  return queue_runtime_job_response("uninstall", uninstaller, {}, "");
}

constexpr const char* kUnauthorizedJson = "{\"ok\":false,\"error\":\"unauthorized\"}";

std::string version_response_json() {
  const std::string version = runtime_version();
  const std::string telemetryMode = telemetry_mode();
  const std::string dongleId = device_dongle_id();
  const std::string hardwareSerial = device_hardware_serial();
  const std::string model = device_model();
  std::ostringstream body;
  body << "{\"version\":\"" << json_escape(version) << "\",";
  body << "\"runtimeVersion\":\"" << json_escape(version) << "\",";
  body << "\"dongleId\":\"" << json_escape(dongleId) << "\",";
  body << "\"dongle_id\":\"" << json_escape(dongleId) << "\",";
  body << "\"hardwareSerial\":\"" << json_escape(hardwareSerial) << "\",";
  body << "\"deviceModel\":\"" << json_escape(model) << "\",";
  body << "\"device\":\"" << json_escape(model) << "\",";
  body << "\"api_port\":" << kDefaultApiPort << ",";
  body << "\"telemetryMode\":\"" << json_escape(telemetryMode) << "\"}";
  return body.str();
}

commaview::api::HttpResponse handle_source_recording_get(const commaview::api::HttpRequest& req,
                                                         const std::string& api_token) {
  // Route archives and UI timelines are private, even on devices where
  // ordinary control endpoints have not yet been paired.
  if (api_token.empty() || !is_authorized(req, api_token)) {
    return make_json(401, kUnauthorizedJson);
  }
  if (req.path == "/commaview/source-recording/current") {
    if (!is_onroad()) {
      return make_json(403, "{\"ok\":false,\"error\":\"onroad required\"}");
    }
    return source_recording_current_response(read_param("CurrentRoute"));
  }
  // Finished drives are served offroad, or parked (in Park, at a standstill, not engaged): the app
  // only asks for segments the comma has finished writing. Never while the car is being driven.
  if (being_driven()) {
    return make_json(403, "{\"ok\":false,\"error\":\"parked or offroad required\"}");
  }
  return source_recording_archive_response(req.path);
}

commaview::api::HttpResponse handle_get(const commaview::api::HttpRequest& req, const std::string& api_token) {
  if (req.path.rfind("/commaview/source-recording/", 0) == 0) {
    return handle_source_recording_get(req, api_token);
  }
  if (req.path == "/commaview/version") {
    return make_json(200, version_response_json());
  }
  if (req.path == "/commaview/status") {
    return make_json(200, runtime_status_json());
  }
  if (req.path == "/commaview/onroad-ui-export/status") {
    return make_json(200, onroad_ui_export_status_response());
  }
  if (req.path == "/commaview/runtime-debug/config") {
    return make_json(200, runtime_debug_state_json());
  }
  if (req.path == "/commaview/wifi/power-save") {
    if (!is_authorized(req, api_token)) {
      return make_json(401, kUnauthorizedJson);
    }
    const std::string body = wifi_power_save_status_response();
    return make_json(body.find("\"ok\":true") != std::string::npos ? 200 : 503, body);
  }
  if (req.path == "/commaview/support/logs") {
    // Gathered only for a paired phone whose user tapped Share; never for anyone else on the LAN.
    if (api_token.empty() || !is_authorized(req, api_token)) {
      return make_json(401, kUnauthorizedJson);
    }
    return make_json(200, support_logs_response_json(api_token));
  }
  if (req.path == "/commaview/drive-stats" || req.path == "/commaview/location") {
    // Where and how this comma drives is private: only a paired phone may ask.
    if (api_token.empty() || !is_authorized(req, api_token)) {
      return make_json(401, kUnauthorizedJson);
    }
    return make_json(200, req.path == "/commaview/location" ? location_response_json() : drive_stats_response_json());
  }
  return make_json(404, "{\"error\":\"not found\"}");
}

commaview::api::HttpResponse handle_post(const commaview::api::HttpRequest& req, const std::string& api_token) {
  if (req.path == "/pairing/redeem") {
    std::string code;
    if (!extract_pair_code(req.body, &code)) {
      return make_json(400, "{\"ok\":false,\"error\":\"pairCode required\"}");
    }
    std::string body = pairing_redeem(code, api_token);
    int status = body.find("\"ok\":true") != std::string::npos ? 200 : 400;
    return make_json(status, body);
  }

  // Everything below /pairing/redeem needs the token, including unknown paths.
  if (!is_authorized(req, api_token)) {
    return make_json(401, kUnauthorizedJson);
  }

  if (req.path == "/pairing/create") {
    return make_json(200, pairing_create(api_token));
  }

  if (req.path == "/commaview/location") {
    if (api_token.empty()) {
      return make_json(401, kUnauthorizedJson);
    }
    const std::string body = location_set_response(req.body);
    return make_json(body.find("\"ok\":true") != std::string::npos ? 200 : 400, body);
  }

  if (req.path == "/commaview/leaderboard/register" || req.path == "/commaview/leaderboard/statement") {
    // This comma's key signs only for its paired phone.
    if (api_token.empty() || !is_authorized(req, api_token)) {
      return make_json(401, kUnauthorizedJson);
    }
    return req.path == "/commaview/leaderboard/register" ? leaderboard_register_response(req.body)
                                                         : leaderboard_statement_response();
  }

  if (req.path == "/commaview/runtime/update") {
    // Replaces this runtime: only a paired phone may ask.
    if (api_token.empty() || !is_authorized(req, api_token)) {
      return make_json(401, kUnauthorizedJson);
    }
    return runtime_update_response(req.body);
  }

  if (req.path == "/commaview/runtime/uninstall") {
    // Removes this runtime: only a paired phone may ask.
    if (api_token.empty() || !is_authorized(req, api_token)) {
      return make_json(401, kUnauthorizedJson);
    }
    return runtime_uninstall_response();
  }

  if (req.path == "/commaview/wifi/power-save") {
    const std::string body = wifi_power_save_set_response(req.body);
    return make_json(body.find("\"ok\":true") != std::string::npos ? 200 : 400, body);
  }

  if (req.path == "/commaview/runtime-debug/config") {
    commaview::runtime_debug::LoadedRuntimeDebugConfig parsed;
    std::string error;
    if (!write_runtime_debug_config_json(req.body, &parsed, &error)) {
      return make_json(400, runtime_debug_write_response(false, load_persisted_runtime_debug_config(), error));
    }
    return make_json(200, runtime_debug_write_response(true, parsed));
  }
  if (req.path == "/commaview/runtime-debug/defaults") {
    std::string body = runtime_debug_restore_defaults_response();
    int code = body.find("\"ok\":true") != std::string::npos ? 200 : 500;
    return make_json(code, body);
  }
  if (req.path == "/commaview/runtime-debug/apply") {
    std::string body = runtime_debug_apply_response();
    int code = body.find("\"ok\":true") != std::string::npos ? 200 : 500;
    return make_json(code, body);
  }
  if (req.path == "/commaview/onroad-ui-export/repair") {
    std::string body = onroad_ui_export_repair_response(req.body);
    int code = body.find("\"ok\":true") != std::string::npos ? 200 :
               body.find("\"deferred\":true") != std::string::npos ? 202 : 500;
    return make_json(code, body);
  }
  return make_json(404, "{\"error\":\"not found\"}");
}

commaview::api::HttpResponse handle_request(const commaview::api::HttpRequest& req, const std::string& api_token) {
  if (req.method == "OPTIONS") {
    commaview::api::HttpResponse r;
    r.status = 204;
    r.headers["Access-Control-Allow-Methods"] = "GET, POST, OPTIONS";
    return r;
  }
  if (req.method == "GET") return handle_get(req, api_token);
  if (req.method == "POST") return handle_post(req, api_token);
  return make_json(405, "{\"error\":\"method not allowed\"}");
}

}  // namespace

int run_road_phase_mode() {
  const bool onroad = is_onroad();
  const auto phase = commaview::road::read_road_phase(onroad, msgq_dir());
  std::printf("%s %s\n", commaview::road::road_phase_name(phase.phase), phase.reason.c_str());
  return phase.phase == commaview::road::RoadPhase::kDriving ? 1 : 0;
}

int run_control_mode(int argc, char* argv[]) {
  int port = kDefaultApiPort;
  for (int i = 2; i < argc; i++) {
    if (std::strcmp(argv[i], "--port") == 0 && (i + 1) < argc) {
      port = std::atoi(argv[i + 1]);
      i++;
    }
  }

  const std::string api_token = load_api_token();

  commaview::api::HttpServer server(port, [api_token](const commaview::api::HttpRequest& req) {
    return handle_request(req, api_token);
  });

  std::string err;
  if (!server.start(&err)) {
    std::fprintf(stderr, "commaviewd control: failed to start server on :%d (%s)\n", port, err.c_str());
    return 2;
  }

  std::printf("commaviewd control: listening on :%d\n", port);
  std::fflush(stdout);
  start_discovery_responder();
  start_drive_watcher();
  start_process_watcher();

  server.serve_forever();
  return 0;
}

}  // namespace commaview::runtime
