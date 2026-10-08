import json
import re
import shlex
import subprocess
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULTS = REPO_ROOT / "comma" / "runtime-debug.defaults.json"
START_SH = REPO_ROOT / "comma" / "start.sh"
STOP_SH = REPO_ROOT / "comma" / "stop.sh"
INSTALL_SH = REPO_ROOT / "comma" / "install.sh"
OLD_INSTALL_SHIM = REPO_ROOT / "comma4" / "install.sh"
UNINSTALL_SH = REPO_ROOT / "comma" / "uninstall.sh"
APPLY_PATCH_SH = REPO_ROOT / "comma" / "scripts" / "apply_onroad_ui_export_patch.sh"
REVERT_PATCH_SH = REPO_ROOT / "comma" / "scripts" / "revert_onroad_ui_export_patch.sh"
VERIFY_PATCH_SH = REPO_ROOT / "comma" / "scripts" / "verify_onroad_ui_export_patch.sh"
SUNNYPILOT_PATCH = REPO_ROOT / "comma" / "patches" / "sunnypilot" / "0001-commaview-ui-export-v2.patch"
CONTROL_CPP = REPO_ROOT / "commaviewd" / "src" / "control_mode.cpp"


def test_legacy_comma4_install_shim_forwards_to_generic_comma_installer():
    text = OLD_INSTALL_SHIM.read_text()
    assert '/comma4/install.sh' in text
    assert '/comma/install.sh' in text
    assert 'curl -fsSL "$SCRIPT_URL" | bash -s -- "$@"' in text
    assert 'case "${args[$i]}" in' in text
    assert '--tag)' in text
    assert '--tag=*)' in text

def test_runtime_debug_defaults_match_policy_contract():
    data = json.loads(DEFAULTS.read_text())
    assert data["configVersion"] == 1
    assert data["instrumentationLevel"] == "standard"
    services = data["services"]
    assert services == {
        "uiStateOnroad": {"mode": "pass"},
        "selfdriveState": {"mode": "pass"},
        "carState": {"mode": "pass"},
        "controlsState": {"mode": "pass"},
        "onroadEvents": {"mode": "pass"},
        "driverMonitoringState": {"mode": "pass"},
        "driverStateV2": {"mode": "pass"},
        "modelV2": {"mode": "pass"},
        "radarState": {"mode": "pass"},
        "liveCalibration": {"mode": "pass"},
        "carOutput": {"mode": "pass"},
        "carControl": {"mode": "pass"},
        "liveParameters": {"mode": "pass"},
        "longitudinalPlan": {"mode": "pass"},
        "carParams": {"mode": "pass"},
        "deviceState": {"mode": "pass"},
        "roadCameraState": {"mode": "pass"},
        "pandaStatesSummary": {"mode": "pass"},
        "onroadProjection": {"mode": "pass"},
        "wideRoadCameraState": {"mode": "pass"},
    }


def test_start_script_seeds_direct_v2_runtime_debug_defaults_and_exports_paths():
    text = START_SH.read_text()
    assert "runtime-debug.defaults.json" in text
    assert "RUNTIME_DEBUG_CONFIG" in text
    assert "RUNTIME_DEBUG_EFFECTIVE" in text
    assert "RUNTIME_DEBUG_STATS" in text
    assert "last-restart-reason.txt" in text
    assert "COMMAVIEWD_RUNTIME_DEBUG_CONFIG" in text
    assert "COMMAVIEWD_RUNTIME_DEBUG_EFFECTIVE" in text
    assert "COMMAVIEWD_RUNTIME_STATS" in text
    assert "invalid runtime debug config JSON" in text
    assert '"uiStateOnroad":{"mode":"pass"}' in text
    assert '"selfdriveState":{"mode":"pass"}' in text
    assert '"carState":{"mode":"pass"}' in text
    assert '"controlsState":{"mode":"pass"}' in text
    assert '"onroadEvents":{"mode":"pass"}' in text
    assert '"driverMonitoringState":{"mode":"pass"}' in text
    assert '"driverStateV2":{"mode":"pass"}' in text
    assert '"modelV2":{"mode":"pass"}' in text
    assert '"radarState":{"mode":"pass"}' in text
    assert '"liveCalibration":{"mode":"pass"}' in text
    assert '"carOutput":{"mode":"pass"}' in text
    assert '"carControl":{"mode":"pass"}' in text
    assert '"liveParameters":{"mode":"pass"}' in text
    assert '"longitudinalPlan":{"mode":"pass"}' in text
    assert '"carParams":{"mode":"pass"}' in text
    assert '"deviceState":{"mode":"pass"}' in text
    assert '"roadCameraState":{"mode":"pass"}' in text
    assert '"pandaStatesSummary":{"mode":"pass"}' in text
    assert '"onroadProjection":{"mode":"pass"}' in text
    assert '"wideRoadCameraState":{"mode":"pass"}' in text

    for forbidden in (
        '"commaViewHudLite"',
        '"commaViewControl"',
        '"commaViewScene"',
        '"commaViewStatus"',
    ):
        assert forbidden not in text


