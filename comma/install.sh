#!/usr/bin/env bash
# CommaView installer for comma devices (AGNOS/openpilot/sunnypilot)
# Installs a prebuilt C++ commaviewd bundle from a pinned release, published at
# commaview.com/commaviewd/<tag>/ (another release repository's come from its GitHub releases).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd || true)"
VERSION_ENV="${SCRIPT_DIR}/version.env"
GITHUB_REPO="${COMMAVIEWD_RELEASE_REPO:-RhynoTech/commaviewd}"
RELEASES_API_URL="${COMMAVIEWD_RELEASES_API_URL:-https://api.github.com/repos/${GITHUB_REPO}/releases?per_page=20}"
# The runtime paired with the current CommaView app release. It describes RhynoTech/commaviewd
# only, so another release repo skips it unless the URL is set explicitly.
CURRENT_RELEASE_URL="${COMMAVIEWD_CURRENT_RELEASE_URL:-}"
if [ -z "$CURRENT_RELEASE_URL" ] && [ "$GITHUB_REPO" = "RhynoTech/commaviewd" ]; then
  CURRENT_RELEASE_URL="https://commaview.com/api/current-release"
fi
# Where RhynoTech/commaviewd's releases are published: <origin>/<tag>/ holds the tarball, its
# .sha256 and the installer's companions (comma/). Not GitHub, so the repository can be private.
RELEASES_ORIGIN=""
if [ "$GITHUB_REPO" = "RhynoTech/commaviewd" ]; then
  RELEASES_ORIGIN="${COMMAVIEWD_RELEASES_ORIGIN:-https://commaview.com/commaviewd}"
fi

resolve_latest_release_tag() {
  curl -fsSL --retry 3 --retry-delay 1 "$RELEASES_API_URL" \
    | tr -d '\r' \
    | grep -m1 '"tag_name":' \
    | sed -E 's/.*"tag_name":[[:space:]]*"([^"]+)".*/\1/' || true
}

resolve_current_release_tag() {
  [ -n "$CURRENT_RELEASE_URL" ] || return 0
  curl -fsL --max-time 5 --retry 1 --retry-delay 1 "$CURRENT_RELEASE_URL" \
    | tr -d '\r\n' \
    | sed -nE 's/.*"runtimeTag"[[:space:]]*:[[:space:]]*"(v[^"]+)".*/\1/p' || true
}

INSTALLED_RELEASE_TAG=""
if [ -f "$VERSION_ENV" ]; then
  # shellcheck disable=SC1090
  . "$VERSION_ENV"
  INSTALLED_RELEASE_TAG="${RELEASE_TAG:-}"
fi

USE_CURRENT_RELEASE=0
RELEASE_TAG="${COMMAVIEWD_RELEASE_TAG:-}"
VERSION="${COMMAVIEWD_VERSION:-}"
ASSET_NAME=""
ASSET_SHA_NAME=""
BASE_URL=""
INSTALLER_REF=""
INSTALLER_RAW_BASE=""

resolve_release_inputs() {
  if [ -z "$RELEASE_TAG" ]; then
    if [ "$USE_CURRENT_RELEASE" = "1" ] && [ -n "$INSTALLED_RELEASE_TAG" ]; then
      RELEASE_TAG="$INSTALLED_RELEASE_TAG"
    elif [ -n "${COMMAVIEWD_DEFAULT_TAG:-}" ]; then
      RELEASE_TAG="$COMMAVIEWD_DEFAULT_TAG"
    elif [ -n "${COMMAVIEWD_INSTALLER_REF:-}" ] && [[ "${COMMAVIEWD_INSTALLER_REF}" == v* ]]; then
      RELEASE_TAG="$COMMAVIEWD_INSTALLER_REF"
    else
      # Prefer the runtime the current app release uses, so a runtime that's tagged but
      # not yet paired with an app release isn't installed by default.
      RELEASE_TAG="$(resolve_current_release_tag)"
      if [ -z "$RELEASE_TAG" ]; then
        RELEASE_TAG="$(resolve_latest_release_tag)"
      fi
    fi
  fi
  if [ -z "$RELEASE_TAG" ]; then
    RELEASE_TAG="v0.0.1-alpha"
  fi

  VERSION="${COMMAVIEWD_VERSION:-${VERSION:-${RELEASE_TAG#v}}}"
  ASSET_NAME="${COMMAVIEWD_ASSET_NAME:-commaview-comma-${RELEASE_TAG}.tar.gz}"
  ASSET_SHA_NAME="${ASSET_NAME}.sha256"
  # Keep installer companions pinned to the same resolved release by default.
  # Falling back to a branch here mixes release assets with moving companion scripts.
  INSTALLER_REF="${COMMAVIEWD_INSTALLER_REF:-$RELEASE_TAG}"
  if [ -n "$RELEASES_ORIGIN" ]; then
    BASE_URL="${COMMAVIEWD_BASE_URL:-${RELEASES_ORIGIN}/${RELEASE_TAG}}"
  else
    BASE_URL="${COMMAVIEWD_BASE_URL:-https://github.com/${GITHUB_REPO}/releases/download/${RELEASE_TAG}}"
  fi
  # A release's companions are published with it; another ref (a branch, while developing) is
  # read from the repository.
  if [ -n "$RELEASES_ORIGIN" ] && [[ "$INSTALLER_REF" == v* ]]; then
    INSTALLER_RAW_BASE="${COMMAVIEWD_INSTALLER_RAW_BASE:-${RELEASES_ORIGIN}/${INSTALLER_REF}/comma}"
  else
    INSTALLER_RAW_BASE="${COMMAVIEWD_INSTALLER_RAW_BASE:-https://raw.githubusercontent.com/${GITHUB_REPO}/${INSTALLER_REF}/comma}"
  fi
}

