#!/usr/bin/env bash
set -euo pipefail

# Build CommaView runtime binaries.
# Outputs (default under repo dist/):
#   dist/commaviewd-host
#   dist/commaviewd-aarch64
#   dist/lib/libcapnp-*.so
#   dist/lib/libkj-*.so

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$ROOT/.." && pwd)"
DEFAULT_OP_ROOT="$REPO_ROOT/../openpilot-src"
if [[ -d "$DEFAULT_OP_ROOT" ]]; then
  OP_ROOT="${OP_ROOT:-$DEFAULT_OP_ROOT}"
else
  OP_ROOT="${OP_ROOT:-$HOME/openpilot-src}"
fi

DIST_DIR="${DIST_DIR:-$REPO_ROOT/dist}"
HOST_CXX="${HOST_CXX:-${CXX:-c++}}"
CROSS_CXX="${CROSS_CXX:-aarch64-linux-gnu-g++}"
SKIP_ARM="${COMMAVIEWD_SKIP_ARM:-0}"

if [[ -f "$OP_ROOT/cereal/log.capnp" ]]; then
  OP_SOURCE_ROOT="$OP_ROOT"
elif [[ -f "$OP_ROOT/openpilot/cereal/log.capnp" ]]; then
  OP_SOURCE_ROOT="$OP_ROOT/openpilot"
else
  echo "[ERR] Missing upstream cereal tree below $OP_ROOT" >&2
  exit 2
fi

if [[ -d "$OP_SOURCE_ROOT/msgq_repo" ]]; then
  MSGQ_ROOT="$OP_SOURCE_ROOT/msgq_repo"
elif [[ -d "$OP_ROOT/msgq_repo" ]]; then
  MSGQ_ROOT="$OP_ROOT/msgq_repo"
else
  echo "[ERR] Missing initialized upstream msgq_repo below $OP_ROOT" >&2
  exit 2
fi

if [[ -f "$OP_SOURCE_ROOT/cereal/car.capnp" ]]; then
  CAR_SCHEMA_ROOT="$OP_SOURCE_ROOT/cereal"
  CAR_SCHEMA_PATH="$OP_SOURCE_ROOT/cereal/car.capnp"
elif [[ -f "$OP_ROOT/opendbc_repo/opendbc/car/car.capnp" ]]; then
  CAR_SCHEMA_ROOT="$OP_ROOT/opendbc_repo/opendbc/car"
  CAR_SCHEMA_PATH="$CAR_SCHEMA_ROOT/car.capnp"
else
  echo "[ERR] Missing initialized upstream car.capnp below $OP_ROOT" >&2
  exit 2
fi

MAIN_SRC="$ROOT/src/main.cc"
BRIDGE_SRC="$ROOT/src/bridge_runtime.cc"
NET_FRAMING_SRC="$ROOT/src/framing.cpp"
NET_SOCKET_SRC="$ROOT/src/socket.cpp"
CONTROL_POLICY_SRC="$ROOT/src/policy.cpp"
VIDEO_ROUTER_SRC="$ROOT/src/router.cpp"
VIDEO_TRANSPORT_POLICY_SRC="$ROOT/src/video_transport_policy.cpp"
VIDEO_CHUNK_PROTOCOL_SRC="$ROOT/src/video_chunk_protocol.cpp"
RUNTIME_VIDEO_SEND_ACCOUNTING_SRC="$ROOT/src/runtime_video_send_accounting.cpp"
RUNTIME_MODE_SRC="$ROOT/src/mode.cpp"
RUNTIME_BRIDGE_MODE_SRC="$ROOT/src/bridge_mode.cpp"
RUNTIME_CONTROL_MODE_SRC="$ROOT/src/control_mode.cpp"
API_HTTP_SERVER_SRC="$ROOT/src/http_server.cpp"
SOURCE_RECORDING_ARCHIVE_SRC="$ROOT/src/source_recording_archive.cpp"
UI_EXPORT_SOCKET_SRC="$ROOT/src/ui_export_socket.cpp"
GPS_PEEK_SRC="$ROOT/src/gps_peek.cpp"
SUPPORT_BUNDLE_SRC="$ROOT/src/support_bundle.cpp"
MANAGER_STATE_PEEK_SRC="$ROOT/src/manager_state_peek.cpp"
ROAD_PHASE_SRC="$ROOT/src/road_phase.cpp"
PROCESS_EVENTS_SRC="$ROOT/src/process_events.cpp"
PROCESS_WATCH_SRC="$ROOT/src/process_watch.cpp"
MSGQ_RING_READER_SRC="$ROOT/src/msgq_ring_reader.cpp"

HOST_OUT="$DIST_DIR/commaviewd-host"
ARM_OUT="$DIST_DIR/commaviewd-aarch64"
BUNDLE_LIB_DIR="$DIST_DIR/lib"
PATCHED_MSGQ_LOCAL="${PATCHED_MSGQ_LOCAL:-$DIST_DIR/msgq-commaviewd.cc}"

