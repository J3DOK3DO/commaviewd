#!/usr/bin/env bash
set -euo pipefail

INSTALL_DIR="${COMMAVIEWD_INSTALL_DIR:-/data/commaview}"
OP_ROOT="${COMMAVIEWD_OP_ROOT:-/data/openpilot}"
SRC_ROOT="$INSTALL_DIR/src"
TRANSFORMER="$INSTALL_DIR/scripts/transform_onroad_ui_export.py"
SMOKE_SCRIPT="$INSTALL_DIR/scripts/smoke_onroad_ui_export_helper.py"
STATE_JSON="$INSTALL_DIR/run/onroad-ui-export-status.json"
STATE_ENV="$INSTALL_DIR/config/onroad-ui-export-patch.env"
UI_RELOAD_MARKER="$INSTALL_DIR/run/onroad-ui-export-ui-restart-needed"
PROC_ROOT="${COMMAVIEWD_PROC_ROOT:-/proc}"
UI_PREFIX=""
HELPER_PATH=""
UI_STATE_PATH=""
AUGMENTED_ROAD_PATHS=()
REQUESTED_UI_PLATFORM="auto"

while [ "$#" -gt 0 ]; do
  case "$1" in
    --json) shift ;;  # accepted for callers; output is always JSON
    --platform) REQUESTED_UI_PLATFORM="${2:-}"; shift 2 ;;
    --platform=*) REQUESTED_UI_PLATFORM="${1#--platform=}"; shift ;;
    -h|--help) echo "Usage: verify_onroad_ui_export_patch.sh [--json] [--platform auto|mici|tizi]"; exit 0 ;;
    *) echo "ERROR: unknown option: $1" >&2; exit 1 ;;
  esac
done

mkdir -p "$(dirname "$STATE_JSON")" "$(dirname "$STATE_ENV")"

# Status reads must not rewrite .git/index: launch_chffrplus.sh skips a staged
# openpilot update when anything under .git is newer than .overlay_init.
export GIT_OPTIONAL_LOCKS=0

# ---- openpilot UI reload status ----
# openpilot cannot reload its UI in place (manager never restarts an exited UI;
# sunnypilot's restart reuses modules manager imported at startup), so UI files
# patched while openpilot runs take effect when manager next starts. apply
# writes UI_RELOAD_MARKER with the boot and uptime of that patch; it stays
# pending until a reboot or a manager started after it.
marker_value() {
  { sed -n "s/^$1=//p" "$UI_RELOAD_MARKER" 2>/dev/null || true; } | tail -n 1
}

current_boot_id() {
  tr -d '\r\n' < "$PROC_ROOT/sys/kernel/random/boot_id" 2>/dev/null || true
}

# Process start time in centiseconds since boot (field 22 of /proc/<pid>/stat).
proc_start_cs() {
  local stat rest clk
  local -a fields
  stat="$(cat "$PROC_ROOT/$1/stat" 2>/dev/null)" || return 1
  rest="${stat##*) }"
  read -r -a fields <<< "$rest"
  [ "${#fields[@]}" -ge 20 ] || return 1
  case "${fields[19]}" in ''|*[!0-9]*) return 1 ;; esac
  clk="$(getconf CLK_TCK 2>/dev/null || echo 100)"
  case "$clk" in ''|*[!0-9]*|0) clk=100 ;; esac
  printf '%s\n' "$(( ${fields[19]} * 100 / clk ))"
}

# Sets OPENPILOT_PROCESS_KIND to "ui" or "manager" when <proc dir> is openpilot's
# UI (setproctitle names it "openpilot.selfdrive.ui.ui" or "selfdrive.ui.ui") or its
# manager ("python3 ./manager.py"); returns 1 otherwise. Sets a variable instead of
# printing so scanning every process costs no forks.
openpilot_process_kind() {
  local first cmd
  local -a args=()
  OPENPILOT_PROCESS_KIND=""
  { mapfile -t -d '' args < "$1/cmdline"; } 2>/dev/null || return 1
  [ "${#args[@]}" -gt 0 ] || return 1
  first="${args[0]%"${args[0]##*[![:space:]]}"}"
  case "$first" in
    *selfdrive.ui.ui) OPENPILOT_PROCESS_KIND="ui"; return 0 ;;
  esac
  cmd=" ${args[*]} "
  case "$cmd" in
    *" ./manager.py "*|*"/system/manager/manager.py "*|*"system.manager.manager "*|*"/selfdrive/manager/manager.py "*|*"selfdrive.manager.manager "*)
      OPENPILOT_PROCESS_KIND="manager"
      return 0
      ;;
  esac
  return 1
}