# --tag and --current pick the release while the arguments are parsed, so only look up the
# default (which needs the network) when neither is given.
default_release_needed=1
for arg in "$@"; do
  case "$arg" in
    --tag|--current) default_release_needed=0 ;;
  esac
done
if [ "$default_release_needed" = "1" ]; then
  resolve_release_inputs
fi

INSTALL_DIR="/data/commaview"
CONTINUE_SH="/data/continue.sh"
MARKER="# commaview-hook"
# Runs synchronously before continue.sh execs launch_openpilot.sh: start.sh applies
# the onroad UI export patch before openpilot's manager loads its UI, then starts
# the runtime in the background. Earlier installs ran "start.sh &" alongside
# launch_openpilot.sh, which raced manager loading the UI.
BOOT_HOOK_CMD="/data/commaview/start.sh --before-openpilot"
PARAMS_DIR="/data/params/d"
FORCE_OFFROAD=0
# Exit status of an install queued until openpilot is offroad (EX_TEMPFAIL).
DEFERRED_EXIT=75
DEFERRED_DIR="${COMMAVIEWD_DEFERRED_DIR:-/data/commaview-deferred}"
tmpdir=""
COMPANION_DIR=""  # set to "$tmpdir/companions" by refresh_required_files
BACKUP_ROOT="${COMMAVIEWD_BACKUP_ROOT:-/data/commaview-backups}"
# The files an install replaces are backed up here on /data (not /tmp, which a power loss empties),
# the newest INSTALL_BACKUP_KEEP kept, so a failed install can always be put back.
INSTALL_BACKUP_ROOT="${COMMAVIEWD_INSTALL_BACKUP_ROOT:-$BACKUP_ROOT/install-previous}"
INSTALL_BACKUP_KEEP="${COMMAVIEWD_INSTALL_BACKUP_KEEP:-3}"
INSTALL_BACKUP_DIR=""
INSTALL_MUTATED=0
INSTALL_SUCCESS=0
INSTALL_ROLLED_BACK=0
PRESERVE_TMPDIR=0
ONROAD_UI_EXPORT_APPLIED=0
MAINT_LOCK="${COMMAVIEWD_MAINT_LOCK:-$DEFERRED_DIR/maint.lock}"
# How long a direct install waits for a deferred job, repair or uninstall that is running.
MAINT_LOCK_WAIT_SEC="${COMMAVIEWD_MAINT_LOCK_WAIT_SEC:-120}"
# A queued install reports its stage to the app beside the queue's status.json.
PROGRESS_FILE=""
if [ "${COMMAVIEWD_DEFERRED_JOB:-0}" = "1" ]; then
  PROGRESS_FILE="$DEFERRED_DIR/progress.json"
fi
LAST_ERROR=""
LAST_FAILED_COMMAND=""

usage() {
  cat <<USAGE
CommaView installer ${VERSION}

Usage:
  install.sh [--tag <release-tag>] [--current] [--force-offroad] [--help]

Options:
  --tag <release-tag>            Install or update to a specific release tag.
  --current                      Reinstall the installed release instead of looking one up.
  --force-offroad                While driving, queue the install to run once the car is parked or offroad
                                 (exit 75) instead of refusing it (exit 42). Never asks openpilot to go offroad.
  -h, --help                     Show this help and exit.

Without --tag or --current, installs the runtime paired with the current CommaView app
release, or the newest GitHub release if that lookup fails.
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --tag)
      [ "$#" -ge 2 ] || { echo "ERROR: --tag requires a value" >&2; exit 1; }
      RELEASE_TAG="$2"
      VERSION=""
      resolve_release_inputs
      shift 2
      ;;
    --current)
      USE_CURRENT_RELEASE=1
      RELEASE_TAG=""
      VERSION=""
      resolve_release_inputs
      shift
      ;;
    --force-offroad)
      FORCE_OFFROAD=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "ERROR: unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

error() {
  LAST_ERROR="$*"
  echo "ERROR: $*" >&2
}

now_ms() {
  local ms
  ms="$(date +%s%3N 2>/dev/null)"
  case "$ms" in ''|*[!0-9]*) ms="$(date +%s)000" ;; esac
  printf '%s\n' "$ms"
}

# Where the install is: a "COMMAVIEW_STAGE <name> [detail]" line on stdout (an SSH session reads
# them live) and, for a queued install, progress.json ({"stage","detail","atMs"}) written whole
# beside the deferred queue's status.json, which commaviewd reports to the app. Stages in order:
# starting, downloading, verifying, installing, restarting, done; or rolled_back / failed with the
# last error as the detail. Never fails the install.
stage() {
  local name="$1" detail="${2:-}" tmp
  detail="$(printf '%s' "$detail" | tr -d '\000-\037"\\' | cut -c1-300)"
  echo "COMMAVIEW_STAGE $name${detail:+ $detail}"
  [ -n "$PROGRESS_FILE" ] || return 0
  mkdir -p "$(dirname "$PROGRESS_FILE")" 2>/dev/null || return 0
  tmp="$PROGRESS_FILE.tmp.$$"
  if printf '{"stage":"%s","detail":"%s","atMs":%s}\n' "$name" "$detail" "$(now_ms)" > "$tmp" 2>/dev/null; then
    mv -f "$tmp" "$PROGRESS_FILE" 2>/dev/null || rm -f "$tmp"
  else
    rm -f "$tmp" 2>/dev/null
  fi
  return 0
}

