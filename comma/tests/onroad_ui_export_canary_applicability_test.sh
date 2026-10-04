#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CACHE_ROOT="${COMMAVIEWD_CANARY_CACHE_ROOT:-$HOME/.cache/ci-ref-checkouts}"
OPENPILOT_REPO="${COMMAVIEWD_OPENPILOT_REPO:-https://github.com/commaai/openpilot.git}"
SUNNYPILOT_REPO="${COMMAVIEWD_SUNNYPILOT_REPO:-https://github.com/sunnypilot/sunnypilot.git}"
APPLY_SCRIPT="$REPO_ROOT/comma/scripts/apply_onroad_ui_export_patch.sh"
VERIFY_SCRIPT="$REPO_ROOT/comma/scripts/verify_onroad_ui_export_patch.sh"
INTERFACE_GUARD="$REPO_ROOT/commaviewd/scripts/upstream-interface-guard.sh"

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

service_consts=(
  COMMAVIEW_UI_STATE_ONROAD_SERVICE_INDEX
  COMMAVIEW_SELFDRIVE_STATE_SERVICE_INDEX
  COMMAVIEW_CAR_STATE_SERVICE_INDEX
  COMMAVIEW_CONTROLS_STATE_SERVICE_INDEX
  COMMAVIEW_ONROAD_EVENTS_SERVICE_INDEX
  COMMAVIEW_DRIVER_MONITORING_STATE_SERVICE_INDEX
  COMMAVIEW_DRIVER_STATE_V2_SERVICE_INDEX
  COMMAVIEW_MODEL_V2_SERVICE_INDEX
  COMMAVIEW_RADAR_STATE_SERVICE_INDEX
  COMMAVIEW_LIVE_CALIBRATION_SERVICE_INDEX
  COMMAVIEW_CAR_OUTPUT_SERVICE_INDEX
  COMMAVIEW_CAR_CONTROL_SERVICE_INDEX
  COMMAVIEW_LIVE_PARAMETERS_SERVICE_INDEX
  COMMAVIEW_LONGITUDINAL_PLAN_SERVICE_INDEX
  COMMAVIEW_CAR_PARAMS_SERVICE_INDEX
  COMMAVIEW_DEVICE_STATE_SERVICE_INDEX
  COMMAVIEW_ROAD_CAMERA_STATE_SERVICE_INDEX
  COMMAVIEW_PANDA_STATES_SUMMARY_SERVICE_INDEX
  COMMAVIEW_WIDE_ROAD_CAMERA_STATE_SERVICE_INDEX
)

payload_markers=(
  'def _ui_state_onroad_payload(self, ui_state) -> dict:'
  'def _selfdrive_state_payload(self, ui_state) -> dict:'
  'def _car_state_payload(self, ui_state) -> dict:'
  'def _controls_state_payload(self, ui_state) -> dict:'
  'def _onroad_events_payload(self, ui_state) -> dict:'
  'def _driver_monitoring_state_payload(self, ui_state) -> dict:'
  'def _driver_state_v2_payload(self, ui_state) -> dict:'
  'def _model_v2_payload(self, ui_state) -> dict:'
  'def _radar_state_payload(self, ui_state) -> dict:'
  'def _live_calibration_payload(self, ui_state) -> dict:'
  'def _car_output_payload(self, ui_state) -> dict:'
  'def _car_control_payload(self, ui_state) -> dict:'
  'def _live_parameters_payload(self, ui_state) -> dict:'
  'def _longitudinal_plan_payload(self, ui_state) -> dict:'
  'def _car_params_payload(self, ui_state) -> dict:'
  'def _device_state_payload(self, ui_state) -> dict:'
  'def _road_camera_state_payload(self, ui_state) -> dict:'
  'def _panda_states_summary_payload(self, ui_state) -> dict:'
  'def set_onroad_projection('
  'def _wide_road_camera_state_payload(self, ui_state) -> dict:'
)