# The UI is forked by manager, so its parent is manager. Without a UI process,
# the oldest process running manager.py.
openpilot_manager_pid() {
  local proc pid ppid best="" best_cs="" cs
  for proc in "$PROC_ROOT"/[0-9]*; do
    openpilot_process_kind "$proc" || continue
    pid="${proc##*/}"
    if [ "$OPENPILOT_PROCESS_KIND" = "ui" ]; then
      ppid="$(sed -n 's/^PPid:[[:space:]]*//p' "$proc/status" 2>/dev/null || true)"
      case "$ppid" in
        ''|*[!0-9]*|0|1) ;;
        *) if [ -d "$PROC_ROOT/$ppid" ]; then printf '%s\n' "$ppid"; return 0; fi ;;
      esac
      continue
    fi
    cs="$(proc_start_cs "$pid" || true)"
    [ -n "$cs" ] || continue
    if [ -z "$best" ] || [ "$cs" -lt "$best_cs" ]; then
      best="$pid"
      best_cs="$cs"
    fi
  done
  [ -n "$best" ] || return 1
  printf '%s\n' "$best"
}

check_fixed() {
  local needle="$1"
  local file="$2"
  grep -Fq -- "$needle" "$file"
}

state_value() {
  local key="$1"
  [ -f "$STATE_ENV" ] || return 1
  sed -n "s/^${key}=//p" "$STATE_ENV" | tail -n 1 | sed 's/^"//; s/"$//'
}

upstream_head() {
  git -C "$OP_ROOT" rev-parse HEAD 2>/dev/null || true
}

upstream_remote() {
  git -C "$OP_ROOT" remote get-url origin 2>/dev/null || true
}

upstream_branch() {
  git -C "$OP_ROOT" symbolic-ref --quiet --short HEAD 2>/dev/null || true
}

detect_ui_prefix() {
  if [ -f "$OP_ROOT/selfdrive/ui/ui_state.py" ]; then
    UI_PREFIX=""
  elif [ -f "$OP_ROOT/openpilot/selfdrive/ui/ui_state.py" ]; then
    UI_PREFIX="openpilot/"
  else
    UI_PREFIX=""
  fi
  HELPER_PATH="$OP_ROOT/${UI_PREFIX}selfdrive/ui/commaview_export.py"
  UI_STATE_PATH="$OP_ROOT/${UI_PREFIX}selfdrive/ui/ui_state.py"
}

normalize_ui_platform() {
  case "$1" in
    auto) printf '%s\n' auto ;;
    mici) printf '%s\n' mici ;;
    tizi) printf '%s\n' tizi ;;
    # Not supported, and named so the caller can say so (see unsupported_device_message).
    tici) printf '%s\n' tici ;;
    *) return 1 ;;
  esac
}

# The comma 3 (tici) runs openpilot's frozen release-tici, whose screen is the native Qt UI: the
# Python UI the transformer edits isn't running there, so a patch would only look like it worked.
UNSUPPORTED_DEVICE_EXIT=43
unsupported_device_message() {
  echo "CommaView doesn't support the comma 3 (tici): its openpilot release (release-tici) is frozen upstream and draws its screen differently. comma 3X and comma four are supported."
}

# Which comma this is, as openpilot reads it (HARDWARE.get_device_type()): "comma tici", "comma
# tizi" or "comma mici".
read_device_model() {
  local model_path="${COMMAVIEWD_DEVICE_MODEL_FILE:-/sys/firmware/devicetree/base/model}"
  [ -f "$model_path" ] || return 1
  tr -d '\000\r\n' < "$model_path" | sed 's/^comma //'
}

detect_ui_platform() {
  local requested="$1"
  local state_platform=""
  local state_op_root=""
  local device_model=""
  if [ "$requested" != "auto" ]; then
    normalize_ui_platform "$requested"
    return $?
  fi
  device_model="$(read_device_model || true)"
  case "$device_model" in
    *mici*|*MICI*|*comma\ 4*) printf '%s\n' mici; return 0 ;;
    *tizi*|*TIZI*) printf '%s\n' tizi; return 0 ;;
    *tici*|*TICI*) printf '%s\n' tici; return 0 ;;
  esac
  state_platform="$(state_value ONROAD_UI_EXPORT_UI_PLATFORM || true)"
  state_op_root="$(state_value ONROAD_UI_EXPORT_OP_ROOT || true)"
  if [ "$state_op_root" = "$OP_ROOT" ] && normalize_ui_platform "$state_platform" >/dev/null 2>&1; then
    normalize_ui_platform "$state_platform"
    return 0
  fi
  if [ -f "$OP_ROOT/${UI_PREFIX}selfdrive/ui/mici/onroad/augmented_road_view.py" ] && [ ! -f "$OP_ROOT/${UI_PREFIX}selfdrive/ui/onroad/augmented_road_view.py" ]; then
    printf '%s\n' mici; return 0
  fi
  if [ -f "$OP_ROOT/${UI_PREFIX}selfdrive/ui/onroad/augmented_road_view.py" ] && [ ! -f "$OP_ROOT/${UI_PREFIX}selfdrive/ui/mici/onroad/augmented_road_view.py" ]; then
    printf '%s\n' tizi; return 0
  fi
  printf '%s\n' auto
}