def test_start_script_refreshes_and_self_heals_onroad_ui_export_status_offroad_only():
    text = START_SH.read_text()
    assert "verify_onroad_ui_export_patch.sh" in text
    assert "apply_onroad_ui_export_patch.sh" in text
    assert "refresh_onroad_ui_export_status" in text
    assert "onroad UI export patch verified at startup" in text
    assert "onroad UI export patch repaired at startup" in text
    assert "skipping startup repair" in text
    assert "onroad-ui-export-ui-restart-needed" in text
    assert "IsOnroad" in text
    # openpilot cannot reload its UI in place: start.sh never signals it, and the
    # boot hook applies the patch before launch_openpilot.sh starts manager.
    assert "pkill" not in text
    assert "report_openpilot_ui_reload_pending" in text
    assert 'if [ "${1:-}" = "--before-openpilot" ]; then' in text
    assert '"$ONROAD_UI_EXPORT_APPLY" --before-openpilot' in text
    assert "timeout -k 5" in text
    assert "COMMAVIEWD_ONROAD_UI_EXPORT_PREPARED=1" in text
    assert "onroad UI export prepared before openpilot start; skipping startup repair" in text
    assert text.index('if [ "${1:-}" = "--before-openpilot" ]; then') < text.index("refresh_onroad_ui_export_status\n")


def test_install_script_stages_release_and_refreshes_pinned_companions_before_mutating_live_tree():
    text = INSTALL_SH.read_text()
    assert "refresh_required_files" in text
    assert 'COMPANION_DIR="$tmpdir/companions"' in text
    assert 'Fetching installer companion:' in text
    assert 'local src="$COMPANION_DIR/$src_rel"' in text
    assert "Staging and validating bundle" in text
    assert 'tar -xzf "$tmpdir/$ASSET_NAME" -C "$STAGED_BUNDLE" --strip-components=1' in text
    assert "backup_managed_install_tree" in text
    assert "restore_previous_install_tree" in text
    assert "ensure_commaview_stopped" in text
    assert 'pkill -f "/data/commaview/commaviewd"' not in text
    assert 'INSTALL_SUCCESS=1' in text
    assert text.index('echo "Staging and validating bundle..."') < text.index('echo "Stopping existing CommaView processes..."')
    assert text.index('backup_managed_install_tree') < text.index('echo "Stopping existing CommaView processes..."')
    assert text.index('ensure_commaview_stopped') < text.index('clean_managed_install_tree')


def test_stop_script_kills_only_runtime_processes_without_pkill_self_match():
    text = STOP_SH.read_text()
    assert "commaview_pids()" in text
    assert '/proc/[0-9]*' in text
    assert '"/data/commaview/commaviewd bridge"' in text
    assert '"/data/commaview/commaviewd control"' in text
    assert 'pkill -f' not in text
    assert 'kill -9 $pids' in text
    assert 'ERROR: CommaView runtime processes still running' in text


def test_install_script_uses_robust_runtime_stop_before_mutating_live_tree():
    text = INSTALL_SH.read_text()
    assert "commaview_pids()" in text
    assert "stop_commaview_processes()" in text
    assert "ensure_commaview_stopped" in text
    assert 'kill -9 $pids' in text
    assert 'rm -f "$INSTALL_DIR/run/bridge.pid" "$INSTALL_DIR/run/control.pid"' in text
    stop_call = text.index('echo "Stopping existing CommaView processes..."')
    clean_call = text.index('\nclean_managed_install_tree', stop_call)
    assert stop_call < clean_call


