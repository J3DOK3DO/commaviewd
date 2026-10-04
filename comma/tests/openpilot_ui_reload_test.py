"""openpilot cannot reload its UI in place, so CommaView must never signal it.

Upstream facts these tests encode (system/manager/process.py, process_config.py):
- openpilot (master, release-tizi/mici-staging) and sunnypilot master: manager never
  restarts a process that exited. PythonProcess.start() returns while self.proc is
  set, and only stop() clears it. The UI exits 0 on SIGINT, so a killed UI stays
  dead until reboot.
- sunnypilot release branches restart a dead "ui" (restart_if_crash=True), but the
  manager preimports every module at startup (PythonProcess.prepare) and forks the
  new UI from it, so the restarted UI runs its old code.

So the patch is applied before openpilot starts (start.sh --before-openpilot from the
continue.sh hook), and a patch applied while openpilot runs is reported as
uiReloadPending until the next reboot.
"""

import json
import os
import re
import shutil
import signal
import subprocess
import sys
import textwrap
import time
from pathlib import Path

import pytest

from onroad_ui_export_transformer_test import (
    APPLY_SCRIPT,
    REPO_ROOT,
    REVERT_SCRIPT,
    assert_ui_state_transformed,
    init_git_repo,
    prepare_lifecycle_install_dir,
    write_full_augmented_tree,
)

VERIFY_SCRIPT = REPO_ROOT / "comma" / "scripts" / "verify_onroad_ui_export_patch.sh"
START_SH = REPO_ROOT / "comma" / "start.sh"
STOP_SH = REPO_ROOT / "comma" / "stop.sh"
INSTALL_SH = REPO_ROOT / "comma" / "install.sh"
UNINSTALL_SH = REPO_ROOT / "comma" / "uninstall.sh"
MARKER_NAME = "onroad-ui-export-ui-restart-needed"
CLK_TCK = os.sysconf("SC_CLK_TCK")


def patched_tree(tmp_path: Path) -> tuple[Path, Path]:
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    subprocess.run(["git", "remote", "add", "origin", "https://github.com/commaai/openpilot.git"], cwd=op_root, check=True)
    return op_root, prepare_lifecycle_install_dir(tmp_path, op_root)


def fake_proc(root: Path, *, uptime_s: float, boot_id: str, procs=()) -> Path:
    """A /proc stand-in: procs are (pid, ppid, start_s, argv)."""
    (root / "sys" / "kernel" / "random").mkdir(parents=True, exist_ok=True)
    (root / "uptime").write_text(f"{uptime_s:.2f} 1.00\n")
    (root / "sys" / "kernel" / "random" / "boot_id").write_text(boot_id + "\n")
    for pid, ppid, start_s, argv in procs:
        add_fake_proc(root, pid, ppid, start_s, argv)
    return root


def add_fake_proc(root: Path, pid: int, ppid: int, start_s: float, argv: list[str]) -> None:
    proc = root / str(pid)
    proc.mkdir(parents=True, exist_ok=True)
    (proc / "cmdline").write_bytes(b"".join(arg.encode() + b"\0" for arg in argv))
    (proc / "status").write_text(f"Name:\tx\nPPid:\t{ppid}\n")
    # comm may hold spaces and parentheses; the parser must split after the last ") ".
    ticks = int(start_s * CLK_TCK)
    (proc / "stat").write_text(f"{pid} (a) b) S {ppid} {pid} {pid} 0 -1 4194560 0 0 0 0 0 0 0 0 20 0 1 0 {ticks} 0 0\n")


OPENPILOT_RUNNING = (
    (100, 1, 30.0, ["/usr/local/venv/bin/python3", "./manager.py"]),
    (200, 100, 35.0, ["openpilot.selfdrive.ui.ui"]),
)


def recording_signal_tools(tmp_path: Path) -> tuple[str, Path]:
    """pkill/pgrep stand-ins that record any use; returns (PATH, log)."""
    bin_dir = tmp_path / "signal-bin"
    bin_dir.mkdir(exist_ok=True)
    log = tmp_path / "signals.log"
    for tool in ("pkill", "pgrep", "killall"):
        script = bin_dir / tool
        script.write_text(f'#!/usr/bin/env bash\necho "{tool} $*" >> "{log}"\nexit 1\n')
        script.chmod(0o755)
    return f"{bin_dir}:{os.environ['PATH']}", log