augmented_paths_for_platform() {
  case "$1" in
    mici) printf '%s\n' "$OP_ROOT/${UI_PREFIX}selfdrive/ui/mici/onroad/augmented_road_view.py" ;;
    tizi) printf '%s\n' "$OP_ROOT/${UI_PREFIX}selfdrive/ui/onroad/augmented_road_view.py" ;;
    auto)
      printf '%s\n' "$OP_ROOT/${UI_PREFIX}selfdrive/ui/mici/onroad/augmented_road_view.py"
      printf '%s\n' "$OP_ROOT/${UI_PREFIX}selfdrive/ui/onroad/augmented_road_view.py"
      ;;
    *) return 1 ;;
  esac
}

detect_ui_prefix
ui_platform="$(detect_ui_platform "$REQUESTED_UI_PLATFORM")" || ui_platform=""
mapfile -t AUGMENTED_ROAD_PATHS < <(augmented_paths_for_platform "${ui_platform:-auto}" || true)

remote_flavor() {
  local remote="$1"
  remote="${remote%.git}"
  remote="${remote%/}"
  case "$remote" in
    *github.com:commaai/openpilot|*github.com/commaai/openpilot) printf '%s\n' openpilot ;;
    *github.com:sunnypilot/sunnypilot|*github.com/sunnypilot/sunnypilot|*github.com:sunnypilot/openpilot|*github.com/sunnypilot/openpilot) printf '%s\n' sunnypilot ;;
    *) return 1 ;;
  esac
}

detect_flavor() {
  local preferred=""
  local remote=""
  local state_flavor=""
  local state_op_root=""

  if git -C "$OP_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    remote="$(git -C "$OP_ROOT" remote get-url origin 2>/dev/null || true)"
    if [ -n "$remote" ]; then
      preferred="$(remote_flavor "$remote")" || return 1
    fi
  fi

  if [ -z "$preferred" ]; then
    state_flavor="$(state_value ONROAD_UI_EXPORT_FLAVOR || true)"
    state_op_root="$(state_value ONROAD_UI_EXPORT_OP_ROOT || true)"
    if [ "$state_op_root" = "$OP_ROOT" ] && { [ "$state_flavor" = "openpilot" ] || [ "$state_flavor" = "sunnypilot" ]; }; then
      preferred="$state_flavor"
    fi
  fi

  if [ -n "$preferred" ] && [ -f "$SRC_ROOT/commaview_export.${preferred}.py" ]; then
    printf '%s\n' "$preferred"
    return 0
  fi
  return 1
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
)

if flavor="$(detect_flavor)"; then
  flavor_detected=1
else
  flavor=""
  flavor_detected=0
fi
template_path="$SRC_ROOT/commaview_export.${flavor}.py"
state="stale"
status_scope="patch-installation"
method="transformer"
reason="onroad UI export status unavailable"
healthy=false
patch_verified=false
repair_needed=true
helper_present=false
ui_state_hook_present=false
runtime_flavor_constant_present=false
socket_path_present=false
socket_env_present=false
frame_version_present=false
unix_socket_present=false
framing_present=false
compact_json_present=false
payload_helpers_present=false
publish_paths_present=false
risk_fields_present=false
legacy_bucket_markers_absent=false
exporter_install_present=false
exporter_publish_present=false
onroad_camera_relay_present=false
onroad_projection_present=false
transformer_present=false
template_present=false
smoke_script_present=false
payload_smoke_passed=false
critical_services_publishable=false
patched_files_compile=false
service_send_count=0
fingerprint=""
service_marker_count=0
upstream_head_value=""
upstream_remote_value=""
upstream_branch_value=""
applied_upstream_head="$(state_value ONROAD_UI_EXPORT_UPSTREAM_HEAD || true)"
upstream_changed_since_apply=false
selected_augmented_road_targets=""
device_model_value="$(read_device_model || true)"

if git -C "$OP_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  upstream_head_value="$(upstream_head)"
  upstream_remote_value="$(upstream_remote)"
  upstream_branch_value="$(upstream_branch)"
fi

