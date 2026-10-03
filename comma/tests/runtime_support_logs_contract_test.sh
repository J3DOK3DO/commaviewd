#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CONTROL="$ROOT/commaviewd/src/control_mode.cpp"
SUPPORT="$ROOT/commaviewd/src/support_bundle.cpp"
MANAGER_STATE="$ROOT/commaviewd/src/manager_state_peek.cpp"
WATCH="$ROOT/commaviewd/src/process_watch.cpp"
EVENTS="$ROOT/commaviewd/src/process_events.cpp"

require_literal() {
  local needle="$1"
  local file="${2:-$CONTROL}"
  if ! grep -Fq -- "$needle" "$file"; then
    echo "missing literal in ${file##*/}: $needle" >&2
    exit 1
  fi
}

forbid_literal() {
  local needle="$1"
  local why="$2"
  shift 2
  local file
  for file in "$@"; do
    if grep -Fq -- "$needle" "$file"; then
      echo "${file##*/} must not contain '$needle': $why" >&2
      exit 1
    fi
  done
}

require_literal '"/commaview/support/logs"'
require_literal 'support_logs_response_json'
require_literal 'commaviewd-bridge.log'
require_literal 'commaviewd-control.log'
require_literal 'onroad-ui-export-startup.log'
require_literal 'runtime-run-events.jsonl'
require_literal 'telemetry-stats.json'
require_literal 'runtime-debug-effective.json'
require_literal 'last-restart-reason.txt'
require_literal 'runtime-debug-apply.log'
require_literal 'kSupportLogPerFileCapBytes'
require_literal 'kSupportLogTotalCapBytes'
require_literal 'is_authorized(req, api_token)'

# openpilot-side sections, gathered only while answering the request.
require_literal 'openpilot-manager-state.json'
require_literal 'openpilot-process-events.log'
require_literal 'kernel-oom-events.log'
require_literal 'peek_manager_state(msgq_dir())'
require_literal 'scan_swaglog(dir, secrets)'
require_literal 'filter_kernel_log('
require_literal 'redact_support_text(body, secrets)'

# Only a paired phone gets a bundle: an install without an API token answers 401.
support_block="$(sed -n '/req.path == "\/commaview\/support\/logs"/,/^  }/p' "$CONTROL")"
if ! grep -Fq 'api_token.empty() || !is_authorized(req, api_token)' <<<"$support_block"; then
  echo "support logs must require a configured API token" >&2
  exit 1
fi

# Built only by the request handler: defined once and called once, never from a thread or timer.
calls="$(grep -c 'support_logs_response_json(' "$CONTROL")"
if [[ "$calls" != "2" ]]; then
  echo "support_logs_response_json must be defined once and called only by the GET handler (found $calls)" >&2
  exit 1
fi

# Read-only: the kernel log is read like plain dmesg, never cleared; journald is not collected.
forbid_literal 'journalctl' 'support logs do not collect the journal' "$CONTROL" "$SUPPORT"
for flag in 'dmesg -c' 'dmesg -C' '"-c"' '"-C"' '--clear' '--read-clear' 'klogctl(4' 'klogctl(5' 'SYSLOG_ACTION_CLEAR' 'SYSLOG_ACTION_READ_CLEAR'; do
  forbid_literal "$flag" 'the kernel ring buffer must never be cleared' "$CONTROL" "$SUPPORT"
done
require_literal 'run_command_with_optional_sudo({"dmesg"}' "$CONTROL"

# managerState is peeked, never subscribed: reader slots are limited and belong to openpilot.
for needle in 'SubSocket' 'SubMaster' 'msgq_init_subscriber' 'subscribe(' 'O_RDWR' 'PROT_WRITE'; do
  forbid_literal "$needle" 'managerState is read without subscribing or writing' "$MANAGER_STATE" "$SUPPORT"
done
require_literal 'commaview::gps::QueuePeek' "$MANAGER_STATE"

# Nothing in the support code talks to the network: the phone fetches, the user shares.
for needle in 'socket(' 'connect(' 'curl' 'http://' 'https://'; do
  forbid_literal "$needle" 'support bundle code must not send anything anywhere' "$SUPPORT" "$MANAGER_STATE"
done

# The process watcher's log is in the bundle, beside the swaglog scan.
require_literal '"process-events.jsonl", process_events_log_path()'
require_literal '"process-events.jsonl.1", process_events_log_path() + ".1"'
require_literal 'start_process_watcher();'

# The watcher only looks: it peeks managerState and deviceState without a reader slot, never
# signals or writes to anything of openpilot's, never syncs, and runs at the lowest priority.
require_literal 'commaview::gps::QueuePeek' "$WATCH"
require_literal 'SCHED_IDLE' "$WATCH"
require_literal 'SYS_ioprio_set' "$WATCH"
for needle in 'SubSocket' 'SubMaster' 'msgq_init_subscriber' 'subscribe(' 'O_RDWR' 'PROT_WRITE' 'kill(' 'sigqueue' \
              'fsync' 'fdatasync' 'sync_file_range' 'socket(' 'connect(' 'system(' 'popen(' 'fork(' 'exec'; do
  forbid_literal "$needle" 'the process watcher only reads openpilot state and appends to its own log' "$WATCH" "$EVENTS"
done
# /dev/kmsg is only read, from where it ends: never the whole ring, never cleared.
require_literal 'O_RDONLY | O_NONBLOCK | O_CLOEXEC' "$EVENTS"
require_literal 'lseek(fd_, 0, SEEK_END)' "$EVENTS"
require_literal 'O_APPEND' "$EVENTS"

echo "PASS: runtime support logs endpoint contract"