def run_script(script: Path, install_dir: Path, op_root: Path, *args: str, **env: str) -> subprocess.CompletedProcess[str]:
    full_env = {
        "PATH": os.environ["PATH"],
        "HOME": os.environ.get("HOME", "/tmp"),
        "COMMAVIEWD_INSTALL_DIR": str(install_dir),
        "COMMAVIEWD_OP_ROOT": str(op_root),
        "COMMAVIEWD_MAINT_LOCK": str(install_dir.parent / "maint.lock"),
        **env,
    }
    return subprocess.run(["bash", str(script), *args], env=full_env, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)


def last_json(stdout: str) -> dict:
    lines = [line for line in stdout.splitlines() if line.startswith("{")]
    assert lines, stdout
    return json.loads(lines[-1])


def git_status(op_root: Path) -> str:
    return subprocess.check_output(["git", "status", "--porcelain"], cwd=op_root, text=True)


def test_apply_while_openpilot_runs_never_signals_ui_and_reports_reboot_pending(tmp_path):
    op_root, install_dir = patched_tree(tmp_path)
    proc_root = fake_proc(tmp_path / "proc", uptime_s=5000.0, boot_id="boot-a", procs=OPENPILOT_RUNNING)
    params = tmp_path / "params"
    params.mkdir()
    (params / "IsOffroad").write_text("1")
    path, signals = recording_signal_tools(tmp_path)

    result = run_script(APPLY_SCRIPT, install_dir, op_root, PATH=path,
                        COMMAVIEWD_PROC_ROOT=str(proc_root), COMMAVIEWD_PARAMS_DIR=str(params))

    assert result.returncode == 0, result.stderr
    assert not signals.exists(), signals.read_text()
    assert_ui_state_transformed(op_root / "selfdrive" / "ui" / "ui_state.py")
    marker = (install_dir / "run" / MARKER_NAME).read_text().splitlines()
    assert marker == ["pending", "bootId=boot-a", "patchedAtUptimeCs=500000"]
    status = last_json(result.stdout)
    assert status["patchVerified"] is True
    assert status["uiReloadPending"] is True
    assert status["uiReloadAction"] == "reboot"
    assert "takes effect after the next reboot" in status["uiReloadReason"]
    assert "takes effect after the next reboot" in status["reason"]
    assert "takes effect after the next reboot" in result.stderr

    # Still pending on a later status read in the same openpilot session.
    again = run_script(VERIFY_SCRIPT, install_dir, op_root, "--json", COMMAVIEWD_PROC_ROOT=str(proc_root))
    assert again.returncode == 0
    assert json.loads(again.stdout)["uiReloadPending"] is True


def test_apply_and_revert_leave_a_real_running_ui_alone(tmp_path):
    """With the real /proc: a stand-in manager.py and a UI child named like openpilot's."""
    op_root, install_dir = patched_tree(tmp_path)
    manager_dir = tmp_path / "manager"
    manager_dir.mkdir()
    signalled = tmp_path / "ui-signalled"
    ui_ready = tmp_path / "ui-ready"
    (manager_dir / "manager.py").write_text(textwrap.dedent(f"""
        import subprocess, sys, time
        ui_code = (
            "import signal, sys, time, pathlib\\n"
            "def hit(sig, frame):\\n"
            "  pathlib.Path({str(signalled)!r}).write_text(str(sig))\\n"
            "  sys.exit(0)\\n"
            "signal.signal(signal.SIGINT, hit)\\n"
            "signal.signal(signal.SIGTERM, hit)\\n"
            "pathlib.Path({str(ui_ready)!r}).write_text('up')\\n"
            "time.sleep(120)\\n"
        )
        ui = subprocess.Popen(["openpilot.selfdrive.ui.ui", "-c", ui_code], executable=sys.executable)
        ui.wait()
    """))
    manager = subprocess.Popen([sys.executable, "./manager.py"], cwd=manager_dir, start_new_session=True)
    try:
        deadline = time.monotonic() + 15
        while not ui_ready.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        assert ui_ready.exists(), "stand-in UI did not start"

        applied = run_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair")
        assert applied.returncode == 0, applied.stderr
        assert last_json(applied.stdout)["uiReloadPending"] is True
        assert (install_dir / "run" / MARKER_NAME).exists()

        reverted = run_script(REVERT_SCRIPT, install_dir, op_root, COMMAVIEWD_BACKUP_ROOT=str(tmp_path / "backups"))
        assert reverted.returncode == 0, reverted.stderr
        assert "keeps the CommaView onroad UI export it loaded until the next reboot" in reverted.stderr

        time.sleep(0.3)
        assert not signalled.exists(), f"UI got signal {signalled.read_text()}"
        assert manager.poll() is None, "stand-in manager (and its UI) exited"
    finally:
        os.killpg(manager.pid, signal.SIGKILL)
        manager.wait()