if [ "$ui_platform" = "tici" ]; then
  # Read-only like every check here: report it so the app can say so, and exit with its own status.
  state="unsupported-device"; reason="$(unsupported_device_message)"; repair_needed=false
elif ! git -C "$OP_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  state="missing-repo"; reason="upstream repo not found at $OP_ROOT"
elif [ -z "$ui_platform" ]; then
  state="unknown-platform"; reason="unable to determine UI platform for $OP_ROOT; pass --platform mici or --platform tizi"
elif [ "$flavor_detected" -ne 1 ]; then
  state="unknown-flavor"; reason="unsupported upstream remote for $OP_ROOT; CommaView currently supports only commaai/openpilot and sunnypilot remotes"
elif [ ! -f "$template_path" ]; then
  state="missing-template"; reason="missing socket UI export helper template for $flavor"
elif [ ! -f "$TRANSFORMER" ]; then
  state="missing-transformer"; reason="missing socket UI export transformer"
elif [ ! -f "$SMOKE_SCRIPT" ]; then
  state="missing-smoke-script"; reason="missing socket UI export helper smoke test"
else
  transformer_present=true
  template_present=true
  smoke_script_present=true
  fingerprint="$(sha256sum "$TRANSFORMER" "$template_path" | sha256sum | awk '{print $1}')"
  expected_runtime_flavor="OPENPILOT"
  if [ "$flavor" = "sunnypilot" ]; then
    expected_runtime_flavor="SUNNYPILOT"
  fi

  [ -f "$HELPER_PATH" ] && helper_present=true || true
  # The import is guarded so openpilot's UI loads even when the exporter can't.
  check_fixed 'from openpilot.selfdrive.ui.commaview_export import _CommaViewSocketExporter, COMMAVIEW_RUNTIME_FLAVOR' "$UI_STATE_PATH" && \
    check_fixed '_CommaViewSocketExporter = None' "$UI_STATE_PATH" && ui_state_hook_present=true || true
  check_fixed "COMMAVIEW_RUNTIME_FLAVOR = \"$expected_runtime_flavor\"" "$HELPER_PATH" && runtime_flavor_constant_present=true || true
  check_fixed 'COMMAVIEW_SOCKET_PATH_DEFAULT = "/data/commaview/run/ui-export.sock"' "$HELPER_PATH" && socket_path_present=true || true
  check_fixed 'os.environ.get("COMMAVIEWD_UI_EXPORT_SOCKET") or COMMAVIEW_SOCKET_PATH_DEFAULT' "$HELPER_PATH" && socket_env_present=true || true
  check_fixed 'COMMAVIEW_FRAME_VERSION = 1' "$HELPER_PATH" && frame_version_present=true || true
  check_fixed 'socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)' "$HELPER_PATH" && unix_socket_present=true || true
  check_fixed 'struct.pack(">I", len(frame)) + frame' "$HELPER_PATH" && framing_present=true || true
  check_fixed 'json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode("utf-8")' "$HELPER_PATH" && compact_json_present=true || true
  check_fixed 'self._commaview_exporter = _CommaViewSocketExporter(COMMAVIEW_RUNTIME_FLAVOR)' "$UI_STATE_PATH" && exporter_install_present=true || true
  check_fixed 'self._commaview_exporter.publish(self)' "$UI_STATE_PATH" && exporter_publish_present=true || true
  augmented_road_candidate_count=0
  onroad_camera_relay_present=true
  onroad_projection_present=true
  for augmented_road_path in "${AUGMENTED_ROAD_PATHS[@]}"; do
    if [ ! -f "$augmented_road_path" ]; then
      continue
    fi
    augmented_road_candidate_count=$((augmented_road_candidate_count + 1))
    rel_augmented_path="${augmented_road_path#$OP_ROOT/}"
    if [ -z "$selected_augmented_road_targets" ]; then
      selected_augmented_road_targets="$rel_augmented_path"
    else
      selected_augmented_road_targets="$selected_augmented_road_targets,$rel_augmented_path"
    fi
    if ! { check_fixed 'self._update_commaview_camera_export()' "$augmented_road_path" && \
      check_fixed 'def _update_commaview_camera_export(self):' "$augmented_road_path" && \
      check_fixed 'active_camera="wideRoad" if self.stream_type == WIDE_CAM else "road"' "$augmented_road_path"; }; then
      onroad_camera_relay_present=false
    fi
    if ! { check_fixed 'model_transform = video_transform @ calib_transform' "$augmented_road_path" && \
      check_fixed 'exporter.set_onroad_projection(' "$augmented_road_path" && \
      check_fixed 'active_camera="wideRoad" if is_wide_camera else "road"' "$augmented_road_path" && \
      check_fixed 'video_frame_matrix=self._cached_matrix' "$augmented_road_path" && \
      { check_fixed 'camera_offset=getattr(self._model_renderer, "_camera_offset", 0.0)' "$augmented_road_path" || check_fixed 'camera_offset=getattr(self.model_renderer, "_camera_offset", 0.0)' "$augmented_road_path"; } && \
      check_fixed 'self._offer_payload(COMMAVIEW_ONROAD_PROJECTION_SERVICE_INDEX, self._latest_onroad_projection)' "$HELPER_PATH"; }; then
      onroad_projection_present=false
    fi
  done
  if [ "$augmented_road_candidate_count" -eq 0 ]; then
    onroad_camera_relay_present=false
    onroad_projection_present=false
  fi

  payload_helpers_present=true
  for marker in "${payload_markers[@]}"; do
    if ! check_fixed "$marker" "$HELPER_PATH"; then
      payload_helpers_present=false
      break
    fi
  done

  publish_paths_present=true
  service_marker_count=0
  for const_name in "${service_consts[@]}"; do
    if check_fixed "(${const_name}, self._" "$HELPER_PATH"; then
      service_marker_count=$((service_marker_count + 1))
    else
      publish_paths_present=false
    fi
  done

  risk_fields_present=true
  for marker in "${risk_markers[@]}"; do
    if ! check_fixed "$marker" "$HELPER_PATH"; then
      risk_fields_present=false
      break
    fi
  done

  legacy_bucket_markers_absent=true
  for marker in "${legacy_markers[@]}"; do
    if check_fixed "$marker" "$HELPER_PATH"; then
      legacy_bucket_markers_absent=false
      break
    fi
  done

  # Every file openpilot's UI imports must compile, checked in memory (py_compile would leave a
  # .pyc in openpilot's tree). A file whose HEAD copy doesn't compile under this python3 either
  # (one older than openpilot's) isn't held against the patch.
  compile_targets=("$HELPER_PATH" "$UI_STATE_PATH" "${AUGMENTED_ROAD_PATHS[@]}")
  if python3 - "$OP_ROOT" "${compile_targets[@]}" <<'PYCOMPILE'