detect_arm_lib() {
  local pattern="$1"
  local first_match=""
  first_match="$(compgen -G "$pattern" | head -n 1 || true)"
  if [[ -n "$first_match" ]]; then
    printf '%s\n' "$first_match"
    return 0
  fi
  return 1
}

ARM_CAPNP_SO="${ARM_CAPNP_SO:-$(detect_arm_lib /usr/lib/aarch64-linux-gnu/libcapnp-*.so || true)}"
ARM_KJ_SO="${ARM_KJ_SO:-$(detect_arm_lib /usr/lib/aarch64-linux-gnu/libkj-*.so || true)}"
ARM_CAPNP_NAME="$(basename "$ARM_CAPNP_SO")"
ARM_KJ_NAME="$(basename "$ARM_KJ_SO")"

mkdir -p "$DIST_DIR"
python3 "$ROOT/scripts/patch-msgq-recv.py" "$MSGQ_ROOT/msgq/msgq.cc" "$PATCHED_MSGQ_LOCAL"
MSGQ_SOURCE="$PATCHED_MSGQ_LOCAL"

if [[ -f "$OP_SOURCE_ROOT/cereal/deprecated.capnp" ]]; then
  DEPRECATED_SCHEMA_NAME="deprecated"
elif [[ -f "$OP_SOURCE_ROOT/cereal/legacy.capnp" ]]; then
  DEPRECATED_SCHEMA_NAME="legacy"
else
  echo "[ERR] Missing required file: expected $OP_SOURCE_ROOT/cereal/deprecated.capnp or $OP_SOURCE_ROOT/cereal/legacy.capnp" >&2
  exit 2
fi
DEPRECATED_SCHEMA_PATH="$OP_SOURCE_ROOT/cereal/${DEPRECATED_SCHEMA_NAME}.capnp"
DEPRECATED_SCHEMA_CPP="$OP_SOURCE_ROOT/cereal/gen/cpp/${DEPRECATED_SCHEMA_NAME}.capnp.c++"

require_file() {
  local p="$1"
  [[ -f "$p" ]] || { echo "[ERR] Missing required file: $p" >&2; exit 2; }
}

for f in \
  "$MAIN_SRC" "$BRIDGE_SRC" "$NET_FRAMING_SRC" "$NET_SOCKET_SRC" \
  "$CONTROL_POLICY_SRC" "$VIDEO_ROUTER_SRC" "$VIDEO_TRANSPORT_POLICY_SRC" "$VIDEO_CHUNK_PROTOCOL_SRC" "$RUNTIME_VIDEO_SEND_ACCOUNTING_SRC" \
  "$RUNTIME_MODE_SRC" "$RUNTIME_BRIDGE_MODE_SRC" "$RUNTIME_CONTROL_MODE_SRC" \
  "$API_HTTP_SERVER_SRC" "$SOURCE_RECORDING_ARCHIVE_SRC" "$UI_EXPORT_SOCKET_SRC" "$GPS_PEEK_SRC" \
  "$SUPPORT_BUNDLE_SRC" "$MANAGER_STATE_PEEK_SRC" "$ROAD_PHASE_SRC" "$PROCESS_EVENTS_SRC" "$PROCESS_WATCH_SRC" \
  "$MSGQ_RING_READER_SRC" \
  "$OP_SOURCE_ROOT/cereal/log.capnp" "$CAR_SCHEMA_PATH" "$OP_SOURCE_ROOT/cereal/services.py" \
  "$OP_SOURCE_ROOT/cereal/messaging/socketmaster.cc" \
  "$MSGQ_ROOT/msgq/event.cc" "$MSGQ_ROOT/msgq/impl_fake.cc" \
  "$MSGQ_ROOT/msgq/impl_msgq.cc" "$MSGQ_ROOT/msgq/ipc.cc"; do
  require_file "$f"
done

if [[ "$SKIP_ARM" != "1" ]]; then
  for f in "$ARM_CAPNP_SO" "$ARM_KJ_SO"; do
    require_file "$f"
  done
fi

mkdir -p "$OP_SOURCE_ROOT/cereal/gen/cpp" "$BUNDLE_LIB_DIR"

VIDEO_SCHEMA_FLAGS=()
if grep -Eq 'narrowRoadEncodeData|cabinEncodeData' "$OP_SOURCE_ROOT/cereal/services.py"; then
  if ! grep -Eq 'narrowRoadEncodeData' "$OP_SOURCE_ROOT/cereal/services.py" || ! grep -Eq 'cabinEncodeData' "$OP_SOURCE_ROOT/cereal/services.py"; then
    echo "[ERR] Upstream encoded-video schema is only partially renamed" >&2
    exit 2
  fi
  VIDEO_SCHEMA_FLAGS+=("-DCOMMAVIEW_CURRENT_VIDEO_SCHEMA")
fi

