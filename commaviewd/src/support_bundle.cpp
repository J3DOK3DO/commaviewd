#include "support_bundle.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/klog.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <utility>
#include <vector>

namespace commaview::support {
namespace {

constexpr size_t kMaxValueBytes = 4096;
constexpr size_t kMaxKeyBytes = 64;

bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alnum(char c) { return is_alpha(c) || is_digit(c); }
bool is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool is_word(char c) { return is_alnum(c) || c == '_'; }
bool is_key_char(char c) { return is_word(c) || c == '-' || c == '.' || c == '$'; }
char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lower_copy(const std::string& in) {
  std::string out = in;
  for (char& c : out) c = to_lower(c);
  return out;
}

std::string trim(const std::string& in) {
  size_t b = 0, e = in.size();
  while (b < e && (in[b] == ' ' || in[b] == '\t' || in[b] == '\r' || in[b] == '\n')) b++;
  while (e > b && (in[e - 1] == ' ' || in[e - 1] == '\t' || in[e - 1] == '\r' || in[e - 1] == '\n')) e--;
  return in.substr(b, e - b);
}

// needle must be lower case.
bool contains_ci(const std::string& haystack, const char* needle) {
  const size_t m = std::strlen(needle);
  if (m == 0) return true;
  if (haystack.size() < m) return false;
  for (size_t i = 0; i + m <= haystack.size(); i++) {
    size_t k = 0;
    while (k < m && to_lower(haystack[i + k]) == needle[k]) k++;
    if (k == m) return true;
  }
  return false;
}

bool starts_with_ci(const std::string& s, size_t at, const char* lower_prefix) {
  const size_t m = std::strlen(lower_prefix);
  if (at + m > s.size()) return false;
  for (size_t k = 0; k < m; k++) {
    if (to_lower(s[at + k]) != lower_prefix[k]) return false;
  }
  return true;
}

bool is_numeric(const std::string& v) {
  size_t i = 0;
  if (i < v.size() && (v[i] == '-' || v[i] == '+')) i++;
  bool digits = false, dot = false;
  for (; i < v.size(); i++) {
    if (is_digit(v[i])) {
      digits = true;
    } else if (v[i] == '.' && !dot) {
      dot = true;
    } else {
      return false;
    }
  }
  return digits;
}

bool is_bare_literal(const std::string& v) {
  const std::string l = lower_copy(v);
  return l == "true" || l == "false" || l == "null" || l == "none" || is_numeric(v);
}

// ---- Keyed values

enum class KeyKind { kNone, kGps, kVin, kSsid, kDeviceId, kSecret };

KeyKind classify_key(const std::string& raw_key) {
  std::string key = lower_copy(raw_key);
  // swaglog files suffix keys with their value's type: "latitude$f", "vin$s".
  const size_t dollar = key.rfind('$');
  if (dollar != std::string::npos) key.resize(dollar);
  // nmcli names the active connection after the network's SSID.
  if (key == "general.connection") return KeyKind::kSsid;
  const size_t dot = key.find_last_of('.');
  const std::string leaf = dot == std::string::npos ? key : key.substr(dot + 1);
  if (leaf.empty()) return KeyKind::kNone;
  for (const char* gps : {"lat", "lon", "lng", "latitude", "longitude", "latitude_deg", "longitude_deg"}) {
    if (leaf == gps) return KeyKind::kGps;
  }
  if (leaf == "vin" || leaf == "carvin" || leaf == "car_vin") return KeyKind::kVin;
  if (leaf.find("ssid") != std::string::npos) return KeyKind::kSsid;
  for (const char* id : {"dongle_id", "dongleid", "hardware_serial", "hardwareserial", "serial", "imei", "imsi", "iccid", "meid"}) {
    if (leaf == id) return KeyKind::kDeviceId;
  }
  for (const char* secret : {"token", "password", "passwd", "secret", "apikey", "api_key", "api-key", "authorization",
                             "cookie", "private_key", "privatekey", "credential", "seed"}) {
    if (leaf.find(secret) != std::string::npos) return KeyKind::kSecret;
  }
  return KeyKind::kNone;
}

// The key a ':' or '=' at sep belongs to: `"key": `, `\"key\":`, `'key':`, `key=`, `key: `.
// *escaped says the key was quoted as \"key\" (a JSON document inside a JSON string).
std::string key_before(const std::string& s, size_t sep, bool* escaped) {
  *escaped = false;
  size_t j = sep;
  while (j > 0 && (s[j - 1] == ' ' || s[j - 1] == '\t') && sep - j < 4) j--;
  if (j > 0 && (s[j - 1] == '"' || s[j - 1] == '\'')) {
    j--;
    if (j > 0 && s[j - 1] == '\\') {
      j--;
      *escaped = true;
    }
  }
  const size_t end = j;
  while (j > 0 && is_key_char(s[j - 1]) && end - j < kMaxKeyBytes) j--;
  return s.substr(j, end - j);
}

bool bare_value_end(char c, KeyKind kind) {
  switch (c) {
    case '\n': case '\r': case ',': case ';': case '&': case '}': case ']': case '"': case '\'':
      return true;
    case ' ': case '\t': case ')': case '<': case '>':
      // SSIDs can hold spaces: take the rest of the field rather than leak its tail.
      return kind != KeyKind::kSsid;
    default:
      return false;
  }
}

// Replaces the value after the ':' or '=' at sep when its key names something private. Returns
// how many input bytes it consumed (0: nothing to do, the caller copies the separator).
size_t redact_key_value(const std::string& s, size_t sep, std::string* out) {
  bool escaped_key = false;
  const KeyKind kind = classify_key(key_before(s, sep, &escaped_key));
  if (kind == KeyKind::kNone) return 0;
  const size_t n = s.size();
  size_t p = sep + 1;
  while (p < n && (s[p] == ' ' || s[p] == '\t') && p - sep <= 4) p++;
  if (p >= n || s[p] == '\n' || s[p] == '\r' || s[p] == '{' || s[p] == '[') return 0;

  bool quoted = false;
  size_t vbegin = p, vend = p, after = p;
  if (s[p] == '\\' && p + 1 < n && s[p + 1] == '"') {
    // A JSON document escaped inside a JSON string: \"key\": \"value\"
    quoted = true;
    vbegin = p + 2;
    size_t k = vbegin;
    while (k < n && s[k] != '\n' && k - vbegin < kMaxValueBytes && !(s[k] == '\\' && k + 1 < n && s[k + 1] == '"')) k++;
    vend = k;
    after = (k + 1 < n && s[k] == '\\' && s[k + 1] == '"') ? k + 2 : k;
  } else if (s[p] == '"' || s[p] == '\'') {
    quoted = true;
    const char q = s[p];
    vbegin = p + 1;
    size_t k = vbegin;
    while (k < n && s[k] != q && s[k] != '\n' && k - vbegin < kMaxValueBytes) k += (s[k] == '\\' && k + 1 < n) ? 2 : 1;
    vend = std::min(k, n);
    after = (vend < n && s[vend] == q) ? vend + 1 : vend;
  } else {
    size_t k = p;
    while (k < n && !bare_value_end(s[k], kind) && k - p < kMaxValueBytes) k++;
    // "Authorization: Bearer abc" - the credential is the word after the scheme.
    if (kind == KeyKind::kSecret && k < n && s[k] == ' ') {
      const std::string scheme = lower_copy(s.substr(p, k - p));
      if (scheme == "bearer" || scheme == "basic" || scheme == "token" || scheme == "digest") {
        k++;
        while (k < n && !bare_value_end(s[k], kind) && k - p < kMaxValueBytes) k++;
      }
    }
    while (k > p && (s[k - 1] == ' ' || s[k - 1] == '\t')) k--;
    vend = after = k;
  }
  const std::string value = s.substr(vbegin, vend - vbegin);
  if (trim(value).empty()) return 0;

  std::string replacement;
  switch (kind) {
    case KeyKind::kGps:
      if (!quoted && !is_numeric(value)) return 0;
      replacement = "redacted-gps";
      break;
    case KeyKind::kVin:
      if (!quoted && is_bare_literal(value) && !is_numeric(value)) return 0;
      replacement = "redacted-vin";
      break;
    case KeyKind::kSsid:
      replacement = "redacted-ssid";
      break;
    case KeyKind::kDeviceId:
      if (!quoted && is_bare_literal(value) && !is_numeric(value)) return 0;
      replacement = mask_identifier(value);
      break;
    case KeyKind::kSecret:
      if (!quoted && is_bare_literal(value)) return 0;
      replacement = "redacted-secret";
      break;
    case KeyKind::kNone:
      return 0;
  }
  out->append(s, sep, vbegin - sep);  // separator, spaces, opening quote
  if (!quoted && is_numeric(value)) {
    // A JSON number becomes a JSON string, so the document stays valid.
    const char* quote = escaped_key ? "\\\"" : "\"";
    *out += quote;
    *out += replacement;
    *out += quote;
  } else {
    *out += replacement;
  }
  out->append(s, vend, after - vend);  // closing quote
  return after - sep;
}

// ---- Shapes that are private wherever they appear

size_t match_mac(const std::string& s, size_t i) {
  if (i > 0 && (is_word(s[i - 1]) || s[i - 1] == ':' || s[i - 1] == '-')) return 0;
  if (i + 17 > s.size()) return 0;
  const char sep = s[i + 2];
  if (sep != ':' && sep != '-') return 0;
  for (size_t k = 0; k < 6; k++) {
    if (!is_hex(s[i + 3 * k]) || !is_hex(s[i + 3 * k + 1])) return 0;
    if (k < 5 && s[i + 3 * k + 2] != sep) return 0;
  }
  if (i + 17 < s.size() && (is_word(s[i + 17]) || s[i + 17] == ':' || s[i + 17] == '-')) return 0;
  return 17;
}

bool ipv4_is_private(int a, int b) {
  return a == 10 || a == 127 || a == 0 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
         (a == 169 && b == 254) || a >= 224;
}

// Returns the length of the address at i (0: none); *keep says whether it stays.
size_t match_ipv4(const std::string& s, size_t i, bool* keep) {
  if (i > 0 && (is_word(s[i - 1]) || s[i - 1] == '.')) return 0;
  int octets[4] = {0, 0, 0, 0};
  size_t j = i;
  for (int k = 0; k < 4; k++) {
    if (k > 0) {
      if (j >= s.size() || s[j] != '.') return 0;
      j++;
    }
    const size_t start = j;
    int value = 0;
    while (j < s.size() && is_digit(s[j]) && j - start < 3) value = value * 10 + (s[j++] - '0');
    if (j == start || value > 255) return 0;
    octets[k] = value;
  }
  if (j < s.size() && (is_word(s[j]) || (s[j] == '.' && j + 1 < s.size() && is_digit(s[j + 1])))) return 0;
  *keep = ipv4_is_private(octets[0], octets[1]);
  return j - i;
}

size_t match_ipv6(const std::string& s, size_t i, bool* keep) {
  if (i > 0 && (is_word(s[i - 1]) || s[i - 1] == ':' || s[i - 1] == '.')) return 0;
  size_t j = i;
  size_t colons = 0;
  while (j < s.size() && j - i < 40 && (is_hex(s[j]) || s[j] == ':')) {
    if (s[j] == ':') colons++;
    j++;
  }
  if (colons < 2) return 0;
  // Leave an embedded IPv4 tail (::ffff:1.2.3.4) and longer words to the other rules.
  if (j < s.size() && (is_word(s[j]) || s[j] == '.')) return 0;
  const std::string run = s.substr(i, j - i);
  if (run.find(":::") != std::string::npos) return 0;
  const size_t dbl = run.find("::");
  if (dbl != std::string::npos && run.find("::", dbl + 1) != std::string::npos) return 0;
  std::vector<std::string> groups;
  size_t start = 0;
  while (true) {
    const size_t colon = run.find(':', start);
    groups.push_back(run.substr(start, colon == std::string::npos ? std::string::npos : colon - start));
    if (colon == std::string::npos) break;
    start = colon + 1;
  }
  size_t nonempty = 0;
  size_t digits = 0;
  for (const auto& g : groups) {
    if (g.size() > 4) return 0;
    if (!g.empty()) nonempty++;
    digits += g.size();
  }
  const bool full = dbl == std::string::npos && groups.size() == 8 && nonempty == 8;
  // "2001::1" is an address; "a::b" is more likely a C++ name.
  const bool compressed = dbl != std::string::npos && groups.size() <= 9 &&
                          (nonempty >= 3 || (nonempty == 2 && digits >= 5));
  if (!full && !compressed) return 0;
  const std::string first = lower_copy(groups.front());
  *keep = (first.size() == 4 && (first.compare(0, 3, "fe8") == 0 || first.compare(0, 3, "fe9") == 0 ||
                                 first.compare(0, 3, "fea") == 0 || first.compare(0, 3, "feb") == 0 ||
                                 first.compare(0, 2, "fc") == 0 || first.compare(0, 2, "fd") == 0 ||
                                 first.compare(0, 2, "ff") == 0)) ||
          (groups.front().empty() && nonempty <= 1);
  return j - i;
}

// A signed decimal with 1-3 integer digits and at least min_decimals decimals.
size_t match_decimal(const std::string& s, size_t i, size_t min_decimals, double* value) {
  size_t j = i;
  if (j < s.size() && s[j] == '-') j++;
  const size_t int_start = j;
  while (j < s.size() && is_digit(s[j]) && j - int_start < 4) j++;
  if (j == int_start || j - int_start > 3 || j >= s.size() || s[j] != '.') return 0;
  j++;
  const size_t dec_start = j;
  while (j < s.size() && is_digit(s[j])) j++;
  if (j - dec_start < min_decimals) return 0;
  *value = std::strtod(s.c_str() + i, nullptr);
  return j - i;
}

// "37.774929, -122.419416": a bare latitude, longitude pair at GPS precision.
size_t match_gps_pair(const std::string& s, size_t i, std::string* out) {
  if (i > 0 && (is_word(s[i - 1]) || s[i - 1] == '.' || s[i - 1] == '-')) return 0;
  double lat = 0.0, lon = 0.0;
  const size_t a = match_decimal(s, i, 5, &lat);
  if (a == 0 || std::fabs(lat) > 90.0) return 0;
  size_t j = i + a;
  const size_t sep_start = j;
  while (j < s.size() && s[j] == ' ' && j - sep_start < 2) j++;
  if (j >= s.size() || s[j] != ',') return 0;
  j++;
  while (j < s.size() && s[j] == ' ' && j - sep_start < 5) j++;
  const size_t b = match_decimal(s, j, 5, &lon);
  if (b == 0 || std::fabs(lon) > 180.0 || (std::fabs(lat) < 1.0 && std::fabs(lon) < 1.0)) return 0;
  const size_t end = j + b;
  if (end < s.size() && (is_word(s[end]) || s[end] == '.')) return 0;
  // Inside a JSON array the numbers become strings; elsewhere plain words.
  size_t prev = i;
  while (prev > 0 && s[prev - 1] == ' ') prev--;
  const bool json_array = prev > 0 && (s[prev - 1] == '[' || s[prev - 1] == ',');
  const char* label = json_array ? "\"redacted-gps\"" : "redacted-gps";
  *out += label;
  out->append(s, i + a, j - (i + a));
  *out += label;
  return end - i;
}

bool vin_char(char c) {
  return is_digit(c) || (c >= 'A' && c <= 'Z' && c != 'I' && c != 'O' && c != 'Q');
}

size_t match_vin(const std::string& s, size_t i) {
  if (i > 0 && is_alnum(s[i - 1])) return 0;
  if (i + 17 > s.size()) return 0;
  int letters = 0, digits = 0;
  for (size_t k = 0; k < 17; k++) {
    const char c = s[i + k];
    if (!vin_char(c)) return 0;
    if (is_digit(c)) digits++; else letters++;
  }
  if (i + 17 < s.size() && is_alnum(s[i + 17])) return 0;
  return (letters >= 1 && digits >= 3) ? 17 : 0;
}

bool b64url_char(char c) { return is_alnum(c) || c == '-' || c == '_'; }

size_t match_jwt(const std::string& s, size_t i) {
  if (s.compare(i, 3, "eyJ") != 0) return 0;
  size_t j = i;
  for (int part = 0; part < 3; part++) {
    const size_t start = j;
    while (j < s.size() && b64url_char(s[j])) j++;
    if (part < 2) {
      if (j - start < 4 || j >= s.size() || s[j] != '.') return 0;
      j++;
    }
  }
  return j - i;
}

bool token_char(char c) {
  return is_alnum(c) || c == '.' || c == '_' || c == '~' || c == '+' || c == '/' || c == '=' || c == '-';
}

size_t match_bearer(const std::string& s, size_t i, std::string* out) {
  if (!starts_with_ci(s, i, "bearer ")) return 0;
  size_t j = i + 7;
  while (j < s.size() && s[j] == ' ') j++;
  const size_t token_start = j;
  while (j < s.size() && token_char(s[j])) j++;
  if (j - token_start < 8) return 0;
  out->append(s, i, token_start - i);
  *out += "redacted-token";
  return j - i;
}

// scheme://user:password@host - the user info goes, the host stays.
size_t match_url_credentials(const std::string& s, size_t i, std::string* out) {
  if (s.compare(i, 3, "://") != 0) return 0;
  size_t j = i + 3;
  while (j < s.size() && j - i < 256) {
    const char c = s[j];
    if (c == '@') break;
    if (c == '/' || c == '?' || c == '#' || c == ' ' || c == '"' || c == '\'' || c == '\n' || c == '\t' || c == '<' ||
        c == '>') {
      return 0;
    }
    j++;
  }
  if (j >= s.size() || s[j] != '@' || j == i + 3) return 0;
  *out += "://redacted@";
  return j + 1 - i;
}

std::string redact_line_capped(const std::string& line, const RedactionSecrets& secrets, size_t cap) {
  std::string out = redact_support_text(line, secrets);
  if (out.size() > cap) {
    out.resize(cap);
    out += " ...[line truncated]";
  }
  return out;
}

// Keeps the newest lines that fit max_bytes (each with its newline), oldest first.
std::string join_newest(const std::deque<std::string>& lines, size_t max_bytes, bool* truncated) {
  size_t total = 0;
  size_t first = lines.size();
  while (first > 0 && total + lines[first - 1].size() + 1 <= max_bytes) {
    total += lines[first - 1].size() + 1;
    first--;
  }
  if (first > 0 && truncated != nullptr) *truncated = true;
  std::string out;
  out.reserve(total);
  for (size_t k = first; k < lines.size(); k++) {
    out += lines[k];
    out += '\n';
  }
  return out;
}

bool is_swaglog_name(const char* name) {
  if (std::strncmp(name, "swaglog.", 8) != 0) return false;
  const char* index = name + 8;
  const size_t len = std::strlen(index);
  if (len == 0 || len > 20) return false;
  for (size_t k = 0; k < len; k++) {
    if (!is_digit(index[k])) return false;
  }
  return true;
}

// Newest bytes of a regular file, at most cap, starting at a whole line. Never follows a symlink.
bool read_regular_file_tail(const std::string& path, size_t cap, std::string* out, bool* cut) {
  out->clear();
  *cut = false;
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return false;
  struct stat st {};
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(fd);
    return false;
  }
  const size_t size = st.st_size > 0 ? static_cast<size_t>(st.st_size) : 0;
  const size_t want = std::min(size, cap);
  const off_t start = static_cast<off_t>(size - want);
  out->resize(want);
  size_t got = 0;
  while (got < want) {
    const ssize_t r = pread(fd, &(*out)[got], want - got, start + static_cast<off_t>(got));
    if (r <= 0) break;
    got += static_cast<size_t>(r);
  }
  out->resize(got);
  // Read once for support; don't push openpilot's working set out of the page cache for it.
  posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
  close(fd);
  if (start > 0) {
    *cut = true;
    const size_t nl = out->find('\n');
    out->erase(0, nl == std::string::npos ? out->size() : nl + 1);
  }
  return true;
}

}  // namespace