# One CommaView maintenance at a time: install, uninstall, the onroad UI export patch and its revert
# and the deferred queue's job all rewrite the same files. The deferred job already holds the lock
# and says so in the environment; the patch scripts this runs see the same. Without flock, or where
# the lock file can't be made, it runs unlocked as before.
take_maint_lock() {
  [ "${COMMAVIEWD_MAINT_LOCK_HELD:-0}" = "1" ] && return 0
  command -v flock >/dev/null 2>&1 || return 0
  mkdir -p "$(dirname "$MAINT_LOCK")" 2>/dev/null || return 0
  { exec {MAINT_LOCK_FD}>>"$MAINT_LOCK"; } 2>/dev/null || return 0
  flock -w "$1" "$MAINT_LOCK_FD" || return 1
  export COMMAVIEWD_MAINT_LOCK_HELD=1 COMMAVIEWD_MAINT_LOCK_FD="$MAINT_LOCK_FD"
}

# Starts the runtime without this install's lock descriptor (this process keeps holding the lock):
# a runtime that inherited it would hold the lock for as long as it runs. start.sh still sees the
# lock is held, so its own repair step doesn't wait for this install.
without_maint_lock_fd() {
  local fd="${COMMAVIEWD_MAINT_LOCK_FD:-}"
  case "$fd" in
    ''|*[!0-9]*) "$@" ;;
    *) "$@" {fd}>&- ;;
  esac
}

# The heavy steps (download, checksum, unpack, copies) run at the lowest CPU and I/O priority, so
# they never compete with openpilot.
low_priority() {
  if command -v ionice >/dev/null 2>&1; then
    nice -n 19 ionice -t -c3 "$@"
  else
    nice -n 19 "$@"
  fi
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    error "missing required command: $1"
    exit 1
  }
}

read_param() {
  local path="$PARAMS_DIR/$1"
  [ -f "$path" ] || return 0
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
ROAD_PHASE_BIN="${COMMAVIEWD_ROAD_PHASE_BIN:-${INSTALL_DIR:-/data/commaview}/commaviewd}"
read_is_driving() {
  [ "$(read_is_onroad)" = "1" ] || { echo 0; return 0; }
  if [ -x "$ROAD_PHASE_BIN" ] && timeout 5 "$ROAD_PHASE_BIN" road-phase >/dev/null 2>&1; then
    echo 0
  else
    echo 1
  fi
}


commaview_pids() {
  local proc pid cmd
  for proc in /proc/[0-9]*; do
    pid="${proc##*/}"
    [ "$pid" = "$$" ] && continue
    [ -r "$proc/cmdline" ] || continue
    cmd="$(tr '\000' ' ' < "$proc/cmdline" 2>/dev/null || true)"
    case "$cmd" in
      *"/data/commaview/commaviewd bridge"*|*"/data/commaview/commaviewd control"*|*"/data/commaview/src/commaview_drive_stats.py"*)
        printf '%s\n' "$pid"
        ;;
    esac
  done | sort -nu
}

# Polls every 0.2s, up to <attempts> times, until none of <pids> is alive.
wait_for_pids_exit() {
  local attempts="$1"
  local pids="$2"
  local pid remaining
  for _ in $(seq 1 "$attempts"); do
    remaining=""
    for pid in $pids; do
      if kill -0 "$pid" 2>/dev/null; then
        remaining="$remaining $pid"
      fi
    done
    [ -z "$remaining" ] && return 0
    sleep 0.2
  done
  return 1
}

stop_commaview_processes() {
  local pids
  # start.sh's supervisors restart a crashed process while they still own their pid file.
  rm -f "$INSTALL_DIR/run/bridge-supervisor.pid" "$INSTALL_DIR/run/control-supervisor.pid"
  pids="$(commaview_pids | tr '\n' ' ')"
  [ -n "$pids" ] || return 0

  # shellcheck disable=SC2086
  kill $pids 2>/dev/null || true
  wait_for_pids_exit 25 "$pids" && return 0

  # shellcheck disable=SC2086
  kill -9 $pids 2>/dev/null || true
  wait_for_pids_exit 10 "$pids" && return 0

  return 1
}

ensure_commaview_stopped() {
  if ! stop_commaview_processes; then
    error "failed to stop existing CommaView runtime processes: $(commaview_pids | tr '\n' ' ')"
    exit 1
  fi
  leftover="$(commaview_pids | tr '\n' ' ')"
  if [ -n "$leftover" ]; then
    error "CommaView runtime processes still running after stop: $leftover"
    exit 1
  fi
  rm -f "$INSTALL_DIR/run/bridge.pid" "$INSTALL_DIR/run/control.pid"
}


