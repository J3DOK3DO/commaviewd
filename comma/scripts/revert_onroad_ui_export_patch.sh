#!/usr/bin/env bash
set -euo pipefail

INSTALL_DIR="${COMMAVIEWD_INSTALL_DIR:-/data/commaview}"
OP_ROOT="${COMMAVIEWD_OP_ROOT:-/data/openpilot}"
STATE_ENV="$INSTALL_DIR/config/onroad-ui-export-patch.env"
STATE_JSON="$INSTALL_DIR/run/onroad-ui-export-status.json"
RESTART_MARKER="$INSTALL_DIR/run/onroad-ui-export-ui-restart-needed"
BACKUP_ROOT="${COMMAVIEWD_BACKUP_ROOT:-/data/commaview-backups}"
PARAMS_DIR="${COMMAVIEWD_PARAMS_DIR:-/data/params/d}"
PREFLIGHT_ONLY=0
UI_PREFIX=""

# No git command here may rewrite .git/index: launch_chffrplus.sh skips a staged
# openpilot update when anything under .git is newer than .overlay_init. Reads run
# without optional locks, and files are restored from HEAD with git cat-file, never
# git checkout or git reset.
export GIT_OPTIONAL_LOCKS=0

MAINT_LOCK="${COMMAVIEWD_MAINT_LOCK:-${COMMAVIEWD_DEFERRED_DIR:-/data/commaview-deferred}/maint.lock}"

# One CommaView maintenance at a time: install, uninstall, this patch and its revert
# and the deferred queue's job all rewrite the same files. A caller holding the lock
# (install.sh, the deferred job) says so in the environment. Without flock, or where
# the lock file can't be made, it runs unlocked as before.
take_maint_lock() {
  [ "${COMMAVIEWD_MAINT_LOCK_HELD:-0}" = "1" ] && return 0
  command -v flock >/dev/null 2>&1 || return 0
  mkdir -p "$(dirname "$MAINT_LOCK")" 2>/dev/null || return 0
  { exec {MAINT_LOCK_FD}>>"$MAINT_LOCK"; } 2>/dev/null || return 0
  flock -w "$1" "$MAINT_LOCK_FD" || return 1
  export COMMAVIEWD_MAINT_LOCK_HELD=1 COMMAVIEWD_MAINT_LOCK_FD="$MAINT_LOCK_FD"
}


read_param() {
  local path="$PARAMS_DIR/$1"
  [[ -f "$path" ]] || return 0
  tr -d '\000\r\n' < "$path" 2>/dev/null || true
}

# openpilot and sunnypilot publish IsOffroad; upstream no longer has an IsOnroad
# param, so reading only IsOnroad always looked offroad - even while driving.
# IsOnroad stays a fallback for older builds and bench rigs. Prints 1 when onroad.
# Anything uncertain is onroad: only a machine without openpilot's params at all (a
# bench or CI checkout) reads offroad when neither param says.
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





ensure_offroad_ready() {
  if [ "$(read_is_driving)" != "1" ]; then
    return 0
  fi

  # CommaView never asks openpilot to go offroad (no OffroadMode), --force-offroad included:
  # a change asked for while driving waits in run_when_offroad.sh until the car is parked.
  echo "ERROR: socket UI export transformer revert blocked while driving" >&2
  exit 42
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    # Accepted from older callers; it changes nothing (nothing forces openpilot offroad).
    --force-offroad) shift ;;
    --preflight-only) PREFLIGHT_ONLY=1; shift ;;
    -h|--help) echo "Usage: revert_onroad_ui_export_patch.sh [--force-offroad] [--preflight-only]"; exit 0 ;;
    *) echo "ERROR: unknown option: $1" >&2; exit 1 ;;
  esac
done

ensure_offroad_ready
if [ "$PREFLIGHT_ONLY" = "1" ]; then
  exit 0
fi
if ! take_maint_lock 30; then
  echo "ERROR: another CommaView install, uninstall or repair is running; try again when it has finished" >&2
  exit 1
fi
# The car may have been driven off while this waited for the lock.
ensure_offroad_ready


detect_ui_prefix() {
  if [ -f "$OP_ROOT/selfdrive/ui/ui_state.py" ]; then
    UI_PREFIX=""
  elif [ -f "$OP_ROOT/openpilot/selfdrive/ui/ui_state.py" ]; then
    UI_PREFIX="openpilot/"
  else
    UI_PREFIX=""
  fi
}

# CommaView never signals openpilot's UI: manager never restarts a UI that exits
# (sunnypilot restarts it from the modules manager imported at startup), so a
# signalled UI would stay dead or keep the old code until reboot. The running UI
# keeps the exporter it loaded until openpilot next starts; without the CommaView
# runtime its socket has no listener and the exporter stays idle.
note_openpilot_ui_keeps_loaded_code() {
  echo "INFO: openpilot's running UI keeps the CommaView onroad UI export it loaded until the next reboot; it is idle without the CommaView runtime" >&2
}

managed_targets() {
  printf '%s\n' \
    "${UI_PREFIX}selfdrive/ui/commaview_export.py" \
    "${UI_PREFIX}selfdrive/ui/ui_state.py" \
    "${UI_PREFIX}selfdrive/ui/mici/onroad/augmented_road_view.py" \
    "${UI_PREFIX}selfdrive/ui/onroad/augmented_road_view.py"
}

