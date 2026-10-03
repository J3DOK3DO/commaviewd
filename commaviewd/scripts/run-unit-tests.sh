#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "--help" ]]; then
  cat <<USAGE
Usage: OP_ROOT=/path/to/openpilot-src commaviewd/scripts/run-unit-tests.sh
Builds commaviewd (via build-ubuntu.sh), then runs the C++ unit tests, the contract and
integration test scripts, and pytest over comma/tests. Run it from the repository root.
USAGE
  exit 0
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$ROOT/.." && pwd)"
DEFAULT_OP_ROOT="$REPO_ROOT/../openpilot-src"
if [[ -d "$DEFAULT_OP_ROOT" ]]; then
  OP_ROOT="${OP_ROOT:-$DEFAULT_OP_ROOT}"
else
  OP_ROOT="${OP_ROOT:-$HOME/openpilot-src}"
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
DIST_DIR="${DIST_DIR:-$REPO_ROOT/dist}"
export COMMAVIEWD_HOST_BIN="$DIST_DIR/commaviewd-host"

if [[ -f "$OP_ROOT/cereal/log.capnp" ]]; then
  OP_SOURCE_ROOT="$OP_ROOT"
elif [[ -f "$OP_ROOT/openpilot/cereal/log.capnp" ]]; then
  OP_SOURCE_ROOT="$OP_ROOT/openpilot"
else
  echo "[ERR] Missing upstream cereal tree below $OP_ROOT" >&2
  exit 2
fi

OP_ROOT="$OP_ROOT" "$ROOT/scripts/build-ubuntu.sh" >/dev/null

if [[ -n "${CXX:-}" ]]; then
  CXX_BIN="$CXX"
elif command -v clang++ >/dev/null 2>&1; then
  CXX_BIN="clang++"
else
  CXX_BIN="c++"
fi
INC=( -I"$ROOT/include" -I"$OP_ROOT" -I"$OP_SOURCE_ROOT" -I"$OP_SOURCE_ROOT/cereal/gen/cpp" -I"$OP_SOURCE_ROOT/cereal/messaging" -I"$OP_ROOT/msgq_repo" )

"$CXX_BIN" --version >/dev/null 2>&1 || {
  echo "[ERR] C++ compiler not found: $CXX_BIN" >&2
  exit 2
}


"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" \
  "$ROOT/tests/test_net_framing.cpp" \
  "$ROOT/src/framing.cpp" \
  -o "$TMP/test_net_framing"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" \
  "$ROOT/tests/test_runtime_mode.cpp" \
  "$ROOT/src/mode.cpp" \
  -o "$TMP/test_runtime_mode"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" \
  "$ROOT/tests/test_control_policy.cpp" \
  "$ROOT/src/policy.cpp" \
  "$ROOT/src/framing.cpp" \
  -o "$TMP/test_control_policy"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" -I"$ROOT/src" \
  "$ROOT/tests/test_telemetry_policy.cpp" \
  -o "$TMP/test_telemetry_policy"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" -I"$ROOT/src" \
  "$ROOT/tests/test_video_transport_policy.cpp" \
  "$ROOT/src/video_transport_policy.cpp" \
  -o "$TMP/test_video_transport_policy"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" -I"$ROOT/src" -DCOMMAVIEW_VIDEO_CHUNK_PROTOCOL_TESTING \
  "$ROOT/tests/test_video_chunk_protocol.cpp" \
  "$ROOT/src/video_chunk_protocol.cpp" \
  -o "$TMP/test_video_chunk_protocol"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" -I"$ROOT/src" \
  "$ROOT/tests/test_video_send_accounting.cpp" \
  "$ROOT/src/runtime_video_send_accounting.cpp" \
  "$ROOT/src/framing.cpp" \
  -o "$TMP/test_video_send_accounting"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" \
  "$ROOT/tests/test_http_server_cloexec.cpp" \
  "$ROOT/src/http_server.cpp" \
  -o "$TMP/test_http_server_cloexec"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" \
  "$ROOT/tests/test_ui_export_socket.cpp" \
  "$ROOT/src/ui_export_socket.cpp" \
  -o "$TMP/test_ui_export_socket"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" \
  "$ROOT/tests/test_source_recording_archive.cpp" \
  "$ROOT/src/source_recording_archive.cpp" \
  -o "$TMP/test_source_recording_archive"