def _shell_function_body(text: str, name: str) -> str:
    match = re.search(rf"\n{name}\(\) \{{(?P<body>.*?)\n\}}", text, re.S)
    assert match, f"missing shell function {name}"
    return match.group("body")


def _shell_function_definition(text: str, name: str) -> str:
    return f"{name}() {{{_shell_function_body(text, name)}\n}}"


def test_install_script_rolls_back_managed_src_tree():
    text = INSTALL_SH.read_text()
    backup_body = _shell_function_body(text, "backup_managed_install_tree")
    clean_body = _shell_function_body(text, "clean_managed_install_tree")
    restore_body = _shell_function_body(text, "restore_previous_install_tree")
    assert "vendor" in backup_body
    assert '"$INSTALL_DIR/vendor"' in clean_body
    deploy_body = _shell_function_body(text, "deploy_required_scripts")

    assert "src" in backup_body
    assert '"$INSTALL_DIR/src"' in clean_body
    assert '"$INSTALL_DIR/src"' in restore_body
    assert 'mkdir -p "$INSTALL_DIR/src"' in deploy_body
    assert deploy_body.index('mkdir -p "$INSTALL_DIR/src"') < deploy_body.index('copy_required_file "src/commaview_export.openpilot.py"')


def test_install_script_failed_rollback_copy_keeps_backup_and_skips_restart():
    text = INSTALL_SH.read_text()
    restore_body = _shell_function_body(text, "restore_previous_install_tree")
    backup_body = _shell_function_body(text, "backup_managed_install_tree")
    cleanup_body = _shell_function_body(text, "cleanup")

    # The backup lives on /data (a power loss empties /tmp) and is synced before anything is removed.
    assert 'BACKUP_ROOT="${COMMAVIEWD_BACKUP_ROOT:-/data/commaview-backups}"' in text
    assert 'INSTALL_BACKUP_ROOT="${COMMAVIEWD_INSTALL_BACKUP_ROOT:-$BACKUP_ROOT/install-previous}"' in text
    assert 'mktemp -d "$INSTALL_BACKUP_ROOT/' in backup_body
    assert "sync" in backup_body
    assert 'if [ "$PRESERVE_TMPDIR" = "1" ]' in cleanup_body

    assert 'cp -a "$backup_dir"/. "$INSTALL_DIR"/ 2>/dev/null || true' not in restore_body
    assert not re.search(r'cp -a "\$backup_dir"/\. "\$INSTALL_DIR"/[^\n]*\|\|\s*true', restore_body)
    assert 'if cp -a "$backup_dir"/. "$INSTALL_DIR"/; then' in restore_body
    assert 'return "$restore_ec"' in restore_body
    assert "the backup stays there for recovery" in restore_body

    restore_copy = restore_body.index('if cp -a "$backup_dir"/. "$INSTALL_DIR"/; then')
    restore_failure_return = restore_body.index('return "$restore_ec"')
    runtime_restart = restore_body.index('COMMAVIEWD_RESTART_REASON=install-rollback')
    assert restore_copy < restore_failure_return < runtime_restart