risk_markers=(
  '"alertHudVisual": 0'
  '"allowThrottle": False'
  '"openpilotLongitudinalControl": bool(getattr(car_params, "openpilotLongitudinalControl", False))'
  '"maxLateralAccel": _safe_float(getattr(car_params, "maxLateralAccel", COMMAVIEW_DEFAULT_MAX_LAT_ACCEL))'
  '"carFingerprint": _safe_str(getattr(car_params, "carFingerprint", ""))'
  '"carName": _safe_str(getattr(car_params, "carName", ""))'
  '"carVin": _safe_str(getattr(car_params, "carVin", ""))'
  '"faceOrientation": _float_list(getattr(driver_data, "faceOrientation", []))'
  '"facePosition": _float_list(getattr(driver_data, "facePosition", []))'
  '"faceOrientationStd": _float_list(getattr(driver_data, "faceOrientationStd", []))'
  '"facePositionStd": _float_list(getattr(driver_data, "facePositionStd", []))'
  '"wheelOnRightProb": _safe_float(getattr(driver_state, "wheelOnRightProb", 0.0))'
  '"deviceType": _safe_str(device_state.deviceType)'
  '"sensor": _safe_str(getattr(road_camera_state, "sensor", ""))'
  '"sensor": _safe_str(getattr(wide_camera_state, "sensor", ""))'
  '"startedFrame": _safe_int(getattr(ui_state, "started_frame", 0))'
  '"startedTime": _safe_float(getattr(ui_state, "started_time", 0.0))'
  '"activeCamera": active_camera'
  '"wideCameraAvailable": wide_camera_available'
  'def set_onroad_camera(self, active_camera: str, wide_camera_available: bool) -> None:'
  'def set_onroad_projection('
  'self._latest_onroad_projection = {'
  '"modelTransform": _matrix3_list(model_transform)'
  '"runtimeFlavor": self._flavor'
  '"enabled": bool(getattr(controls_state, "enabled", False))'
  '"active": bool(getattr(controls_state, "active", False))'
  '"engageable": bool(getattr(controls_state, "engageable", False))'
  '"setSpeed": _safe_float(getattr(hud_control, "setSpeed", 0.0))'
  '"speedVisible": bool(getattr(hud_control, "speedVisible", False))'
  '_panda_states_summary(ui_state)'
)

legacy_markers=(
  'def _control_payload(self, ui_state) -> dict:'
  'def _scene_payload(self, ui_state) -> dict:'
  'def _status_payload(self, ui_state) -> dict:'
  'COMMAVIEW_CONTROL_SERVICE_INDEX'
  'COMMAVIEW_SCENE_SERVICE_INDEX'
  'COMMAVIEW_STATUS_SERVICE_INDEX'
  'commaViewControl'
  'commaViewScene'
  'commaViewStatus'
  'COMMAVIEW_RUNTIME_FLAVOR_UNKNOWN = "UNKNOWN"'
  'from cereal import custom'
  'def _speed_limit_pre_active_icon(self, ui_state) -> str:'
)

