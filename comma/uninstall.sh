#!/usr/bin/env bash
set +e
INSTALL_DIR="${COMMAVIEWD_INSTALL_DIR:-/data/commaview}"
FORCE_OFFROAD=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --force-offroad) FORCE_OFFROAD=1; shift ;;
    -h|--help) echo "Usage: uninstall.sh [--force-offroad]"; exit 0 ;;
    *) echo "ERROR: unknown option: $1" >&2; exit 1 ;;
  esac
done

PARAMS_DIR="${COMMAVIEWD_PARAMS_DIR:-/data/params/d}"
DEFERRED_DIR="${COMMAVIEWD_DEFERRED_DIR:-/data/commaview-deferred}"
RUNNER="$INSTALL_DIR/scripts/run_when_offroad.sh"

read_param() {
  local path="$PARAMS_DIR/$1"
  [ -f "$path" ] || return 0
  tr -d '\000\r\n' < "$path" 2>/dev/null || true
}

# openpilot and sunnypilot publish IsOffroad; IsOnroad stays a fallback for older builds and bench
# rigs. Prints 1 when onroad. Anything uncertain is onroad: only a machine without openpilot's
# params at all (a bench or CI checkout) reads offroad when neither param says.
read_is_onroad() {
  case "$(read_param IsOffroad)" in
    0) echo 1 ;;
    1) echo 0 ;;
    *)
      case "$(read_param IsOnroad)" in
        1) echo 1 ;;
        0) echo 0 ;;
        *) if [ -d "$PARAMS_DIR" ]; then echo 1; else echo 0; fi ;;
      esac
      ;;
  esac
}

# Prints 1 while the car is being driven: onroad and not parked. Parked is in Park, at a standstill
# and not engaged (sunnypilot's MADS included), as commaviewd road-phase reads it from openpilot's
# queues without subscribing. A runtime without road-phase, or one that can't tell, leaves onroad
# as driving.
ROAD_PHASE_BIN="${COMMAVIEWD_ROAD_PHASE_BIN:-$INSTALL_DIR/commaviewd}"
read_is_driving() {
  [ "$(read_is_onroad)" = "1" ] || { echo 0; return 0; }
  if [ -x "$ROAD_PHASE_BIN" ] && timeout 5 "$ROAD_PHASE_BIN" road-phase >/dev/null 2>&1; then
    echo 0
  else
    echo 1
  fi
}

# Uninstalling stops the runtime and restores openpilot's UI files, so it runs offroad or parked,
# never while driving. CommaView never asks openpilot to go offroad: with --force-offroad an
# uninstall asked for while driving is queued (run_when_offroad.sh) and runs once the car is
# parked; exit 75 says so.
if [ "$(read_is_driving)" = "1" ]; then
  if [ "$FORCE_OFFROAD" = "1" ] && [ -f "$RUNNER" ]; then
    if COMMAVIEWD_DEFERRED_DIR="$DEFERRED_DIR" bash "$RUNNER" queue uninstall --file "$INSTALL_DIR/uninstall.sh" -- \
        bash @JOB@/uninstall.sh >/dev/null; then
      echo "DEFERRED: CommaView will be uninstalled once the car is in Park with openpilot disengaged, or offroad."
      echo "COMMAVIEW_MAINTENANCE_DEFERRED=uninstall"
      exit 75
    fi
  fi
  echo "ERROR: uninstall blocked while driving. Shift into Park (openpilot not engaged), or rerun with --force-offroad to uninstall once it is parked." >&2
  exit 42
fi

MAINT_LOCK="${COMMAVIEWD_MAINT_LOCK:-$DEFERRED_DIR/maint.lock}"
# One CommaView maintenance at a time (install, uninstall, the onroad UI export patch and its revert,
# the deferred queue's job). A queued uninstall already holds the lock and says so in the environment.
take_maint_lock() {
  [ "${COMMAVIEWD_MAINT_LOCK_HELD:-0}" = "1" ] && return 0
  command -v flock >/dev/null 2>&1 || return 0
  mkdir -p "$(dirname "$MAINT_LOCK")" 2>/dev/null || return 0
  { exec {MAINT_LOCK_FD}>>"$MAINT_LOCK"; } 2>/dev/null || return 0
  flock -w "$1" "$MAINT_LOCK_FD" || return 1
  export COMMAVIEWD_MAINT_LOCK_HELD=1 COMMAVIEWD_MAINT_LOCK_FD="$MAINT_LOCK_FD"
}

if ! take_maint_lock "${COMMAVIEWD_MAINT_LOCK_WAIT_SEC:-120}"; then
  echo "ERROR: another CommaView install, uninstall or repair is running; try again when it has finished" >&2
  exit 1
fi

revert_args=()
if [ "$FORCE_OFFROAD" = "1" ]; then
  revert_args+=(--force-offroad)
fi
revert_helper="$INSTALL_DIR/scripts/revert_onroad_ui_export_patch.sh"

if [ -x "$revert_helper" ]; then
  echo "Reverting direct v2 onroad UI export transformer..."
  COMMAVIEWD_INSTALL_DIR="$INSTALL_DIR" bash "$revert_helper" "${revert_args[@]}"
  revert_ec=$?
  if [ "$revert_ec" -ne 0 ]; then
    echo "ERROR: uninstall aborted before stopping services or removing boot hook; direct v2 onroad UI export transformer revert failed with exit $revert_ec; preserving $INSTALL_DIR for recovery" >&2
    exit "$revert_ec"
  fi
else
  echo "ERROR: direct v2 onroad UI export transformer revert helper missing; uninstall aborted before stopping services or removing files; preserving $INSTALL_DIR for recovery" >&2
  exit 1
fi

# A queued install would put back what this removes. (Run as the queued job itself, the queue
# cleans up after it.)
if [ "${COMMAVIEWD_DEFERRED_JOB:-0}" != "1" ] && [ -f "$DEFERRED_DIR/runner.sh" ]; then
  COMMAVIEWD_DEFERRED_DIR="$DEFERRED_DIR" bash "$DEFERRED_DIR/runner.sh" cancel >/dev/null 2>&1 || true
  rm -rf "$DEFERRED_DIR"
fi

echo "Stopping services..."
bash "$INSTALL_DIR/stop.sh" 2>/dev/null || true

echo "Removing boot hook..."
sed -i '/# commaview-hook/d; /commaview\/start.sh/d' /data/continue.sh 2>/dev/null || true

echo "Removing files..."
rm -rf "$INSTALL_DIR"
# A position from the drive log, if location was on (stop.sh clears it too).
rm -rf /dev/shm/commaview
echo "CommaView uninstalled"