def test_install_cleanup_reverts_transformer_before_restoring_previous_install(tmp_path):
    text = INSTALL_SH.read_text()
    cleanup_definition = _shell_function_definition(text, "cleanup")
    revert_definition = _shell_function_definition(text, "revert_onroad_ui_export_after_failed_install")
    install_dir = tmp_path / "commaview"
    scripts_dir = install_dir / "scripts"
    scripts_dir.mkdir(parents=True)
    op_file = tmp_path / "openpilot" / "selfdrive" / "ui" / "ui_state.py"
    op_file.parent.mkdir(parents=True)
    op_file.write_text("patched by failed install\n")
    order_file = tmp_path / "order.txt"
    revert_helper = scripts_dir / "revert_onroad_ui_export_patch.sh"
    revert_helper.write_text(
        "#!/usr/bin/env bash\n"
        f"printf 'revert\\n' >> {shlex.quote(str(order_file))}\n"
        f"printf 'original upstream ui\\n' > {shlex.quote(str(op_file))}\n"
    )
    revert_helper.chmod(0o755)
    script = f"""
set -euo pipefail
INSTALL_DIR={shlex.quote(str(install_dir))}
tmpdir={shlex.quote(str(tmp_path / "tmp"))}
PRESERVE_TMPDIR=0
ONROAD_UI_EXPORT_APPLIED=1
INSTALL_SUCCESS=0
INSTALL_MUTATED=1
FORCE_OFFROAD=0
restore_previous_install_tree() {{ printf 'restore-install\\n' >> {shlex.quote(str(order_file))}; }}
{revert_definition}
{cleanup_definition}
cleanup
"""

    result = subprocess.run(
        ["bash", "-c", script],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )

    assert result.returncode == 0, result.stdout + result.stderr
    assert op_file.read_text() == "original upstream ui\n"
    assert order_file.read_text().splitlines() == ["revert", "restore-install"]