# The generated cereal sources the build step just wrote, for tests that read real events.
CEREAL_SRCS=( "$OP_SOURCE_ROOT/cereal/gen/cpp/log.capnp.c++" "$OP_SOURCE_ROOT/cereal/gen/cpp/car.capnp.c++" "$OP_SOURCE_ROOT/cereal/gen/cpp/custom.capnp.c++" )
for legacy in deprecated legacy; do
  if [[ -f "$OP_SOURCE_ROOT/cereal/gen/cpp/$legacy.capnp.c++" ]]; then
    CEREAL_SRCS+=( "$OP_SOURCE_ROOT/cereal/gen/cpp/$legacy.capnp.c++" )
  fi
done
MSGQ_ROOT_DIR="$OP_SOURCE_ROOT/msgq_repo"
[[ -d "$MSGQ_ROOT_DIR" ]] || MSGQ_ROOT_DIR="$OP_ROOT/msgq_repo"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" -I"$ROOT/src" -I"$MSGQ_ROOT_DIR" \
  "$ROOT/tests/test_gps_peek.cpp" \
  "$ROOT/src/gps_peek.cpp" \
  "$ROOT/src/msgq_ring_reader.cpp" \
  "${CEREAL_SRCS[@]}" \
  -lcapnp -lkj -lpthread -o "$TMP/test_gps_peek"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" -I"$ROOT/src" -I"$MSGQ_ROOT_DIR" \
  "$ROOT/tests/test_road_phase.cpp" \
  "$ROOT/src/road_phase.cpp" \
  "$ROOT/src/gps_peek.cpp" \
  "$ROOT/src/msgq_ring_reader.cpp" \
  "${CEREAL_SRCS[@]}" \
  -lcapnp -lkj -lpthread -o "$TMP/test_road_phase"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" -I"$ROOT/src" \
  "$ROOT/tests/test_support_bundle.cpp" \
  "$ROOT/src/support_bundle.cpp" \
  -o "$TMP/test_support_bundle"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" -I"$ROOT/src" -I"$MSGQ_ROOT_DIR" \
  "$ROOT/tests/test_manager_state_peek.cpp" \
  "$ROOT/src/manager_state_peek.cpp" \
  "$ROOT/src/gps_peek.cpp" \
  "$ROOT/src/msgq_ring_reader.cpp" \
  "${CEREAL_SRCS[@]}" \
  -lcapnp -lkj -lpthread -o "$TMP/test_manager_state_peek"
"$TMP/test_road_phase"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" -I"$ROOT/src" -I"$MSGQ_ROOT_DIR" \
  "$ROOT/tests/test_process_events.cpp" \
  "$ROOT/src/process_events.cpp" \
  "$ROOT/src/process_watch.cpp" \
  "$ROOT/src/support_bundle.cpp" \
  "$ROOT/src/manager_state_peek.cpp" \
  "$ROOT/src/gps_peek.cpp" \
  "$ROOT/src/msgq_ring_reader.cpp" \
  "${CEREAL_SRCS[@]}" \
  -lcapnp -lkj -lpthread -o "$TMP/test_process_events"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" \
  "$ROOT/tests/test_msgq_header_recovery.cpp" \
  "$DIST_DIR/msgq-commaviewd.cc" \
  -lpthread -o "$TMP/test_msgq_header_recovery"