import os
import subprocess
import sys

op_root = sys.argv[1]
ok = True
for path in sys.argv[2:]:
    if not os.path.isfile(path):
        continue
    try:
        with open(path, encoding="utf-8") as source:
            compile(source.read(), path, "exec")
        continue
    except (OSError, SyntaxError, ValueError, UnicodeDecodeError):
        pass
    rel = os.path.relpath(path, op_root)
    head = subprocess.run(["git", "-C", op_root, "cat-file", "-p", f"HEAD:./{rel}"],
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=False)
    try:
        if head.returncode != 0:
            raise ValueError("not in HEAD")
        compile(head.stdout.decode("utf-8"), path, "exec")
    except (SyntaxError, ValueError, UnicodeDecodeError):
        continue
    print(f"does not compile: {rel}", file=sys.stderr)
    ok = False
sys.exit(0 if ok else 1)
PYCOMPILE
  then
    patched_files_compile=true
  fi

  smoke_json="{}"
  if $helper_present && [ -f "$SMOKE_SCRIPT" ]; then
    # No .pyc left in openpilot's tree beside the helper.
    smoke_json="$(PYTHONDONTWRITEBYTECODE=1 python3 "$SMOKE_SCRIPT" "$HELPER_PATH" "$expected_runtime_flavor" 2>/dev/null || true)"
    smoke_eval="$(SMOKE_JSON="$smoke_json" python3 - <<'PYSMOKE'
import json
import os
try:
    payload = json.loads(os.environ.get("SMOKE_JSON", "{}"))
except json.JSONDecodeError:
    payload = {}
print("payload_smoke_passed=" + ("true" if payload.get("payloadSmokePassed") else "false"))
print("critical_services_publishable=" + ("true" if payload.get("criticalServicesPublishable") else "false"))
try:
    count = int(payload.get("serviceSendCount", 0))
except (TypeError, ValueError):
    count = 0
