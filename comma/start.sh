#!/usr/bin/env bash
set +e

INSTALL_DIR=/data/commaview
LOG_DIR="$INSTALL_DIR/logs"
RUN_DIR="$INSTALL_DIR/run"
CONFIG_DIR="$INSTALL_DIR/config"
DEFAULTS_FILE="${COMMAVIEWD_RUNTIME_DEBUG_DEFAULTS:-$INSTALL_DIR/runtime-debug.defaults.json}"
RUNTIME_DEBUG_CONFIG="${COMMAVIEWD_RUNTIME_DEBUG_CONFIG:-$CONFIG_DIR/runtime-debug.json}"
RUNTIME_DEBUG_EFFECTIVE="${COMMAVIEWD_RUNTIME_DEBUG_EFFECTIVE:-$RUN_DIR/runtime-debug-effective.json}"
RUNTIME_DEBUG_STATS="${COMMAVIEWD_RUNTIME_STATS:-$RUN_DIR/telemetry-stats.json}"
RESTART_REASON="${COMMAVIEWD_RESTART_REASON:-startup}"
ONROAD_UI_EXPORT_VERIFY="$INSTALL_DIR/scripts/verify_onroad_ui_export_patch.sh"
ONROAD_UI_EXPORT_APPLY="$INSTALL_DIR/scripts/apply_onroad_ui_export_patch.sh"
ONROAD_UI_EXPORT_LOG="$LOG_DIR/onroad-ui-export-startup.log"
ONROAD_UI_EXPORT_RESTART_MARKER="$RUN_DIR/onroad-ui-export-ui-restart-needed"
ONROAD_UI_EXPORT_PREPARE_TIMEOUT_SEC="${COMMAVIEWD_ONROAD_UI_EXPORT_PREPARE_TIMEOUT_SEC:-60}"
COMMAVIEWD_LOG_MAX_BYTES="${COMMAVIEWD_LOG_MAX_BYTES:-8388608}"
COMMAVIEWD_LOG_MAX_FILES_PER_LOG="${COMMAVIEWD_LOG_MAX_FILES_PER_LOG:-14}"
COMMAVIEWD_LOG_MAX_AGE_DAYS="${COMMAVIEWD_LOG_MAX_AGE_DAYS:-14}"
COMMAVIEWD_LOG_TOTAL_MAX_BYTES="${COMMAVIEWD_LOG_TOTAL_MAX_BYTES:-268435456}"
COMMAVIEWD_LOG_ROTATE_INTERVAL_SEC="${COMMAVIEWD_LOG_ROTATE_INTERVAL_SEC:-600}"
# A crashed bridge or control process is started again after a backoff that doubles from
# FIRST_BACKOFF up to MAX_BACKOFF, and given up on after MAX_RESTARTS crashes in a row; a run of
# HEALTHY_SEC or more starts the count over. Restarts can't disturb openpilot: the bridge reads its
# queues without a msgq reader slot.
COMMAVIEWD_SUPERVISOR_FIRST_BACKOFF_SEC="${COMMAVIEWD_SUPERVISOR_FIRST_BACKOFF_SEC:-5}"
COMMAVIEWD_SUPERVISOR_MAX_BACKOFF_SEC="${COMMAVIEWD_SUPERVISOR_MAX_BACKOFF_SEC:-300}"
COMMAVIEWD_SUPERVISOR_MAX_RESTARTS="${COMMAVIEWD_SUPERVISOR_MAX_RESTARTS:-8}"
COMMAVIEWD_SUPERVISOR_HEALTHY_SEC="${COMMAVIEWD_SUPERVISOR_HEALTHY_SEC:-600}"
COMMAVIEWD_BIN="${COMMAVIEWD_BIN:-/data/commaview/commaviewd}"
DEFERRED_DIR="${COMMAVIEWD_DEFERRED_DIR:-/data/commaview-deferred}"

mkdir -p "$LOG_DIR" "$RUN_DIR" "$CONFIG_DIR"

