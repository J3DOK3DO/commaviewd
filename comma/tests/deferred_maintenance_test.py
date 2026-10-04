"""run_when_offroad.sh: maintenance asked for while driving waits for the car to be parked or
openpilot offroad, and never asks openpilot to go offroad itself."""

import json
import os
import pathlib
import subprocess
import time

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
RUNNER = REPO_ROOT / "comma" / "scripts" / "run_when_offroad.sh"


def env_for(tmp_path):
    env = dict(os.environ)
    env.update(
        COMMAVIEWD_DEFERRED_DIR=str(tmp_path / "deferred"),
        COMMAVIEWD_PARAMS_DIR=str(tmp_path / "params"),
        COMMAVIEWD_DEFERRED_POLL_SEC="1",
        COMMAVIEWD_DEFERRED_OFFROAD_STABLE_SEC="2",
        COMMAVIEWD_DEFERRED_REQUIRE_MANAGER="0",
        COMMAVIEWD_DEFERRED_PARKED_STABLE_SEC="2",
        # No road-phase: onroad is driving, as on a runtime that predates it.
        COMMAVIEWD_ROAD_PHASE_BIN=str(tmp_path / "no-commaviewd"),
    )
    (tmp_path / "params").mkdir(exist_ok=True)
    return env


def run(env, *args, check=True):
    return subprocess.run(["bash", str(RUNNER), *args], env=env, capture_output=True, text=True, check=check, timeout=30)


def status(env):
    return json.loads(run(env, "status").stdout)