# Real msgq (commaviewd's linked copy) on private queues: the encoder queues are read without a
# reader slot, so no number of bridge restarts or reconnects can evict loggerd.
"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" -I"$ROOT/src" -I"$MSGQ_ROOT_DIR" \
  "$ROOT/tests/test_msgq_ring_reader.cpp" \
  "$ROOT/src/msgq_ring_reader.cpp" \
  "$ROOT/src/video_transport_policy.cpp" \
  "$DIST_DIR/msgq-commaviewd.cc" \
  "$MSGQ_ROOT_DIR/msgq/impl_msgq.cc" "$MSGQ_ROOT_DIR/msgq/ipc.cc" \
  "$MSGQ_ROOT_DIR/msgq/event.cc" "$MSGQ_ROOT_DIR/msgq/impl_fake.cc" \
  -lpthread -o "$TMP/test_msgq_ring_reader"

"$TMP/test_net_framing"
"$TMP/test_runtime_mode"
"$TMP/test_control_policy"
"$TMP/test_telemetry_policy"
"$TMP/test_video_transport_policy"
"$TMP/test_video_chunk_protocol"
"$TMP/test_video_send_accounting"
"$TMP/test_http_server_cloexec"
"$TMP/test_ui_export_socket"
"$TMP/test_source_recording_archive"
"$TMP/test_msgq_header_recovery"
"$TMP/test_msgq_ring_reader"
"$TMP/test_gps_peek"
"$TMP/test_support_bundle"
"$TMP/test_manager_state_peek"
"$TMP/test_process_events"

"$ROOT/tests/control_mode_api_contract_test.sh"
"$ROOT/tests/control_mode_pairing_integration_test.sh"
COMMAVIEWD_ROAD_QUEUE_TOOL="$TMP/test_road_phase" python3 "$ROOT/tests/source_archive_endpoint_integration_test.py"
python3 "$ROOT/tests/wifi_power_save_api_integration_test.py"
COMMAVIEWD_ROAD_QUEUE_TOOL="$TMP/test_road_phase" python3 "$ROOT/tests/runtime_control_integration_test.py"
COMMAVIEWD_GPS_QUEUE_TOOL="$TMP/test_gps_peek" python3 "$ROOT/tests/drive_stats_api_integration_test.py"
python3 "$ROOT/tests/leaderboard_api_integration_test.py"
"$ROOT/tests/leaderboard_contract_test.sh"
COMMAVIEWD_MANAGER_STATE_QUEUE_TOOL="$TMP/test_manager_state_peek" python3 "$ROOT/tests/support_logs_api_integration_test.py"
"$ROOT/tests/local_discovery_contract_test.sh"
"$ROOT/tests/upstream_interface_guard_transformer_test.sh"
"$ROOT/tests/device_test_workflow_contract_test.sh"
"$ROOT/tests/ci_workflow_contract_test.sh"
"$ROOT/tests/release_workflow_contract_test.sh"
"$ROOT/tests/raw_only_runtime_contract_test.sh"
"$ROOT/tests/video_msgq_conflate_contract_test.sh"
"$ROOT/tests/msgq_reader_slot_contract_test.sh"
"$ROOT/tests/video_transport_bridge_contract_test.sh"
"$ROOT/tests/runtime_split_transport_contract_test.sh"
"$ROOT/tests/build_video_schema_contract_test.sh"
"$ROOT/tests/runtime_debug_policy_contract_test.sh"
"$ROOT/tests/timestamped_video_runtime_contract_test.sh"
"$ROOT/tests/onroad_ui_export_ci_contract_test.sh"
"$REPO_ROOT/comma/tests/onroad_ui_export_patch_contract_test.sh"
"$REPO_ROOT/comma/tests/runtime_lifecycle_log_contract_test.sh"
"$REPO_ROOT/comma/tests/runtime_log_rotation_contract_test.sh"
"$REPO_ROOT/comma/tests/runtime_process_supervisor_contract_test.sh"
"$REPO_ROOT/comma/tests/runtime_support_logs_contract_test.sh"
"$REPO_ROOT/comma/tests/install_release_resolution_contract_test.sh"
"$REPO_ROOT/comma/tests/install_release_origin_contract_test.sh"
python3 -m pytest "$REPO_ROOT/comma/tests" -q
echo "PASS: commaviewd unit tests passed"