# The /data/continue.sh boot hook runs "start.sh --before-openpilot" right before
# it execs launch_openpilot.sh. openpilot cannot reload its UI in place: manager
# never restarts a UI process that exits (sunnypilot release restarts it, but
# from the modules manager imported at startup), so patched UI files only take
# effect when manager starts. Apply the onroad UI export patch here, before
# manager exists, then start the runtime in the background as the hook used to.
# The apply step is bounded so a hang can never keep openpilot from starting.
prepare_onroad_ui_export_before_openpilot() {
  local ec=0
  if [ ! -x "$ONROAD_UI_EXPORT_APPLY" ]; then
    return 0
  fi
  set -- "$ONROAD_UI_EXPORT_APPLY" --before-openpilot
  if command -v timeout >/dev/null 2>&1; then
    set -- timeout -k 5 "$ONROAD_UI_EXPORT_PREPARE_TIMEOUT_SEC" "$@"
  fi
  "$@" >> "$ONROAD_UI_EXPORT_LOG" 2>&1 || ec=$?
  if [ "$ec" -eq 0 ]; then
    echo "INFO: onroad UI export prepared before openpilot start" >> "$ONROAD_UI_EXPORT_LOG"
  else
    echo "WARN: onroad UI export not prepared before openpilot start (exit $ec); openpilot starts with its UI unchanged" >> "$ONROAD_UI_EXPORT_LOG"
  fi
}

if [ "${1:-}" = "--before-openpilot" ]; then
  prepare_onroad_ui_export_before_openpilot
  COMMAVIEWD_ONROAD_UI_EXPORT_PREPARED=1 bash "${BASH_SOURCE[0]}" &
  exit 0
fi

echo "$RESTART_REASON" > "$RUN_DIR/last-restart-reason.txt"

if [ ! -s "$RUNTIME_DEBUG_CONFIG" ]; then
  if [ -s "$DEFAULTS_FILE" ]; then
    cp "$DEFAULTS_FILE" "$RUNTIME_DEBUG_CONFIG"
  else
    printf "%s\n" '{"configVersion":1,"instrumentationLevel":"standard","services":{"uiStateOnroad":{"mode":"pass"},"selfdriveState":{"mode":"pass"},"carState":{"mode":"pass"},"controlsState":{"mode":"pass"},"onroadEvents":{"mode":"pass"},"driverMonitoringState":{"mode":"pass"},"driverStateV2":{"mode":"pass"},"modelV2":{"mode":"pass"},"radarState":{"mode":"pass"},"liveCalibration":{"mode":"pass"},"carOutput":{"mode":"pass"},"carControl":{"mode":"pass"},"liveParameters":{"mode":"pass"},"longitudinalPlan":{"mode":"pass"},"carParams":{"mode":"pass"},"deviceState":{"mode":"pass"},"roadCameraState":{"mode":"pass"},"pandaStatesSummary":{"mode":"pass"},"onroadProjection":{"mode":"pass"},"wideRoadCameraState":{"mode":"pass"}}}' > "$RUNTIME_DEBUG_CONFIG"
  fi
fi

python3 - "$RUNTIME_DEBUG_CONFIG" >/dev/null 2>&1 <<'PY'
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as f:
    json.load(f)
PY
if [ $? -ne 0 ]; then
  echo "WARN: invalid runtime debug config JSON; bridge/control will fall back to safe defaults" >> "$LOG_DIR/commaviewd-control.log"
fi

START_PARAMS_DIR="${COMMAVIEWD_PARAMS_DIR:-/data/params/d}"

read_start_param() {
  cat "$START_PARAMS_DIR/$1" 2>/dev/null | tr -d "\000\r\n"
}

# openpilot and sunnypilot publish IsOffroad; upstream no longer has an IsOnroad
# param, so reading only IsOnroad always looked offroad - even while driving -
# and let startup repair rewrite openpilot's UI files and kill its UI mid-drive.
# Anything uncertain is onroad, so startup repair waits. (The boot hook's
# --before-openpilot step doesn't ask: openpilot hasn't started, so nothing drives.)
read_is_onroad() {
  case "$(read_start_param IsOffroad)" in
    0) echo 1 ;;
    1) echo 0 ;;
    *)
      case "$(read_start_param IsOnroad)" in
        1) echo 1 ;;
        0) echo 0 ;;
        *) if [ -d "$START_PARAMS_DIR" ]; then echo 1; else echo 0; fi ;;
      esac
      ;;
  esac
}