std::string mask_identifier(const std::string& id) {
  const std::string clean = trim(id);
  if (clean.empty()) return "unknown";
  return "******" + (clean.size() > 6 ? clean.substr(clean.size() - 6) : clean);
}

std::string redact_support_text(const std::string& in, const RedactionSecrets& secrets) {
  std::vector<std::pair<std::string, std::string>> literals;
  const auto add_literal = [&literals](const std::string& raw, std::string replacement) {
    const std::string value = trim(raw);
    if (value.size() >= 6) literals.emplace_back(value, std::move(replacement));
  };
  add_literal(secrets.api_token, "redacted-token");
  add_literal(secrets.leaderboard_seed, "redacted-secret");
  add_literal(secrets.dongle_id, mask_identifier(secrets.dongle_id));
  add_literal(secrets.hardware_serial, mask_identifier(secrets.hardware_serial));
  // Longest first, so a value holding another is replaced whole.
  std::sort(literals.begin(), literals.end(),
            [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });

  std::string out;
  out.reserve(in.size() + in.size() / 16 + 16);
  const size_t n = in.size();
  size_t i = 0;
  while (i < n) {
    const char c = in[i];

    bool replaced = false;
    for (const auto& literal : literals) {
      if (c == literal.first[0] && in.compare(i, literal.first.size(), literal.first) == 0) {
        out += literal.second;
        i += literal.first.size();
        replaced = true;
        break;
      }
    }
    if (replaced) continue;

    if (c == ':' || c == '=') {
      if (c == ':') {
        if (const size_t used = match_url_credentials(in, i, &out)) {
          i += used;
          continue;
        }
      }
      if (const size_t used = redact_key_value(in, i, &out)) {
        i += used;
        continue;
      }
    }

    const bool word_start = i == 0 || !is_word(in[i - 1]);
    if (word_start && (is_hex(c) || c == ':' || c == '-' || c == 'b' || c == 'B')) {
      size_t used = match_mac(in, i);
      if (used) {
        out += "redacted-mac";
        i += used;
        continue;
      }
      bool keep = false;
      used = match_ipv6(in, i, &keep);
      if (!used) used = match_ipv4(in, i, &keep);
      if (used) {
        if (keep) out.append(in, i, used); else out += "redacted-ip";
        i += used;
        continue;
      }
      if ((used = match_gps_pair(in, i, &out)) || (used = match_bearer(in, i, &out))) {
        i += used;
        continue;
      }
    }
    if (word_start || (i > 0 && in[i - 1] == '_')) {
      if (const size_t used = match_vin(in, i)) {
        out += "redacted-vin";
        i += used;
        continue;
      }
      if (const size_t used = match_jwt(in, i)) {
        out += "redacted-token";
        i += used;
        continue;
      }
    }

    const unsigned char uc = static_cast<unsigned char>(c);
    if ((uc < 0x20 && c != '\n' && c != '\t' && c != '\r') || uc == 0x7f) {
      out.push_back(' ');
    } else {
      out.push_back(c);
    }
    i++;
  }
  return out;
}