def test_verify_drops_pending_after_reboot(tmp_path):
    op_root, install_dir = patched_tree(tmp_path)
    proc_root = fake_proc(tmp_path / "proc", uptime_s=5000.0, boot_id="boot-a", procs=OPENPILOT_RUNNING)
    applied = run_script(APPLY_SCRIPT, install_dir, op_root, COMMAVIEWD_PROC_ROOT=str(proc_root))
    assert applied.returncode == 0, applied.stderr
    assert last_json(applied.stdout)["uiReloadPending"] is True

    rebooted = fake_proc(tmp_path / "proc-after-reboot", uptime_s=40.0, boot_id="boot-b",
                         procs=((90, 1, 9.0, ["python3", "./manager.py"]), (95, 90, 12.0, ["openpilot.selfdrive.ui.ui"])))
    status = run_script(VERIFY_SCRIPT, install_dir, op_root, "--json", COMMAVIEWD_PROC_ROOT=str(rebooted))

    assert status.returncode == 0
    payload = json.loads(status.stdout)
    assert payload["uiReloadPending"] is False
    assert payload["uiReloadAction"] == ""
    assert "reboot" not in payload["reason"]
    assert not (install_dir / "run" / MARKER_NAME).exists()


def test_verify_drops_pending_when_openpilot_started_after_the_patch(tmp_path):
    op_root, install_dir = patched_tree(tmp_path)
    proc_root = fake_proc(tmp_path / "proc", uptime_s=5000.0, boot_id="boot-a", procs=OPENPILOT_RUNNING)
    applied = run_script(APPLY_SCRIPT, install_dir, op_root, COMMAVIEWD_PROC_ROOT=str(proc_root))
    assert applied.returncode == 0, applied.stderr

    # Same boot, openpilot restarted at 6000 s, after the patch at 5000 s. No UI
    # process yet, so the manager is found by its command line.
    restarted = fake_proc(tmp_path / "proc-restarted", uptime_s=6005.0, boot_id="boot-a",
                          procs=((300, 1, 6000.0, ["python3", "./manager.py"]),))
    status = run_script(VERIFY_SCRIPT, install_dir, op_root, "--json", COMMAVIEWD_PROC_ROOT=str(restarted))

    assert json.loads(status.stdout)["uiReloadPending"] is False
    assert not (install_dir / "run" / MARKER_NAME).exists()


def test_verify_uses_the_ui_parent_as_manager_start(tmp_path):
    op_root, install_dir = patched_tree(tmp_path)
    proc_root = fake_proc(tmp_path / "proc", uptime_s=5000.0, boot_id="boot-a", procs=OPENPILOT_RUNNING)
    applied = run_script(APPLY_SCRIPT, install_dir, op_root, COMMAVIEWD_PROC_ROOT=str(proc_root))
    assert applied.returncode == 0, applied.stderr

    # sunnypilot release restarted a crashed UI after the patch: the UI is new, but
    # its parent (manager) is the one that preimported the old code. Still pending.
    restarted_ui = fake_proc(tmp_path / "proc-ui-restarted", uptime_s=5100.0, boot_id="boot-a",
                             procs=((100, 1, 30.0, ["python3", "./manager.py"]), (777, 100, 5050.0, ["selfdrive.ui.ui"])))
    status = run_script(VERIFY_SCRIPT, install_dir, op_root, "--json", COMMAVIEWD_PROC_ROOT=str(restarted_ui))

    assert json.loads(status.stdout)["uiReloadPending"] is True