# What runs on in the background (the supervisors, the log rotation loop, the deferred queue's
# waiter) keeps none of the descriptors start.sh was handed: an install's maintenance lock would
# stay held for as long as the runtime runs, a parent's sockets would stay open.
release_inherited_fds() {
  local fd
  unset COMMAVIEWD_MAINT_LOCK_HELD COMMAVIEWD_MAINT_LOCK_FD
  for fd in /proc/"$BASHPID"/fd/*; do
    fd="${fd##*/}"
    case "$fd" in
      0|1|2|255|*[!0-9]*) continue ;;
    esac
    eval "exec $fd>&-" 2>/dev/null
  done
}

# CommaView never signals openpilot's UI: a signalled UI stays dead on openpilot
# (and restarts with its old code on sunnypilot) until reboot. A patch applied
# while openpilot ran stays pending - verify reports "uiReloadPending" - until
# openpilot next starts.
report_openpilot_ui_reload_pending() {
  if [ -f "$ONROAD_UI_EXPORT_RESTART_MARKER" ]; then
    echo "INFO: onroad UI export takes effect after the next reboot; openpilot cannot reload its running UI" >> "$ONROAD_UI_EXPORT_LOG"
  fi
}

refresh_onroad_ui_export_status() {
  if [ ! -x "$ONROAD_UI_EXPORT_VERIFY" ]; then
    return 0
  fi

  if "$ONROAD_UI_EXPORT_VERIFY" --json >> "$ONROAD_UI_EXPORT_LOG" 2>&1; then
    echo "INFO: onroad UI export patch verified at startup" >> "$ONROAD_UI_EXPORT_LOG"
    return 0
  fi

  is_onroad="$(read_is_onroad)"
  if [ "$is_onroad" = "1" ]; then
    echo "WARN: onroad UI export verify failed while onroad; skipping startup repair" >> "$ONROAD_UI_EXPORT_LOG"
    return 0
  fi

  if [ ! -x "$ONROAD_UI_EXPORT_APPLY" ]; then
    echo "WARN: onroad UI export verify failed and repair helper is missing" >> "$ONROAD_UI_EXPORT_LOG"
    return 0
  fi

  if "$ONROAD_UI_EXPORT_APPLY" >> "$ONROAD_UI_EXPORT_LOG" 2>&1; then
    echo "INFO: onroad UI export patch repaired at startup" >> "$ONROAD_UI_EXPORT_LOG"
  else
    echo "WARN: onroad UI export startup repair failed" >> "$ONROAD_UI_EXPORT_LOG"
  fi
}

rotate_runtime_log_if_large() {
  log_file="$1"
  max_bytes="$2"
  if [ ! -f "$log_file" ]; then
    return 0
  fi
  size_bytes="$(wc -c < "$log_file" 2>/dev/null || echo 0)"
  case "$size_bytes" in
    ''|*[!0-9]*) size_bytes=0 ;;
  esac
  if [ "$size_bytes" -le "$max_bytes" ]; then
    return 0
  fi
  idx="$COMMAVIEWD_LOG_MAX_FILES_PER_LOG"
  while [ "$idx" -gt 1 ]; do
    next_idx="$idx"
    idx=$((idx - 1))
    if [ -f "$log_file.$idx" ]; then
      mv -f "$log_file.$idx" "$log_file.$next_idx" 2>/dev/null || true
    fi
  done
  cp "$log_file" "$log_file.1" 2>/dev/null || true
  : > "$log_file" 2>/dev/null || true
}

prune_runtime_logs_by_age() {
  find "$LOG_DIR" -type f \( \
    -name 'commaviewd-bridge.log.*' -o \
    -name 'commaviewd-control.log.*' -o \
    -name 'onroad-ui-export-startup.log.*' -o \
    -name 'commaview-drive-stats.log.*' -o \
    -name 'runtime-run-events.jsonl.*' \
  \) -mtime +"$COMMAVIEWD_LOG_MAX_AGE_DAYS" -exec rm -f {} + 2>/dev/null || true
}

runtime_logs_total_bytes() {
  total=0
  for file in \
    "$LOG_DIR"/commaviewd-bridge.log* \
    "$LOG_DIR"/commaviewd-control.log* \
    "$LOG_DIR"/onroad-ui-export-startup.log* \
    "$LOG_DIR"/commaview-drive-stats.log* \
    "$LOG_DIR"/runtime-run-events.jsonl*; do
    [ -f "$file" ] || continue
    size_bytes="$(wc -c < "$file" 2>/dev/null || echo 0)"
    case "$size_bytes" in
      ''|*[!0-9]*) size_bytes=0 ;;
    esac
    total=$((total + size_bytes))
  done
  printf '%s\n' "$total"
}

