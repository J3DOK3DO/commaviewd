#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GUARD="$ROOT/scripts/upstream-interface-guard.sh"

tmpdir="$(mktemp -d)"
cleanup() { python3 - "$tmpdir" <<'PY'
import shutil
import sys
shutil.rmtree(sys.argv[1], ignore_errors=True)
PY
}
trap cleanup EXIT

op_root="$tmpdir/openpilot-src"
manifest="$tmpdir/manifest.json"
source_root="$op_root/cereal"
mkdir -p "$source_root"

# The lines of openpilot's msgq ring that the read-only readers (live location's GPS, the
# bridge's encoder queues) rely on.
write_msgq_fixture() {
  mkdir -p "$1/msgq_repo/msgq"
  cat > "$1/msgq_repo/msgq/msgq.h" <<'H'
#define NUM_READERS 15
#define ALIGN(n) ((n + (8 - 1)) & -8)
#define PACK64(output, higher, lower) output = ((uint64_t)higher << 32) | ((uint64_t)lower & 0xFFFFFFFF)
struct  msgq_header_t {
  uint64_t num_readers;
  uint64_t write_pointer;
  uint64_t write_uid;
  uint64_t read_pointers[NUM_READERS];
  uint64_t read_valids[NUM_READERS];
  uint64_t read_uids[NUM_READERS];
};
H
  cat > "$1/msgq_repo/msgq/msgq.cc" <<'CC'
  int rc = ftruncate(fd, size + sizeof(msgq_header_t));
  q->data = mem + sizeof(msgq_header_t);
int msgq_msg_send(msgq_msg_t * msg, msgq_queue_t *q){
  uint64_t total_msg_size = ALIGN(msg->size + sizeof(int64_t));
  assert(3 * total_msg_size <= q->size);
    *(int64_t*)p = -1;
    write_cycles = write_cycles + 1;
  *size_p = msg->size;
  memcpy(p + sizeof(int64_t), msg->data, msg->size);
  __sync_synchronize();
  uint32_t new_ptr = ALIGN(write_pointer + msg->size + sizeof(int64_t));
  PACK64(*q->write_pointer, write_cycles, new_ptr);
}
CC
}
write_msgq_fixture "$op_root"

# The carState fields the road phase reads.
write_car_fixture() {
  cat > "$1/car.capnp" <<'CAPNP'
struct CarState {
  standstill @18 :Bool;
  gearShifter @14 :GearShifter;
  enum GearShifter {
    unknown @0;
    park @1;
    drive @2;
  }
}
CAPNP
}
write_car_fixture "$source_root"

cat > "$source_root/services.py" <<'PY'
roadEncodeData = None
wideRoadEncodeData = None
driverEncodeData = None
livestreamRoadEncodeData = None
livestreamWideRoadEncodeData = None
livestreamDriverEncodeData = None
carState = None
selfdriveState = None
deviceState = None
liveCalibration = None
radarState = None
modelV2 = None
controlsState = None
onroadEvents = None
driverMonitoringState = None
driverStateV2 = None
carOutput = None
carControl = None
liveParameters = None
longitudinalPlan = None
carParams = None
roadCameraState = None
pandaStates = None
wideRoadCameraState = None
gpsLocationExternal = None
gpsLocation = None
PY

cat > "$source_root/log.capnp" <<'CAPNP'
roadEncodeData
wideRoadEncodeData
driverEncodeData
livestreamRoadEncodeData
livestreamWideRoadEncodeData
livestreamDriverEncodeData
carState
selfdriveState
deviceState
liveCalibration
radarState
modelV2
controlsState
onroadEvents
driverMonitoringState
driverStateV2
carOutput
carControl
liveParameters
longitudinalPlan
carParams
roadCameraState
pandaStates
wideRoadCameraState
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
gpsLocationExternal
gpsLocation
hasFix
horizontalAccuracy
bearingDeg
unixTimestampMillis
managerState
shouldBeRunning
exitCode
deviceState
started
struct SelfdriveState {
  state @0 :OpenpilotState;
  enabled @1 :Bool;
}
CAPNP

git -C "$op_root" init -q
git -C "$op_root" remote add origin https://github.com/commaai/openpilot.git