restore_previous_install_tree() {
  local backup_dir="$INSTALL_BACKUP_DIR"
  local restore_ec=0
  [ "$INSTALL_MUTATED" = "1" ] || return 0
  [ "$INSTALL_SUCCESS" = "0" ] || return 0
  [ -n "$backup_dir" ] && [ -d "$backup_dir" ] || return 0

  echo "WARN: install failed after modifying live tree; restoring previous CommaView files" >&2
  rm -f \
    "$INSTALL_DIR/commaviewd" \
    "$INSTALL_DIR/VERSION" \
    "$INSTALL_DIR/install.sh" \
    "$INSTALL_DIR/start.sh" \
    "$INSTALL_DIR/stop.sh" \
    "$INSTALL_DIR/uninstall.sh" \
    "$INSTALL_DIR/runtime-debug.defaults.json" \
    "$INSTALL_DIR/version.env"
  rm -rf \
    "$INSTALL_DIR/lib" \
    "$INSTALL_DIR/scripts" \
    "$INSTALL_DIR/patches" \
    "$INSTALL_DIR/vendor" \
    "$INSTALL_DIR/src"
  if cp -a "$backup_dir"/. "$INSTALL_DIR"/; then
    :
  else
    restore_ec=$?
    echo "ERROR: failed to restore previous CommaView install tree from $backup_dir; the backup stays there for recovery" >&2
    return "$restore_ec"
  fi
  sync
  INSTALL_ROLLED_BACK=1
  if [ -x "$INSTALL_DIR/start.sh" ]; then
    echo "WARN: restarting restored CommaView runtime after failed install" >&2
    without_maint_lock_fd env COMMAVIEWD_RESTART_REASON=install-rollback bash "$INSTALL_DIR/start.sh" >/dev/null 2>&1 || \
      echo "WARN: failed to restart restored CommaView runtime" >&2
  fi
}

revert_onroad_ui_export_after_failed_install() {
  local revert_helper="$INSTALL_DIR/scripts/revert_onroad_ui_export_patch.sh"
  local revert_args=()
  local revert_ec=0
  [ "$ONROAD_UI_EXPORT_APPLIED" = "1" ] || return 0
  [ "$INSTALL_SUCCESS" = "0" ] || return 0

  if [ ! -x "$revert_helper" ]; then
    echo "ERROR: install failed after applying onroad UI export transformer, but revert helper is missing; preserving $INSTALL_DIR for recovery" >&2
    PRESERVE_TMPDIR=1
    return 1
  fi

  if [ "$FORCE_OFFROAD" = "1" ]; then
    revert_args+=(--force-offroad)
  fi

  echo "WARN: install failed after applying onroad UI export transformer; reverting upstream UI files" >&2
  COMMAVIEWD_INSTALL_DIR="$INSTALL_DIR" bash "$revert_helper" "${revert_args[@]}" || revert_ec=$?
  if [ "$revert_ec" -ne 0 ]; then
    echo "ERROR: failed to revert onroad UI export transformer after failed install; preserving $INSTALL_DIR for recovery" >&2
    PRESERVE_TMPDIR=1
    return "$revert_ec"
  fi
  return 0
}

cleanup() {
  local exit_status=$?
  local restore_ec=0
  local revert_failed=0
  [ -n "${COMMAVIEWD_INSTALLER_HANDOFF_FILE:-}" ] && rm -f "$COMMAVIEWD_INSTALLER_HANDOFF_FILE"
  if revert_onroad_ui_export_after_failed_install; then
    restore_previous_install_tree || restore_ec=$?
  else
    restore_ec=$?
    revert_failed=1
    echo "ERROR: skipping previous install restore because upstream UI transformer revert failed" >&2
  fi
  if [ "$PRESERVE_TMPDIR" = "1" ]; then
    echo "WARN: preserving installer temporary directory for rollback diagnostics: $tmpdir" >&2
  elif [ -n "${tmpdir:-}" ]; then
    rm -rf "$tmpdir"
  fi
  # Exit 42 (the car is being driven) and 75 (queued) changed nothing: the queue waits again.
  if [ "${INSTALL_SUCCESS:-0}" != "1" ] && [ "$exit_status" -ne 0 ] && [ "$exit_status" -ne 42 ] && \
     [ "$exit_status" -ne "${DEFERRED_EXIT:-75}" ]; then
    local why="${LAST_ERROR:-${LAST_FAILED_COMMAND:-exit $exit_status}}"
    if [ "$revert_failed" = "1" ]; then
      stage failed "$why; openpilot UI files could not be reverted"
    elif [ "$restore_ec" -ne 0 ]; then
      stage failed "$why; the previous release could not be restored"
    elif [ "${INSTALL_ROLLED_BACK:-0}" = "1" ]; then
      stage rolled_back "$why"
    else
      stage failed "$why"
    fi
  fi
  return "$restore_ec"
}

# Prunes the install backups to the newest INSTALL_BACKUP_KEEP.
prune_install_backups() {
  local old
  [ -d "$INSTALL_BACKUP_ROOT" ] || return 0
  find "$INSTALL_BACKUP_ROOT" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' 2>/dev/null | sort -r | \
    tail -n +"$((INSTALL_BACKUP_KEEP + 1))" | while IFS= read -r old; do
      [ -n "$old" ] && rm -rf "${INSTALL_BACKUP_ROOT:?}/$old"
    done
  return 0
}