print("service_send_count=" + str(count))
PYSMOKE
    )"
    eval "$smoke_eval"
  fi

  if $helper_present && $ui_state_hook_present && $runtime_flavor_constant_present && \
     $socket_path_present && $socket_env_present && $frame_version_present && $unix_socket_present && \
     $framing_present && $compact_json_present && $payload_helpers_present && $publish_paths_present && \
     $risk_fields_present && $legacy_bucket_markers_absent && $exporter_install_present && \
     $exporter_publish_present && $onroad_camera_relay_present && $onroad_projection_present && \
     $payload_smoke_passed && $critical_services_publishable && $patched_files_compile; then
    if [ -z "$applied_upstream_head" ]; then
      state="repair-needed"
      reason="source transformer socket UI export markers verified but patch provenance is missing; rerun repair against the current upstream checkout"
    elif [ -n "$upstream_head_value" ] && [ "$applied_upstream_head" != "$upstream_head_value" ]; then
      state="repair-needed"
      reason="upstream checkout changed since patch was applied; rerun repair to regenerate the matching CommaView UI export patch"
      upstream_changed_since_apply=true
    else
      state="patch-verified"
      reason="source transformer socket UI export markers and generated helper smoke verified for current upstream checkout; runtime telemetry not proven"
      patch_verified=true
      repair_needed=false
    fi
  else
    state="repair-needed"
    reason="source transformer socket UI export markers or generated helper smoke missing from upstream UI tree"
  fi
fi

ui_reload_pending=false
ui_reload_action=""
ui_reload_reason=""
if [ -f "$UI_RELOAD_MARKER" ]; then
  marker_pending=true
  marker_boot_id="$(marker_value bootId)"
  marker_patched_cs="$(marker_value patchedAtUptimeCs)"
  boot_id_value="$(current_boot_id)"
  manager_pid=""
  manager_start_cs=""
  if [ -n "$marker_boot_id" ] && [ -n "$boot_id_value" ] && [ "$marker_boot_id" != "$boot_id_value" ]; then
    # Rebooted since the patch: this boot's manager loaded the patched files.
    marker_pending=false
  elif [[ "$marker_patched_cs" =~ ^[0-9]+$ ]] && manager_pid="$(openpilot_manager_pid)" && \
       manager_start_cs="$(proc_start_cs "$manager_pid")" && [ "$manager_start_cs" -gt "$marker_patched_cs" ]; then
    # openpilot was restarted after the patch, so its UI loaded the patched files.
    marker_pending=false
  fi
  if [ "$marker_pending" = "false" ]; then
    rm -f "$UI_RELOAD_MARKER"
  elif $patch_verified; then
    ui_reload_pending=true
    ui_reload_action="reboot"
    ui_reload_reason="openpilot's UI was already running when CommaView patched it, and openpilot cannot reload its UI in place, so the onroad UI export takes effect after the next reboot"
    reason="$reason; $ui_reload_reason"
  fi
fi

json="$(
  HEALTHY="$healthy" \
  PATCH_VERIFIED="$patch_verified" \
  METHOD="$method" \
  STATUS_SCOPE="$status_scope" \
  REPAIR_NEEDED="$repair_needed" \
  STATE="$state" \
  REASON="$reason" \
  FLAVOR="$flavor" \
  OP_ROOT_JSON="$OP_ROOT" \
  TRANSFORMER_JSON="$TRANSFORMER" \
  TEMPLATE_PATH_JSON="$template_path" \
  FINGERPRINT="$fingerprint" \
  HELPER_PRESENT="$helper_present" \
  UI_STATE_HOOK_PRESENT="$ui_state_hook_present" \
  RUNTIME_FLAVOR_CONSTANT_PRESENT="$runtime_flavor_constant_present" \
  SOCKET_PATH_PRESENT="$socket_path_present" \
  SOCKET_ENV_PRESENT="$socket_env_present" \
  FRAME_VERSION_PRESENT="$frame_version_present" \
  UNIX_SOCKET_PRESENT="$unix_socket_present" \
  FRAMING_PRESENT="$framing_present" \
  COMPACT_JSON_PRESENT="$compact_json_present" \
  PAYLOAD_HELPERS_PRESENT="$payload_helpers_present" \
  PUBLISH_PATHS_PRESENT="$publish_paths_present" \
  RISK_FIELDS_PRESENT="$risk_fields_present" \
  LEGACY_BUCKET_MARKERS_ABSENT="$legacy_bucket_markers_absent" \
  EXPORTER_INSTALL_PRESENT="$exporter_install_present" \
  EXPORTER_PUBLISH_PRESENT="$exporter_publish_present" \
  ONROAD_CAMERA_RELAY_PRESENT="$onroad_camera_relay_present" \
  ONROAD_PROJECTION_PRESENT="$onroad_projection_present" \
  TRANSFORMER_PRESENT="$transformer_present" \
  TEMPLATE_PRESENT="$template_present" \
  SMOKE_SCRIPT_PRESENT="$smoke_script_present" \
  PAYLOAD_SMOKE_PASSED="$payload_smoke_passed" \
  CRITICAL_SERVICES_PUBLISHABLE="$critical_services_publishable" \
  PATCHED_FILES_COMPILE="$patched_files_compile" \
  SERVICE_SEND_COUNT="$service_send_count" \
  SERVICE_MARKER_COUNT="$service_marker_count" \
  UPSTREAM_HEAD="$upstream_head_value" \
  UPSTREAM_REMOTE="$upstream_remote_value" \
  UPSTREAM_BRANCH="$upstream_branch_value" \
  APPLIED_UPSTREAM_HEAD="$applied_upstream_head" \
  UPSTREAM_CHANGED_SINCE_APPLY="$upstream_changed_since_apply" \
  UI_PLATFORM="$ui_platform" \
  DEVICE_MODEL="$device_model_value" \
  SELECTED_AUGMENTED_ROAD_TARGETS="$selected_augmented_road_targets" \
  UI_RELOAD_PENDING="$ui_reload_pending" \
  UI_RELOAD_ACTION="$ui_reload_action" \
  UI_RELOAD_REASON="$ui_reload_reason" \
  CAR_OP_ROOT="$OP_ROOT" \
  CAR_PARAMS_DIR="${COMMAVIEWD_TEST_PARAMS_DIR:-/data/params/d}" \
  CAR_IDENTITY_CACHE="$INSTALL_DIR/run/car-identity.json" \
  python3 - <<'PYJSON'