def test_install_backup_and_rollback_put_the_previous_release_back_whole(tmp_path):
    text = INSTALL_SH.read_text()
    functions = "\n".join(_shell_function_definition(text, name) for name in (
        "low_priority", "without_maint_lock_fd", "backup_managed_install_tree", "clean_managed_install_tree",
        "restore_previous_install_tree", "prune_install_backups"))
    install_dir = tmp_path / "commaview"
    for rel, body in {"commaviewd": "old binary", "install.sh": "old installer", "start.sh": "exit 0",
                      "vendor/orjson.py": "old vendor", "scripts/apply.sh": "old apply", "run/car-identity.json": "{}",
                      "run/bridge.pid": "123"}.items():
        (install_dir / rel).parent.mkdir(parents=True, exist_ok=True)
        (install_dir / rel).write_text(body)
    backups = tmp_path / "backups"
    for old in ("20200101-000000.aaaaaa", "20200102-000000.bbbbbb", "20200103-000000.cccccc"):
        (backups / old).mkdir(parents=True)
    script = f"""
set -euo pipefail
INSTALL_DIR={shlex.quote(str(install_dir))}
INSTALL_BACKUP_ROOT={shlex.quote(str(backups))}
INSTALL_BACKUP_KEEP=2
INSTALL_BACKUP_DIR=""
INSTALL_MUTATED=0
INSTALL_SUCCESS=0
INSTALL_ROLLED_BACK=0
{functions}
backup_managed_install_tree
clean_managed_install_tree
printf 'new binary' > "$INSTALL_DIR/commaviewd"
printf 'new installer' > "$INSTALL_DIR/install.sh"
mkdir -p "$INSTALL_DIR/vendor" && printf 'new vendor' > "$INSTALL_DIR/vendor/new.py"
restore_previous_install_tree
echo "rolled back=$INSTALL_ROLLED_BACK"
prune_install_backups
"""
    result = subprocess.run(["bash", "-c", script], text=True, capture_output=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "rolled back=1" in result.stdout
    assert (install_dir / "commaviewd").read_text() == "old binary"
    assert (install_dir / "install.sh").read_text() == "old installer"
    assert (install_dir / "vendor" / "orjson.py").read_text() == "old vendor"
    assert not (install_dir / "vendor" / "new.py").exists(), "vendor is put back, not merged"
    # run/ keeps its state through an install; only the stopped runtime's pid files go.
    assert (install_dir / "run" / "car-identity.json").read_text() == "{}"
    assert not (install_dir / "run" / "bridge.pid").exists()
    kept = sorted(path.name for path in backups.iterdir())
    assert len(kept) == 2 and "20200103-000000.cccccc" in kept, kept


def test_install_script_preserves_patch_flavor_state_and_supports_explicit_current_reinstall():
    text = INSTALL_SH.read_text()
    assert "clean_managed_install_tree" in text
    assert '"$INSTALL_DIR/run"' in text
    assert '"$INSTALL_DIR/config/onroad-ui-export-patch.env"' not in text
    assert '"$INSTALL_DIR/config/hud-lite-patch.env"' in text
    assert "--current" in text
    assert "USE_CURRENT_RELEASE" in text
    assert 'RELEASE_TAG="$(resolve_fallback_release_tag)"' in text
    assert 'RELEASE_TAG="v0.0.1-alpha"' not in text
    assert "--force-offroad" in text
    # Never asks openpilot to go offroad: an install asked for while driving is queued instead.
    assert 'write_param' not in text
    assert 'queue_install_until_offroad' in text
    assert 'bash "$runner" queue install --file "$COMPANION_DIR/install.sh"' in text
    assert '"scripts/run_when_offroad.sh"' in text
    assert 'ensure_offroad_ready' in text
    assert 'apply_onroad_ui_export_patch.sh" --force-repair' in text


def test_install_script_prints_one_time_pair_code_after_starting_runtime():
    text = INSTALL_SH.read_text()
    assert 'print_pairing_code()' in text
    assert 'http://127.0.0.1:5002/pairing/create' in text
    assert 'X-CommaView-Token: $token' in text
    assert 'CommaView pair code:' in text
    assert text.index('bash "$INSTALL_DIR/start.sh"') < text.rindex('print_pairing_code')


def test_sunnypilot_patch_declares_every_managed_target_for_force_repair():
    text = SUNNYPILOT_PATCH.read_text()
    assert "+++ b/selfdrive/ui/commaview_export.py" in text
    assert "+++ b/selfdrive/ui/ui_state.py" in text
    assert "+++ b/selfdrive/ui/mici/onroad/augmented_road_view.py" in text
    assert text.index("+++ b/selfdrive/ui/mici/onroad/augmented_road_view.py") < text.index("self._update_commaview_camera_export()")
    assert "exporter.set_onroad_projection(" in text


def test_verify_patch_script_requires_onroad_projection_markers():
    text = VERIFY_PATCH_SH.read_text()
    assert "onroad_projection_present=false" in text
    assert '"onroadProjectionPresent"' in text
    assert "exporter.set_onroad_projection(" in text
    assert "self._latest_onroad_projection = {" in text
    assert '"modelTransform": _matrix3_list(model_transform)' in text
    assert "video_frame_matrix=self._cached_matrix" in text
    assert "$onroad_projection_present" in text


def assert_no_index_writing_git(text: str) -> None:
    """git checkout / reset / add / stash / update-index rewrite openpilot's .git/index, which makes
    launch_chffrplus.sh take the tree for modified and mishandle a staged openpilot update."""
    for line in text.splitlines():
        if line.lstrip().startswith("#"):
            continue
        assert not re.search(r"\bgit\b[^|;&]*\b(checkout|reset|add|stash|update-index|restore|clean)\b", line), line


def test_apply_patch_script_refuses_implicit_destructive_repair_and_backs_up_force_repair():
    text = APPLY_PATCH_SH.read_text()
    restore_body = _shell_function_body(text, "restore_managed_targets_from_backup")
    reset_body = _shell_function_body(text, "reset_managed_targets")
    assert "managed_targets()" in text
    assert "backup_managed_targets()" in text
    assert "dirty_managed_targets()" in text
    assert "reset_managed_targets()" in text
    assert "restore_managed_targets_from_backup()" in text
    assert "force_repair_managed_targets()" in text
    assert '--maintenance-mode' not in text
    assert 'COMMAVIEWD_MAINTENANCE_MODE' not in text
    assert '--force-repair' in text
    assert 'if [ "$FORCE_REPAIR" != "1" ] && [ -x "$VERIFY_SCRIPT" ]' in text
    assert 'onroad UI export transformer target files have local changes' in text
    assert 'refusing to modify dirty upstream files without --force-repair' in text
    assert 'mkdir -p "$backup_parent" || return $?' in text
    assert 'backup_root="$(mktemp -d "$backup_parent/$(date -u +%Y%m%d-%H%M%S).XXXXXX")" || return $?' in text
    assert 'cp -a "$OP_ROOT/$rel" "$backup_root/$rel" || return $?' in text
    assert 'failed to back up managed onroad UI export transformer targets; refusing force repair' in text
    assert 'failed to reset managed onroad UI export transformer targets; restored managed targets from $backup_root' in text
    assert 'failed to reset managed onroad UI export transformer targets and rollback failed or incomplete from $backup_root' in text
    assert 'failed to back up managed onroad UI export transformer targets before transform' in text
    assert 'local reset_ec=0' in reset_body
    assert 'return "$reset_ec"' in reset_body
    assert 'restore_target_from_head "$rel" || reset_ec=1' in reset_body
    assert 'reset_managed_targets || reset_ec=$?' in text
    assert 'reset of managed targets had errors before rollback restore' in text
    assert 'while IFS= read -r rel; do' in restore_body
    assert 'mkdir -p "$OP_ROOT/$(dirname "$rel")"' in restore_body
    assert 'atomic_copy "$backup_root/$rel" "$OP_ROOT/$rel"' in restore_body
    assert 'rm -f "$OP_ROOT/$rel"' in restore_body
    assert 'rollback_managed_targets_clean()' in text
    assert 'managed rollback incomplete; dirty targets remain:' in text
    assert 'rollback_managed_targets_clean || rollback_clean_ec=$?' in text
    assert 'if [ "$restore_ec" -eq 0 ] && [ "$rollback_clean_ec" -eq 0 ]; then' in text
    assert text.index('reset_managed_targets || reset_ec=$?') < text.index('restore_managed_targets_from_backup "$transform_backup_root"')
    assert text.index('restore_managed_targets_from_backup "$transform_backup_root"') < text.index('rollback_managed_targets_clean || rollback_clean_ec=$?')
    assert 'backups written to $backup_root' in text
    assert 'transformer failed; restored managed targets' in text
    assert 'transformer failed and rollback failed or incomplete' in text
    assert 'cp -a "$backup_root"/. "$OP_ROOT"/ 2>/dev/null || true' not in text
    assert 'remote_flavor()' in text
    assert 'github.com:commaai/openpilot' in text
    assert 'github.com:sunnypilot/sunnypilot' in text
    assert 'github.com:sunnypilot/openpilot' in text
    assert 'selfdrive/ui/mici' in text
    assert_no_index_writing_git(text)
    assert '--force-offroad' in text
    assert 'write_param' not in text


def test_transformer_state_files_are_parsed_without_sourcing_shell():
    apply_text = APPLY_PATCH_SH.read_text()
    verify_text = VERIFY_PATCH_SH.read_text()
    assert '. "$STATE_ENV"' not in apply_text
    assert '. "$STATE_ENV"' not in verify_text
    assert 'source "$STATE_ENV"' not in apply_text
    assert 'source "$STATE_ENV"' not in verify_text
    assert 'state_value()' in apply_text
    assert 'state_value()' in verify_text
    assert 'unsupported upstream remote' in apply_text
    assert 'unsupported upstream remote' in verify_text
    assert 'json.dumps(payload' in verify_text


def test_uninstall_script_actual_revert_runs_before_uninstall_mutations():
    text = UNINSTALL_SH.read_text()
    revert_index = text.index("Reverting direct v2 onroad UI export transformer")
    stop_index = text.index("Stopping services")
    boot_hook_index = text.index("Removing boot hook")

    assert "uninstall aborted before stopping services or removing boot hook" in text
    assert revert_index < stop_index
    assert revert_index < boot_hook_index
    assert text.count('bash "$revert_helper" "${revert_args[@]}"') == 1


def test_uninstall_script_aborts_when_revert_helper_is_missing(tmp_path):
    install_dir = tmp_path / "commaview"
    install_dir.mkdir()
    stop_marker = install_dir / "stop-called"
    (install_dir / "stop.sh").write_text(
        "#!/usr/bin/env bash\n"
        f"printf stopped > {shlex.quote(str(stop_marker))}\n"
    )
    (install_dir / "stop.sh").chmod(0o755)

    result = subprocess.run(
        ["bash", str(UNINSTALL_SH)],
        env={"COMMAVIEWD_INSTALL_DIR": str(install_dir), "PATH": "/usr/bin:/bin"},
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )

    assert result.returncode != 0
    assert install_dir.exists()
    assert not stop_marker.exists()
    assert "Stopping services" not in result.stdout
    assert "Removing boot hook" not in result.stdout
    assert "Removing files" not in result.stdout
    assert "revert helper missing" in result.stderr
    assert "preserving" in result.stderr


def test_uninstall_script_aborts_before_mutation_when_actual_revert_fails_after_preflight_would_pass(tmp_path):
    install_dir = tmp_path / "commaview"
    scripts_dir = install_dir / "scripts"
    scripts_dir.mkdir(parents=True)
    revert_helper = scripts_dir / "revert_onroad_ui_export_patch.sh"
    stop_marker = install_dir / "stop-called"
    revert_helper.write_text(
        "#!/usr/bin/env bash\n"
        "printf '%s\\n' \"$*\" >> \"$COMMAVIEWD_INSTALL_DIR/revert-calls.txt\"\n"
        "for arg in \"$@\"; do\n"
        "  if [ \"$arg\" = --preflight-only ]; then\n"
        "    exit 0\n"
        "  fi\n"
        "done\n"
        "exit 42\n"
    )
    revert_helper.chmod(0o755)
    (install_dir / "stop.sh").write_text(
        "#!/usr/bin/env bash\n"
        f"printf stopped > {shlex.quote(str(stop_marker))}\n"
    )

    result = subprocess.run(
        ["bash", str(UNINSTALL_SH)],
        env={"COMMAVIEWD_INSTALL_DIR": str(install_dir), "PATH": "/usr/bin:/bin"},
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )

    assert result.returncode == 42
    assert not stop_marker.exists()
    assert "Stopping services" not in result.stdout
    assert "Removing boot hook" not in result.stdout
    assert "Removing files" not in result.stdout
    assert "uninstall aborted before stopping services or removing boot hook" in result.stderr


def test_uninstall_script_reverts_transformer_before_removing_install_tree():
    text = UNINSTALL_SH.read_text()
    assert "revert_onroad_ui_export_patch.sh" in text
    assert "Reverting direct v2 onroad UI export transformer" in text
    assert 'bash "$INSTALL_DIR/stop.sh"' in text
    assert 'rm -rf "$INSTALL_DIR"' in text
    assert '--force-offroad' in text
    assert 'revert_args+=(--force-offroad)' in text
    assert 'preserving $INSTALL_DIR for recovery' in text
    assert text.index("Reverting direct v2 onroad UI export transformer") < text.index('bash "$INSTALL_DIR/stop.sh"')
    assert text.rindex("revert_onroad_ui_export_patch.sh") < text.index('rm -rf "$INSTALL_DIR"')


def test_revert_patch_script_resets_every_transformer_managed_target():
    text = REVERT_PATCH_SH.read_text()
    assert "managed_targets()" in text
    assert "backup_managed_targets()" in text
    assert "reset_managed_targets()" in text
    assert "note_openpilot_ui_keeps_loaded_code" in text
    assert "pkill" not in text
    assert "COMMAVIEWD_BACKUP_ROOT:-/data/commaview-backups" in text
    assert "ensure_offroad_ready" in text
    assert "socket UI export transformer revert blocked while driving" in text
    assert "--force-offroad" in text
    assert "selfdrive/ui/commaview_export.py" in text
    assert "selfdrive/ui/ui_state.py" in text
    assert "selfdrive/ui/mici/onroad/augmented_road_view.py" in text
    assert "selfdrive/ui/onroad/augmented_road_view.py" in text
    assert_no_index_writing_git(text)
    assert 'git -C "$OP_ROOT" cat-file -p "HEAD:./$rel"' in text
    assert 'rm -f "$dst"' in text


def test_apply_patch_script_records_ui_reload_instead_of_restarting_openpilot_ui():
    text = APPLY_PATCH_SH.read_text()
    # openpilot's manager never restarts a UI that exits (sunnypilot release restarts
    # it from preimported modules), so apply never signals the UI.
    assert "pkill" not in text
    assert "pgrep" not in text
    assert "note_openpilot_ui_reload_needed" in text
    assert "COMMAVIEWD_SKIP_OPENPILOT_UI_RESTART" in text
    assert 'read_param IsOnroad' in text
    assert 'onroad-ui-export-ui-restart-needed' in text
    assert "printf 'pending\\nbootId=%s\\npatchedAtUptimeCs=%s\\n'" in text
    assert "--before-openpilot) BEFORE_OPENPILOT=1" in text
    assert "openpilot_root_after_launch" in text
    assert "export GIT_OPTIONAL_LOCKS=0" in text
    assert 'if [ "$FORCE_REPAIR" != "1" ] && [ -x "$VERIFY_SCRIPT" ] && "$VERIFY_SCRIPT" --json --platform "$ui_platform" >/dev/null 2>&1; then\n  # Already patched: no UI file changes, so nothing new for openpilot to reload.\n  exit 0' in text
    assert '"$VERIFY_SCRIPT" --json --platform "$ui_platform" || verify_ec=$?' in text
    assert 'verification failed; restored managed targets' in text
    assert 'managed_targets_match_backup "$transform_backup_root"' in text
    # The reload need is recorded before the post-transform verify prints status.
    assert text.index('if [ "$(managed_targets_digest)" != "$targets_before" ]; then\n  note_openpilot_ui_reload_needed') < text.index('"$VERIFY_SCRIPT" --json --platform "$ui_platform" || verify_ec=$?')


def test_control_mode_routes_present_for_runtime_debug_config():
    text = CONTROL_CPP.read_text()
    assert "/commaview/runtime-debug/config" in text
    assert "/commaview/runtime-debug/defaults" in text
    assert "/commaview/runtime-debug/apply" in text
    assert "runtime_status_json" in text
    assert "runtime_debug_state_json" in text
    assert "runtime_debug_apply_response" in text
    assert "runtimeVersion" in text
    assert "roadState" in text
    assert "isOnroad" in text
    assert "onroad" in text
    assert "maintenance_mode_requested" not in text
    assert "--maintenance-mode" not in text
    assert "live_onroad_ui_export_status_json(false)" in text
    assert "run_onroad_ui_export_apply_status_json" in text
    assert 'json_field_true(request_body, "forceOffroad")' in text


# Standalone device scripts cannot source a shared helper: install.sh runs piped from curl,
# and control mode/start.sh exec the patch scripts individually. These helpers are therefore
# duplicated on purpose; this guard keeps the copies from drifting apart.
DUPLICATED_SHELL_HELPERS = {
    "commaview_pids": (INSTALL_SH, STOP_SH),
    "read_is_onroad": (INSTALL_SH, UNINSTALL_SH, APPLY_PATCH_SH, REVERT_PATCH_SH),
    "read_param": (APPLY_PATCH_SH, REVERT_PATCH_SH),
    "openpilot_process_kind": (APPLY_PATCH_SH, VERIFY_PATCH_SH),
    "current_boot_id": (APPLY_PATCH_SH, VERIFY_PATCH_SH),
    "managed_targets": (APPLY_PATCH_SH, REVERT_PATCH_SH),
    "reset_managed_targets": (APPLY_PATCH_SH, REVERT_PATCH_SH),
    "restore_target_from_head": (APPLY_PATCH_SH, REVERT_PATCH_SH),
    "atomic_copy": (APPLY_PATCH_SH, REVERT_PATCH_SH),
    "take_maint_lock": (APPLY_PATCH_SH, REVERT_PATCH_SH),
    "normalize_ui_platform": (APPLY_PATCH_SH, VERIFY_PATCH_SH),
    "read_device_model": (APPLY_PATCH_SH, VERIFY_PATCH_SH),
    "remote_flavor": (APPLY_PATCH_SH, VERIFY_PATCH_SH),
    "state_value": (APPLY_PATCH_SH, VERIFY_PATCH_SH),
    "upstream_head": (APPLY_PATCH_SH, VERIFY_PATCH_SH),
    "upstream_remote": (APPLY_PATCH_SH, VERIFY_PATCH_SH),
}


def test_duplicated_standalone_script_helpers_stay_in_sync():
    for name, paths in DUPLICATED_SHELL_HELPERS.items():
        bodies = {path.name: _shell_function_body(path.read_text(), name) for path in paths}
        assert len(set(bodies.values())) == 1, f"{name} drifted between {sorted(bodies)}"