# Queues this install in run_when_offroad.sh, which runs it once the car has been parked (or
# openpilot offroad) for a little while. The queued copy is this release's own installer, pinned to the tag resolved now.
queue_install_until_offroad() {
  local runner="$COMPANION_DIR/scripts/run_when_offroad.sh"
  if [ ! -f "$runner" ]; then
    echo "ERROR: install blocked while driving, and this release cannot queue it. Park the vehicle and retry." >&2
    exit 42
  fi
  if ! COMMAVIEWD_DEFERRED_DIR="$DEFERRED_DIR" bash "$runner" queue install --file "$COMPANION_DIR/install.sh" -- \
      bash @JOB@/install.sh --tag "$RELEASE_TAG" >/dev/null; then
    echo "ERROR: install blocked while driving and could not be queued. Park the vehicle and retry." >&2
    exit 42
  fi
  echo "DEFERRED: CommaView ${RELEASE_TAG} will install once the car is in Park with openpilot disengaged, or offroad."
  echo "COMMAVIEW_MAINTENANCE_DEFERRED=install"
  exit "$DEFERRED_EXIT"
}

# The release being installed installs itself: an installer asked for another release (the comma's
# own update queues the installed install.sh with --tag) hands over to that release's install.sh,
# fetched on its own before the other companions (which that installer fetches itself), before it
# changes anything. Piped installs (curl | bash) already run the release's own installer.
handoff_to_release_installer() {
  [ "${COMMAVIEWD_INSTALLER_HANDOFF:-0}" = "1" ] && return 0
  local self="${BASH_SOURCE[0]:-}" next="$COMPANION_DIR/install.sh" copy
  [ -f "$self" ] && [ -f "$next" ] || return 0
  [ "$(sha256sum < "$self")" = "$(sha256sum < "$next")" ] && return 0
  copy="$(mktemp /tmp/commaview-install-next.XXXXXX)" || return 0
  cp -f "$next" "$copy" || { rm -f "$copy"; return 0; }
  local args=(--tag "$RELEASE_TAG")
  [ "$FORCE_OFFROAD" = "1" ] && args+=(--force-offroad)
  echo "Handing over to the ${RELEASE_TAG} installer"
  rm -rf "$tmpdir"
  trap - EXIT
  COMMAVIEWD_INSTALLER_HANDOFF=1 COMMAVIEWD_INSTALLER_HANDOFF_FILE="$copy" exec bash "$copy" "${args[@]}"
}

# A queued job would undo or repeat what an install run directly does now.
cancel_deferred_maintenance() {
  [ "${COMMAVIEWD_DEFERRED_JOB:-0}" = "1" ] && return 0
  [ -f "$DEFERRED_DIR/runner.sh" ] || return 0
  COMMAVIEWD_DEFERRED_DIR="$DEFERRED_DIR" bash "$DEFERRED_DIR/runner.sh" cancel >/dev/null 2>&1 || true
}

# Installing restarts the runtime and rewrites openpilot's UI files (which its running UI never
# reloads), so it runs offroad or parked, never while driving. CommaView never asks openpilot to go
# offroad (no OffroadMode): with --force-offroad an install asked for while driving is queued until
# the car is parked instead.
ensure_offroad_ready() {
  if [ "$(read_is_driving)" != "1" ]; then
    cancel_deferred_maintenance
    return 0
  fi

  if [ "$FORCE_OFFROAD" != "1" ]; then
    echo "ERROR: install blocked while driving. Shift into Park (openpilot not engaged), or rerun with --force-offroad to install once it is parked." >&2
    exit 42
  fi
  queue_install_until_offroad
}

copy_required_file() {
  local src_rel="$1"
  local dst="$2"
  local mode="${3:-755}"
  local src="$COMPANION_DIR/$src_rel"

  if [ ! -f "$src" ]; then
    error "missing required installer file: $src_rel"
    exit 1
  fi

  mkdir -p "$(dirname "$dst")"
  cp "$src" "$dst"
  chmod "$mode" "$dst"
}

required_files=(
  "install.sh"
  "start.sh"
  "stop.sh"
  "uninstall.sh"
  "runtime-debug.defaults.json"
  "scripts/verify_onroad_ui_export_patch.sh"
  "scripts/apply_onroad_ui_export_patch.sh"
  "scripts/revert_onroad_ui_export_patch.sh"
  "scripts/smoke_onroad_ui_export_helper.py"
  "scripts/transform_onroad_ui_export.py"
  "scripts/run_when_offroad.sh"
  "src/commaview_export.openpilot.py"
  "src/commaview_export.sunnypilot.py"
  "src/commaview_drive_stats.py"
)

# Fetches the named companions (all of them by default) that aren't fetched yet: install.sh comes
# first on its own, so an installer handing over to another release's fetches nothing else twice.
refresh_required_files() {
  local rel dst url dir
  local failed=()
  local wanted=("$@")
  [ "${#wanted[@]}" -gt 0 ] || wanted=("${required_files[@]}")

  COMPANION_DIR="$tmpdir/companions"
  mkdir -p "$COMPANION_DIR"

  for rel in "${wanted[@]}"; do
    dst="$COMPANION_DIR/$rel"
    url="${INSTALLER_RAW_BASE}/$rel"
    dir="$(dirname "$dst")"
    [ -f "$dst" ] && continue

    if ! mkdir -p "$dir"; then
      error "unable to create directory for installer companion files: $dir"
      failed+=("$rel")
      continue
    fi

    echo "Fetching installer companion: $rel"
    if ! low_priority curl -fsSL --retry 3 --retry-delay 1 -o "$dst" "$url"; then
      rm -f "$dst"
      failed+=("$rel")
    fi
  done

  if [ "${#failed[@]}" -gt 0 ]; then
    error "failed to fetch required installer files from ${INSTALLER_RAW_BASE}:"
    for rel in "${failed[@]}"; do
      echo "  - $rel" >&2
    done
    exit 1
  fi
}