oldest_runtime_log() {
  find "$LOG_DIR" -type f \( \
    -name 'commaviewd-bridge.log*' -o \
    -name 'commaviewd-control.log*' -o \
    -name 'onroad-ui-export-startup.log*' -o \
    -name 'commaview-drive-stats.log*' -o \
    -name 'runtime-run-events.jsonl*' \
  \) -printf '%T@ %p\n' 2>/dev/null | sort -n | head -n 1 | sed 's/^[^ ]* //'
}

prune_runtime_logs_by_total_size() {
  while [ "$(runtime_logs_total_bytes)" -gt "$COMMAVIEWD_LOG_TOTAL_MAX_BYTES" ]; do
    victim="$(oldest_runtime_log)"
    [ -n "$victim" ] || break
    rm -f "$victim" 2>/dev/null || break
  done
}

rotate_runtime_logs() {
  rotate_runtime_log_if_large "$LOG_DIR/commaviewd-bridge.log" "$COMMAVIEWD_LOG_MAX_BYTES"
  rotate_runtime_log_if_large "$LOG_DIR/commaviewd-control.log" "$COMMAVIEWD_LOG_MAX_BYTES"
  rotate_runtime_log_if_large "$LOG_DIR/onroad-ui-export-startup.log" "$COMMAVIEWD_LOG_MAX_BYTES"
  rotate_runtime_log_if_large "$LOG_DIR/commaview-drive-stats.log" "$COMMAVIEWD_LOG_MAX_BYTES"
  rotate_runtime_log_if_large "$LOG_DIR/runtime-run-events.jsonl" "$COMMAVIEWD_LOG_MAX_BYTES"
  prune_runtime_logs_by_age
  prune_runtime_logs_by_total_size
}

start_runtime_log_rotation_loop() {
  (
    release_inherited_fds
    while true; do
      sleep "$COMMAVIEWD_LOG_ROTATE_INTERVAL_SEC"
      rotate_runtime_logs
    done
  ) >/dev/null 2>&1 &
  echo $! > "$RUN_DIR/log-rotation.pid"
}

runtime_event_ts_ms() {
  python3 - <<'PY' 2>/dev/null || date +%s000
import time
print(int(time.time() * 1000))
PY
}

append_runtime_run_event() {
  event="$1"
  component="$2"
  pid="$3"
  exit_status="${4:-}"
  restart_in_sec="${5:-}"
  ts_ms="$(runtime_event_ts_ms)"
  {
    printf '{"tsMs":%s,"event":"%s","component":"%s","pid":%s,"restartReason":"%s"' "$ts_ms" "$event" "$component" "$pid" "$RESTART_REASON"
    if [ -n "$exit_status" ]; then
      printf ',"exitStatus":%s' "$exit_status"
    fi
    if [ -n "$restart_in_sec" ]; then
      printf ',"restartInSec":%s' "$restart_in_sec"
    fi
    printf '}\n'
  } >> "$LOG_DIR/runtime-run-events.jsonl" 2>/dev/null || true
}

# The supervisor's own pid file still names it: stop.sh and install remove the pid files before
# they stop anything, and a later start.sh writes new ones, so a supervisor that no longer owns its
# file has been told to stand down and must not restart anything.
supervisor_owns() {
  [ "$(cat "$RUN_DIR/$1" 2>/dev/null)" = "$BASHPID" ]
}