def test_verify_does_not_report_pending_reload_for_an_unverified_patch(tmp_path):
    op_root, install_dir = patched_tree(tmp_path)
    run_dir = install_dir / "run"
    run_dir.mkdir(parents=True, exist_ok=True)
    (run_dir / MARKER_NAME).write_text("pending\n")
    proc_root = fake_proc(tmp_path / "proc", uptime_s=50.0, boot_id="boot-a", procs=OPENPILOT_RUNNING)

    status = run_script(VERIFY_SCRIPT, install_dir, op_root, "--json", COMMAVIEWD_PROC_ROOT=str(proc_root))

    payload = json.loads(status.stdout)
    assert payload["patchVerified"] is False
    assert payload["uiReloadPending"] is False
    assert (run_dir / MARKER_NAME).exists()


def test_apply_without_file_changes_does_not_request_a_reload(tmp_path):
    op_root, install_dir = patched_tree(tmp_path)
    no_openpilot = fake_proc(tmp_path / "proc-boot", uptime_s=5.0, boot_id="boot-a")
    first = run_script(APPLY_SCRIPT, install_dir, op_root, COMMAVIEWD_PROC_ROOT=str(no_openpilot))
    assert first.returncode == 0, first.stderr
    assert not (install_dir / "run" / MARKER_NAME).exists()

    running = fake_proc(tmp_path / "proc", uptime_s=5000.0, boot_id="boot-a", procs=OPENPILOT_RUNNING)
    already = run_script(APPLY_SCRIPT, install_dir, op_root, COMMAVIEWD_PROC_ROOT=str(running))
    assert already.returncode == 0, already.stderr
    assert not (install_dir / "run" / MARKER_NAME).exists()

    # --force-repair resets and re-applies the same output: the files openpilot
    # loaded are unchanged, so still nothing to reload.
    forced = run_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair", COMMAVIEWD_PROC_ROOT=str(running))
    assert forced.returncode == 0, forced.stderr
    assert not (install_dir / "run" / MARKER_NAME).exists()
    assert last_json(forced.stdout)["uiReloadPending"] is False


def test_apply_restores_reload_marker_when_post_transform_verify_fails(tmp_path):
    op_root, install_dir = patched_tree(tmp_path)
    run_dir = install_dir / "run"
    run_dir.mkdir(parents=True, exist_ok=True)
    (run_dir / MARKER_NAME).write_text("pending\nbootId=boot-a\npatchedAtUptimeCs=100\n")
    verify = install_dir / "scripts" / "verify_onroad_ui_export_patch.sh"
    verify.write_text("#!/usr/bin/env bash\necho forced verify failure >&2\nexit 77\n")
    verify.chmod(0o755)
    proc_root = fake_proc(tmp_path / "proc", uptime_s=5000.0, boot_id="boot-a", procs=OPENPILOT_RUNNING)

    result = run_script(APPLY_SCRIPT, install_dir, op_root, COMMAVIEWD_PROC_ROOT=str(proc_root))

    assert result.returncode == 77
    assert git_status(op_root) == ""
    assert (run_dir / MARKER_NAME).read_text() == "pending\nbootId=boot-a\npatchedAtUptimeCs=100\n"


def test_apply_before_openpilot_ignores_stale_isoffroad_and_clears_old_reload_marker(tmp_path):
    op_root, install_dir = patched_tree(tmp_path)
    params = tmp_path / "params"
    params.mkdir()
    (params / "IsOffroad").write_text("0")  # last session ended onroad; manager clears it at startup
    run_dir = install_dir / "run"
    run_dir.mkdir(parents=True, exist_ok=True)
    (run_dir / MARKER_NAME).write_text("pending\n")  # legacy marker from an earlier build
    boot = fake_proc(tmp_path / "proc", uptime_s=8.0, boot_id="boot-b")

    result = run_script(APPLY_SCRIPT, install_dir, op_root, "--before-openpilot",
                        COMMAVIEWD_PARAMS_DIR=str(params), COMMAVIEWD_PROC_ROOT=str(boot),
                        COMMAVIEWD_STAGING_ROOT=str(tmp_path / "no-staging"))

    assert result.returncode == 0, result.stderr
    assert "openpilot has not started" in result.stderr
    assert_ui_state_transformed(op_root / "selfdrive" / "ui" / "ui_state.py")
    assert not (run_dir / MARKER_NAME).exists()
    assert last_json(result.stdout)["uiReloadPending"] is False
    assert (params / "IsOffroad").read_text() == "0"
    assert not (params / "OffroadMode").exists()