output="$(OP_ROOT="$op_root" "$GUARD" --manifest "$manifest" 2>&1)" || {
  printf '%s\n' "$output" >&2
  echo "FAIL: transformer-era upstream guard should not require static direct-v2 patch applicability" >&2
  exit 1
}

printf '%s\n' "$output" | grep -Fq 'PASS: upstream interface guard' || {
  printf '%s\n' "$output" >&2
  echo "FAIL: guard did not report success" >&2
  exit 1
}

python3 - "$manifest" <<'PY'
import json
import sys
manifest = json.loads(open(sys.argv[1]).read())
checks = manifest.get("checks", {})
if checks.get("onroadUiExportMethod") != "transformer":
    raise SystemExit(f"missing transformer method in manifest: {checks}")
if "directV2PatchFlavor" in checks:
    raise SystemExit(f"stale static patch manifest key present: {checks}")
expected_services = 24
if checks.get("requiredServices", 0) < expected_services:
    raise SystemExit(f"guard should cover all exporter services; expected at least {expected_services}, got {checks.get('requiredServices')}: {checks}")
expected = {
    "calibration": "liveCalibration", "roadCamera": "roadCameraState", "vehicleParameters": "liveParameters",
    "roadEncode": "roadEncodeData", "cabinEncode": "driverEncodeData",
    "livestreamRoadEncode": "livestreamRoadEncodeData", "livestreamCabinEncode": "livestreamDriverEncodeData",
}
if manifest.get("resolvedServices") != expected:
    raise SystemExit(f"legacy aliases were not resolved: {manifest}")
PY

echo "PASS: upstream interface guard validates transformer prerequisites"

current_op_root="$tmpdir/openpilot-current"
current_manifest="$tmpdir/current-manifest.json"
mkdir -p "$current_op_root/cereal"
sed -e 's/liveCalibration/extrinsicsCalibration/g' \
    -e 's/roadCameraState/narrowRoadCameraState/g' \
    -e 's/liveParameters/vehicleParameters/g' \
    -e 's/livestreamRoadEncodeData/livestreamNarrowRoadEncodeData/g' \
    -e 's/livestreamDriverEncodeData/livestreamCabinEncodeData/g' \
    -e 's/roadEncodeData/narrowRoadEncodeData/g' \
    -e 's/driverEncodeData/cabinEncodeData/g' \
    "$source_root/services.py" > "$current_op_root/cereal/services.py"
sed -e 's/liveCalibration/extrinsicsCalibration/g' \
    -e 's/roadCameraState/narrowRoadCameraState/g' \
    -e 's/liveParameters/vehicleParameters/g' \
    -e 's/livestreamRoadEncodeData/livestreamNarrowRoadEncodeData/g' \
    -e 's/livestreamDriverEncodeData/livestreamCabinEncodeData/g' \
    -e 's/roadEncodeData/narrowRoadEncodeData/g' \
    -e 's/driverEncodeData/cabinEncodeData/g' \
    "$source_root/log.capnp" > "$current_op_root/cereal/log.capnp"
write_msgq_fixture "$current_op_root"
write_car_fixture "$current_op_root/cereal"
git -C "$current_op_root" init -q
git -C "$current_op_root" remote add origin https://github.com/commaai/openpilot.git

current_output="$(OP_ROOT="$current_op_root" "$GUARD" --manifest "$current_manifest" 2>&1)" || {
  printf '%s\n' "$current_output" >&2
  echo "FAIL: upstream guard should accept current semantic aliases" >&2
  exit 1
}

python3 - "$current_manifest" <<'PY'
import json
import sys
manifest = json.loads(open(sys.argv[1]).read())
expected = {
    "calibration": "extrinsicsCalibration", "roadCamera": "narrowRoadCameraState", "vehicleParameters": "vehicleParameters",
    "roadEncode": "narrowRoadEncodeData", "cabinEncode": "cabinEncodeData",
    "livestreamRoadEncode": "livestreamNarrowRoadEncodeData", "livestreamCabinEncode": "livestreamCabinEncodeData",
}
if manifest.get("resolvedServices") != expected:
    raise SystemExit(f"current aliases were not resolved: {manifest}")
PY

echo "PASS: upstream interface guard validates current semantic aliases"

