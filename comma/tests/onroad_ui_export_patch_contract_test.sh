#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OPENPILOT_TEMPLATE="$REPO_ROOT/comma/src/commaview_export.openpilot.py"
SUNNYPILOT_TEMPLATE="$REPO_ROOT/comma/src/commaview_export.sunnypilot.py"
TRANSFORMER="$REPO_ROOT/comma/scripts/transform_onroad_ui_export.py"
SMOKE_SCRIPT="$REPO_ROOT/comma/scripts/smoke_onroad_ui_export_helper.py"
INSTALLER="$REPO_ROOT/comma/install.sh"
APPLY_SCRIPT="$REPO_ROOT/comma/scripts/apply_onroad_ui_export_patch.sh"
VERIFY_SCRIPT="$REPO_ROOT/comma/scripts/verify_onroad_ui_export_patch.sh"

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
  '"runtimeFlavor": self._flavor'
  '"enabled": bool(getattr(controls_state, "enabled", False))'
  '"active": bool(getattr(controls_state, "active", False))'
  '"engageable": bool(getattr(controls_state, "engageable", False))'
  '"setSpeed": _safe_float(getattr(hud_control, "setSpeed", 0.0))'
  '"speedVisible": bool(getattr(hud_control, "speedVisible", False))'
  '"valid": bool(ui_state.sm.valid["radarState"])'
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

[[ -f "$TRANSFORMER" ]] || fail "missing transformer $TRANSFORMER"
[[ -f "$SMOKE_SCRIPT" ]] || fail "missing helper smoke script $SMOKE_SCRIPT"
[[ -f "$APPLY_SCRIPT" ]] || fail "missing apply script $APPLY_SCRIPT"
[[ -f "$VERIFY_SCRIPT" ]] || fail "missing verify script $VERIFY_SCRIPT"
grep -Fq 'python3 "$TRANSFORMER" --op-root "$OP_ROOT" --flavor "$flavor" --platform "$ui_platform"' "$APPLY_SCRIPT" || fail "apply script does not invoke transformer with selected platform"
grep -Fq -- '--platform auto|mici|tizi]' "$APPLY_SCRIPT" || fail "apply script help missing platform selector"
grep -Fq 'ONROAD_UI_EXPORT_UI_PLATFORM=%s' "$APPLY_SCRIPT" || fail "apply script does not persist UI platform"
grep -Fq 'ONROAD_UI_EXPORT_METHOD=transformer' "$APPLY_SCRIPT" || fail "apply script does not persist transformer method"
grep -Fq '"method": os.environ.get("METHOD", "")' "$VERIFY_SCRIPT" || fail "verify status missing method field wiring"
grep -Fq '"uiPlatform": os.environ.get("UI_PLATFORM", "")' "$VERIFY_SCRIPT" || fail "verify status missing UI platform"
grep -Fq '"deviceModel": os.environ.get("DEVICE_MODEL", "")' "$VERIFY_SCRIPT" || fail "verify status missing device model"
grep -Fq '"selectedAugmentedRoadTargets":' "$VERIFY_SCRIPT" || fail "verify status missing selected augmented targets"
grep -Fq 'json.dumps(payload, separators=(",", ":"))' "$VERIFY_SCRIPT" || fail "verify status missing compact json.dumps output"
grep -Fq '"src/commaview_export.openpilot.py"' "$INSTALLER" || fail "installer missing openpilot helper template"
grep -Fq '"src/commaview_export.sunnypilot.py"' "$INSTALLER" || fail "installer missing sunnypilot helper template"
grep -Fq '"scripts/transform_onroad_ui_export.py"' "$INSTALLER" || fail "installer missing transformer script"
grep -Fq '"scripts/smoke_onroad_ui_export_helper.py"' "$INSTALLER" || fail "installer missing helper smoke script"
grep -Fq 'smoke_onroad_ui_export_helper.py' "$VERIFY_SCRIPT" || fail "verify script missing generated helper smoke gate"
grep -Fq '"payloadSmokePassed"' "$VERIFY_SCRIPT" || fail "verify script missing payload smoke status"
grep -Fq '"criticalServicesPublishable"' "$VERIFY_SCRIPT" || fail "verify script missing critical service smoke status"
! grep -Fq 'git -C "$OP_ROOT" apply' "$APPLY_SCRIPT" || fail "apply script still uses static git apply lifecycle"