run_ref() {
  local label="$1"
  local repo="$2"
  local ref="$3"
  local checkout="$4"
  local expected_runtime_flavor="$5"
  local ui_platform="$6"
  local status_json="$REPO_ROOT/comma/run/onroad-ui-export-status.json"
  local interface_manifest="$checkout/commaview-interface-manifest.json"

  echo "=== ${label} ==="
  mkdir -p "$(dirname "$checkout")"
  if [[ -e "$checkout/.git" ]]; then
    git -C "$checkout" remote set-url origin "$repo"
    git -C "$checkout" fetch --depth 1 origin "$ref"
    git -C "$checkout" checkout -q FETCH_HEAD
  else
    # Blobless and sparse: the driving models are most of a release tree (1.8 of 2.0 GB on
    # release-chestnut) and nothing here reads them. Later fetches keep the blobless filter.
    git clone --depth 1 --filter=blob:none --no-checkout --branch "$ref" "$repo" "$checkout"
    git -C "$checkout" sparse-checkout set --no-cone '/*' '!/selfdrive/modeld/models/' '!/openpilot/selfdrive/modeld/models/'
    git -C "$checkout" checkout -q "$ref"
  fi

  git -C "$checkout" reset --hard -q HEAD
  git -C "$checkout" clean -fdq

  OP_ROOT="$checkout" DIST_DIR="$checkout" \
    "$INTERFACE_GUARD" --telemetry-only --manifest "$interface_manifest" >/dev/null || fail "semantic interface guard failed for ${label}"
  python3 - <<'PY' "$interface_manifest" "$label"
import json, sys
path, label = sys.argv[1:]
with open(path) as f:
    manifest = json.load(f)
resolved = manifest.get("resolvedServices", {})
expected = {
    "calibration", "roadCamera", "vehicleParameters",
    "roadEncode", "cabinEncode", "livestreamRoadEncode", "livestreamCabinEncode",
}
if set(resolved) != expected or not all(resolved.values()):
    raise SystemExit(f"bad semantic service mapping for {label}: {manifest}")
PY

  COMMAVIEWD_INSTALL_DIR="$REPO_ROOT/comma" \
    COMMAVIEWD_OP_ROOT="$checkout" \
    COMMAVIEWD_SKIP_OPENPILOT_UI_RESTART=1 \
    "$APPLY_SCRIPT" --platform "$ui_platform" || fail "transformer apply failed for ${label}"
  COMMAVIEWD_INSTALL_DIR="$REPO_ROOT/comma" \
    COMMAVIEWD_OP_ROOT="$checkout" \
    "$VERIFY_SCRIPT" --json --platform "$ui_platform" >/dev/null || fail "transformer verify failed for ${label}"

  python3 - <<'PY' "$status_json" "$label"
import json, sys
path, label = sys.argv[1:]
with open(path) as f:
    status = json.load(f)
if status.get("method") != "transformer" or not status.get("patchVerified") or status.get("repairNeeded"):
    raise SystemExit(f"bad transformer status for {label}: {status}")
if status.get("uiPlatform") not in {"mici", "tizi"} or not status.get("selectedAugmentedRoadTargets"):
    raise SystemExit(f"bad UI platform status for {label}: {status}")
PY

  ui_prefix=""
  if [[ -f "$checkout/openpilot/selfdrive/ui/ui_state.py" ]]; then
    ui_prefix="openpilot/"
  fi
  helper_path="$checkout/${ui_prefix}selfdrive/ui/commaview_export.py"
  ui_state_path="$checkout/${ui_prefix}selfdrive/ui/ui_state.py"
  if [[ "$ui_platform" == "mici" ]]; then
    augmented_road_path="$checkout/${ui_prefix}selfdrive/ui/mici/onroad/augmented_road_view.py"
  else
    augmented_road_path="$checkout/${ui_prefix}selfdrive/ui/onroad/augmented_road_view.py"
  fi
  [[ -f "$augmented_road_path" ]] || fail "selected augmented road target missing for ${label}: $augmented_road_path"

  grep -Fq 'from openpilot.selfdrive.ui.commaview_export import _CommaViewSocketExporter, COMMAVIEW_RUNTIME_FLAVOR' "$ui_state_path" || fail "ui_state exporter import missing for ${label}"
  grep -Fq 'self._commaview_exporter = _CommaViewSocketExporter(COMMAVIEW_RUNTIME_FLAVOR)' "$ui_state_path" || fail "ui_state exporter install missing for ${label}"
  grep -Fq 'self._commaview_exporter.publish(self)' "$ui_state_path" || fail "ui_state exporter publish missing for ${label}"
  grep -Fq 'cloudlog.exception("commaview ui export publish failed")' "$ui_state_path" || fail "ui_state exporter guardrail missing for ${label}"
  grep -Fq 'self._update_commaview_camera_export()' "$augmented_road_path" || fail "onroad camera relay call missing for ${label}"
  grep -Fq 'def _update_commaview_camera_export(self):' "$augmented_road_path" || fail "onroad camera relay helper missing for ${label}"
  grep -Fq 'active_camera="wideRoad" if self.stream_type == WIDE_CAM else "road"' "$augmented_road_path" || fail "onroad stream-type camera relay mapping missing for ${label}"
  grep -Fq 'active_camera="wideRoad" if is_wide_camera else "road"' "$augmented_road_path" || fail "wide onroad projection camera mapping missing for ${label}"
  grep -Fq 'model_transform = video_transform @ calib_transform' "$augmented_road_path" || fail "model transform assignment missing for ${label}"
  grep -Fq 'camera_offset=getattr(self._model_renderer, "_camera_offset", 0.0)' "$augmented_road_path" || grep -Fq 'camera_offset=getattr(self.model_renderer, "_camera_offset", 0.0)' "$augmented_road_path" || fail "projection camera offset missing for ${label}"
  grep -Fq 'self._offer_payload(COMMAVIEW_ONROAD_PROJECTION_SERVICE_INDEX, self._latest_onroad_projection)' "$helper_path" || fail "latest-value onroad projection offer missing for ${label}"
  grep -Fq "COMMAVIEW_RUNTIME_FLAVOR = \"$expected_runtime_flavor\"" "$helper_path" || fail "runtime flavor constant missing for ${label}"
  grep -Fq 'COMMAVIEW_FRAME_VERSION = 1' "$helper_path" || fail "frame version missing for ${label}"
  grep -Fq 'COMMAVIEW_SOCKET_PATH_DEFAULT = "/data/commaview/run/ui-export.sock"' "$helper_path" || fail "socket path missing for ${label}"
  grep -Fq 'os.environ.get("COMMAVIEWD_UI_EXPORT_SOCKET") or COMMAVIEW_SOCKET_PATH_DEFAULT' "$helper_path" || fail "socket env override missing for ${label}"
  grep -Fq 'socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)' "$helper_path" || fail "unix socket client missing for ${label}"
  grep -Fq 'struct.pack(">I", len(frame)) + frame' "$helper_path" || fail "frame packing missing for ${label}"
  grep -Fq 'raw = _encode_json(payload)' "$helper_path" || fail "pluggable compact json encoding missing for ${label}"
  grep -Fq 'from opendbc.car import ACCELERATION_DUE_TO_GRAVITY' "$helper_path" || fail "torque accel helper import missing for ${label}"
  grep -Fq 'def _torque_bar_value(ui_state, service_resolver=None) -> float:' "$helper_path" || fail "torque bar helper missing for ${label}"
  grep -Fq 'def _worker_main(self) -> None:' "$helper_path" || fail "export worker missing for ${label}"

  for marker in "${payload_markers[@]}"; do
    grep -Fq "$marker" "$helper_path" || fail "payload helper missing for ${label}: $marker"
  done

  for const_name in "${service_consts[@]}"; do
    grep -Fq "(${const_name}, self._" "$helper_path" || fail "publish path missing for ${label}: $const_name"
  done

  for marker in "${risk_markers[@]}"; do
    grep -Fq "$marker" "$helper_path" || fail "risk-field marker missing for ${label}: $marker"
  done

  # Sunnypilot params (rainbow included) are exported only for the SUNNYPILOT flavor; openpilot exports false.
  grep -Fq '"rainbowPathEnabled": self._flavor == "SUNNYPILOT" and bool(getattr(ui_state, "rainbow_path", False))' "$helper_path" || fail "$label missing truthful rainbow export"
  grep -Fq 'if self._flavor != "SUNNYPILOT":' "$helper_path" || fail "$label must gate sunnypilot params on the runtime flavor"

  for marker in "${legacy_markers[@]}"; do
    ! grep -Fq "$marker" "$helper_path" || fail "legacy marker still present for ${label}: $marker"
  done

  printf '%s\n' "{\"healthy\":false,\"patchVerified\":true,\"method\":\"transformer\",\"statusScope\":\"patch-installation\",\"repairNeeded\":false,\"state\":\"patch-verified\",\"reason\":\"source transformer socket ui export verified on real canary ref\"}"

  git -C "$checkout" reset --hard -q HEAD
  git -C "$checkout" clean -fdq
}