bool swaglog_line_matches(const std::string& line) {
  // Lowered once, then searched with memmem: a support bundle scans megabytes of these.
  thread_local std::string lower;
  lower.assign(line);
  for (char& c : lower) c = to_lower(c);
  const auto has = [](const char* needle) {
    return memmem(lower.data(), lower.size(), needle, std::strlen(needle)) != nullptr;
  };
  for (const char* needle : {"process_not_running", "is dead with", "killing", "starting process ", "starting python ",
                             "starting daemon ", "restarting ", "failed, trying again", "failed after retry"}) {
    if (has(needle)) return true;
  }
  // The audio daemons (micd, soundd): their errors, not their routine info and warnings.
  if (!has("micd") && !has("soundd")) return false;
  const char* levelnum = static_cast<const char*>(memmem(lower.data(), lower.size(), "\"levelnum\"", 10));
  if (levelnum != nullptr) {
    const char* p = levelnum + 10;
    const char* end = lower.data() + lower.size();
    while (p < end && (*p == ':' || *p == ' ')) p++;
    int level = 0;
    while (p < end && is_digit(*p) && level < 1000) level = level * 10 + (*p++ - '0');
    if (level >= 40) return true;
  }
  return has("error") || has("exception") || has("traceback") || has("failed");
}

namespace {

struct SwaglogFile {
  std::string name;
  int64_t mtime_ns = 0;
};

}  // namespace