for template in "$OPENPILOT_TEMPLATE" "$SUNNYPILOT_TEMPLATE"; do
  [[ -f "$template" ]] || fail "missing helper template $template"
  expected_runtime_flavor='OPENPILOT'
  if [[ "$template" == *'.sunnypilot.py' ]]; then
    expected_runtime_flavor='SUNNYPILOT'
  fi

  grep -Fq "COMMAVIEW_RUNTIME_FLAVOR = \"$expected_runtime_flavor\"" "$template" || fail "$template missing runtime flavor constant"
  grep -Fq 'COMMAVIEW_FRAME_VERSION = 1' "$template" || fail "$template missing frame version"
  grep -Fq 'COMMAVIEW_SOCKET_PATH_DEFAULT = "/data/commaview/run/ui-export.sock"' "$template" || fail "$template missing default socket path"
  grep -Fq 'os.environ.get("COMMAVIEWD_UI_EXPORT_SOCKET") or COMMAVIEW_SOCKET_PATH_DEFAULT' "$template" || fail "$template missing socket env override"
  grep -Fq 'socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)' "$template" || fail "$template missing unix socket client"
  grep -Fq 'struct.pack(">I", len(frame)) + frame' "$template" || fail "$template missing framing"
  grep -Fq 'raw = _encode_json(payload)' "$template" || fail "$template missing pluggable compact json encoding"
  grep -Fq 'self._offer_payload(COMMAVIEW_ONROAD_PROJECTION_SERVICE_INDEX, self._latest_onroad_projection)' "$template" || fail "$template missing latest-value onroad projection offer"
  grep -Fq 'from opendbc.car import ACCELERATION_DUE_TO_GRAVITY' "$template" || fail "$template missing torque helper import"
  grep -Fq 'def _torque_bar_value(ui_state, service_resolver=None) -> float:' "$template" || fail "$template missing torque bar helper"
  grep -Fq 'payload_fn = self._payload_builders[service_index]' "$template" || fail "$template missing per-service payload isolation in publish()"
  grep -Fq 'def _worker_main(self) -> None:' "$template" || fail "$template missing isolated export worker"
  grep -Fq 'UPSTREAM_SERVICE_ALIASES = {' "$template" || fail "$template missing semantic service aliases"
  grep -Fq 'except Exception:' "$template" || fail "$template missing per-service payload isolation"

  for marker in "${payload_markers[@]}"; do
    grep -Fq "$marker" "$template" || fail "$template missing payload marker: $marker"
  done

  for const_name in "${service_consts[@]}"; do
    grep -Fq "(${const_name}, self._" "$template" || fail "$template missing publish path for $const_name"
  done

  for marker in "${risk_markers[@]}"; do
    grep -Fq "$marker" "$template" || fail "$template missing risk-field marker: $marker"
  done

  # Both templates share every line but the flavor constant: sunnypilot params are exported only for the
  # SUNNYPILOT flavor (rainbowPathEnabled stays false on openpilot) and sunnypilot services are optional.
  grep -Fq '"rainbowPathEnabled": self._flavor == "SUNNYPILOT" and bool(getattr(ui_state, "rainbow_path", False))' "$template" || fail "$template missing truthful rainbow export"
  grep -Fq '("RainbowMode", "rainbow_path", "bool", False)' "$template" || fail "$template missing sunnypilot rainbow param source"
  grep -Fq 'if self._flavor != "SUNNYPILOT":' "$template" || fail "$template must gate sunnypilot params on the runtime flavor"
  grep -Fq '"speedLimitPreActive": speed_limit_pre_active' "$template" || fail "$template missing sunnypilot speed-limit pre-active export"
  grep -Fq '"speedLimitPreActiveIcon": speed_limit_pre_active_icon' "$template" || fail "$template missing sunnypilot speed-limit pre-active icon export"
  grep -Fq 'speed_limit_final_last = ui_state.sm["longitudinalPlanSP"].speedLimit.resolver.speedLimitFinalLast' "$template" || fail "$template missing sunnypilot speed-limit target source"

  for marker in "${legacy_markers[@]}"; do
    ! grep -Fq "$marker" "$template" || fail "$template still contains legacy marker: $marker"
  done