# The refs CI (ci/targets.json) and the canaries (ci/canary-*.json) check. The comma 3's frozen
# release-tici isn't supported, so it isn't here.
run_ref 'openpilot release-mici' "$OPENPILOT_REPO" 'release-mici' "$CACHE_ROOT/openpilot-release-mici" 'OPENPILOT' 'mici'
run_ref 'openpilot release-tizi' "$OPENPILOT_REPO" 'release-tizi' "$CACHE_ROOT/openpilot-release-tizi" 'OPENPILOT' 'tizi'
run_ref 'openpilot release-chestnut (mici)' "$OPENPILOT_REPO" 'release-chestnut' "$CACHE_ROOT/openpilot-release-chestnut" 'OPENPILOT' 'mici'
run_ref 'openpilot release-chestnut (tizi)' "$OPENPILOT_REPO" 'release-chestnut' "$CACHE_ROOT/openpilot-release-chestnut" 'OPENPILOT' 'tizi'
run_ref 'openpilot release-mici-staging' "$OPENPILOT_REPO" 'release-mici-staging' "$CACHE_ROOT/openpilot-release-mici-staging" 'OPENPILOT' 'mici'
run_ref 'openpilot release-tizi-staging' "$OPENPILOT_REPO" 'release-tizi-staging' "$CACHE_ROOT/openpilot-release-tizi-staging" 'OPENPILOT' 'tizi'
run_ref 'openpilot release-chestnut-staging (mici)' "$OPENPILOT_REPO" 'release-chestnut-staging' "$CACHE_ROOT/openpilot-release-chestnut-staging" 'OPENPILOT' 'mici'
run_ref 'openpilot release-chestnut-staging (tizi)' "$OPENPILOT_REPO" 'release-chestnut-staging' "$CACHE_ROOT/openpilot-release-chestnut-staging" 'OPENPILOT' 'tizi'
run_ref 'openpilot nightly (mici)' "$OPENPILOT_REPO" 'nightly' "$CACHE_ROOT/openpilot-nightly" 'OPENPILOT' 'mici'
run_ref 'openpilot nightly (tizi)' "$OPENPILOT_REPO" 'nightly' "$CACHE_ROOT/openpilot-nightly" 'OPENPILOT' 'tizi'
run_ref 'openpilot nightly-dev (mici)' "$OPENPILOT_REPO" 'nightly-dev' "$CACHE_ROOT/openpilot-nightly-dev" 'OPENPILOT' 'mici'
run_ref 'openpilot nightly-dev (tizi)' "$OPENPILOT_REPO" 'nightly-dev' "$CACHE_ROOT/openpilot-nightly-dev" 'OPENPILOT' 'tizi'
run_ref 'openpilot nightly-chestnut (mici)' "$OPENPILOT_REPO" 'nightly-chestnut' "$CACHE_ROOT/openpilot-nightly-chestnut" 'OPENPILOT' 'mici'
run_ref 'openpilot nightly-chestnut (tizi)' "$OPENPILOT_REPO" 'nightly-chestnut' "$CACHE_ROOT/openpilot-nightly-chestnut" 'OPENPILOT' 'tizi'
run_ref 'openpilot nightly-chestnut-dev (mici)' "$OPENPILOT_REPO" 'nightly-chestnut-dev' "$CACHE_ROOT/openpilot-nightly-chestnut-dev" 'OPENPILOT' 'mici'
run_ref 'openpilot nightly-chestnut-dev (tizi)' "$OPENPILOT_REPO" 'nightly-chestnut-dev' "$CACHE_ROOT/openpilot-nightly-chestnut-dev" 'OPENPILOT' 'tizi'

