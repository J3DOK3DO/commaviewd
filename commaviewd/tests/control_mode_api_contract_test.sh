#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER_CPP="$ROOT/src/http_server.cpp"
CONTROL_CPP="$ROOT/src/control_mode.cpp"
DOC_SHORT="$ROOT/docs/ai/telemetry-raw-only-readme.md"
DOC_DEEP="$ROOT/docs/ai/telemetry-raw-only-deep-dive.md"

[ -f "$SERVER_CPP" ] || { echo "FAIL: missing $SERVER_CPP"; exit 1; }
[ -f "$CONTROL_CPP" ] || { echo "FAIL: missing $CONTROL_CPP"; exit 1; }
[ -f "$DOC_SHORT" ] || { echo "FAIL: missing $DOC_SHORT"; exit 1; }
[ -f "$DOC_DEEP" ] || { echo "FAIL: missing $DOC_DEEP"; exit 1; }

grep -Fq "telemetryMode" "$CONTROL_CPP" || { echo "FAIL: /commaview/status should expose telemetryMode"; exit 1; }
grep -Fq 'return "direct-v2-ui-export";' "$CONTROL_CPP" || { echo "FAIL: telemetryMode should report direct-v2-ui-export"; exit 1; }
grep -Fq "persistedConfig" "$CONTROL_CPP" || { echo "FAIL: /commaview/status should expose persistedConfig"; exit 1; }
grep -Fq "effectiveConfig" "$CONTROL_CPP" || { echo "FAIL: /commaview/status should expose effectiveConfig"; exit 1; }
grep -Fq "runtimeStats" "$CONTROL_CPP" || { echo "FAIL: /commaview/status should expose runtimeStats"; exit 1; }
grep -Fq "configVersion" "$CONTROL_CPP" || { echo "FAIL: /commaview/status should expose configVersion"; exit 1; }
grep -Fq "configHash" "$CONTROL_CPP" || { echo "FAIL: /commaview/status should expose configHash"; exit 1; }
grep -Fq "warnings" "$CONTROL_CPP" || { echo "FAIL: /commaview/status should expose warnings"; exit 1; }
grep -Fq "safeFallback" "$CONTROL_CPP" || { echo "FAIL: /commaview/status should expose safeFallback state"; exit 1; }
grep -Fq "onroadUiExport" "$CONTROL_CPP" || { echo "FAIL: /commaview/status should expose onroadUiExport"; exit 1; }
grep -Fq "runtimeVersion" "$CONTROL_CPP" || { echo "FAIL: control mode should expose runtimeVersion alias"; exit 1; }
grep -Fq "device_dongle_id" "$CONTROL_CPP" || { echo "FAIL: control mode should read openpilot DongleId"; exit 1; }
grep -Fq "DongleId" "$CONTROL_CPP" || { echo "FAIL: control mode should read upstream DongleId param"; exit 1; }
grep -Fq "dongleId" "$CONTROL_CPP" || { echo "FAIL: control mode should expose dongleId"; exit 1; }
grep -Fq "dongle_id" "$CONTROL_CPP" || { echo "FAIL: control mode should expose dongle_id compatibility alias"; exit 1; }
grep -Fq "live_onroad_ui_export_status_json(false)" "$CONTROL_CPP" || { echo "FAIL: /commaview/status should use live onroad UI export verification"; exit 1; }
# uiReloadPending/uiReloadAction (patch waiting for a reboot, since openpilot cannot
# reload its UI in place) come from verify_onroad_ui_export_patch.sh's JSON, which
# the control API must pass through rather than rebuild.
grep -Fq 'run_command({kOnroadUiExportVerifyScript, "--json"}, &rc, &out, &err)' "$CONTROL_CPP" || { echo "FAIL: onroad UI export status should come from the verify script"; exit 1; }
grep -Fq 'if (!body.empty()) return body;' "$CONTROL_CPP" || { echo "FAIL: onroad UI export status should pass the verify JSON through"; exit 1; }
grep -Fq "/commaview/onroad-ui-export/status" "$CONTROL_CPP" || { echo "FAIL: missing /commaview/onroad-ui-export/status route"; exit 1; }
grep -Fq "/commaview/onroad-ui-export/repair" "$CONTROL_CPP" || { echo "FAIL: missing /commaview/onroad-ui-export/repair route"; exit 1; }
grep -Fq 'json_field_true(request_body, "forceOffroad")' "$CONTROL_CPP" || { echo "FAIL: repair route should parse forceOffroad"; exit 1; }
grep -Fq '/commaview/runtime-debug/config' "$CONTROL_CPP" || { echo "FAIL: missing /commaview/runtime-debug/config route"; exit 1; }
grep -Fq '/commaview/runtime-debug/defaults' "$CONTROL_CPP" || { echo "FAIL: missing /commaview/runtime-debug/defaults route"; exit 1; }
grep -Fq '/commaview/runtime-debug/apply' "$CONTROL_CPP" || { echo "FAIL: missing /commaview/runtime-debug/apply route"; exit 1; }
grep -Fq "runtime_debug_write_response" "$CONTROL_CPP" || { echo "FAIL: control mode missing runtime debug write handler"; exit 1; }
grep -Fq "runtime_debug_apply_response" "$CONTROL_CPP" || { echo "FAIL: control mode missing runtime debug apply handler"; exit 1; }
# Settings apply in place (SIGHUP to the running bridge); nothing restarts the runtime mid-drive.
grep -Fq '::kill(bridge, SIGHUP)' "$CONTROL_CPP" || { echo "FAIL: runtime debug apply should reload the bridge in place"; exit 1; }
! grep -Fq 'runtime-debug-apply bash /data/commaview/start.sh' "$CONTROL_CPP" || { echo "FAIL: runtime debug apply must not restart the runtime"; exit 1; }
grep -Fq '"\"roadPhase\":\""' "$CONTROL_CPP" || { echo "FAIL: /commaview/status should expose roadPhase"; exit 1; }
grep -Fq '"\"deferredMaintenance\":"' "$CONTROL_CPP" || { echo "FAIL: /commaview/status should expose deferredMaintenance"; exit 1; }
structured_stats_line="$(grep -nF '{"telemetry-stats.json"' "$CONTROL_CPP" | head -1 | cut -d: -f1)"
rolling_log_line="$(grep -nF '{"runtime-run-events.jsonl"' "$CONTROL_CPP" | head -1 | cut -d: -f1)"
[ -n "$structured_stats_line" ] && [ -n "$rolling_log_line" ] && [ "$structured_stats_line" -lt "$rolling_log_line" ] || {
  echo "FAIL: support bundles must include bounded structured counters before capped rolling logs"
  exit 1
}
grep -Fq '"/commaview/reboot"' "$CONTROL_CPP" || { echo "FAIL: missing /commaview/reboot route"; exit 1; }
# A reboot goes through openpilot's own DoReboot (its manager shuts down cleanly), never a raw reboot.
grep -Fq '"/DoReboot"' "$CONTROL_CPP" || { echo "FAIL: reboot should set openpilot's DoReboot param"; exit 1; }
! grep -Eq '"(/s?bin/|/usr/s?bin/)?reboot"' "$CONTROL_CPP" || { echo "FAIL: control mode must not reboot the comma itself"; exit 1; }
grep -Fq "X-CommaView-Token" "$SERVER_CPP" || { echo "FAIL: missing auth token header handling"; exit 1; }
! grep -Fq "/tailscale/" "$CONTROL_CPP" || { echo "FAIL: control mode still exposes tailscale routes"; exit 1; }
! grep -Fq '"tailscale":' "$CONTROL_CPP" || { echo "FAIL: /commaview/status should not expose tailscale state anymore"; exit 1; }

! grep -Fq "COMMAVIEWD_TELEMETRY_MODE" "$CONTROL_CPP" || { echo "FAIL: control mode should not depend on COMMAVIEWD_TELEMETRY_MODE"; exit 1; }
grep -Fq "docs/ai/telemetry-raw-only-readme.md" README.md || { echo "FAIL: missing short raw-only doc link in README"; exit 1; }
grep -Fq "docs/ai/telemetry-raw-only-deep-dive.md" README.md || { echo "FAIL: missing deep raw-only doc link in README"; exit 1; }

echo "PASS: control mode API contract routes present"