backup_managed_targets() {
  local backup_parent="$BACKUP_ROOT/onroad-ui-export-revert"
  local backup_root=""
  local rel=""
  mkdir -p "$backup_parent" || return $?
  backup_root="$(mktemp -d "$backup_parent/$(date -u +%Y%m%d-%H%M%S).XXXXXX")" || return $?
  while IFS= read -r rel; do
    [ -n "$rel" ] || continue
    if [ -e "$OP_ROOT/$rel" ]; then
      mkdir -p "$backup_root/$(dirname "$rel")" || return $?
      cp -a "$OP_ROOT/$rel" "$backup_root/$rel" || return $?
    fi
  done < <(managed_targets)
  printf '%s\n' "$backup_root" || return $?
}

restore_managed_targets_from_backup() {
  local backup_root="$1"
  local rel=""
  local restore_ec=0
  [ -d "$backup_root" ] || return 1
  while IFS= read -r rel; do
    [ -n "$rel" ] || continue
    if [ -e "$backup_root/$rel" ]; then
      if ! mkdir -p "$OP_ROOT/$(dirname "$rel")"; then
        echo "WARN: failed to create parent for managed revert rollback target: $rel" >&2
        restore_ec=1
        continue
      fi
      if ! atomic_copy "$backup_root/$rel" "$OP_ROOT/$rel"; then
        echo "WARN: failed to restore managed revert rollback target from backup: $rel" >&2
        restore_ec=1
      fi
    else
      if ! rm -f "$OP_ROOT/$rel"; then
        echo "WARN: failed to remove managed revert rollback target absent from backup: $rel" >&2
        restore_ec=1
      fi
    fi
  done < <(managed_targets)
  return "$restore_ec"
}

# Replaces dst with src's content all at once: a temp file beside it, synced, then
# renamed over it, so a kill or power loss leaves openpilot the old file or the new
# one, never a truncated one.
atomic_copy() {
  local src="$1" dst="$2" tmp
  tmp="$(dirname "$dst")/.$(basename "$dst").commaview-tmp.$$"
  if ! cp -p "$src" "$tmp"; then
    rm -f "$tmp"
    return 1
  fi
  sync "$tmp" 2>/dev/null || sync
  mv -f "$tmp" "$dst" || { rm -f "$tmp"; return 1; }
}

# Puts a managed target back the way openpilot's HEAD has it (removes it when HEAD
# has none). The original is read with git cat-file and only a file that differs is
# replaced, atomically: git checkout and git reset would rewrite .git/index.
restore_target_from_head() {
  local rel="$1"
  local dst="$OP_ROOT/$rel"
  local tmp
  if ! git -C "$OP_ROOT" cat-file -e "HEAD:./$rel" 2>/dev/null; then
    if ! rm -f "$dst"; then
      echo "WARN: failed to remove untracked managed target: $rel" >&2
      return 1
    fi
    return 0
  fi
  if ! mkdir -p "$(dirname "$dst")"; then
    echo "WARN: failed to restore tracked managed target from HEAD: $rel" >&2
    return 1
  fi
  tmp="$(dirname "$dst")/.$(basename "$dst").commaview-head.$$"
  if ! git -C "$OP_ROOT" cat-file -p "HEAD:./$rel" > "$tmp"; then
    rm -f "$tmp"
    echo "WARN: failed to restore tracked managed target from HEAD: $rel" >&2
    return 1
  fi
  if [ -f "$dst" ] && cmp -s "$tmp" "$dst"; then
    rm -f "$tmp"
    return 0
  fi
  if [ -f "$dst" ]; then
    chmod --reference="$dst" "$tmp" 2>/dev/null || chmod 644 "$tmp"
  else
    chmod 644 "$tmp"
  fi
  sync "$tmp" 2>/dev/null || sync
  if ! mv -f "$tmp" "$dst"; then
    rm -f "$tmp"
    echo "WARN: failed to restore tracked managed target from HEAD: $rel" >&2
    return 1
  fi
}

reset_managed_targets() {
  local rel=""
  local reset_ec=0
  while IFS= read -r rel; do
    [ -n "$rel" ] || continue
    restore_target_from_head "$rel" || reset_ec=1
  done < <(managed_targets)
  return "$reset_ec"
}

if ! git -C "$OP_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "WARN: upstream repo not found at $OP_ROOT; skipping onroad UI export transformer revert" >&2
  exit 0
fi

detect_ui_prefix

if ! backup_root="$(backup_managed_targets)"; then
  echo "ERROR: failed to back up managed onroad UI export transformer targets; refusing revert" >&2
  exit 1
fi
echo "WARN: reverting managed onroad UI export transformer targets" >&2
echo "WARN: backups written to $backup_root" >&2
reset_ec=0
reset_managed_targets || reset_ec=$?
if [ "$reset_ec" -ne 0 ]; then
  restore_ec=0
  restore_managed_targets_from_backup "$backup_root" || restore_ec=$?
  if [ "$restore_ec" -eq 0 ]; then
    echo "ERROR: revert failed; restored managed targets from $backup_root" >&2
    exit "$reset_ec"
  fi
  echo "ERROR: revert failed and rollback failed or incomplete from $backup_root" >&2
  exit "$restore_ec"
fi
rm -f "$STATE_ENV" "$STATE_JSON" "$RESTART_MARKER"
note_openpilot_ui_keeps_loaded_code

echo "CommaView onroad UI export transformer reverted"
