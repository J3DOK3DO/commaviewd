#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONTROL_CPP="$ROOT/src/control_mode.cpp"

fail() { echo "FAIL: $*" >&2; exit 1; }
assert_contains_fixed() {
  local needle="$1" file="$2" message="$3"
  grep -Fq "$needle" "$file" || fail "$message"
}

assert_contains_fixed 'constexpr int kDiscoveryPort = 5004;' "$CONTROL_CPP" 'control runtime should reserve UDP discovery port 5004'
assert_contains_fixed 'constexpr const char* kDiscoveryQuery = "COMMAVIEW_DISCOVER_V1";' "$CONTROL_CPP" 'discovery query contract missing'
assert_contains_fixed '\"type\":\"commaview.discovery.v1\"' "$CONTROL_CPP" 'discovery response type missing'
assert_contains_fixed '\"apiPort\":' "$CONTROL_CPP" 'discovery response should include API port'
assert_contains_fixed '\"videoPorts\":{\"road\":8200,\"wide\":8201,\"driver\":8202}' "$CONTROL_CPP" 'discovery response should include stream ports'
awk '/std::string discovery_response_json\(\)/,/void discovery_responder_loop/' "$CONTROL_CPP" | grep -Fq '\"runtimeVersion\"' || fail 'discovery response should include runtimeVersion compatibility alias'
awk '/std::string discovery_response_json\(\)/,/void discovery_responder_loop/' "$CONTROL_CPP" | grep -Fq '\"deviceType\"' || fail 'discovery response should include deviceType (tici, tizi or mici)'
assert_contains_fixed 'std::thread(discovery_responder_loop).detach();' "$CONTROL_CPP" 'control mode should run discovery responder in background'
assert_contains_fixed 'start_discovery_responder();' "$CONTROL_CPP" 'control mode should start discovery responder with HTTP API'

echo "PASS: local discovery responder contract present"