validate_required_files() {
  local missing=()
  local rel
  for rel in "${required_files[@]}"; do
    if [ ! -f "$COMPANION_DIR/$rel" ]; then
      missing+=("$COMPANION_DIR/$rel")
    fi
  done

  if [ "${#missing[@]}" -gt 0 ]; then
    error "missing required installer files:"
    for path in "${missing[@]}"; do
      echo "  - $path" >&2
    done
    exit 1
  fi
}

deploy_required_scripts() {
  mkdir -p "$INSTALL_DIR/src"
  copy_required_file "install.sh" "$INSTALL_DIR/install.sh"
  copy_required_file "start.sh" "$INSTALL_DIR/start.sh"
  copy_required_file "runtime-debug.defaults.json" "$INSTALL_DIR/runtime-debug.defaults.json" 644
  copy_required_file "scripts/verify_onroad_ui_export_patch.sh" "$INSTALL_DIR/scripts/verify_onroad_ui_export_patch.sh"
  copy_required_file "scripts/apply_onroad_ui_export_patch.sh" "$INSTALL_DIR/scripts/apply_onroad_ui_export_patch.sh"
  copy_required_file "scripts/revert_onroad_ui_export_patch.sh" "$INSTALL_DIR/scripts/revert_onroad_ui_export_patch.sh"
  copy_required_file "scripts/smoke_onroad_ui_export_helper.py" "$INSTALL_DIR/scripts/smoke_onroad_ui_export_helper.py"
  copy_required_file "scripts/transform_onroad_ui_export.py" "$INSTALL_DIR/scripts/transform_onroad_ui_export.py"
  copy_required_file "scripts/run_when_offroad.sh" "$INSTALL_DIR/scripts/run_when_offroad.sh"
  # Remove the retired experimental subprocess helper when upgrading from that build.
  rm -f "$INSTALL_DIR/scripts/commaview_export_worker.py"
  copy_required_file "src/commaview_export.openpilot.py" "$INSTALL_DIR/src/commaview_export.openpilot.py" 644
  copy_required_file "src/commaview_export.sunnypilot.py" "$INSTALL_DIR/src/commaview_export.sunnypilot.py" 644
  copy_required_file "src/commaview_drive_stats.py" "$INSTALL_DIR/src/commaview_drive_stats.py" 644
  copy_required_file "stop.sh" "$INSTALL_DIR/stop.sh"
  copy_required_file "uninstall.sh" "$INSTALL_DIR/uninstall.sh"

  cat > "$INSTALL_DIR/version.env" <<EOF
VERSION="${VERSION}"
RELEASE_TAG="${RELEASE_TAG}"
EOF
  chmod 644 "$INSTALL_DIR/version.env"
}


ensure_api_auth_token() {
  local token_path="/data/commaview/api/auth.token"
  if [ -s "$token_path" ]; then
    chmod 600 "$token_path" 2>/dev/null || true
    return 0
  fi

  echo "Generating CommaView API auth token..."
  umask 077
  python3 - <<'PYTOKEN' > "$token_path"
import secrets
print(secrets.token_urlsafe(32))
PYTOKEN
  chmod 600 "$token_path" 2>/dev/null || true
}

# The new runtime is healthy once its control API answers: what the app (and the next update)
# talks to.
wait_for_control_api() {
  local _
  for _ in $(seq 1 "${COMMAVIEWD_INSTALL_HEALTH_TIMEOUT_SEC:-30}"); do
    curl -fsS --max-time 2 http://127.0.0.1:5002/commaview/version >/dev/null 2>&1 && return 0
    sleep 1
  done
  return 1
}

print_pairing_code() {
  local token_path="/data/commaview/api/auth.token"
  local token=""
  local response=""
  local pair_code=""

  if [ -r "$token_path" ]; then
    token="$(tr -d '\r\n' < "$token_path")"
  fi

  if [ -z "$token" ]; then
    echo "Pair code: unavailable (missing API token)"
    return 0
  fi

  response="$(curl -fsS --retry 5 --retry-delay 1 -X POST \
    -H "X-CommaView-Token: $token" \
    http://127.0.0.1:5002/pairing/create 2>/dev/null || true)"
  pair_code="$(PAIRING_RESPONSE="$response" python3 - <<'PYPAIR'
import json
import os
try:
    data = json.loads(os.environ.get("PAIRING_RESPONSE", ""))
    print(data.get("pairCode", ""))
except Exception:
    print("")
PYPAIR
)"

  if [ -n "$pair_code" ]; then
    echo ""
    echo "CommaView pair code: $pair_code"
    echo "Enter this one-time pair code in the CommaView app."
  else
    echo "Pair code: unavailable (open CommaView settings to generate one after install)"
  fi
}