import json
import os

def car_identity() -> dict:
    """The car openpilot last identified, from its CarParamsPersistent param: there offroad too, so the
    app can name the car and fetch its picture without a drive. Decoded with the checkout's own
    car.capnp (carFingerprint @1 and carVin @38 in every schema), cached until the param changes."""
    params_dir = os.environ.get("CAR_PARAMS_DIR", "")
    cache_path = os.environ.get("CAR_IDENTITY_CACHE", "")
    op_root = os.environ.get("CAR_OP_ROOT", "")
    for key in ("CarParamsPersistent", "CarParamsCache"):
        path = os.path.join(params_dir, key)
        try:
            st = os.stat(path)
        except OSError:
            continue
        stamp = f"{key}:{st.st_mtime_ns}:{st.st_size}"
        try:
            with open(cache_path, encoding="utf-8") as cached:
                saved = json.load(cached)
            if saved.get("stamp") == stamp:
                return saved.get("identity", {})
        except (OSError, ValueError):
            pass
        try:
            with open(path, "rb") as source:
                data = source.read()
            identity = decode_car_params(data, op_root)
        except Exception:
            identity = {}
        if identity.get("carFingerprint"):
            try:
                with open(cache_path + ".tmp", "w", encoding="utf-8") as out:
                    json.dump({"stamp": stamp, "identity": identity}, out)
                os.replace(cache_path + ".tmp", cache_path)
            except OSError:
                pass
            return identity
    return {}

def decode_car_params(data: bytes, op_root: str) -> dict:
    import capnp
    capnp.remove_import_hook()
    for rel in ("opendbc_repo/opendbc/car/car.capnp", "opendbc/car/car.capnp", "cereal/car.capnp"):
        schema_path = os.path.join(op_root, rel)
        if not os.path.isfile(schema_path):
            continue
        schema = capnp.load(schema_path, imports=[os.path.dirname(schema_path)])
        reader = schema.CarParams.from_bytes(data)
        if hasattr(reader, "__enter__"):
            with reader as cp:
                return car_fields(cp)
        return car_fields(reader)
    return {}

def car_fields(cp) -> dict:
    def text(name: str) -> str:
        try:
            value = getattr(cp, name)
        except (AttributeError, Exception):
            return ""
        return value if isinstance(value, str) else ""
    return {"carFingerprint": text("carFingerprint"), "carName": text("carName"), "carVin": text("carVin")}

def env_bool(name: str) -> bool:
    return os.environ.get(name, "false") == "true"

def env_int(name: str) -> int:
    try:
        return int(os.environ.get(name, "0"))
    except ValueError:
        return 0