broken_op_root="$tmpdir/openpilot-broken"
broken_manifest="$tmpdir/broken-manifest.json"
cp -a "$current_op_root" "$broken_op_root"
sed -i '/narrowRoadEncodeData/d' "$broken_op_root/cereal/services.py" "$broken_op_root/cereal/log.capnp"
if OP_ROOT="$broken_op_root" "$GUARD" --manifest "$broken_manifest" >/dev/null 2>&1; then
  echo "FAIL: guard accepted a current schema missing narrowRoadEncodeData" >&2
  exit 1
fi

echo "PASS: upstream interface guard rejects a missing encoded-video alias"

ring_op_root="$tmpdir/openpilot-ring"
cp -a "$current_op_root" "$ring_op_root"
sed -i 's/\*(int64_t\*)p = -1;/*(int64_t*)p = WRAP_TAG;/' "$ring_op_root/msgq_repo/msgq/msgq.cc"
if ring_output="$(OP_ROOT="$ring_op_root" "$GUARD" --manifest "$tmpdir/ring-manifest.json" 2>&1)"; then
  echo "FAIL: guard accepted a msgq ring whose wrap tag changed" >&2
  exit 1
fi
printf '%s\n' "$ring_output" | grep -Fq 'msgq-ring:*(int64_t*)p = -1;' || {
  printf '%s\n' "$ring_output" >&2
  echo "FAIL: guard did not name the changed msgq ring line" >&2
  exit 1
}

echo "PASS: upstream interface guard rejects a changed msgq ring"

expect_guard_failure() {
  local name="$1" needle="$2" edit="$3" file="$4"
  local root="$tmpdir/openpilot-$name"
  cp -a "$current_op_root" "$root"
  sed -i "$edit" "$root/$file"
  local out
  if out="$(OP_ROOT="$root" "$GUARD" --manifest "$tmpdir/$name-manifest.json" 2>&1)"; then
    echo "FAIL: guard accepted $name" >&2
    exit 1
  fi
  printf '%s\n' "$out" | grep -Fq "$needle" || {
    printf '%s\n' "$out" >&2
    echo "FAIL: guard did not name $needle for $name" >&2
    exit 1
  }
}
expect_guard_failure many-readers 'msgq-layout:NUM_READERS' 's/NUM_READERS 15/NUM_READERS 64/' msgq_repo/msgq/msgq.h
expect_guard_failure header-order 'msgq-layout:msgq_header_t' '/uint64_t write_uid;/d' msgq_repo/msgq/msgq.h
expect_guard_failure send-order 'msgq-ring:msgq_msg_send stores the size tag' '/__sync_synchronize();/d' msgq_repo/msgq/msgq.cc
expect_guard_failure gear 'road-phase:CarState.gearShifter' '/gearShifter @14/d' cereal/car.capnp
expect_guard_failure selfdrive-enabled 'road-phase:SelfdriveState.enabled' '/enabled @1 :Bool;/d' cereal/log.capnp

echo "PASS: upstream interface guard rejects a changed msgq layout or road-phase field"

nested_op_root="$tmpdir/openpilot-nested"
nested_manifest="$tmpdir/nested-manifest.json"
mkdir -p "$nested_op_root/openpilot"
cp -a "$op_root/cereal" "$nested_op_root/openpilot/cereal"
write_msgq_fixture "$nested_op_root/openpilot"
git -C "$nested_op_root" init -q
git -C "$nested_op_root" remote add origin https://github.com/commaai/openpilot.git

nested_output="$(OP_ROOT="$nested_op_root" "$GUARD" --manifest "$nested_manifest" 2>&1)" || {
  printf '%s\n' "$nested_output" >&2
  echo "FAIL: upstream guard should support nested openpilot package roots" >&2
  exit 1
}

printf '%s\n' "$nested_output" | grep -Fq 'PASS: upstream interface guard' || {
  printf '%s\n' "$nested_output" >&2
  echo "FAIL: nested guard did not report success" >&2
  exit 1
}

python3 - "$nested_manifest" "$nested_op_root/openpilot" <<'PY'
import json
import sys
manifest = json.loads(open(sys.argv[1]).read())
if manifest.get("opSourceRoot") != sys.argv[2]:
    raise SystemExit(f"nested manifest should record opSourceRoot: {manifest}")
PY

echo "PASS: upstream interface guard validates nested openpilot package roots"