def test_apply_before_openpilot_keeps_onroad_guard_when_openpilot_is_running(tmp_path):
    op_root, install_dir = patched_tree(tmp_path)
    params = tmp_path / "params"
    params.mkdir()
    (params / "IsOffroad").write_text("0")
    running = fake_proc(tmp_path / "proc", uptime_s=900.0, boot_id="boot-a", procs=OPENPILOT_RUNNING)

    result = run_script(APPLY_SCRIPT, install_dir, op_root, "--before-openpilot",
                        COMMAVIEWD_PARAMS_DIR=str(params), COMMAVIEWD_PROC_ROOT=str(running))

    assert result.returncode == 42
    assert "openpilot is already running" in result.stderr
    assert "blocked while driving" in result.stderr
    assert git_status(op_root) == ""


def staged_update(tmp_path: Path, live_root: Path) -> Path:
    """A finalized update the way updated.py leaves it, and a live tree that launch will replace."""
    staging = tmp_path / "safe_staging"
    finalized = staging / "finalized"
    shutil.copytree(live_root, finalized, symlinks=True)
    (finalized / ".overlay_consistent").touch()
    time.sleep(0.05)
    (live_root / ".overlay_init").touch()
    return staging


def test_apply_before_openpilot_patches_the_staged_update_launch_will_install(tmp_path):
    op_root, install_dir = patched_tree(tmp_path)
    staging = staged_update(tmp_path, op_root)
    finalized = staging / "finalized"
    boot = fake_proc(tmp_path / "proc", uptime_s=8.0, boot_id="boot-b")

    result = run_script(APPLY_SCRIPT, install_dir, op_root, "--before-openpilot",
                        COMMAVIEWD_PROC_ROOT=str(boot), COMMAVIEWD_STAGING_ROOT=str(staging))

    assert result.returncode == 0, result.stderr
    assert "is about to install the staged openpilot update" in result.stderr
    assert_ui_state_transformed(finalized / "selfdrive" / "ui" / "ui_state.py")
    assert git_status(op_root) == "?? .overlay_init\n"
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=finalized, text=True).strip()
    assert f"ONROAD_UI_EXPORT_UPSTREAM_HEAD={head}" in (install_dir / "config" / "onroad-ui-export-patch.env").read_text()

    # After launch_chffrplus.sh moves the update into place, it verifies as patched.
    live_after = tmp_path / "live-after-swap"
    finalized.rename(live_after)
    status = run_script(VERIFY_SCRIPT, install_dir, live_after, "--json", COMMAVIEWD_PROC_ROOT=str(boot))
    assert status.returncode == 0, status.stdout
    assert json.loads(status.stdout)["patchVerified"] is True


@pytest.mark.parametrize("blocker", ["git-newer-than-overlay-init", "old-openpilot-backup", "not-consistent"])
def test_apply_before_openpilot_patches_live_tree_when_launch_will_not_install_update(tmp_path, blocker):
    op_root, install_dir = patched_tree(tmp_path)
    staging = staged_update(tmp_path, op_root)
    if blocker == "git-newer-than-overlay-init":
        time.sleep(0.05)
        (op_root / ".git" / "local-change").write_text("x")
    elif blocker == "old-openpilot-backup":
        (staging / "old_openpilot").mkdir()
    else:
        (staging / "finalized" / ".overlay_consistent").unlink()
    boot = fake_proc(tmp_path / "proc", uptime_s=8.0, boot_id="boot-b")

    result = run_script(APPLY_SCRIPT, install_dir, op_root, "--before-openpilot",
                        COMMAVIEWD_PROC_ROOT=str(boot), COMMAVIEWD_STAGING_ROOT=str(staging))

    assert result.returncode == 0, result.stderr
    assert "staged openpilot update" not in result.stderr
    assert_ui_state_transformed(op_root / "selfdrive" / "ui" / "ui_state.py")
    finalized_status = git_status(staging / "finalized")
    assert "selfdrive/ui" not in finalized_status, finalized_status


