#pragma once

// What a support bundle (GET /commaview/support/logs) adds beside CommaView's own log files, and
// the redaction every part of it goes through. All of it is read only when the app asks for a
// bundle, which it does only when the user taps Share; nothing here runs in the background or
// writes anywhere.

#include <cstddef>
#include <cstdint>
#include <string>

namespace commaview::support {

// Values this comma knows are private, replaced wherever they appear verbatim.
struct RedactionSecrets {
  std::string api_token;
  std::string dongle_id;
  std::string hardware_serial;
  // The leaderboard key's seed (data/leaderboard-key.json). The bundle never reads that file; this
  // takes the seed out of anything that does end up holding it.
  std::string leaderboard_seed;
};

// An identifier masked the way the app's shared diagnostics mask one: only its last six
// characters stay ("••••••" is spelled "******" here to keep the runtime's text ASCII).
std::string mask_identifier(const std::string& id);

// Support text with private details taken out, line structure kept:
//  - GPS coordinates (lat/lon/latitude/longitude values, and bare high-precision "lat, lon" pairs)
//  - VINs (a "vin" value, or any 17-character VIN-shaped token)
//  - the dongle id and hardware serial (masked to their last six, verbatim or as a value)
//  - Wi-Fi SSIDs (any *ssid* value, nmcli's GENERAL.CONNECTION) and MAC addresses / BSSIDs
//  - IP addresses outside private, loopback and link-local ranges (IPv4 and IPv6)
//  - credentials: the API token and the leaderboard key's seed verbatim, any value whose key names a
//    token, password, secret, key, seed or cookie, bearer tokens, JWTs and user:password@ in URLs
// Control characters other than newline and tab become spaces so the text embeds in JSON.
// Linear in the input.
std::string redact_support_text(const std::string& text, const RedactionSecrets& secrets);

// openpilot's swaglog lines that tell why a process stopped or never ran:
//  - selfdrived's process_not_running event
//  - manager's "starting process|python|daemon <proc>", "killing <proc>", "<proc> is dead with <code>"
//    and "Restarting <proc> (exitcode ...)"
//  - common.utils.retry's "<fn> failed, trying again" / "failed after retry" (where a fork logs them)
//  - the audio daemons' (micd, soundd) error and exception lines (levelnum 40 and up, or naming an
//    error, exception, traceback or failure)
bool swaglog_line_matches(const std::string& line);

struct SwaglogScanLimits {
  // Files modified within this long before now (swaglog rolls over every minute or 256 KiB).
  int64_t max_age_sec = 24 * 60 * 60;
  int64_t now_unix_sec = 0;              // 0: the clock's now (tests pin it)
  size_t max_files = 2500;               // logmessaged keeps at most 2500
  size_t max_scan_bytes = 16 * 1024 * 1024;
  int max_scan_ms = 1500;
  size_t max_lines = 200;                // newest matching lines kept
  size_t max_line_bytes = 2048;          // per line, after redaction
  size_t max_output_bytes = 64 * 1024;
};

struct SwaglogScan {
  std::string text;  // matching lines, oldest first, each redacted then capped
  bool dir_exists = false;
  size_t files_seen = 0;
  size_t files_recent = 0;  // modified within max_age_sec
  size_t files_scanned = 0;
  size_t bytes_scanned = 0;
  size_t lines_matched = 0;
  bool truncated = false;  // a limit stopped the scan or cut the output
};

// The newest matching lines of openpilot's swaglog files (dir/swaglog.<index>) modified within
// max_age_sec, read newest first (by modification time, then index) within the limits. Files are
// opened read-only, never followed through a symlink, and dropped from the page cache after reading.
SwaglogScan scan_swaglog(const std::string& dir, const RedactionSecrets& secrets,
                         const SwaglogScanLimits& limits = SwaglogScanLimits());

// Kernel log lines about memory pressure kills: out of memory, the OOM killer and reaper,
// "Killed process", Android's lowmemorykiller.
bool kernel_line_matches(const std::string& line);

struct KernelLogLimits {
  size_t max_lines = 100;
  size_t max_line_bytes = 512;
  size_t max_output_bytes = 32 * 1024;
};

// The newest matching lines of a kernel log dump, oldest first, each redacted then capped.
std::string filter_kernel_log(const std::string& log, const RedactionSecrets& secrets,
                              const KernelLogLimits& limits, size_t* matched, bool* truncated);

// The kernel ring buffer as dmesg reads it (SYSLOG_ACTION_READ_ALL: read-only, never cleared,
// never blocks), at most its newest max_bytes. Empty with *error set when the kernel refuses.
std::string read_kernel_log(size_t max_bytes, std::string* error);

}  // namespace commaview::support
