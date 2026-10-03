#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "--help" ]]; then
  cat <<USAGE
Usage: OP_ROOT=/path/to/openpilot-src commaviewd/scripts/upstream-interface-guard.sh [--manifest <path>] [--telemetry-only]
Fast-fails when upstream schema/service interfaces drift in ways that can break CommaViewD.
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
DIST_DIR="${DIST_DIR:-$REPO_ROOT/dist}"
MANIFEST="$DIST_DIR/upstream-interface-manifest.json"
TELEMETRY_ONLY=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --manifest) MANIFEST="$2"; shift 2 ;;
    --telemetry-only) TELEMETRY_ONLY=1; shift ;;
    *) echo "Unknown arg: $1" >&2; exit 2 ;;
  esac
done

missing=()

source_root() {
  if [[ -f "$OP_ROOT/cereal/log.capnp" && -f "$OP_ROOT/cereal/services.py" ]]; then
    printf '%s\n' "$OP_ROOT"
    return 0
  fi
  if [[ -f "$OP_ROOT/openpilot/cereal/log.capnp" && -f "$OP_ROOT/openpilot/cereal/services.py" ]]; then
    printf '%s\n' "$OP_ROOT/openpilot"
    return 0
  fi
  printf '%s\n' "$OP_ROOT"
}

OP_SOURCE_ROOT="$(source_root)"

check_file() {
  local p="$1"
  [[ -f "$p" ]] || missing+=("file:$p")
}

has_token() {
  local file="$1"
  local token="$2"
  grep -Eq "(^|[^A-Za-z0-9_])${token}([^A-Za-z0-9_]|$)" "$file"
}

check_token() {
  has_token "$1" "$2" || missing+=("token:$2@$1")
}

required_files=(
  "$OP_SOURCE_ROOT/cereal/log.capnp"
  "$OP_SOURCE_ROOT/cereal/services.py"
)

for f in "${required_files[@]}"; do
  check_file "$f"
done

remote="$(git -C "$OP_ROOT" remote get-url origin 2>/dev/null || true)"
onroad_ui_export_flavor="openpilot"
if printf '%s' "$remote" | grep -qi 'sunnypilot'; then
  onroad_ui_export_flavor="sunnypilot"
fi

required_transformer_files=(
  "$REPO_ROOT/comma/scripts/transform_onroad_ui_export.py"
  "$REPO_ROOT/comma/scripts/apply_onroad_ui_export_patch.sh"
  "$REPO_ROOT/comma/scripts/verify_onroad_ui_export_patch.sh"
  "$REPO_ROOT/comma/src/commaview_export.$onroad_ui_export_flavor.py"
)

for f in "${required_transformer_files[@]}"; do
  check_file "$f"
done