def test_apply_and_verify_do_not_rewrite_git_index(tmp_path):
    """launch_chffrplus.sh skips a staged openpilot update when .git is newer than .overlay_init."""
    op_root, install_dir = patched_tree(tmp_path)
    index = op_root / ".git" / "index"
    # Stale stat data: a plain "git status" (which apply runs on its targets) would
    # refresh the index and rewrite it.
    time.sleep(0.05)
    for rel in ("selfdrive/ui/ui_state.py", "selfdrive/ui/mici/onroad/augmented_road_view.py"):
        os.utime(op_root / rel, None)
    before = index.stat().st_mtime_ns

    applied = run_script(APPLY_SCRIPT, install_dir, op_root, COMMAVIEWD_SKIP_OPENPILOT_UI_RESTART="1")
    verified = run_script(VERIFY_SCRIPT, install_dir, op_root, "--json")

    assert applied.returncode == 0, applied.stderr
    assert verified.returncode == 0, verified.stdout
    assert index.stat().st_mtime_ns == before


# ---- start.sh --before-openpilot (the continue.sh boot hook) ----

def install_start_scripts(tmp_path: Path, install_dir: Path) -> Path:
    """start.sh and stop.sh with /data/commaview pointed at install_dir."""
    for src in (START_SH, STOP_SH):
        text = src.read_text().replace("/data/commaview", str(install_dir))
        text = text.replace("/dev/shm/commaview", str(tmp_path / "shm-commaview"))
        dst = install_dir / src.name
        dst.write_text(text)
        dst.chmod(0o755)
    return install_dir / "start.sh"


def stop_background_runtime(install_dir: Path) -> None:
    run_dir = install_dir / "run"
    deadline = time.monotonic() + 15
    while not (run_dir / "log-rotation.pid").exists() and time.monotonic() < deadline:
        time.sleep(0.1)
    for name in ("log-rotation.pid", "bridge-supervisor.pid", "control-supervisor.pid"):
        try:
            os.kill(int((run_dir / name).read_text().strip()), signal.SIGKILL)
        except (FileNotFoundError, ValueError, ProcessLookupError):
            pass


def run_start_before_openpilot(tmp_path: Path, start_sh: Path, install_dir: Path, op_root: Path, **env: str):
    out = tmp_path / "start.out"
    full_env = {
        "PATH": os.environ["PATH"],
        "HOME": os.environ.get("HOME", "/tmp"),
        "COMMAVIEWD_INSTALL_DIR": str(install_dir),
        "COMMAVIEWD_OP_ROOT": str(op_root),
        "COMMAVIEWD_LOG_ROTATE_INTERVAL_SEC": "1",
        **env,
    }
    started = time.monotonic()
    with out.open("w") as handle:
        result = subprocess.run(["bash", str(start_sh), "--before-openpilot"], env=full_env,
                                stdout=handle, stderr=subprocess.STDOUT, check=False, timeout=60)
    return result, time.monotonic() - started


def test_start_before_openpilot_patches_then_starts_runtime_in_background(tmp_path):
    op_root, install_dir = patched_tree(tmp_path)
    start_sh = install_start_scripts(tmp_path, install_dir)
    params = tmp_path / "params"
    params.mkdir()
    (params / "IsOffroad").write_text("0")
    boot = fake_proc(tmp_path / "proc", uptime_s=8.0, boot_id="boot-b")
    try:
        result, _ = run_start_before_openpilot(
            tmp_path, start_sh, install_dir, op_root,
            COMMAVIEWD_PARAMS_DIR=str(params), COMMAVIEWD_PROC_ROOT=str(boot),
            COMMAVIEWD_STAGING_ROOT=str(tmp_path / "no-staging"))
        assert result.returncode == 0
        # The patch is in place by the time the hook returns and continue.sh execs launch_openpilot.sh.
        assert_ui_state_transformed(op_root / "selfdrive" / "ui" / "ui_state.py")
        log = install_dir / "logs" / "onroad-ui-export-startup.log"
        assert "INFO: onroad UI export prepared before openpilot start" in log.read_text()

        deadline = time.monotonic() + 15
        while "skipping startup repair" not in log.read_text() and time.monotonic() < deadline:
            time.sleep(0.1)
        assert "onroad UI export prepared before openpilot start; skipping startup repair" in log.read_text()
    finally:
        stop_background_runtime(install_dir)