start_runtime_process() {
  component="$1"
  pid_file="$2"
  supervisor_pid_file="$3"
  log_file="$4"
  mode="$5"
  shift 5
  (
    release_inherited_fds
    crashes=0
    backoff="$COMMAVIEWD_SUPERVISOR_FIRST_BACKOFF_SEC"
    while :; do
      started_at="$(date +%s)"
      "$@" "$COMMAVIEWD_BIN" "$mode" >> "$log_file" 2>&1 &
      child_pid="$!"
      echo "$child_pid" > "$RUN_DIR/$pid_file"
      append_runtime_run_event process_launch "$component" "$child_pid"
      wait "$child_pid"
      exit_status="$?"
      append_runtime_run_event process_exit "$component" "$child_pid" "$exit_status"
      # Stopped on purpose (exit, SIGINT, SIGTERM), or by stop.sh / install / a newer start.sh.
      case "$exit_status" in
        0|130|143) exit "$exit_status" ;;
      esac
      supervisor_owns "$supervisor_pid_file" || exit "$exit_status"
      # A crash (a signal such as SIGSEGV or the OOM killer's SIGKILL, or an error exit).
      if [ $(( $(date +%s) - started_at )) -ge "$COMMAVIEWD_SUPERVISOR_HEALTHY_SEC" ]; then
        crashes=0
        backoff="$COMMAVIEWD_SUPERVISOR_FIRST_BACKOFF_SEC"
      fi
      crashes=$((crashes + 1))
      if [ "$crashes" -gt "$COMMAVIEWD_SUPERVISOR_MAX_RESTARTS" ]; then
        append_runtime_run_event process_restart_gave_up "$component" "$child_pid" "$exit_status"
        exit "$exit_status"
      fi
      append_runtime_run_event process_restart_scheduled "$component" "$child_pid" "$exit_status" "$backoff"
      sleep "$backoff"
      supervisor_owns "$supervisor_pid_file" || exit "$exit_status"
      backoff=$((backoff * 2))
      if [ "$backoff" -gt "$COMMAVIEWD_SUPERVISOR_MAX_BACKOFF_SEC" ]; then
        backoff="$COMMAVIEWD_SUPERVISOR_MAX_BACKOFF_SEC"
      fi
    done
  ) >/dev/null 2>&1 &
  echo $! > "$RUN_DIR/$supervisor_pid_file"
}

if [ "${COMMAVIEWD_ONROAD_UI_EXPORT_PREPARED:-0}" = "1" ]; then
  # Started by --before-openpilot, which already patched the tree launch_openpilot.sh
  # runs. Repairing again now would race manager importing the UI files.
  echo "INFO: onroad UI export prepared before openpilot start; skipping startup repair" >> "$ONROAD_UI_EXPORT_LOG"
else
  refresh_onroad_ui_export_status
fi
report_openpilot_ui_reload_pending
rotate_runtime_logs

# Stop stale runtime processes first
bash /data/commaview/stop.sh >/dev/null 2>&1 || true

# Launch bridge mode (video/telemetry stream)
start_runtime_process bridge bridge.pid bridge-supervisor.pid "$LOG_DIR/commaviewd-bridge.log" bridge \
  nohup env \
  COMMAVIEWD_RUNTIME_DEBUG_CONFIG="$RUNTIME_DEBUG_CONFIG" \
  COMMAVIEWD_RUNTIME_DEBUG_DEFAULTS="$DEFAULTS_FILE" \
  COMMAVIEWD_RUNTIME_DEBUG_EFFECTIVE="$RUNTIME_DEBUG_EFFECTIVE" \
  COMMAVIEWD_RUNTIME_STATS="$RUNTIME_DEBUG_STATS" \
  COMMAVIEWD_RESTART_REASON="$RESTART_REASON" \
  nice -n 19

# Launch control mode (API + pairing/runtime status)
start_runtime_process control control.pid control-supervisor.pid "$LOG_DIR/commaviewd-control.log" control \
  nohup env \
  COMMAVIEWD_API_TOKEN_FILE=/data/commaview/api/auth.token \
  COMMAVIEWD_RUNTIME_DEBUG_CONFIG="$RUNTIME_DEBUG_CONFIG" \
  COMMAVIEWD_RUNTIME_DEBUG_DEFAULTS="$DEFAULTS_FILE" \
  COMMAVIEWD_RUNTIME_DEBUG_EFFECTIVE="$RUNTIME_DEBUG_EFFECTIVE" \
  COMMAVIEWD_RUNTIME_STATS="$RUNTIME_DEBUG_STATS" \
  COMMAVIEWD_RESTART_REASON="$RESTART_REASON" \
  nice -n 19

start_runtime_log_rotation_loop

# An install, uninstall or repair asked for while onroad waits in run_when_offroad.sh's queue until
# openpilot is offroad. A reboot ends its waiter; start it again.
if [ -f "$DEFERRED_DIR/runner.sh" ]; then
  ( release_inherited_fds; COMMAVIEWD_DEFERRED_DIR="$DEFERRED_DIR" bash "$DEFERRED_DIR/runner.sh" resume >/dev/null 2>&1 ) || true
fi

echo "CommaView runtime started (bridge+control)"