[[ ${#missing[@]} -eq 0 ]] || {
  printf 'FAIL: upstream interface guard missing current transformer prerequisites:\n' >&2
  printf '  - %s\n' "${missing[@]}" >&2
  exit 1
}

required_services=(
  wideRoadEncodeData
  livestreamWideRoadEncodeData
  carState
  selfdriveState
  deviceState
  radarState
  modelV2
  controlsState
  onroadEvents
  driverMonitoringState
  driverStateV2
  carOutput
  carControl
  longitudinalPlan
  carParams
  pandaStates
  wideRoadCameraState
  gpsLocationExternal
  gpsLocation
)

checked_required_services=0
for svc in "${required_services[@]}"; do
  if [[ "$TELEMETRY_ONLY" -eq 1 && "$svc" =~ ^(wideRoadEncodeData|livestreamWideRoadEncodeData)$ ]]; then
    continue
  fi
  check_token "$OP_SOURCE_ROOT/cereal/services.py" "$svc"
  check_token "$OP_SOURCE_ROOT/cereal/log.capnp" "$svc"
  checked_required_services=$((checked_required_services + 1))
done

resolve_service_alias() {
  local service
  for service in "$@"; do
    if has_token "$OP_SOURCE_ROOT/cereal/services.py" "$service" && \
       has_token "$OP_SOURCE_ROOT/cereal/log.capnp" "$service"; then
      printf '%s\n' "$service"
      return 0
    fi
  done
  return 1
}

calibration_service="$(resolve_service_alias extrinsicsCalibration liveCalibration || true)"
road_camera_service="$(resolve_service_alias narrowRoadCameraState roadCameraState || true)"
vehicle_parameters_service="$(resolve_service_alias vehicleParameters liveParameters || true)"
road_encode_service="$(resolve_service_alias narrowRoadEncodeData roadEncodeData || true)"
driver_encode_service="$(resolve_service_alias cabinEncodeData driverEncodeData || true)"
livestream_road_encode_service="$(resolve_service_alias livestreamNarrowRoadEncodeData livestreamRoadEncodeData || true)"
livestream_driver_encode_service="$(resolve_service_alias livestreamCabinEncodeData livestreamDriverEncodeData || true)"
[[ -n "$calibration_service" ]] || missing+=("service-alias:calibration(one-of:extrinsicsCalibration liveCalibration)")
[[ -n "$road_camera_service" ]] || missing+=("service-alias:road_camera(one-of:narrowRoadCameraState roadCameraState)")
[[ -n "$vehicle_parameters_service" ]] || missing+=("service-alias:vehicle_parameters(one-of:vehicleParameters liveParameters)")
if [[ "$TELEMETRY_ONLY" -ne 1 ]]; then
  [[ -n "$road_encode_service" ]] || missing+=("service-alias:road_encode(one-of:narrowRoadEncodeData roadEncodeData)")
  [[ -n "$driver_encode_service" ]] || missing+=("service-alias:cabin_encode(one-of:cabinEncodeData driverEncodeData)")
  [[ -n "$livestream_road_encode_service" ]] || missing+=("service-alias:livestream_road_encode(one-of:livestreamNarrowRoadEncodeData livestreamRoadEncodeData)")
  [[ -n "$livestream_driver_encode_service" ]] || missing+=("service-alias:livestream_cabin_encode(one-of:livestreamCabinEncodeData livestreamDriverEncodeData)")
fi

required_capnp_fields=(
  alertText1
  alertText2
  alertType
  laneLineProbs
  laneLineStds
  roadEdgeStds
  leadsV3
  rpyCalib
  height
  calStatus
  calPerc
  wideFromDeviceEuler
  roll
  frameId
  timestampEof
  timestampSof
  sensor
  hasFix
  horizontalAccuracy
  bearingDeg
  unixTimestampMillis
  # Support bundles peek managerState's process table (src/manager_state_peek.cpp).
  managerState
  shouldBeRunning
  exitCode
  # The process watcher (src/process_watch.cpp) notes deviceState.started with each event.
  deviceState
  started
)

for field in "${required_capnp_fields[@]}"; do
  check_token "$OP_SOURCE_ROOT/cereal/log.capnp" "$field"
done

# Live location reads the comma's GPS from openpilot's msgq ring without subscribing
# (src/gps_peek.cpp), so the ring's framing is an interface too: the size tag, the -1 wrap tag,
# 8-byte alignment, and the write pointer moved only after a message is whole.
msgq_root="$OP_SOURCE_ROOT/msgq_repo"
[[ -d "$msgq_root" ]] || msgq_root="$OP_ROOT/msgq_repo"
check_file "$msgq_root/msgq/msgq.h"
check_file "$msgq_root/msgq/msgq.cc"
if [[ -f "$msgq_root/msgq/msgq.cc" ]]; then
  for needle in \
    'uint64_t total_msg_size = ALIGN(msg->size + sizeof(int64_t));' \
    '*(int64_t*)p = -1;' \
    'PACK64(*q->write_pointer, write_cycles, new_ptr);' \
    'q->data = mem + sizeof(msgq_header_t);'; do
    grep -Fq "$needle" "$msgq_root/msgq/msgq.cc" || missing+=("msgq-ring:$needle")
  done
fi

# The bridge follows the encoder queues the same way, message by message, without a reader slot
# (src/msgq_ring_reader.cpp), so it relies on the rest of the layout too: the header's fields and
# their order (write_pointer second; the size, 24 + 24 * NUM_READERS, is worked out from the file,
# which only works while it stays under 1 KiB and the queue sizes are whole KiB), the packed
# lap/offset pointer, the order of a send (size tag, bytes, barrier, then the pointer), and the
# one-third-of-the-ring bound on a message that sizes the margin kept from the writer.
if [[ -f "$msgq_root/msgq/msgq.h" && -f "$msgq_root/msgq/msgq.cc" ]]; then
  while IFS= read -r problem; do
    [[ -n "$problem" ]] && missing+=("$problem")
  done < <(python3 - "$msgq_root/msgq/msgq.h" "$msgq_root/msgq/msgq.cc" "$OP_SOURCE_ROOT/cereal/services.py" <<'PY'
import pathlib
import re
import sys

header = pathlib.Path(sys.argv[1]).read_text()
ring = pathlib.Path(sys.argv[2]).read_text()
services_path = pathlib.Path(sys.argv[3])


def flat(text):
    return re.sub(r'\s+', ' ', text)


if not re.search(r'struct\s+msgq_header_t\s*\{\s*uint64_t\s+num_readers;\s*uint64_t\s+write_pointer;\s*'
                 r'uint64_t\s+write_uid;\s*uint64_t\s+read_pointers\[NUM_READERS\];\s*'
                 r'uint64_t\s+read_valids\[NUM_READERS\];\s*uint64_t\s+read_uids\[NUM_READERS\];\s*\};', header):
    print('msgq-layout:msgq_header_t is num_readers, write_pointer, write_uid, read_pointers/read_valids/read_uids[NUM_READERS]')
readers = re.search(r'#define\s+NUM_READERS\s+(\d+)\b', header)
if readers is None or not 1 <= int(readers.group(1)) <= 41:
    print('msgq-layout:NUM_READERS between 1 and 41 (24 + 24 * NUM_READERS header bytes under 1 KiB)')
for macro in ('#define ALIGN(n) ((n + (8 - 1)) & -8)',
              '#define PACK64(output, higher, lower) output = ((uint64_t)higher << 32) | ((uint64_t)lower & 0xFFFFFFFF)'):
    if macro not in flat(header):
        print(f'msgq-layout:{macro}')

for needle in ('int rc = ftruncate(fd, size + sizeof(msgq_header_t));',
               'assert(3 * total_msg_size <= q->size);',
               'write_cycles = write_cycles + 1;'):
    if needle not in ring:
        print(f'msgq-ring:{needle}')
send = ring.split('int msgq_msg_send(', 1)[-1]
order = ['*size_p = msg->size;', 'memcpy(p + sizeof(int64_t), msg->data, msg->size);', '__sync_synchronize();',
         'uint32_t new_ptr = ALIGN(write_pointer + msg->size + sizeof(int64_t));',
         'PACK64(*q->write_pointer, write_cycles, new_ptr);']
positions = [send.find(needle) for needle in order]
if -1 in positions or positions != sorted(positions):
    print('msgq-ring:msgq_msg_send stores the size tag, the bytes, a barrier, then the write pointer')

if services_path.is_file():
    services = services_path.read_text()
    queue_sizes = re.search(r'class\s+QueueSize\b.*?:\n((?:[ \t]+.*\n)+)', services)
    if queue_sizes:
        for name, value in re.findall(r'^\s+([A-Z_]+)\s*=\s*([^#\n]+)', queue_sizes.group(1), re.M):
            if not re.fullmatch(r'\d+\s*\*\s*1024(\s*\*\s*1024)?', value.strip()):
                print(f'msgq-layout:QueueSize.{name} = {value.strip()} (queue sizes must be whole KiB)')
PY
)
fi

# The control API's road phase (offroad / parked / driving, src/road_phase.cpp) reads carState and
# selfdriveState from their queues the same read-only way.
car_schema="$OP_SOURCE_ROOT/cereal/car.capnp"
[[ -f "$car_schema" ]] || car_schema="$OP_ROOT/opendbc_repo/opendbc/car/car.capnp"
check_file "$car_schema"
if [[ -f "$car_schema" ]]; then
  while IFS= read -r problem; do
    [[ -n "$problem" ]] && missing+=("$problem")
  done < <(python3 - "$car_schema" "$OP_SOURCE_ROOT/cereal/log.capnp" <<'PY'
import pathlib
import re
import sys

car = pathlib.Path(sys.argv[1]).read_text()
log = pathlib.Path(sys.argv[2]).read_text() if pathlib.Path(sys.argv[2]).is_file() else ''
if not re.search(r'\bgearShifter\s+@\d+\s*:GearShifter;', car):
    print('road-phase:CarState.gearShifter :GearShifter')
if not re.search(r'enum\s+GearShifter\s*\{\s*unknown\s+@0;\s*park\s+@1;', car):
    print('road-phase:GearShifter { unknown @0; park @1; ... }')
if not re.search(r'\bstandstill\s+@\d+\s*:Bool;', car):
    print('road-phase:CarState.standstill :Bool')
selfdrive = re.search(r'struct\s+SelfdriveState\s*\{(.*?)\n\}', log, re.S)
if selfdrive is None or not re.search(r'\benabled\s+@\d+\s*:Bool;', selfdrive.group(1)):
    print('road-phase:SelfdriveState.enabled :Bool')
PY
)
fi

if [[ ${#missing[@]} -gt 0 ]]; then
  printf 'FAIL: upstream interface drift detected:\n' >&2
  printf '  - %s\n' "${missing[@]}" >&2
  exit 1
fi

mkdir -p "$(dirname "$MANIFEST")"
upstream_sha="unknown"
if git -C "$OP_ROOT" rev-parse --short HEAD >/dev/null 2>&1; then
  upstream_sha="$(git -C "$OP_ROOT" rev-parse --short HEAD)"
fi
required_service_count=$((checked_required_services + 3))
if [[ "$TELEMETRY_ONLY" -ne 1 ]]; then
  required_service_count=$((required_service_count + 4))
fi

cat > "$MANIFEST" <<JSON
{
  "opRoot": "${OP_ROOT}",
  "opSourceRoot": "${OP_SOURCE_ROOT}",
  "upstreamSha": "${upstream_sha}",
  "telemetryOnly": $([[ "$TELEMETRY_ONLY" -eq 1 ]] && printf true || printf false),
  "resolvedServices": {
    "calibration": "${calibration_service}",
    "roadCamera": "${road_camera_service}",
    "vehicleParameters": "${vehicle_parameters_service}"
    ,"roadEncode": "${road_encode_service}"
    ,"cabinEncode": "${driver_encode_service}"
    ,"livestreamRoadEncode": "${livestream_road_encode_service}"
    ,"livestreamCabinEncode": "${livestream_driver_encode_service}"
  },
  "checks": {
    "onroadUiExportMethod": "transformer",
    "onroadUiExportFlavor": "${onroad_ui_export_flavor}",
    "requiredTransformerFiles": ${#required_transformer_files[@]},
    "requiredServices": ${required_service_count},
    "requiredCapnpFields": ${#required_capnp_fields[@]},
    "requiredFiles": ${#required_files[@]}
  }
}
JSON

echo "PASS: upstream interface guard"
echo "manifest: $MANIFEST"