payload = {
    "healthy": env_bool("HEALTHY"),
    "patchVerified": env_bool("PATCH_VERIFIED"),
    "method": os.environ.get("METHOD", ""),
    "statusScope": os.environ.get("STATUS_SCOPE", ""),
    "repairNeeded": env_bool("REPAIR_NEEDED"),
    "state": os.environ.get("STATE", ""),
    "reason": os.environ.get("REASON", ""),
    "flavor": os.environ.get("FLAVOR", ""),
    "uiPlatform": os.environ.get("UI_PLATFORM", ""),
    "deviceModel": os.environ.get("DEVICE_MODEL", ""),
    "selectedAugmentedRoadTargets": [value for value in os.environ.get("SELECTED_AUGMENTED_ROAD_TARGETS", "").split(",") if value],
    "opRoot": os.environ.get("OP_ROOT_JSON", ""),
    "transformer": os.environ.get("TRANSFORMER_JSON", ""),
    "template": os.environ.get("TEMPLATE_PATH_JSON", ""),
    "transformerFingerprint": os.environ.get("FINGERPRINT", ""),
    "helperPresent": env_bool("HELPER_PRESENT"),
    "uiStateHookPresent": env_bool("UI_STATE_HOOK_PRESENT"),
    "runtimeFlavorConstantPresent": env_bool("RUNTIME_FLAVOR_CONSTANT_PRESENT"),
    "socketPathPresent": env_bool("SOCKET_PATH_PRESENT"),
    "socketEnvPresent": env_bool("SOCKET_ENV_PRESENT"),
    "frameVersionPresent": env_bool("FRAME_VERSION_PRESENT"),
    "unixSocketPresent": env_bool("UNIX_SOCKET_PRESENT"),
    "framingPresent": env_bool("FRAMING_PRESENT"),
    "compactJsonPresent": env_bool("COMPACT_JSON_PRESENT"),
    "payloadHelpersPresent": env_bool("PAYLOAD_HELPERS_PRESENT"),
    "publishPathsPresent": env_bool("PUBLISH_PATHS_PRESENT"),
    "riskFieldsPresent": env_bool("RISK_FIELDS_PRESENT"),
    "legacyBucketMarkersAbsent": env_bool("LEGACY_BUCKET_MARKERS_ABSENT"),
    "exporterInstallPresent": env_bool("EXPORTER_INSTALL_PRESENT"),
    "exporterPublishPresent": env_bool("EXPORTER_PUBLISH_PRESENT"),
    "onroadCameraRelayPresent": env_bool("ONROAD_CAMERA_RELAY_PRESENT"),
    "onroadProjectionPresent": env_bool("ONROAD_PROJECTION_PRESENT"),
    "transformerPresent": env_bool("TRANSFORMER_PRESENT"),
    "templatePresent": env_bool("TEMPLATE_PRESENT"),
    "smokeScriptPresent": env_bool("SMOKE_SCRIPT_PRESENT"),
    "payloadSmokePassed": env_bool("PAYLOAD_SMOKE_PASSED"),
    "criticalServicesPublishable": env_bool("CRITICAL_SERVICES_PUBLISHABLE"),
    "patchedFilesCompile": env_bool("PATCHED_FILES_COMPILE"),
    "serviceSendCount": env_int("SERVICE_SEND_COUNT"),
    "serviceMarkerCount": env_int("SERVICE_MARKER_COUNT"),
    "upstreamHead": os.environ.get("UPSTREAM_HEAD", ""),
    "upstreamRemote": os.environ.get("UPSTREAM_REMOTE", ""),
    "upstreamBranch": os.environ.get("UPSTREAM_BRANCH", ""),
    "appliedUpstreamHead": os.environ.get("APPLIED_UPSTREAM_HEAD", ""),
    "upstreamChangedSinceApply": env_bool("UPSTREAM_CHANGED_SINCE_APPLY"),
    # True while openpilot's running UI predates the verified patch; the patch
    # loads at the next openpilot start, i.e. after uiReloadAction ("reboot").
    "uiReloadPending": env_bool("UI_RELOAD_PENDING"),
    "uiReloadAction": os.environ.get("UI_RELOAD_ACTION", ""),
    "uiReloadReason": os.environ.get("UI_RELOAD_REASON", ""),
}
payload.update(car_identity())
print(json.dumps(payload, separators=(",", ":")))
PYJSON
)"
printf '%s\n' "$json" > "$STATE_JSON"
if [ -n "$fingerprint" ] && $patch_verified; then
  printf 'ONROAD_UI_EXPORT_FLAVOR=%s\nONROAD_UI_EXPORT_METHOD=transformer\nONROAD_UI_EXPORT_UI_PLATFORM=%s\nONROAD_UI_EXPORT_TRANSFORMER_SHA=%s\nONROAD_UI_EXPORT_OP_ROOT=%s\nONROAD_UI_EXPORT_UPSTREAM_HEAD=%s\nONROAD_UI_EXPORT_UPSTREAM_REMOTE=%s\n' \
    "$flavor" "$ui_platform" "$fingerprint" "$OP_ROOT" "$upstream_head_value" "$upstream_remote_value" > "$STATE_ENV"
fi
printf '%s\n' "$json"
if $patch_verified; then exit 0; fi
if [ "$state" = "unsupported-device" ]; then exit "$UNSUPPORTED_DEVICE_EXIT"; fi
exit 1