def test_start_before_openpilot_never_holds_up_openpilot_start(tmp_path):
    op_root, install_dir = patched_tree(tmp_path)
    start_sh = install_start_scripts(tmp_path, install_dir)
    hung_apply = install_dir / "scripts" / "apply_onroad_ui_export_patch.sh"
    hung_apply.write_text("#!/usr/bin/env bash\nsleep 30\n")
    hung_apply.chmod(0o755)
    try:
        result, elapsed = run_start_before_openpilot(
            tmp_path, start_sh, install_dir, op_root, COMMAVIEWD_ONROAD_UI_EXPORT_PREPARE_TIMEOUT_SEC="1")
        assert result.returncode == 0
        assert elapsed < 15
        log = (install_dir / "logs" / "onroad-ui-export-startup.log").read_text()
        assert "WARN: onroad UI export not prepared before openpilot start (exit 124)" in log
    finally:
        stop_background_runtime(install_dir)


def test_lifecycle_scripts_never_signal_openpilot_ui():
    for script in (
        APPLY_SCRIPT,
        REVERT_SCRIPT,
        VERIFY_SCRIPT,
        START_SH,
        STOP_SH,
        INSTALL_SH,
        UNINSTALL_SH,
    ):
        for line in script.read_text().splitlines():
            code = line.split("#", 1)[0]
            assert not re.search(r"\b(pkill|pgrep|killall)\b", code), f"{script.name}: {line.strip()}"
            if "selfdrive.ui.ui" in code:
                # Only as a process-detection case pattern, never as a signal target.
                assert code.strip().startswith("*selfdrive.ui.ui)"), f"{script.name}: {line.strip()}"


# ---- install.sh boot hook ----

def boot_hook_harness(tmp_path: Path, continue_sh: Path) -> Path:
    text = INSTALL_SH.read_text()
    marker = re.search(r'^MARKER=.*$', text, re.M).group(0)
    hook_cmd = re.search(r'^BOOT_HOOK_CMD=.*$', text, re.M).group(0)
    function = re.search(r'^install_boot_hook\(\) \{\n.*?^\}\n', text, re.M | re.S).group(0)
    harness = tmp_path / "hook-harness.sh"
    harness.write_text(f'set -euo pipefail\nCONTINUE_SH="{continue_sh}"\n{marker}\n{hook_cmd}\n{function}\ninstall_boot_hook\n')
    return harness


AGNOS_CONTINUE_SH = "#!/usr/bin/env bash\n\ncd /data/openpilot\nexec ./launch_openpilot.sh\n"
HOOKED_CONTINUE_SH = (
    "#!/usr/bin/env bash\n\ncd /data/openpilot\n"
    "# commaview-hook\n/data/commaview/start.sh --before-openpilot\n"
    "exec ./launch_openpilot.sh\n"
)


@pytest.mark.parametrize(
    "before",
    [
        AGNOS_CONTINUE_SH,
        # The earlier hook ran start.sh in the background, racing manager loading the UI.
        "#!/usr/bin/env bash\n\ncd /data/openpilot\n# commaview-hook\n/data/commaview/start.sh &\nexec ./launch_openpilot.sh\n",
        HOOKED_CONTINUE_SH,
    ],
    ids=["fresh", "upgrade-background-hook", "already-current"],
)
def test_install_boot_hook_runs_start_synchronously_before_launch(tmp_path, before):
    continue_sh = tmp_path / "continue.sh"
    continue_sh.write_text(before)
    harness = boot_hook_harness(tmp_path, continue_sh)

    for _ in range(2):  # idempotent
        result = subprocess.run(["bash", str(harness)], text=True, capture_output=True, check=False)
        assert result.returncode == 0, result.stderr
        assert continue_sh.read_text() == HOOKED_CONTINUE_SH

    # uninstall.sh's removal still matches the hook and restores the original file.
    uninstall_sed = re.search(r"sed -i '([^']*)' /data/continue.sh", UNINSTALL_SH.read_text()).group(1)
    subprocess.run(["sed", "-i", uninstall_sed, str(continue_sh)], check=True)
    assert continue_sh.read_text() == AGNOS_CONTINUE_SH


def test_install_boot_hook_leaves_continue_sh_without_launch_line_alone(tmp_path):
    continue_sh = tmp_path / "continue.sh"
    original = "#!/usr/bin/env bash\n# commaview-hook\n/data/commaview/start.sh &\n/usr/comma/other.sh\n"
    continue_sh.write_text(original)

    result = subprocess.run(["bash", str(boot_hook_harness(tmp_path, continue_sh))], text=True, capture_output=True, check=False)

    assert result.returncode == 0, result.stderr
    assert "boot hook not changed" in result.stderr
    assert continue_sh.read_text() == original