def wait_for(predicate, timeout=20.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(0.2)
    return False


def set_offroad(tmp_path, value):
    (tmp_path / "params" / "IsOffroad").write_text(value)


def test_job_waits_for_offroad_then_runs_once(tmp_path):
    env = env_for(tmp_path)
    set_offroad(tmp_path, "0")
    marker = tmp_path / "ran"
    script = tmp_path / "job.sh"
    script.write_text(f'#!/usr/bin/env bash\necho "$COMMAVIEWD_DEFERRED_JOB $1" >> "{marker}"\n')
    try:
        queued = json.loads(run(env, "queue", "install", "--file", str(script), "--", "bash", "@JOB@/job.sh", "v9").stdout)
        assert queued["state"] == "waiting" and queued["action"] == "install" and queued["waitsFor"] == "parked"
        time.sleep(3.5)
        assert not marker.exists(), "ran while onroad"
        assert status(env)["state"] == "waiting"
        # The queued copy runs, not the original (an install replaces the scripts under it).
        script.write_text("#!/usr/bin/env bash\nexit 9\n")
        set_offroad(tmp_path, "1")
        assert wait_for(lambda: status(env)["state"] == "done")
        assert marker.read_text() == "1 v9\n"
        final = status(env)
        assert final["exitStatus"] == 0 and final["attempts"] == 1
        assert not (tmp_path / "deferred" / "job").exists()
        # Never asked openpilot to go offroad.
        assert not (tmp_path / "params" / "OffroadMode").exists()
    finally:
        run(env, "cancel", check=False)


def fake_road_phase(tmp_path, env):
    """A commaviewd stand-in whose road-phase answer is the contents of tmp_path/phase."""
    phase_file = tmp_path / "phase"
    phase_file.write_text("driving not-in-park")
    fake = tmp_path / "commaviewd"
    fake.write_text(
        "#!/usr/bin/env bash\n"
        '[ "$1" = road-phase ] || exit 2\n'
        f'read -r phase reason < "{phase_file}"\n'
        'echo "$phase $reason"\n'
        '[ "$phase" != driving ]\n'
    )
    fake.chmod(0o755)
    env["COMMAVIEWD_ROAD_PHASE_BIN"] = str(fake)
    return phase_file


def test_job_runs_while_parked_onroad(tmp_path):
    env = env_for(tmp_path)
    set_offroad(tmp_path, "0")
    phase = fake_road_phase(tmp_path, env)
    marker = tmp_path / "ran"
    try:
        run(env, "queue", "install", "--", "touch", str(marker))
        time.sleep(3.5)
        assert not marker.exists(), "ran while driving"
        phase.write_text("parked parked")
        assert wait_for(lambda: status(env)["state"] == "done")
        assert marker.exists()
        assert not (tmp_path / "params" / "OffroadMode").exists()
    finally:
        run(env, "cancel", check=False)


def test_parked_blink_is_not_enough(tmp_path):
    env = env_for(tmp_path)
    env["COMMAVIEWD_DEFERRED_PARKED_STABLE_SEC"] = "4"
    set_offroad(tmp_path, "0")
    phase = fake_road_phase(tmp_path, env)
    marker = tmp_path / "ran"
    try:
        run(env, "queue", "repair", "--", "touch", str(marker))
        phase.write_text("parked parked")
        time.sleep(1.5)
        phase.write_text("driving engaged")
        time.sleep(2.5)
        assert not marker.exists()
        phase.write_text("parked parked")
        assert wait_for(lambda: marker.exists())
    finally:
        run(env, "cancel", check=False)


def test_blink_offroad_is_not_enough(tmp_path):
    env = env_for(tmp_path)
    env["COMMAVIEWD_DEFERRED_OFFROAD_STABLE_SEC"] = "4"
    marker = tmp_path / "ran"
    set_offroad(tmp_path, "1")
    try:
        run(env, "queue", "repair", "--", "touch", str(marker))
        time.sleep(1.5)
        set_offroad(tmp_path, "0")
        time.sleep(2.5)
        assert not marker.exists()
        set_offroad(tmp_path, "1")
        assert wait_for(lambda: marker.exists())
    finally:
        run(env, "cancel", check=False)


def test_unknown_road_state_never_runs(tmp_path):
    env = env_for(tmp_path)
    marker = tmp_path / "ran"
    try:
        run(env, "queue", "repair", "--", "touch", str(marker))
        time.sleep(3.5)
        assert not marker.exists(), "a missing IsOffroad must not count as offroad"
    finally:
        run(env, "cancel", check=False)


def test_requeue_replaces_and_cancel_drops(tmp_path):
    env = env_for(tmp_path)
    set_offroad(tmp_path, "0")
    first, second = tmp_path / "first", tmp_path / "second"
    run(env, "queue", "install", "--", "touch", str(first))
    run(env, "queue", "uninstall", "--", "touch", str(second))
    assert status(env)["action"] == "uninstall"
    run(env, "cancel")
    assert status(env)["state"] == "cancelled"
    set_offroad(tmp_path, "1")
    time.sleep(3.5)
    assert not first.exists() and not second.exists()


def test_onroad_again_at_start_goes_back_to_waiting(tmp_path):
    env = env_for(tmp_path)
    counter = tmp_path / "count"
    script = tmp_path / "job.sh"
    # Exits 42 (blocked while driving) the first time, succeeds the second.
    script.write_text(f'#!/usr/bin/env bash\necho x >> "{counter}"\n[ "$(wc -l < "{counter}")" -ge 2 ] || exit 42\n')
    set_offroad(tmp_path, "1")
    try:
        run(env, "queue", "install", "--file", str(script), "--", "bash", "@JOB@/job.sh")
        assert wait_for(lambda: status(env)["state"] == "done", timeout=30)
        assert status(env)["attempts"] == 2
    finally:
        run(env, "cancel", check=False)


def test_resume_restarts_the_waiter_after_a_reboot(tmp_path):
    env = env_for(tmp_path)
    set_offroad(tmp_path, "0")
    marker = tmp_path / "ran"
    run(env, "queue", "install", "--", "touch", str(marker))
    pid = int((tmp_path / "deferred" / "waiter.pid").read_text())
    os.kill(pid, 9)  # what a reboot does to it
    time.sleep(0.3)
    try:
        run(env, "resume")
        set_offroad(tmp_path, "1")
        assert wait_for(lambda: marker.exists())
    finally:
        run(env, "cancel", check=False)


def test_scripts_never_force_offroad():
    for rel in ("install.sh", "uninstall.sh", "scripts/apply_onroad_ui_export_patch.sh",
                "scripts/revert_onroad_ui_export_patch.sh", "scripts/run_when_offroad.sh", "start.sh"):
        text = (REPO_ROOT / "comma" / rel).read_text()
        assert 'write_param "OffroadMode" "1"' not in text, rel


def shell_function(text, name):
    import re
    match = re.search(r"^" + re.escape(name) + r"\(\) \{\n.*?^\}\n", text, re.S | re.M)
    assert match, name
    return match.group(0)


def test_install_while_onroad_refuses_or_queues_never_forces(tmp_path):
    env = env_for(tmp_path)
    set_offroad(tmp_path, "0")
    companions = tmp_path / "companions"
    (companions / "scripts").mkdir(parents=True)
    (companions / "install.sh").write_text("#!/usr/bin/env bash\necho installing \"$@\"\n")
    (companions / "scripts" / "run_when_offroad.sh").write_text(RUNNER.read_text())
    text = (REPO_ROOT / "comma" / "install.sh").read_text()
    functions = "".join(shell_function(text, name) for name in (
        "read_is_onroad", "road_phase_bin", "read_is_driving", "keep_road_phase_kit", "queue_install_until_offroad",
        "cancel_deferred_maintenance", "ensure_offroad_ready"))
    # The check after the download: a release staged (this stand-in has no road-phase of its own).
    (tmp_path / "staged").mkdir()

    def ensure(force):
        script = f"""
set -euo pipefail
PARAMS_DIR={tmp_path / 'params'}
read_param() {{ [ -f "$PARAMS_DIR/$1" ] && tr -d '\\000\\r\\n' < "$PARAMS_DIR/$1" || true; }}
COMPANION_DIR={companions}
DEFERRED_DIR={tmp_path / 'deferred'}
DEFERRED_EXIT=75
RELEASE_TAG=v9.9.9
ROAD_PHASE_BIN="$COMMAVIEWD_ROAD_PHASE_BIN"
ROAD_PHASE_KIT={tmp_path / 'deferred' / 'road-phase'}
STAGED_BUNDLE={tmp_path / 'staged'}
FORCE_OFFROAD={1 if force else 0}
{functions}
ensure_offroad_ready
echo proceeding
"""
        return subprocess.run(["bash", "-c", script], env=env, capture_output=True, text=True, timeout=30)

    try:
        refused = ensure(False)
        assert refused.returncode == 42, refused.stdout + refused.stderr
        assert "proceeding" not in refused.stdout
        queued = ensure(True)
        assert queued.returncode == 75, queued.stdout + queued.stderr
        assert "COMMAVIEW_MAINTENANCE_DEFERRED=install" in queued.stdout
        assert status(env)["state"] == "waiting" and status(env)["action"] == "install"
        args = (tmp_path / "deferred" / "job" / "args").read_bytes().split(b"\0")
        assert args[:4] == [b"bash", str(tmp_path / "deferred" / "job" / "install.sh").encode(), b"--tag", b"v9.9.9"]
        assert not (tmp_path / "params" / "OffroadMode").exists()
        # Run directly once parked (still onroad), the install drops the queued one.
        phase = fake_road_phase(tmp_path, env)
        phase.write_text("parked parked")
        direct = ensure(True)
        assert direct.returncode == 0 and "proceeding" in direct.stdout, direct.stdout + direct.stderr
        assert status(env)["state"] == "cancelled"
        # Engaged in Park is driving.
        phase.write_text("driving engaged")
        assert ensure(False).returncode == 42
        set_offroad(tmp_path, "1")
        assert ensure(False).returncode == 0
    finally:
        run(env, "cancel", check=False)


def test_uninstall_while_onroad_refuses_or_queues(tmp_path):
    env = env_for(tmp_path)
    set_offroad(tmp_path, "0")
    install_dir = tmp_path / "commaview"
    (install_dir / "scripts").mkdir(parents=True)
    (install_dir / "scripts" / "run_when_offroad.sh").write_text(RUNNER.read_text())
    (install_dir / "uninstall.sh").write_text((REPO_ROOT / "comma" / "uninstall.sh").read_text())
    stop_marker = install_dir / "stopped"
    (install_dir / "stop.sh").write_text(f"#!/usr/bin/env bash\ntouch {stop_marker}\n")
    env["COMMAVIEWD_INSTALL_DIR"] = str(install_dir)
    try:
        refused = subprocess.run(["bash", str(install_dir / "uninstall.sh")], env=env, capture_output=True, text=True, timeout=30)
        assert refused.returncode == 42 and "blocked while driving" in refused.stderr
        queued = subprocess.run(["bash", str(install_dir / "uninstall.sh"), "--force-offroad"], env=env,
                                capture_output=True, text=True, timeout=30)
        assert queued.returncode == 75, queued.stdout + queued.stderr
        assert "COMMAVIEW_MAINTENANCE_DEFERRED=uninstall" in queued.stdout
        assert status(env)["action"] == "uninstall"
        assert install_dir.exists() and not stop_marker.exists()
        assert not (tmp_path / "params" / "OffroadMode").exists()
    finally:
        run(env, "cancel", check=False)


def test_a_direct_run_clears_a_finished_jobs_status(tmp_path):
    # The app shows a failed or finished job until something replaces it; a direct install,
    # uninstall or repair calls cancel, which must not leave the old job's status behind.
    env = env_for(tmp_path)
    set_offroad(tmp_path, "0")
    run(env, "queue", "install", "--", "false")
    set_offroad(tmp_path, "1")
    try:
        assert wait_for(lambda: status(env)["state"] == "failed"), status(env)
    finally:
        run(env, "cancel", check=False)
    assert status(env) == {"state": "none"}


def test_installer_hands_over_to_the_release_it_installs(tmp_path):
    # The comma's own update queues the installed install.sh with --tag; it runs that release's
    # installer (fetched with its companions) instead of its own logic, once.
    text = (REPO_ROOT / "comma" / "install.sh").read_text()
    handoff = shell_function(text, "handoff_to_release_installer")
    companions = tmp_path / "companions"
    companions.mkdir()
    (companions / "install.sh").write_text('echo "next installer $* handoff=$COMMAVIEWD_INSTALLER_HANDOFF"\n')
    old = tmp_path / "old-install.sh"
    old.write_text(f"""
set -euo pipefail
COMPANION_DIR={companions}
RELEASE_TAG=v9.9.9
FORCE_OFFROAD=1
tmpdir={tmp_path / 'tmp'}
mkdir -p "$tmpdir"
trap 'echo cleanup ran' EXIT
{handoff}
handoff_to_release_installer
echo "old installer kept going"
""")
    result = subprocess.run(["bash", str(old)], capture_output=True, text=True, timeout=30,
                            env={k: v for k, v in os.environ.items() if not k.startswith("COMMAVIEWD_INSTALLER")})
    assert result.returncode == 0, result.stderr
    assert "next installer --tag v9.9.9 --force-offroad handoff=1" in result.stdout
    assert "old installer kept going" not in result.stdout
    assert "cleanup ran" not in result.stdout and not (tmp_path / "tmp").exists()
    # The same installer (or one already handed over to) carries on itself.
    (companions / "install.sh").write_text(old.read_text())
    same = subprocess.run(["bash", str(old)], capture_output=True, text=True, timeout=30)
    assert "old installer kept going" in same.stdout


# ---- one maintenance at a time, and the stage a queued install has reached ----

def hold_lock(path):
    """Holds a flock on path (as a direct install would) until the returned process is killed."""
    path.parent.mkdir(parents=True, exist_ok=True)
    holder = subprocess.Popen(["flock", "-o", str(path), "sleep", "60"])
    assert wait_for(lambda: path.exists())
    time.sleep(0.2)
    return holder


def test_job_waits_while_another_maintenance_holds_the_lock(tmp_path):
    env = env_for(tmp_path)
    set_offroad(tmp_path, "1")
    marker = tmp_path / "ran"
    holder = hold_lock(tmp_path / "deferred" / "maint.lock")
    try:
        run(env, "queue", "install", "--", "touch", str(marker))
        time.sleep(4.5)
        assert not marker.exists(), "ran alongside another install"
        assert status(env)["state"] == "waiting"
        holder.kill()
        holder.wait()
        assert wait_for(lambda: status(env)["state"] == "done")
        assert marker.exists() and status(env)["attempts"] == 1
    finally:
        holder.kill()
        run(env, "cancel", check=False)


def test_job_is_told_it_holds_the_lock_and_inherits_no_lock_descriptor(tmp_path):
    env = env_for(tmp_path)
    set_offroad(tmp_path, "1")
    report = tmp_path / "report"
    script = tmp_path / "job.sh"
    script.write_text(
        "#!/usr/bin/env bash\n"
        f'echo "held=$COMMAVIEWD_MAINT_LOCK_HELD job=$COMMAVIEWD_DEFERRED_JOB" > "{report}"\n'
        f'ls -l /proc/$$/fd | grep -c "\\.lock" >> "{report}"\n'
        # Like a direct install would: the job doesn't wait on the lock its waiter holds.
        f'flock -n "{tmp_path / "deferred" / "maint.lock"}" true && echo free >> "{report}" || echo held >> "{report}"\n')
    try:
        run(env, "queue", "install", "--file", str(script), "--", "bash", "@JOB@/job.sh")
        assert wait_for(lambda: status(env)["state"] == "done")
        assert report.read_text().split() == ["held=1", "job=1", "0", "held"], report.read_text()
    finally:
        run(env, "cancel", check=False)


def test_two_waiters_never_run_the_job_twice(tmp_path):
    env = env_for(tmp_path)
    set_offroad(tmp_path, "0")
    counter = tmp_path / "count"
    script = tmp_path / "job.sh"
    script.write_text(f'#!/usr/bin/env bash\necho x >> "{counter}"\nsleep 2\n')
    try:
        run(env, "queue", "install", "--file", str(script), "--", "bash", "@JOB@/job.sh")
        # A second waiter, as two resumes racing (start.sh and a queue) could start.
        (tmp_path / "deferred" / "waiter.pid").write_text("1\n")
        run(env, "resume")
        time.sleep(1.5)
        set_offroad(tmp_path, "1")
        assert wait_for(lambda: status(env)["state"] == "done", timeout=30)
        time.sleep(3)
        assert counter.read_text() == "x\n"
    finally:
        run(env, "cancel", check=False)


def test_progress_is_cleared_when_queued_and_kept_when_the_job_ends(tmp_path):
    env = env_for(tmp_path)
    set_offroad(tmp_path, "1")
    progress = tmp_path / "deferred" / "progress.json"
    script = tmp_path / "job.sh"
    script.write_text(
        "#!/usr/bin/env bash\n"
        'printf \'{"stage":"rolled_back","detail":"checksum mismatch","atMs":5}\\n\' > "$COMMAVIEWD_DEFERRED_DIR/progress.json"\n'
        "exit 1\n")
    try:
        (tmp_path / "deferred").mkdir(exist_ok=True)
        progress.write_text('{"stage":"done","detail":"old","atMs":1}\n')
        queued = json.loads(run(env, "queue", "install", "--file", str(script), "--", "bash", "@JOB@/job.sh").stdout)
        assert queued["state"] == "waiting" and not progress.exists(), "a new job starts with no stage"
        assert wait_for(lambda: status(env)["state"] == "failed")
        assert json.loads(progress.read_text())["stage"] == "rolled_back", "the app reads why after the job"
        run(env, "cancel")
        assert not progress.exists()
    finally:
        run(env, "cancel", check=False)


def test_driven_again_at_start_clears_the_stage_and_waits(tmp_path):
    env = env_for(tmp_path)
    set_offroad(tmp_path, "1")
    progress = tmp_path / "deferred" / "progress.json"
    counter = tmp_path / "count"
    script = tmp_path / "job.sh"
    script.write_text(
        "#!/usr/bin/env bash\n"
        f'echo x >> "{counter}"\n'
        'printf \'{"stage":"downloading","detail":"","atMs":5}\\n\' > "$COMMAVIEWD_DEFERRED_DIR/progress.json"\n'
        f'[ "$(wc -l < "{counter}")" -ge 2 ] || {{ echo 0 > "{tmp_path / "params" / "IsOffroad"}"; exit 42; }}\n')
    try:
        run(env, "queue", "install", "--file", str(script), "--", "bash", "@JOB@/job.sh")
        assert wait_for(lambda: counter.exists() and status(env)["state"] == "waiting" and status(env).get("exitStatus") == 42)
        assert not progress.exists()
        set_offroad(tmp_path, "1")
        assert wait_for(lambda: status(env)["state"] == "done", timeout=30)
    finally:
        run(env, "cancel", check=False)


INSTALL_SH = REPO_ROOT / "comma" / "install.sh"


def test_install_reports_its_stages_in_order():
    text = INSTALL_SH.read_text()
    main = text[text.index('tmpdir="$(mktemp -d /tmp/commaview-install.XXXXXX)"'):]
    order = ["stage starting", "refresh_required_files install.sh", "handoff_to_release_installer",
             "\nrefresh_required_files\n", "take_maint_lock", "stage downloading", "stage verifying",
             "\nensure_offroad_ready\n\nstage installing", "backup_managed_install_tree",
             "ensure_commaview_stopped", "stage restarting", 'bash "$INSTALL_DIR/start.sh"', "INSTALL_SUCCESS=1",
             "stage done"]
    positions = [main.index(step) for step in order]
    assert positions == sorted(positions), list(zip(order, positions))
    # The onroad check runs again after the download, right before anything changes.
    assert main.count("ensure_offroad_ready") == 2


def install_functions(*names):
    text = INSTALL_SH.read_text()
    return "".join(shell_function(text, name) for name in names)


def run_install_flow(tmp_path, body, **env_extra):
    """install.sh's own stage/cleanup functions around a stand-in for its main flow."""
    deferred = tmp_path / "deferred"
    deferred.mkdir(exist_ok=True)
    functions = install_functions(
        "error", "now_ms", "stage", "read_param", "read_is_onroad", "road_phase_bin", "read_is_driving",
        "keep_road_phase_kit", "queue_install_until_offroad",
        "cancel_deferred_maintenance", "ensure_offroad_ready", "restore_previous_install_tree",
        "revert_onroad_ui_export_after_failed_install", "cleanup", "without_maint_lock_fd")
    install_dir = tmp_path / "commaview"
    install_dir.mkdir(exist_ok=True)
    script = f"""
set -euo pipefail
INSTALL_DIR={install_dir}
PARAMS_DIR={tmp_path / 'params'}
DEFERRED_DIR={deferred}
PROGRESS_FILE={deferred / 'progress.json'}
DEFERRED_EXIT=75
FORCE_OFFROAD=0
RELEASE_TAG=v9.9.9
COMPANION_DIR={tmp_path / 'companions'}
ROAD_PHASE_BIN={tmp_path / 'no-commaviewd'}
ROAD_PHASE_KIT={deferred / 'road-phase'}
STAGED_BUNDLE=""
tmpdir={tmp_path / 'tmp'}
mkdir -p "$tmpdir"
INSTALL_BACKUP_DIR=""
INSTALL_MUTATED=0
INSTALL_SUCCESS=0
INSTALL_ROLLED_BACK=0
PRESERVE_TMPDIR=0
ONROAD_UI_EXPORT_APPLIED=0
LAST_ERROR=""
LAST_FAILED_COMMAND=""
{functions}
trap cleanup EXIT
{body}
"""
    env = dict(os.environ, COMMAVIEWD_DEFERRED_JOB="1", **env_extra)
    return subprocess.run(["bash", "-c", script], env=env, capture_output=True, text=True, timeout=30)


def stage_lines(stdout):
    return [line.split(" ", 2)[1] for line in stdout.splitlines() if line.startswith("COMMAVIEW_STAGE ")]


def test_install_stage_report_on_success_and_on_rollback(tmp_path):
    (tmp_path / "params").mkdir()
    set_offroad(tmp_path, "1")
    progress = tmp_path / "deferred" / "progress.json"

    done = run_install_flow(tmp_path, "stage starting; stage downloading v9.9.9; stage verifying; stage installing;"
                                      " stage restarting; INSTALL_SUCCESS=1; stage done v9.9.9")
    assert done.returncode == 0, done.stderr
    assert stage_lines(done.stdout) == ["starting", "downloading", "verifying", "installing", "restarting", "done"]
    final = json.loads(progress.read_text())
    assert final["stage"] == "done" and final["detail"] == "v9.9.9" and final["atMs"] > 1_600_000_000_000

    # Fails after changing the live tree: the previous release is put back and the stage says so.
    install_dir = tmp_path / "commaview"
    backup = tmp_path / "backup"
    backup.mkdir()
    (backup / "commaviewd").write_text("old")
    rolled = run_install_flow(tmp_path, f"""
stage starting; stage installing
INSTALL_BACKUP_DIR={backup}
INSTALL_MUTATED=1
printf new > "$INSTALL_DIR/commaviewd"
error 'the v9.9.9 runtime did not answer on its control API; rolling back "now"'
exit 1
""")
    assert rolled.returncode == 1
    assert stage_lines(rolled.stdout)[-1] == "rolled_back"
    final = json.loads(progress.read_text())
    assert final["stage"] == "rolled_back", final
    assert final["detail"] == "the v9.9.9 runtime did not answer on its control API; rolling back now", final
    assert (install_dir / "commaviewd").read_text() == "old"

    # Fails before anything changed: failed, with the command that stopped it when no ERROR said why.
    failed = run_install_flow(tmp_path, "set -E\ntrap 'LAST_FAILED_COMMAND=\"command failed (exit $?): $BASH_COMMAND\"' ERR\n"
                                        "stage downloading; false")
    assert failed.returncode == 1
    final = json.loads(progress.read_text())
    assert final["stage"] == "failed" and "command failed (exit 1): false" in final["detail"], final


def test_install_stops_before_changing_anything_when_the_car_is_driven_off(tmp_path):
    (tmp_path / "params").mkdir()
    set_offroad(tmp_path, "1")
    mutated = tmp_path / "mutated"
    result = run_install_flow(tmp_path, f"""
stage starting
ensure_offroad_ready
stage downloading v9.9.9
echo 0 > "$PARAMS_DIR/IsOffroad"   # driven off during the download
STAGED_BUNDLE="$tmpdir/staged-bundle"; mkdir -p "$STAGED_BUNDLE"
ensure_offroad_ready
stage installing
touch {mutated}
""")
    assert result.returncode == 42, result.stdout + result.stderr
    assert "install blocked while driving" in result.stderr
    assert not mutated.exists()
    # Not a failure: the deferred queue takes exit 42 as "wait for the next parked stretch".
    assert json.loads((tmp_path / "deferred" / "progress.json").read_text())["stage"] == "downloading"
    assert stage_lines(result.stdout) == ["starting", "downloading"]


def test_unknown_road_state_is_onroad_for_every_script(tmp_path):
    """A missing IsOffroad (and IsOnroad) never authorizes maintenance where openpilot's params exist."""
    params = tmp_path / "params"
    params.mkdir()
    for rel in ("install.sh", "uninstall.sh", "scripts/apply_onroad_ui_export_patch.sh",
                "scripts/revert_onroad_ui_export_patch.sh"):
        text = (REPO_ROOT / "comma" / rel).read_text()
        functions = shell_function(text, "read_param") + shell_function(text, "read_is_onroad")
        for state, expected in ((None, "1"), ("0", "1"), ("1", "0")):
            for name in ("IsOffroad", "IsOnroad"):
                (params / name).unlink(missing_ok=True)
            if state is not None:
                (params / "IsOffroad").write_text(state)
            out = subprocess.run(["bash", "-c", f"PARAMS_DIR={params}\n{functions}\nread_is_onroad"],
                                 capture_output=True, text=True, check=True).stdout.strip()
            assert out == expected, (rel, state, out)
        # No openpilot params at all (a bench or CI checkout): offroad.
        out = subprocess.run(["bash", "-c", f"PARAMS_DIR={tmp_path / 'none'}\n{functions}\nread_is_onroad"],
                             capture_output=True, text=True, check=True).stdout.strip()
        assert out == "0", rel


def test_first_install_tells_park_from_driving_with_the_release_itself(tmp_path):
    """Nothing installed yet: the install asks the release it downloaded, and a queued first install
    keeps that release's road-phase beside the queue so the queue can see Park too."""
    env = env_for(tmp_path)  # no installed runtime: COMMAVIEWD_ROAD_PHASE_BIN doesn't exist
    set_offroad(tmp_path, "0")
    marker = tmp_path / "installed"
    companions = tmp_path / "companions"
    (companions / "scripts").mkdir(parents=True)
    (companions / "install.sh").write_text(f'#!/usr/bin/env bash\necho "$@" > "{marker}"\n')
    (companions / "scripts" / "run_when_offroad.sh").write_text(RUNNER.read_text())
    # The downloaded release, unpacked: a commaviewd whose road-phase answers tmp_path/phase, and its lib/.
    phase = tmp_path / "phase"
    phase.write_text("driving not-in-park")
    staged = tmp_path / "staged"
    (staged / "lib").mkdir(parents=True)
    (staged / "lib" / "libkj-1.0.1.so").write_text("lib")
    release = staged / "commaviewd"
    release.write_text(
        "#!/usr/bin/env bash\n"
        '[ "$1" = road-phase ] || exit 2\n'
        f'read -r p reason < "{phase}"\n'
        'echo "$p $reason"\n'
        '[ "$p" != driving ]\n'
    )
    release.chmod(0o755)
    kit = tmp_path / "deferred" / "road-phase"
    functions = install_functions(
        "read_is_onroad", "road_phase_bin", "read_is_driving", "keep_road_phase_kit", "queue_install_until_offroad",
        "cancel_deferred_maintenance", "ensure_offroad_ready")

    def ensure(force, staged_bundle):
        script = f"""
set -euo pipefail
PARAMS_DIR={tmp_path / 'params'}
read_param() {{ [ -f "$PARAMS_DIR/$1" ] && tr -d '\\000\\r\\n' < "$PARAMS_DIR/$1" || true; }}
COMPANION_DIR={companions}
DEFERRED_DIR={tmp_path / 'deferred'}
DEFERRED_EXIT=75
RELEASE_TAG=v9.9.9
ROAD_PHASE_BIN="$COMMAVIEWD_ROAD_PHASE_BIN"
ROAD_PHASE_KIT={kit}
STAGED_BUNDLE={staged_bundle}
FORCE_OFFROAD={1 if force else 0}
{functions}
ensure_offroad_ready
echo proceeding
"""
        return subprocess.run(["bash", "-c", script], env=env, capture_output=True, text=True, timeout=30)

    try:
        # Before the download nothing can tell, so it downloads instead of refusing or queueing.
        first = ensure(True, '""')
        assert first.returncode == 0 and "proceeding" in first.stdout, first.stdout + first.stderr
        assert "checking again after the download" in first.stdout
        assert status(env)["state"] == "none"
        # After it: in Park with the car on, it installs now.
        phase.write_text("parked parked")
        assert ensure(True, str(staged)).returncode == 0
        # Driven: refused without --force-offroad, queued with it, keeping the release's road-phase.
        phase.write_text("driving not-in-park")
        assert ensure(False, str(staged)).returncode == 42
        queued = ensure(True, str(staged))
        assert queued.returncode == 75, queued.stdout + queued.stderr
        assert status(env)["state"] == "waiting"
        assert os.access(kit / "commaviewd", os.X_OK) and (kit / "lib" / "libkj-1.0.1.so").exists()
        # The queue asks the kept road-phase: still driving, it waits; in Park, the install runs.
        time.sleep(3.5)
        assert not marker.exists(), "ran while driving"
        phase.write_text("parked parked")
        assert wait_for(lambda: status(env)["state"] == "done")
        assert marker.read_text().split() == ["--tag", "v9.9.9"]
        assert not (tmp_path / "params" / "OffroadMode").exists()
        # Cancelling (or a direct run, which cancels) drops the kept copy.
        phase.write_text("driving not-in-park")
        assert ensure(True, str(staged)).returncode == 75 and kit.exists()
        run(env, "cancel")
        assert not kit.exists()
    finally:
        run(env, "cancel", check=False)


def test_install_drops_the_kept_road_phase_once_installed():
    text = INSTALL_SH.read_text()
    main = text[text.index('tmpdir="$(mktemp -d /tmp/commaview-install.XXXXXX)"'):]
    assert main.index('rm -rf "$ROAD_PHASE_KIT"') > main.index("INSTALL_SUCCESS=1")
    # The kept copy is made before the job is queued, so the waiter can use it from its first poll.
    queue = shell_function(text, "queue_install_until_offroad")
    assert queue.index("keep_road_phase_kit") < queue.index('"$runner" queue install')