run_ref 'sunnypilot release-mici' "$SUNNYPILOT_REPO" 'release-mici' "$CACHE_ROOT/sunnypilot-release-mici" 'SUNNYPILOT' 'mici'
run_ref 'sunnypilot release-tizi' "$SUNNYPILOT_REPO" 'release-tizi' "$CACHE_ROOT/sunnypilot-release-tizi" 'SUNNYPILOT' 'tizi'
run_ref 'sunnypilot release-mici-staging' "$SUNNYPILOT_REPO" 'release-mici-staging' "$CACHE_ROOT/sunnypilot-release-mici-staging" 'SUNNYPILOT' 'mici'
run_ref 'sunnypilot release-tizi-staging' "$SUNNYPILOT_REPO" 'release-tizi-staging' "$CACHE_ROOT/sunnypilot-release-tizi-staging" 'SUNNYPILOT' 'tizi'
run_ref 'sunnypilot staging-chestnut (mici)' "$SUNNYPILOT_REPO" 'staging-chestnut' "$CACHE_ROOT/sunnypilot-staging-chestnut" 'SUNNYPILOT' 'mici'
run_ref 'sunnypilot staging-chestnut (tizi)' "$SUNNYPILOT_REPO" 'staging-chestnut' "$CACHE_ROOT/sunnypilot-staging-chestnut" 'SUNNYPILOT' 'tizi'
run_ref 'sunnypilot dev (mici)' "$SUNNYPILOT_REPO" 'dev' "$CACHE_ROOT/sunnypilot-dev" 'SUNNYPILOT' 'mici'
run_ref 'sunnypilot dev (tizi)' "$SUNNYPILOT_REPO" 'dev' "$CACHE_ROOT/sunnypilot-dev" 'SUNNYPILOT' 'tizi'
run_ref 'sunnypilot dev-chestnut (mici)' "$SUNNYPILOT_REPO" 'dev-chestnut' "$CACHE_ROOT/sunnypilot-dev-chestnut" 'SUNNYPILOT' 'mici'
run_ref 'sunnypilot dev-chestnut (tizi)' "$SUNNYPILOT_REPO" 'dev-chestnut' "$CACHE_ROOT/sunnypilot-dev-chestnut" 'SUNNYPILOT' 'tizi'
