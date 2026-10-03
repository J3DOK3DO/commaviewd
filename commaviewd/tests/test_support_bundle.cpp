// The support bundle's redaction, openpilot swaglog scan and kernel OOM filter: private details
// never leave the comma, the newest matching lines are kept within their caps, and nothing outside
// openpilot's swaglog files is read.
#include "support_bundle.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace {

using commaview::support::RedactionSecrets;
using commaview::support::redact_support_text;

int failures = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    failures++;
  }
}

void expect(const std::string& in, const std::string& want, const RedactionSecrets& secrets = {}) {
  const std::string got = redact_support_text(in, secrets);
  if (got != want) {
    std::fprintf(stderr, "FAIL: redact\n  in:   %s\n  got:  %s\n  want: %s\n", in.c_str(), got.c_str(), want.c_str());
    failures++;
  }
}

bool has(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

void write_text(const std::string& path, const std::string& text) {
  std::ofstream(path, std::ios::binary) << text;
}

void redaction() {
  RedactionSecrets secrets;
  secrets.api_token = "tok_8f2b1c9e7a6d5f4e3c2b1a0";
  secrets.dongle_id = "0123456789abcdef";
  secrets.hardware_serial = "c0ffee12";

  // This comma's own secrets, wherever they appear.
  expect("auth tok_8f2b1c9e7a6d5f4e3c2b1a0 used", "auth redacted-token used", secrets);
  expect("device comma-0123456789abcdef up", "device comma-******abcdef up", secrets);
  expect("serial c0ffee12.", "serial ******ffee12.", secrets);

  // Device identifiers by key, masked like the app masks them.
  expect("{\"dongle_id\": \"fedcba9876543210\", \"version\": \"0.10.1\"}",
         "{\"dongle_id\": \"******543210\", \"version\": \"0.10.1\"}");
  expect("{\"hardwareSerial\":\"abc123456\"}", "{\"hardwareSerial\":\"******123456\"}");
  expect("imei=356938035643809 ok", "imei=\"******643809\" ok");

  // GPS: keyed values (JSON stays valid), swaglog's typed keys, bare high-precision pairs.
  expect("{\"lat\":37.774929,\"lon\":-122.419416,\"accuracyM\":4.5}",
         "{\"lat\":\"redacted-gps\",\"lon\":\"redacted-gps\",\"accuracyM\":4.5}");
  expect("{\"latitude$f\": 37.7749, \"longitude$f\": -122.4194}",
         "{\"latitude$f\": \"redacted-gps\", \"longitude$f\": \"redacted-gps\"}");
  expect("fix lat=37.77 lon=-122.41 acc=3", "fix lat=\"redacted-gps\" lon=\"redacted-gps\" acc=3");
  expect("{\"msg$s\": \"{\\\"lat\\\": 37.123456}\"}", "{\"msg$s\": \"{\\\"lat\\\": \\\"redacted-gps\\\"}\"}");
  expect("{\"lat\": null}", "{\"lat\": null}");
  expect("position 37.774929, -122.419416 saved", "position redacted-gps, redacted-gps saved");
  expect("[37.774929,-122.419416]", "[\"redacted-gps\",\"redacted-gps\"]");
  expect("probs [0.12345, 0.67891]", "probs [0.12345, 0.67891]");
  expect("v 1.25, 2.5", "v 1.25, 2.5");

  // VINs, keyed or bare; other 17-character words stay.
  expect("{\"vin$s\": \"1HGCM82633A004352\"}", "{\"vin$s\": \"redacted-vin\"}");
  expect("VIN 1HGCM82633A004352 matched", "VIN redacted-vin matched");
  expect("car_vin_1HGCM82633A004352", "car_vin_redacted-vin");
  expect("ABCDEFGHJKLMNPRST", "ABCDEFGHJKLMNPRST");
  expect("commit 0d1e2f3a4b5c6d7e8f9a0b1c2d3e4f5a6b7c8d9e", "commit 0d1e2f3a4b5c6d7e8f9a0b1c2d3e4f5a6b7c8d9e");

  // Wi-Fi names, BSSIDs and MAC addresses.
  expect("{\"ssid\": \"Home WiFi\"}", "{\"ssid\": \"redacted-ssid\"}");
  expect("ssid=Home WiFi, rssi=-40", "ssid=redacted-ssid, rssi=-40");
  expect("GENERAL.CONNECTION:Cafe Guest\n", "GENERAL.CONNECTION:redacted-ssid\n");
  expect("802-11-wireless.ssid: MyNet", "802-11-wireless.ssid: redacted-ssid");
  expect("bssid=aa:bb:cc:dd:ee:ff", "bssid=redacted-ssid");
  expect("wlan0 peer 00:11:22:33:44:55 up", "wlan0 peer redacted-mac up");

  // IP addresses: private, loopback, link-local and netmasks stay; public ones go.
  expect("dns 8.8.8.8 ok", "dns redacted-ip ok");
  expect("phone 192.168.1.10:5002, comma 10.0.0.2, lan 172.16.4.4, lo 127.0.0.1, ll 169.254.1.2",
         "phone 192.168.1.10:5002, comma 10.0.0.2, lan 172.16.4.4, lo 127.0.0.1, ll 169.254.1.2");
  expect("mask 255.255.255.0 gw 172.32.0.1", "mask 255.255.255.0 gw redacted-ip");
  expect("not an ip 1.2.3.4.5 or v1.2.3.4", "not an ip 1.2.3.4.5 or v1.2.3.4");
  expect("v6 2001:4860:4860::8888 up", "v6 redacted-ip up");
  expect("v6 2001:0db8:85a3:0000:0000:8a2e:0370:7334", "v6 redacted-ip");
  expect("ll fe80::1c2:3ff:fe4:5%wlan0 ula fd12:3456:789a::1", "ll fe80::1c2:3ff:fe4:5%wlan0 ula fd12:3456:789a::1");
  expect("at 12:34:56 std::string a::b", "at 12:34:56 std::string a::b");
  expect("mapped ::ffff:192.168.0.5", "mapped ::ffff:192.168.0.5");

  // Credentials.
  expect("{\"apiToken\":\"abc123\"}", "{\"apiToken\":\"redacted-secret\"}");
  expect("X-CommaView-Token: abcdef123\n", "X-CommaView-Token: redacted-secret\n");
  expect("Authorization: Bearer abc.def.ghi\n", "Authorization: redacted-secret\n");
  expect("sent Bearer abcdefgh12345 to x", "sent Bearer redacted-token to x");
  expect("jwt eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxIn0.c2lnbmF0dXJl end", "jwt redacted-token end");
  expect("remote https://user:ghp_secret@github.com/me/op.git", "remote https://redacted@github.com/me/op.git");
  expect("GET /x?token=abc123&page=2", "GET /x?token=redacted-secret&page=2");
  expect("{\"password\": \"hunter2\", \"tokenRequired\": false, \"keyframeWaitDropCount\": 3}",
         "{\"password\": \"redacted-secret\", \"tokenRequired\": false, \"keyframeWaitDropCount\": 3}");

  // The leaderboard key: its file's contents, the seed by key name, and the seed verbatim anywhere.
  RedactionSecrets with_key;
  with_key.leaderboard_seed = "AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA";
  expect("{\"version\":1,\"seed\":\"AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA\",\"createdMs\":1790899200000}",
         "{\"version\":1,\"seed\":\"redacted-secret\",\"createdMs\":1790899200000}");
  expect("seed=_-z9aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa next", "seed=redacted-secret next");
  expect("oops AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA in a log", "oops redacted-secret in a log", with_key);
  expect("x=AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA", "x=redacted-secret", with_key);
  // The public key and counter are not secrets.
  expect("{\"publicKey\":\"ebVWLo_mVPlAeLES6KmLp5AfhTrmlb7X4OORC60ElmQ\",\"seq\":3}",
         "{\"publicKey\":\"ebVWLo_mVPlAeLES6KmLp5AfhTrmlb7X4OORC60ElmQ\",\"seq\":3}", with_key);

  // Everything else is untouched, and control characters can't break the JSON it's embedded in.
  expect("plain log line: started bridge on port 5001\n", "plain log line: started bridge on port 5001\n");
  expect(std::string("color \x1b[31mred\x07\n"), "color  [31mred \n");
  expect("", "");
}

void swaglog(const std::string& root) {
  using commaview::support::scan_swaglog;
  using commaview::support::SwaglogScanLimits;
  using commaview::support::swaglog_line_matches;

  check(swaglog_line_matches("{\"msg$s\": {\"event$s\": \"process_not_running\", \"not_running$a\": [\"modeld\"]}}"),
        "process_not_running matches");
  check(swaglog_line_matches("{\"msg$s\": \"modeld is dead with -9\"}"), "is dead with matches");
  check(swaglog_line_matches("{\"msg$s\": \"killing camerad with SIGKILL\"}"), "killing matches");
  check(!swaglog_line_matches("{\"msg$s\": \"modeld started\"}"), "other lines don't match");
  check(swaglog_line_matches("{\"msg\": \"starting process micd\", \"level\": \"INFO\", \"levelnum\": 20}"),
        "manager starting lines match");
  check(swaglog_line_matches("{\"msg\": \"Restarting soundd (exitcode -11)\", \"levelnum\": 40}"),
        "restart_if_crash lines match");
  check(swaglog_line_matches("{\"msg\": \"init_stream failed, trying again\"}") &&
            swaglog_line_matches("{\"msg\": \"init_stream failed after retry\"}"),
        "retry lines match");
  check(swaglog_line_matches("{\"msg\": \"PortAudioError: device unavailable\", \"ctx\": {\"daemon\": \"micd\"}, "
                             "\"level\": \"ERROR\", \"levelnum\": 40}"),
        "micd errors match");
  check(swaglog_line_matches("{\"msg\": \"stream gone\", \"ctx\": {\"daemon\": \"soundd\"}, \"levelnum\": 50}"),
        "soundd critical lines match");
  check(!swaglog_line_matches("{\"msg\": \"soundd stream over/underflow: input underflow\", \"ctx\": {\"daemon\": "
                              "\"soundd\"}, \"level\": \"WARNING\", \"levelnum\": 30}"),
        "audio daemons' warnings don't match");
  check(!swaglog_line_matches("{\"msg\": \"micd stream started\", \"ctx\": {\"daemon\": \"micd\"}, \"levelnum\": 20}"),
        "audio daemons' info lines don't match");
  check(!swaglog_line_matches("{\"msg\": \"error reading can\", \"ctx\": {\"daemon\": \"pandad\"}, \"levelnum\": 40}"),
        "other daemons' errors are not widened");

  RedactionSecrets secrets;
  secrets.dongle_id = "0123456789abcdef";
  const auto missing = scan_swaglog(root + "/no-such-dir", secrets);
  check(!missing.dir_exists && missing.text.empty(), "a missing swaglog dir is reported, not read");

  const std::string dir = root + "/log";
  mkdir(dir.c_str(), 0755);
  const std::string ctx = "\"ctx\": {\"dongle_id\": \"0123456789abcdef\", \"origin\": \"https://u:p@github.com/x\"}";
  write_text(dir + "/swaglog.0000000001", "{\"msg$s\": \"killing modeld\", " + ctx + "}\n{\"msg$s\": \"boring\"}\n");
  write_text(dir + "/swaglog.0000000002",
             "{\"msg$s\": \"noise\"}\n{\"msg$s\": {\"event$s\": \"process_not_running\", \"not_running$a\": [\"modeld\"]}, " +
                 ctx + "}\n{\"msg$s\": \"lat 37.774929, -122.419416\"}\n");
  write_text(dir + "/swaglog.0000000010", "{\"msg$s\": \"modeld is dead with -9\", \"host\": \"8.8.4.4\"}");
  write_text(dir + "/other.log", "killing should-not-be-read\n");
  write_text(root + "/outside.txt", "killing outside-the-log-dir\n");
  check(symlink((root + "/outside.txt").c_str(), (dir + "/swaglog.0000000099").c_str()) == 0, "symlink made");

  const auto scan = scan_swaglog(dir, secrets);
  check(scan.dir_exists, "swaglog dir found");
  check(scan.files_seen == 4, "swaglog.<index> names only (and the symlink by name): " + std::to_string(scan.files_seen));
  check(scan.files_scanned == 3, "the symlink is never followed: " + std::to_string(scan.files_scanned));
  check(scan.lines_matched == 3, "three matching lines: " + std::to_string(scan.lines_matched));
  check(!scan.truncated, "nothing capped");
  const size_t a = scan.text.find("killing modeld");
  const size_t b = scan.text.find("process_not_running");
  const size_t c = scan.text.find("is dead with -9");
  check(a != std::string::npos && b != std::string::npos && c != std::string::npos && a < b && b < c,
        "matching lines oldest first:\n" + scan.text);
  check(!has(scan.text, "boring") && !has(scan.text, "noise") && !has(scan.text, "37.774929"), "other lines left out");
  check(!has(scan.text, "should-not-be-read") && !has(scan.text, "outside-the-log-dir"), "only swaglog files are read");
  check(!has(scan.text, "0123456789abcdef") && has(scan.text, "******abcdef"), "dongle id masked");
  check(!has(scan.text, "u:p@") && !has(scan.text, "8.8.4.4"), "credentials and public IPs redacted");

  SwaglogScanLimits two_lines;
  two_lines.max_lines = 2;
  const auto newest_two = scan_swaglog(dir, secrets, two_lines);
  check(newest_two.truncated && !has(newest_two.text, "killing modeld") && has(newest_two.text, "is dead with"),
        "the line cap keeps the newest lines");

  SwaglogScanLimits one_file;
  one_file.max_files = 1;
  const auto newest_file = scan_swaglog(dir, secrets, one_file);
  check(newest_file.files_scanned == 1 && newest_file.truncated && has(newest_file.text, "is dead with") &&
            !has(newest_file.text, "process_not_running"),
        "the file cap reads the newest file only");

  SwaglogScanLimits short_lines;
  short_lines.max_line_bytes = 24;
  const auto clipped = scan_swaglog(dir, secrets, short_lines);
  check(has(clipped.text, "...[line truncated]") && !has(clipped.text, "0123456789abcdef"),
        "long lines are cut after redaction");

  SwaglogScanLimits tiny_scan;
  tiny_scan.max_scan_bytes = 40;
  const auto tail_only = scan_swaglog(dir, secrets, tiny_scan);
  check(tail_only.truncated && tail_only.bytes_scanned <= 40, "the byte budget bounds what is read");

  // By time: files modified in the last 24 h, newest modification first, whatever their index.
  const std::string timed = root + "/timed";
  mkdir(timed.c_str(), 0755);
  const int64_t now = 2000000000;
  const auto touch = [&](const std::string& name, const std::string& body, int64_t age_sec) {
    write_text(timed + "/" + name, body);
    timespec times[2] = {{static_cast<time_t>(now - age_sec), 0}, {static_cast<time_t>(now - age_sec), 0}};
    check(utimensat(AT_FDCWD, (timed + "/" + name).c_str(), times, 0) == 0, "mtime set");
  };
  touch("swaglog.0000000001", "{\"msg\": \"micd is dead with -9 (yesterday)\"}\n", 30 * 3600);
  touch("swaglog.0000000002", "{\"msg\": \"starting process micd\"}\n", 3 * 3600);
  touch("swaglog.0000000003", "{\"msg\": \"killing soundd\"}\n", 2 * 3600);
  // A higher index written earlier (a clock jump): the modification time decides.
  touch("swaglog.0000000004", "{\"msg\": \"init_stream failed, trying again\"}\n", 4 * 3600);
  SwaglogScanLimits day;
  day.now_unix_sec = now;
  const auto recent = scan_swaglog(timed, secrets, day);
  check(recent.files_seen == 4 && recent.files_recent == 3 && recent.files_scanned == 3 && recent.lines_matched == 3,
        "only files from the last 24 h are read: seen " + std::to_string(recent.files_seen) + ", recent " +
            std::to_string(recent.files_recent));
  check(!has(recent.text, "yesterday"), "an older file is left out");
  const size_t retry = recent.text.find("trying again");
  const size_t starting = recent.text.find("starting process micd");
  const size_t killing = recent.text.find("killing soundd");
  check(retry != std::string::npos && starting != std::string::npos && killing != std::string::npos &&
            retry < starting && starting < killing,
        "oldest modification first in the output:\n" + recent.text);
  SwaglogScanLimits newest_only = day;
  newest_only.max_files = 1;
  const auto one = scan_swaglog(timed, secrets, newest_only);
  check(one.files_scanned == 1 && has(one.text, "killing soundd") && one.truncated,
        "the newest by modification time is read first");
  SwaglogScanLimits week = day;
  week.max_age_sec = 7 * 24 * 3600;
  check(scan_swaglog(timed, secrets, week).files_recent == 4, "the window is a limit");
}

void kernel() {
  using commaview::support::filter_kernel_log;
  using commaview::support::kernel_line_matches;
  using commaview::support::KernelLogLimits;

  check(kernel_line_matches("<3>[ 812.1] Out of memory: Killed process 4242 (python3) total-vm:1000kB"), "OOM line");
  check(kernel_line_matches("<6>[ 812.0] modeld invoked oom-killer: gfp_mask=0x6200ca"), "oom-killer line");
  check(kernel_line_matches("<6>[ 812.2] oom_reaper: reaped process 4242 (python3)"), "oom_reaper line");
  check(kernel_line_matches("<6>[ 812.3] lowmemorykiller: Killing 'ui' (77), adj 900"), "lowmemorykiller line");
  check(kernel_line_matches("[  pid  ]   uid  tgid total_vm      rss oom_score_adj name"), "OOM table header");
  check(!kernel_line_matches("<6>[   1.0] usb 1-1: new high-speed USB device"), "other kernel lines");
  check(!kernel_line_matches("bathroom zoom broom"), "oom inside other words");

  const std::string log =
      "<6>[ 1.0] boot\n"
      "<3>[ 2.0] Out of memory: Killed process 1 (a) from 8.8.8.8\n"
      "<6>[ 3.0] wlan0: associated with aa:bb:cc:dd:ee:ff\n"
      "<3>[ 4.0] Out of memory: Killed process 2 (b)\n"
      "<3>[ 5.0] Out of memory: Killed process 3 (c)\n";
  size_t matched = 0;
  bool truncated = false;
  const std::string all = filter_kernel_log(log, {}, KernelLogLimits(), &matched, &truncated);
  check(matched == 3 && !truncated, "three OOM lines");
  check(!has(all, "boot") && !has(all, "associated"), "only OOM lines kept");
  check(!has(all, "8.8.8.8") && has(all, "redacted-ip"), "kernel lines are redacted");
  KernelLogLimits two;
  two.max_lines = 2;
  const std::string newest = filter_kernel_log(log, {}, two, &matched, &truncated);
  check(truncated && !has(newest, "process 1 ") && has(newest, "process 2 ") && has(newest, "process 3 ") &&
            newest.find("process 2 ") < newest.find("process 3 "),
        "the newest OOM lines, oldest first");

  // The real ring buffer: either readable or a reason, never a crash (CI containers often refuse).
  std::string error;
  const std::string ring = commaview::support::read_kernel_log(64 * 1024, &error);
  check(ring.size() <= 64 * 1024, "the kernel log read is capped");
  check(!ring.empty() || !error.empty() || ring.empty(), "kernel log read returns");
}

}  // namespace

int main() {
  char dir_template[] = "/tmp/commaview-support-bundle-XXXXXX";
  const std::string root = mkdtemp(dir_template);

  redaction();
  swaglog(root);
  kernel();

  const std::string cleanup = "rm -rf '" + root + "'";
  if (std::system(cleanup.c_str()) != 0) std::fprintf(stderr, "warning: could not remove %s\n", root.c_str());
  if (failures == 0) std::printf("PASS: support bundle redaction, swaglog scan and kernel OOM filter\n");
  return failures == 0 ? 0 : 1;
}