done

grep -Fq 'from openpilot.selfdrive.ui.commaview_export import _CommaViewSocketExporter, COMMAVIEW_RUNTIME_FLAVOR' "$TRANSFORMER" || fail "transformer missing ui_state exporter import marker"
grep -Fq 'self._commaview_exporter = _CommaViewSocketExporter(COMMAVIEW_RUNTIME_FLAVOR)' "$TRANSFORMER" || fail "transformer missing exporter installation marker"
grep -Fq 'self._commaview_exporter.publish(self)' "$TRANSFORMER" || fail "transformer missing exporter publish marker"
grep -Fq 'self._update_commaview_camera_export()' "$TRANSFORMER" || fail "transformer missing onroad camera relay call marker"
grep -Fq 'def _update_commaview_camera_export(self):' "$TRANSFORMER" || fail "transformer missing onroad camera relay helper marker"
grep -Fq '"wideRoad" if self.stream_type == WIDE_CAM else "road"' "$TRANSFORMER" || fail "transformer missing stream-type camera relay mapping"
grep -Fq 'active_camera="wideRoad" if is_wide_camera else "road"' "$TRANSFORMER" || fail "transformer missing wide onroad projection camera mapping"
grep -Fq 'cloudlog.exception("commaview ui export publish failed")' "$TRANSFORMER" || fail "transformer missing publish guardrail"
grep -Fq 'parser.add_argument("--platform", default="auto", choices=PLATFORM_CHOICES)' "$TRANSFORMER" || fail "transformer missing platform argument"
# The comma 3 (tici) is refused by name (exit 43), never folded into the comma 3X (tizi).
! grep -Fq 'NORMALIZED_PLATFORMS' "$TRANSFORMER" || fail "transformer must not alias tici to tizi"
grep -Fq 'UNSUPPORTED_DEVICE_EXIT = 43' "$TRANSFORMER" || fail "transformer should refuse tici with exit 43"
for script in "$APPLY_SCRIPT" "$VERIFY_SCRIPT"; do
  grep -Fq 'UNSUPPORTED_DEVICE_EXIT=43' "$script" || fail "$script should refuse tici with exit 43"
  ! grep -Fq 'tizi|tici)' "$script" || fail "$script must not fold tici into tizi"
done

# openpilot cannot reload its UI in place: manager never restarts a UI that exits,
# and sunnypilot's restart_if_crash restarts it from modules manager preimported.
# So nothing may signal it; the boot hook patches before launch_openpilot.sh, and
# verify reports a patch applied while openpilot runs as pending a reboot.
START_SCRIPT="$REPO_ROOT/comma/start.sh"
REVERT_SCRIPT="$REPO_ROOT/comma/scripts/revert_onroad_ui_export_patch.sh"
for script in "$APPLY_SCRIPT" "$REVERT_SCRIPT" "$VERIFY_SCRIPT" "$START_SCRIPT" "$INSTALLER"; do
  ! grep -Eq '^[^#]*\b(pkill|pgrep|killall)\b' "$script" || fail "$script must not signal openpilot processes"
done
grep -Fq -- '--before-openpilot) BEFORE_OPENPILOT=1' "$APPLY_SCRIPT" || fail "apply script missing pre-launch mode"
grep -Fq 'openpilot_root_after_launch "$OP_ROOT"' "$APPLY_SCRIPT" || fail "apply pre-launch mode must patch the tree launch_chffrplus.sh will run"
grep -Fq '"$ONROAD_UI_EXPORT_APPLY" --before-openpilot' "$START_SCRIPT" || fail "start.sh boot step must apply before openpilot starts"
grep -Fq 'BOOT_HOOK_CMD="/data/commaview/start.sh --before-openpilot"' "$INSTALLER" || fail "installer must hook start.sh synchronously before launch_openpilot"
grep -Fq '"uiReloadPending": env_bool("UI_RELOAD_PENDING")' "$VERIFY_SCRIPT" || fail "verify status missing uiReloadPending"
grep -Fq '"uiReloadAction": os.environ.get("UI_RELOAD_ACTION", "")' "$VERIFY_SCRIPT" || fail "verify status missing uiReloadAction"

echo "PASS: source transformer socket UI export contract present"
