"""openpilot's UI files must never be left broken by CommaView, and its .git/index never touched.

- Every write to openpilot's tree is atomic (temp file, fsync, rename): a kill or power loss leaves
  the old file or the new one.
- A file left damaged anyway (cut short by an older runtime, say) is restored from openpilot's HEAD
  at boot and patched again; someone else's change is left alone.
- The injected import and exporter construction are guarded, so the UI loads even when the
  exporter can't.
- Nothing rewrites .git/index: launch_chffrplus.sh would take the tree for modified.
"""

import hashlib
import importlib.util
import os
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest

from onroad_ui_export_transformer_test import (
    APPLY_SCRIPT,
    EXPORT_INSTALL,
    NEW_UI_STATE,
    REVERT_SCRIPT,
    TRANSFORMER,
    assert_ui_state_transformed,
    init_git_repo,
    prepare_lifecycle_install_dir,
    run_transformer,
    write_full_augmented_tree,
    write_upstream_tree,
)

UI_STATE = Path("selfdrive/ui/ui_state.py")
VERIFY_SCRIPT = APPLY_SCRIPT.parent / "verify_onroad_ui_export_patch.sh"


def load_transformer():
    spec = importlib.util.spec_from_file_location("transform_onroad_ui_export", TRANSFORMER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def patched_tree(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    subprocess.run(["git", "remote", "add", "origin", "https://github.com/commaai/openpilot.git"], cwd=op_root, check=True)
    return op_root, prepare_lifecycle_install_dir(tmp_path, op_root)


def run(script, install_dir, op_root, *args, **env):
    proc = install_dir.parent / "proc-boot"
    (proc / "sys" / "kernel" / "random").mkdir(parents=True, exist_ok=True)
    (proc / "uptime").write_text("8.00 1.00\n")
    (proc / "sys" / "kernel" / "random" / "boot_id").write_text("boot\n")
    full_env = {
        "PATH": os.environ["PATH"],
        "HOME": os.environ.get("HOME", "/tmp"),
        "COMMAVIEWD_INSTALL_DIR": str(install_dir),
        "COMMAVIEWD_OP_ROOT": str(op_root),
        "COMMAVIEWD_PROC_ROOT": str(proc),
        "COMMAVIEWD_STAGING_ROOT": str(install_dir.parent / "no-staging"),
        "COMMAVIEWD_BACKUP_ROOT": str(install_dir.parent / "backups"),
        "COMMAVIEWD_MAINT_LOCK": str(install_dir.parent / "maint.lock"),
        **env,
    }
    return subprocess.run(["bash", str(script), *args], env=full_env, text=True, capture_output=True, check=False)


def index_state(op_root):
    index = op_root / ".git" / "index"
    return index.stat().st_mtime_ns, hashlib.sha256(index.read_bytes()).hexdigest()


def git_status(op_root):
    # Without optional locks, so the check itself doesn't refresh the index.
    return subprocess.check_output(["git", "status", "--porcelain"], cwd=op_root, text=True,
                                   env={**os.environ, "GIT_OPTIONAL_LOCKS": "0"})


def compiles(path):
    compile(path.read_text(), str(path), "exec")


def test_atomic_write_keeps_the_old_file_when_the_write_dies(tmp_path, monkeypatch):
    transformer = load_transformer()
    target = tmp_path / "ui_state.py"
    target.write_text("original\n")
    target.chmod(0o640)

    def power_loss(fd):
        raise OSError("power loss")

    monkeypatch.setattr(transformer.os, "fsync", power_loss)
    with pytest.raises(OSError):
        transformer.atomic_write_text(target, "half written")
    assert target.read_text() == "original\n"
    assert sorted(p.name for p in tmp_path.iterdir()) == ["ui_state.py"], "no temp file left behind"

    monkeypatch.undo()
    transformer.atomic_write_text(target, "patched\n")
    assert target.read_text() == "patched\n"
    assert target.stat().st_mode & 0o777 == 0o640, "keeps openpilot's file mode"
    assert sorted(p.name for p in tmp_path.iterdir()) == ["ui_state.py"]


def test_transformer_never_writes_a_patch_that_does_not_compile(tmp_path, monkeypatch):
    transformer = load_transformer()
    op_root = write_upstream_tree(tmp_path, ui_state=NEW_UI_STATE)
    ui_state = op_root / UI_STATE
    original = ui_state.read_text()
    monkeypatch.setattr(transformer, "EXPORT_INSTALL", "self._commaview_exporter = (")
    with pytest.raises(SystemExit):
        transformer.transform_ui_state(ui_state)
    assert ui_state.read_text() == original


def test_injected_import_and_exporter_never_break_the_ui(tmp_path):
    op_root = write_upstream_tree(tmp_path, ui_state=NEW_UI_STATE)
    assert run_transformer(op_root).returncode == 0
    text = (op_root / UI_STATE).read_text()
    assert_ui_state_transformed(op_root / UI_STATE)

    # The import: openpilot.selfdrive.ui.commaview_export can't be imported here at all.
    lines = text.splitlines()
    start = lines.index("try:")
    import_block = "\n".join(lines[start:start + 5])
    namespace = {}
    exec(import_block, namespace)
    assert namespace["_CommaViewSocketExporter"] is None

    # UIState.update with that fallback: the exporter is tried once, and the UI carries on.
    first = next(i for i, line in enumerate(lines) if line.strip() == "device.update()")
    end = next(i for i in range(first, len(lines)) if lines[i].startswith("  def ") and i > first)
    body = textwrap.dedent("\n".join(lines[first:end]))
    source = "def update(self):\n" + textwrap.indent(body, "  ")
    logged = []

    class Log:
        def exception(self, message):
            logged.append(message)

    class Device:
        def update(self):
            pass

    namespace = {"device": Device(), "cloudlog": Log(), "_CommaViewSocketExporter": None, "COMMAVIEW_RUNTIME_FLAVOR": ""}
    exec(source, namespace)

    class State:
        pass

    state = State()
    namespace["update"](state)
    namespace["update"](state)
    assert state._commaview_exporter is None
    assert logged == ["commaview ui export unavailable"], logged
    assert EXPORT_INSTALL in text


@pytest.mark.parametrize("damage", ["cut-mid-line", "cut-at-line", "emptied", "deleted"])
def test_boot_restores_a_damaged_ui_file_from_head_then_patches_it(tmp_path, damage):
    op_root, install_dir = patched_tree(tmp_path)
    head_text = (op_root / UI_STATE).read_text()
    applied = run(APPLY_SCRIPT, install_dir, op_root, "--force-repair")
    assert applied.returncode == 0, applied.stderr
    patched = (op_root / UI_STATE).read_text()
    before = index_state(op_root)

    ui_state = op_root / UI_STATE
    if damage == "cut-mid-line":
        ui_state.write_text(patched[: len(patched) // 2 + 7])
    elif damage == "cut-at-line":
        # Still compiles: only the comparison with HEAD shows it was cut short.
        ui_state.write_text("".join(head_text.splitlines(keepends=True)[:3]))
    elif damage == "emptied":
        ui_state.write_text("")
    else:
        ui_state.unlink()

    booted = run(APPLY_SCRIPT, install_dir, op_root, "--before-openpilot")

    assert booted.returncode == 0, booted.stderr
    assert "restoring damaged or outdated onroad UI export targets" in booted.stderr
    assert ui_state.read_text() == patched
    compiles(ui_state)
    assert index_state(op_root) == before, ".git/index must not be rewritten"
    assert not list(ui_state.parent.glob(".ui_state.py.commaview-*")), "no temp file left behind"


def test_boot_replaces_an_older_commaview_patch(tmp_path):
    op_root, install_dir = patched_tree(tmp_path)
    ui_state = op_root / UI_STATE
    # What earlier releases inserted: an unguarded import.
    old = ui_state.read_text().replace(
        "from openpilot.selfdrive.ui.lib.prime_state import PrimeState",
        "from openpilot.selfdrive.ui.commaview_export import _CommaViewSocketExporter, COMMAVIEW_RUNTIME_FLAVOR\n"
        "from openpilot.selfdrive.ui.lib.prime_state import PrimeState",
    ).replace("    device.update()\n", "    device.update()\n    if not hasattr(self, \"_commaview_exporter\"):\n"
              "      self._commaview_exporter = _CommaViewSocketExporter(COMMAVIEW_RUNTIME_FLAVOR)\n"
              "    self._commaview_exporter.publish(self)\n")
    ui_state.write_text(old)

    booted = run(APPLY_SCRIPT, install_dir, op_root, "--before-openpilot")

    assert booted.returncode == 0, booted.stderr
    assert_ui_state_transformed(ui_state)
    assert "_CommaViewSocketExporter = None" in ui_state.read_text()


def test_boot_leaves_someone_elses_change_alone(tmp_path):
    op_root, install_dir = patched_tree(tmp_path)
    ui_state = op_root / UI_STATE
    mine = ui_state.read_text().replace("PARAM_UPDATE_TIME = 1 / 5.0", "PARAM_UPDATE_TIME = 1 / 2.0")
    ui_state.write_text(mine)

    booted = run(APPLY_SCRIPT, install_dir, op_root, "--before-openpilot")

    assert booted.returncode == 44, booted.stderr
    assert "refusing to modify dirty upstream files without --force-repair" in booted.stderr
    assert ui_state.read_text() == mine


def test_apply_and_revert_never_rewrite_git_index(tmp_path):
    op_root, install_dir = patched_tree(tmp_path)
    before = index_state(op_root)

    for args in ((), ("--force-repair",), ("--force-repair",)):
        result = run(APPLY_SCRIPT, install_dir, op_root, *args)
        assert result.returncode == 0, result.stderr
        assert index_state(op_root) == before, args
    reverted = run(REVERT_SCRIPT, install_dir, op_root)
    assert reverted.returncode == 0, reverted.stderr
    assert index_state(op_root) == before
    assert git_status(op_root) == ""
    # Reverting a clean tree writes nothing at all.
    mtimes = {p: p.stat().st_mtime_ns for p in (op_root / "selfdrive" / "ui").rglob("*.py")}
    again = run(REVERT_SCRIPT, install_dir, op_root)
    assert again.returncode == 0, again.stderr
    assert {p: p.stat().st_mtime_ns for p in mtimes} == mtimes
    assert index_state(op_root) == before


def test_verify_requires_the_patched_files_to_compile(tmp_path):
    op_root, install_dir = patched_tree(tmp_path)
    applied = run(APPLY_SCRIPT, install_dir, op_root)
    assert applied.returncode == 0, applied.stderr
    flat = op_root / "selfdrive" / "ui" / "onroad" / "augmented_road_view.py"
    flat.write_text(flat.read_text() + "\ndef broken(:\n")

    verified = run(VERIFY_SCRIPT, install_dir, op_root, "--json")

    assert verified.returncode == 1
    import json
    status = json.loads(verified.stdout)
    assert status["patchedFilesCompile"] is False and status["patchVerified"] is False


def test_apply_waits_for_the_maintenance_lock(tmp_path):
    op_root, install_dir = patched_tree(tmp_path)
    lock = install_dir.parent / "maint.lock"
    holder = subprocess.Popen(["flock", "-o", str(lock), "sleep", "30"])
    try:
        import time
        time.sleep(0.3)
        started = time.monotonic()
        blocked = run(APPLY_SCRIPT, install_dir, op_root, "--before-openpilot")
        assert blocked.returncode == 1
        assert "another CommaView install, uninstall or repair is running" in blocked.stderr
        assert time.monotonic() - started >= 9
        assert git_status(op_root) == ""
        # A caller that holds the lock (install.sh, the deferred job) says so and isn't held up.
        held = run(APPLY_SCRIPT, install_dir, op_root, COMMAVIEWD_MAINT_LOCK_HELD="1")
        assert held.returncode == 0, held.stderr
    finally:
        holder.kill()
        holder.wait()