SwaglogScan scan_swaglog(const std::string& dir, const RedactionSecrets& secrets, const SwaglogScanLimits& limits) {
  SwaglogScan scan;
  DIR* d = opendir(dir.c_str());
  if (d == nullptr) return scan;
  scan.dir_exists = true;
  const int64_t now_sec = limits.now_unix_sec > 0 ? limits.now_unix_sec : static_cast<int64_t>(std::time(nullptr));
  const int64_t oldest_ns = (now_sec - limits.max_age_sec) * 1000000000LL;
  std::vector<SwaglogFile> files;
  while (const dirent* entry = readdir(d)) {
    if (!is_swaglog_name(entry->d_name)) continue;
    scan.files_seen++;
    struct stat st {};
    if (fstatat(dirfd(d), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
    const int64_t mtime_ns = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
    if (mtime_ns < oldest_ns) continue;
    files.push_back({entry->d_name, mtime_ns});
  }
  closedir(d);
  scan.files_recent = files.size();
  // Newest first: by modification time, then by index (it only grows; zero-padded, but compared
  // as numbers anyway).
  std::sort(files.begin(), files.end(), [](const SwaglogFile& a, const SwaglogFile& b) {
    if (a.mtime_ns != b.mtime_ns) return a.mtime_ns > b.mtime_ns;
    return a.name.size() != b.name.size() ? a.name.size() > b.name.size() : a.name > b.name;
  });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(limits.max_scan_ms);
  std::vector<std::vector<std::string>> per_file;  // newest file first, each file's lines in order
  size_t kept = 0;
  for (const auto& file : files) {
    const std::string& name = file.name;
    if (kept >= limits.max_lines || scan.files_scanned >= limits.max_files ||
        scan.bytes_scanned >= limits.max_scan_bytes || std::chrono::steady_clock::now() >= deadline) {
      scan.truncated = true;
      break;
    }
    std::string body;
    bool cut = false;
    if (!read_regular_file_tail(dir + "/" + name, limits.max_scan_bytes - scan.bytes_scanned, &body, &cut)) continue;
    if (cut) scan.truncated = true;
    scan.files_scanned++;
    scan.bytes_scanned += body.size();
    std::vector<std::string> matches;
    size_t pos = 0;
    while (pos < body.size()) {
      size_t nl = body.find('\n', pos);
      if (nl == std::string::npos) nl = body.size();
      const std::string line = body.substr(pos, nl - pos);
      if (swaglog_line_matches(line)) matches.push_back(line);
      pos = nl + 1;
    }
    scan.lines_matched += matches.size();
    kept += matches.size();
    per_file.push_back(std::move(matches));
  }

  std::deque<std::string> lines;
  for (auto it = per_file.rbegin(); it != per_file.rend(); ++it) {
    for (auto& line : *it) lines.push_back(std::move(line));
  }
  while (lines.size() > limits.max_lines) {
    lines.pop_front();
    scan.truncated = true;
  }
  for (auto& line : lines) line = redact_line_capped(line, secrets, limits.max_line_bytes);
  scan.text = join_newest(lines, limits.max_output_bytes, &scan.truncated);
  return scan;
}

bool kernel_line_matches(const std::string& line) {
  if (contains_ci(line, "out of memory") || contains_ci(line, "oom-kill") || contains_ci(line, "oom_kill") ||
      contains_ci(line, "oom_reaper") || contains_ci(line, "killed process") || contains_ci(line, "lowmemorykiller")) {
    return true;
  }
  // "oom" as a word of its own ("invoked oom-killer", "oom_score_adj"), not inside "room" or "zoom".
  for (size_t at = 0; (at = line.find("oom", at)) != std::string::npos; at += 3) {
    const bool before = at == 0 || !is_alnum(line[at - 1]);
    const bool after = at + 3 >= line.size() || !is_alnum(line[at + 3]);
    if (before && after) return true;
  }
  return false;
}

std::string filter_kernel_log(const std::string& log, const RedactionSecrets& secrets, const KernelLogLimits& limits,
                              size_t* matched, bool* truncated) {
  if (matched != nullptr) *matched = 0;
  if (truncated != nullptr) *truncated = false;
  std::deque<std::string> lines;
  size_t pos = 0;
  while (pos < log.size()) {
    size_t nl = log.find('\n', pos);
    if (nl == std::string::npos) nl = log.size();
    const std::string line = log.substr(pos, nl - pos);
    if (kernel_line_matches(line)) {
      if (matched != nullptr) (*matched)++;
      lines.push_back(line);
      if (lines.size() > limits.max_lines) {
        lines.pop_front();
        if (truncated != nullptr) *truncated = true;
      }
    }
    pos = nl + 1;
  }
  for (auto& line : lines) line = redact_line_capped(line, secrets, limits.max_line_bytes);
  return join_newest(lines, limits.max_output_bytes, truncated);
}

std::string read_kernel_log(size_t max_bytes, std::string* error) {
  if (error != nullptr) error->clear();
  // 10 = SYSLOG_ACTION_SIZE_BUFFER, 3 = SYSLOG_ACTION_READ_ALL: what plain `dmesg` does. Never
  // 4 (READ_CLEAR) or 5 (CLEAR): the ring is the kernel's and openpilot's, not ours to empty.
  int size = klogctl(10, nullptr, 0);
  if (size <= 0) size = 1 << 20;
  size = std::min(size, 16 << 20);
  std::string buf(static_cast<size_t>(size), '\0');
  const int got = klogctl(3, &buf[0], size);
  if (got < 0) {
    if (error != nullptr) *error = std::strerror(errno);
    return "";
  }
  buf.resize(static_cast<size_t>(got));
  if (buf.size() > max_bytes) {
    buf.erase(0, buf.size() - max_bytes);
    const size_t nl = buf.find('\n');
    buf.erase(0, nl == std::string::npos ? buf.size() : nl + 1);
  }
  return buf;
}

}  // namespace commaview::support