# Installs or upgrades the continue.sh boot hook. The hook goes right before
# continue.sh's "exec ... launch_openpilot" line; without one there is nowhere
# safe to put it, so an existing hook is left as it is.
install_boot_hook() {
  [ -f "$CONTINUE_SH" ] || return 0
  if ! grep -q '^exec .*launch_openpilot' "$CONTINUE_SH"; then
    echo "WARN: $CONTINUE_SH has no exec launch_openpilot line; boot hook not changed" >&2
    return 0
  fi
  if grep -qF "$MARKER" "$CONTINUE_SH" && grep -qxF "$BOOT_HOOK_CMD" "$CONTINUE_SH" && \
     [ "$(grep -c 'commaview/start.sh' "$CONTINUE_SH")" = "1" ]; then
    echo "Boot hook already present"
    return 0
  fi
  if grep -qF "$MARKER" "$CONTINUE_SH" || grep -q 'commaview/start.sh' "$CONTINUE_SH"; then
    sed -i '/# commaview-hook/d; /commaview\/start.sh/d' "$CONTINUE_SH"
    echo "Upgrading boot hook to prepare the onroad UI export before openpilot starts"
  fi
  sed -i "/^exec .*launch_openpilot/i\\
$MARKER\\
$BOOT_HOOK_CMD" "$CONTINUE_SH"
  echo "Boot hook installed"
}

# Everything clean_managed_install_tree removes or deploy replaces, so a failed install can put the
# previous release back whole. Kept on /data and synced before anything is removed.
backup_managed_install_tree() {
  local backup_dir
  mkdir -p "$INSTALL_BACKUP_ROOT"
  backup_dir="$(mktemp -d "$INSTALL_BACKUP_ROOT/$(date -u +%Y%m%d-%H%M%S).XXXXXX")"
  INSTALL_BACKUP_DIR="$backup_dir"
  for rel in \
    commaviewd \
    VERSION \
    install.sh \
    start.sh \
    stop.sh \
    uninstall.sh \
    runtime-debug.defaults.json \
    version.env \
    lib \
    scripts \
    patches \
    vendor \
    src; do
    [ -e "$INSTALL_DIR/$rel" ] || continue
    low_priority cp -a "$INSTALL_DIR/$rel" "$backup_dir/$rel"
  done
  sync
}