echo "[1/5] Generating cereal headers/sources..."
capnpc --src-prefix="$OP_SOURCE_ROOT/cereal" --src-prefix="$CAR_SCHEMA_ROOT" \
  --import-path="$OP_SOURCE_ROOT/cereal" --import-path="$CAR_SCHEMA_ROOT" \
  "$OP_SOURCE_ROOT/cereal/log.capnp" \
  "$CAR_SCHEMA_PATH" \
  "$DEPRECATED_SCHEMA_PATH" \
  "$OP_SOURCE_ROOT/cereal/custom.capnp" \
  -o c++:"$OP_SOURCE_ROOT/cereal/gen/cpp/"
python3 "$OP_SOURCE_ROOT/cereal/services.py" > "$OP_SOURCE_ROOT/cereal/services.h"

COMMON_SRCS=(
  "$MAIN_SRC" "$BRIDGE_SRC"
  "$RUNTIME_MODE_SRC" "$RUNTIME_BRIDGE_MODE_SRC" "$RUNTIME_CONTROL_MODE_SRC"
  "$API_HTTP_SERVER_SRC" "$SOURCE_RECORDING_ARCHIVE_SRC" "$UI_EXPORT_SOCKET_SRC" "$GPS_PEEK_SRC"
  "$SUPPORT_BUNDLE_SRC" "$MANAGER_STATE_PEEK_SRC" "$ROAD_PHASE_SRC" "$PROCESS_EVENTS_SRC" "$PROCESS_WATCH_SRC"
  "$MSGQ_RING_READER_SRC"
  "$NET_FRAMING_SRC" "$NET_SOCKET_SRC" "$CONTROL_POLICY_SRC"
  "$VIDEO_ROUTER_SRC" "$VIDEO_TRANSPORT_POLICY_SRC" "$VIDEO_CHUNK_PROTOCOL_SRC" "$RUNTIME_VIDEO_SEND_ACCOUNTING_SRC"
  "$OP_SOURCE_ROOT/cereal/messaging/socketmaster.cc"
  "$MSGQ_SOURCE"
  "$MSGQ_ROOT/msgq/event.cc" "$MSGQ_ROOT/msgq/impl_fake.cc"
  "$MSGQ_ROOT/msgq/impl_msgq.cc" "$MSGQ_ROOT/msgq/ipc.cc"
  "$OP_SOURCE_ROOT/cereal/gen/cpp/log.capnp.c++" "$OP_SOURCE_ROOT/cereal/gen/cpp/car.capnp.c++"
  "$DEPRECATED_SCHEMA_CPP" "$OP_SOURCE_ROOT/cereal/gen/cpp/custom.capnp.c++"
)

echo "[2/5] Building host sanity binary (x86_64)..."
"$HOST_CXX" -O2 -std=c++17 -DCOMMAVIEW_BRIDGE_NO_MAIN \
  "${VIDEO_SCHEMA_FLAGS[@]}" \
  -I"$ROOT/include" -I"$OP_ROOT" -I"$OP_SOURCE_ROOT" -I"$OP_SOURCE_ROOT/cereal/gen/cpp" -I"$OP_SOURCE_ROOT/cereal/messaging" -I"$MSGQ_ROOT" \
  -o "$HOST_OUT" "${COMMON_SRCS[@]}" \
  -lzmq -lcapnp -lkj -lpthread

if [[ "$SKIP_ARM" == "1" ]]; then
  echo "[3/5] Skipping aarch64 build (COMMAVIEWD_SKIP_ARM=1)"
  echo "[4/5] Skipping runtime lib bundle (COMMAVIEWD_SKIP_ARM=1)"
  echo "[5/5] Done"
  ls -lh "$HOST_OUT"
  file "$HOST_OUT"
  exit 0
fi

echo "[3/5] Building deploy binary (aarch64)..."
"$CROSS_CXX" -O2 -std=c++17 -DCOMMAVIEW_BRIDGE_NO_MAIN \
  "${VIDEO_SCHEMA_FLAGS[@]}" \
  -I"$ROOT/include" -I"$OP_ROOT" -I"$OP_SOURCE_ROOT" -I"$OP_SOURCE_ROOT/cereal/gen/cpp" -I"$OP_SOURCE_ROOT/cereal/messaging" -I"$MSGQ_ROOT" \
  -L/usr/lib/aarch64-linux-gnu -Wl,-rpath,\$ORIGIN/lib \
  -o "$ARM_OUT" "${COMMON_SRCS[@]}" \
  -lzmq -lcapnp -lkj -lpthread

echo "[4/5] Bundling comma runtime libs (detected ABI)..."
install -m 755 "$ARM_CAPNP_SO" "$BUNDLE_LIB_DIR/$ARM_CAPNP_NAME"
install -m 755 "$ARM_KJ_SO" "$BUNDLE_LIB_DIR/$ARM_KJ_NAME"

echo "[5/5] Done"
ls -lh "$HOST_OUT" "$ARM_OUT" "$BUNDLE_LIB_DIR/$ARM_CAPNP_NAME" "$BUNDLE_LIB_DIR/$ARM_KJ_NAME"
file "$HOST_OUT" "$ARM_OUT"
aarch64-linux-gnu-readelf -d "$ARM_OUT" | grep -E "NEEDED|RPATH|RUNPATH" || true