clean_managed_install_tree() {
  echo "Removing stale managed CommaView files..."
  INSTALL_MUTATED=1
  rm -f \
    "$INSTALL_DIR/commaviewd" \
    "$INSTALL_DIR/VERSION" \
    "$INSTALL_DIR/start.sh" \
    "$INSTALL_DIR/stop.sh" \
    "$INSTALL_DIR/uninstall.sh" \
    "$INSTALL_DIR/runtime-debug.defaults.json" \
    "$INSTALL_DIR/version.env"
  rm -rf \
    "$INSTALL_DIR/lib" \
    "$INSTALL_DIR/scripts" \
    "$INSTALL_DIR/patches" \
    "$INSTALL_DIR/vendor" \
    "$INSTALL_DIR/src"
  # run/ keeps its state (the UI reload marker, the car identity cache); only what names the
  # stopped runtime goes.
  rm -f "$INSTALL_DIR"/run/*.pid "$INSTALL_DIR/run/ui-export.sock"
  mkdir -p \
    "$INSTALL_DIR/logs" \
    "$INSTALL_DIR/run" \
    "$INSTALL_DIR/lib" \
    "$INSTALL_DIR/api" \
    "$INSTALL_DIR/config"
  rm -f \
    "$INSTALL_DIR/config/hud-lite-patch.env"
}

need_cmd curl
need_cmd tar
need_cmd sha256sum
need_cmd cp

tmpdir="$(mktemp -d /tmp/commaview-install.XXXXXX)"
trap cleanup EXIT
# What failed, for the stage report, when a command stops the install without an ERROR line.
set -E
trap 'LAST_FAILED_COMMAND="command failed (exit $?): $BASH_COMMAND"' ERR
stage starting
refresh_required_files install.sh
handoff_to_release_installer
refresh_required_files
validate_required_files

echo "=== CommaView ${VERSION} Installer ==="
echo "Release: ${RELEASE_TAG}"
echo "Repo:    ${GITHUB_REPO}"

ensure_offroad_ready

if ! take_maint_lock "$MAINT_LOCK_WAIT_SEC"; then
  error "another CommaView install, uninstall or repair is running; try again when it has finished"
  exit 1
fi

mkdir -p "$INSTALL_DIR/logs" "$INSTALL_DIR/run" "$INSTALL_DIR/lib" "$INSTALL_DIR/api"

stage downloading "$RELEASE_TAG"
echo "Downloading release assets..."
low_priority curl -fL --retry 3 --retry-delay 1 -o "$tmpdir/$ASSET_NAME" "$BASE_URL/$ASSET_NAME"
low_priority curl -fL --retry 3 --retry-delay 1 -o "$tmpdir/$ASSET_SHA_NAME" "$BASE_URL/$ASSET_SHA_NAME"

stage verifying "$RELEASE_TAG"
expected_sha="$(awk 'NF{print $1; exit}' "$tmpdir/$ASSET_SHA_NAME" | tr -d '\r\n')"
if ! echo "$expected_sha" | grep -Eq '^[0-9a-fA-F]{64}$'; then
  error "invalid sha256 file format: $ASSET_SHA_NAME"
  exit 1
fi
actual_sha="$(low_priority sha256sum "$tmpdir/$ASSET_NAME" | awk '{print $1}')"
if [ "$expected_sha" != "$actual_sha" ]; then
  error "checksum mismatch"
  echo "  expected: $expected_sha" >&2
  echo "  actual:   $actual_sha" >&2
  exit 1
fi

STAGED_BUNDLE="$tmpdir/staged-bundle"
mkdir -p "$STAGED_BUNDLE"
echo "Staging and validating bundle..."
low_priority tar -xzf "$tmpdir/$ASSET_NAME" -C "$STAGED_BUNDLE" --strip-components=1
if [ ! -f "$STAGED_BUNDLE/commaviewd" ]; then
  error "bundle missing commaviewd"
  exit 1
fi
staged_capnp_lib_count=$(find "$STAGED_BUNDLE/lib" -maxdepth 1 -type f -name 'libcapnp-*.so' 2>/dev/null | wc -l | tr -d ' ')
staged_kj_lib_count=$(find "$STAGED_BUNDLE/lib" -maxdepth 1 -type f -name 'libkj-*.so' 2>/dev/null | wc -l | tr -d ' ')
if [ "$staged_capnp_lib_count" -eq 0 ] || [ "$staged_kj_lib_count" -eq 0 ]; then
  error "bundle missing required runtime libs"
  exit 1
fi

# The car may have been driven off while this downloaded: check again right before anything
# changes. Nothing has changed yet, so a queued install just waits for the next parked stretch.
ensure_offroad_ready

stage installing "$RELEASE_TAG"
backup_managed_install_tree

echo "Stopping existing CommaView processes..."
ensure_commaview_stopped

clean_managed_install_tree

echo "Installing staged bundle..."
low_priority cp -a "$STAGED_BUNDLE"/. "$INSTALL_DIR"/

deploy_required_scripts
ensure_api_auth_token
echo "Applying direct v2 onroad UI export patch lifecycle..."
if [ -x "$INSTALL_DIR/scripts/apply_onroad_ui_export_patch.sh" ]; then
  apply_ec=0
  COMMAVIEWD_INSTALL_DIR="$INSTALL_DIR" bash "$INSTALL_DIR/scripts/apply_onroad_ui_export_patch.sh" --force-repair || apply_ec=$?
  if [ "$apply_ec" -ne 0 ]; then
    # apply puts openpilot's UI files back itself when it fails.
    error "the onroad UI export patch failed (exit $apply_ec)"
    exit "$apply_ec"
  fi
  ONROAD_UI_EXPORT_APPLIED=1
else
  error "missing onroad UI export patch apply helper"
  exit 1
fi

if [ ! -f "$INSTALL_DIR/commaviewd" ]; then
  error "bundle missing $INSTALL_DIR/commaviewd"
  exit 1
fi
capnp_lib_count=$(find "$INSTALL_DIR/lib" -maxdepth 1 -type f -name 'libcapnp-*.so' | wc -l | tr -d ' ')
kj_lib_count=$(find "$INSTALL_DIR/lib" -maxdepth 1 -type f -name 'libkj-*.so' | wc -l | tr -d ' ')
if [ "$capnp_lib_count" -eq 0 ] || [ "$kj_lib_count" -eq 0 ]; then
  error "bundle missing required runtime libs in $INSTALL_DIR/lib"
  exit 1
fi

chmod +x "$INSTALL_DIR/commaviewd"
BINARY_SIZE=$(ls -lh "$INSTALL_DIR/commaviewd" | awk '{print $5}')

install_boot_hook
# On disk before the new runtime starts: a power loss from here on boots a whole release.
sync

stage restarting "$RELEASE_TAG"
echo "Starting CommaView runtime..."
without_maint_lock_fd bash "$INSTALL_DIR/start.sh"
if ! wait_for_control_api; then
  # The cleanup trap puts the previous release back and starts it.
  error "the ${RELEASE_TAG} runtime did not answer on its control API; rolling back"
  stop_commaview_processes || true
  exit 1
fi
# A queued install updates an app that is already paired: no new pair code (and none in its log).
if [ "${COMMAVIEWD_DEFERRED_JOB:-0}" != "1" ]; then
  print_pairing_code
fi
INSTALL_SUCCESS=1
prune_install_backups
stage done "$RELEASE_TAG"

echo ""
echo "=== CommaView ${VERSION} installed ==="
if [ -f "$INSTALL_DIR/run/onroad-ui-export-ui-restart-needed" ]; then
  echo "  Reboot the comma device to load the onroad UI export: openpilot cannot reload"
  echo "  its running UI, so CommaView leaves it alone until openpilot next starts."
fi
echo "  Source:      ${BASE_URL}/${ASSET_NAME}"
echo "  Binary:      $INSTALL_DIR/commaviewd ($BINARY_SIZE)"
echo "  Runtime:     commaviewd dual-mode (bridge + control)"
echo "  Direct v2 onroad UI export: install-time patch lifecycle enforced"
echo "  Install/update: bash $INSTALL_DIR/install.sh [--tag <release-tag>] [--current] [--force-offroad]"
echo "  Uninstall:   bash $INSTALL_DIR/uninstall.sh"
