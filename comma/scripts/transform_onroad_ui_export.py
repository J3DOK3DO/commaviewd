#!/usr/bin/env python3
"""Strict source transformer for CommaView onroad UI export hooks."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

EXPORT_IMPORT = "from openpilot.selfdrive.ui.commaview_export import _CommaViewSocketExporter, COMMAVIEW_RUNTIME_FLAVOR"
# openpilot's UI must load even when CommaView's exporter can't (an upstream rename, a broken helper).
EXPORT_IMPORT_FALLBACK = "_CommaViewSocketExporter = None"
EXPORT_INSTALL = "self._commaview_exporter = _CommaViewSocketExporter(COMMAVIEW_RUNTIME_FLAVOR)"
EXPORT_PUBLISH = "self._commaview_exporter.publish(self)"
CAMERA_EXPORT_CALL = "self._update_commaview_camera_export()"
CAMERA_EXPORT_HELPER = "def _update_commaview_camera_export(self):"
PROJECTION_EXPORT_CALL = "exporter.set_onroad_projection("
MODEL_TRANSFORM_ASSIGN = "model_transform = video_transform @ calib_transform"
MODEL_RENDERER_FIELDS = ("self._model_renderer", "self.model_renderer")
MODEL_TRANSFORM_ANCHORS = tuple(f"{field}.set_transform(video_transform @ calib_transform)" for field in MODEL_RENDERER_FIELDS)
AUGMENTED_ROAD_RELATIVE_PATHS = (
    Path("selfdrive/ui/mici/onroad/augmented_road_view.py"),
    Path("selfdrive/ui/onroad/augmented_road_view.py"),
)
PLATFORM_CHOICES = ("auto", "mici", "tizi", "tici")
NORMALIZED_PLATFORMS = {"mici": "mici", "tizi": "tizi", "tici": "tizi"}


def fail(message: str) -> None:
    print(f"ERROR: {message}", file=sys.stderr)
    raise SystemExit(1)


def line_indent(line: str) -> int:
    return len(line) - len(line.lstrip(" "))


def find_single(lines: list[str], predicate, description: str) -> int:
    matches = [idx for idx, line in enumerate(lines) if predicate(line)]
    if len(matches) != 1:
        fail(f"expected exactly one {description}, found {len(matches)}")
    return matches[0]


def block_end(lines: list[str], start: int, indent: int) -> int:
    for idx in range(start + 1, len(lines)):
        stripped = lines[idx].strip()
        if not stripped:
            continue
        if line_indent(lines[idx]) <= indent:
            return idx
    return len(lines)


def atomic_write_text(path: Path, text: str) -> None:
    """Replaces path with text all at once: a temp file beside it, fsynced, renamed over it. A kill or
    power loss leaves openpilot's old file or the new one, never a truncated one."""
    mode = path.stat().st_mode & 0o7777 if path.exists() else 0o644
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=f".{path.name}.commaview-")
    try:
        with os.fdopen(fd, "w") as out:
            out.write(text)
            out.flush()
            os.fsync(out.fileno())
        os.chmod(tmp, mode)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
    try:
        dir_fd = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(dir_fd)
        finally:
            os.close(dir_fd)
    except OSError:
        pass


def compiles(text: str, path: Path) -> bool:
    """Compiled in memory (py_compile would leave a .pyc in openpilot's tree)."""
    try:
        compile(text, str(path), "exec")
    except (SyntaxError, ValueError):
        return False
    return True


def write_lines_if_changed(path: Path, original: str, lines: list[str]) -> bool:
    new_text = "\n".join(lines) + ("\n" if original.endswith("\n") else "")
    if new_text == original:
        return False
    # Judged against openpilot's own file: a python3 older than openpilot's can't read either.
    if compiles(original, path) and not compiles(new_text, path):
        fail(f"patched {path.name} does not compile; leaving it unchanged")
    atomic_write_text(path, new_text)
    return True


def ensure_export_import(lines: list[str]) -> bool:
    if any(line.strip() == EXPORT_IMPORT for line in lines):
        return False

    insert_at = None
    for idx, line in enumerate(lines):
        if line.startswith("from openpilot.selfdrive.ui."):
            insert_at = idx
            break

    if insert_at is None:
        for idx, line in enumerate(lines):
            if line.startswith("from openpilot.common."):
                insert_at = idx + 1

    if insert_at is None:
        fail("unable to place commaview exporter import")

    lines[insert_at:insert_at] = [
        "try:",
        f"  {EXPORT_IMPORT}",
        "except Exception:",
        f"  {EXPORT_IMPORT_FALLBACK}",
        '  COMMAVIEW_RUNTIME_FLAVOR = ""',
    ]
    return True


def normalize_platform(platform: str) -> str:
    if platform == "auto":
        return "auto"
    try:
        return NORMALIZED_PLATFORMS[platform]
    except KeyError:
        fail(f"unsupported UI platform: {platform}")


def source_root(op_root: Path) -> Path:
    if (op_root / "selfdrive" / "ui" / "ui_state.py").is_file():
        return op_root
    nested = op_root / "openpilot"
    if (nested / "selfdrive" / "ui" / "ui_state.py").is_file():
        return nested
    fail(f"missing ui_state.py under {op_root} or {nested}")


def augmented_paths_for_platform(op_root: Path, platform: str) -> list[Path]:
    normalized = normalize_platform(platform)
    if normalized == "mici":
        return [op_root / AUGMENTED_ROAD_RELATIVE_PATHS[0]]
    if normalized == "tizi":
        return [op_root / AUGMENTED_ROAD_RELATIVE_PATHS[1]]

    augmented_paths = [op_root / relpath for relpath in AUGMENTED_ROAD_RELATIVE_PATHS if (op_root / relpath).exists()]
    if not augmented_paths:
        augmented_paths = [op_root / AUGMENTED_ROAD_RELATIVE_PATHS[0]]
    return augmented_paths


def install_helper_source(op_root: Path, flavor: str) -> bool:
    template_path = Path(__file__).resolve().parents[1] / "src" / f"commaview_export.{flavor}.py"
    if not template_path.is_file():
        fail(f"missing commaview export helper template: {template_path}")

    target_path = op_root / "selfdrive" / "ui" / "commaview_export.py"
    target_path.parent.mkdir(parents=True, exist_ok=True)
    template = template_path.read_text()
    if target_path.exists() and target_path.read_text() == template:
        return False
    atomic_write_text(target_path, template)
    return True


def transform_ui_state(ui_state_path: Path) -> bool:
    if not ui_state_path.is_file():
        fail(f"missing ui_state.py at {ui_state_path}")

    original = ui_state_path.read_text()
    lines = original.splitlines()
    ensure_export_import(lines)

    class_idx = find_single(
        lines,
        lambda line: line.startswith("class UIState") and line.rstrip().endswith(":"),
        "UIState class",
    )
    class_indent = line_indent(lines[class_idx])
    class_stop = block_end(lines, class_idx, class_indent)

    update_matches = [
        idx
        for idx in range(class_idx + 1, class_stop)
        if line_indent(lines[idx]) > class_indent and lines[idx].lstrip().startswith("def update(self")
    ]
    if len(update_matches) != 1:
        fail(f"expected exactly one UIState.update method, found {len(update_matches)}")

    update_idx = update_matches[0]
    update_indent = line_indent(lines[update_idx])
    update_stop = block_end(lines, update_idx, update_indent)
    update_body = lines[update_idx:update_stop]

    device_matches = [
        update_idx + offset
        for offset, line in enumerate(update_body)
        if line.strip() == "device.update()"
    ]
    if len(device_matches) != 1:
        fail(f"expected exactly one device.update() in UIState.update, found {len(device_matches)}")

    if not any(EXPORT_PUBLISH in line for line in update_body):
        device_idx = device_matches[0]
        body_indent = lines[device_idx][: line_indent(lines[device_idx])]
        inner_indent = body_indent + "  "
        # Constructed once; if that fails the UI carries on without the exporter (logged once).
        export_block = [
            f'{body_indent}if not hasattr(self, "_commaview_exporter"):',
            f"{inner_indent}try:",
            f"{inner_indent}  {EXPORT_INSTALL}",
            f"{inner_indent}except Exception:",
            f"{inner_indent}  self._commaview_exporter = None",
            f'{inner_indent}  cloudlog.exception("commaview ui export unavailable")',
            f"{body_indent}if self._commaview_exporter is not None:",
            f"{inner_indent}try:",
            f"{inner_indent}  {EXPORT_PUBLISH}",
            f"{inner_indent}except Exception:",
            f'{inner_indent}  cloudlog.exception("commaview ui export publish failed")',
        ]
        lines[device_idx + 1 : device_idx + 1] = export_block

    return write_lines_if_changed(ui_state_path, original, lines)


def transform_augmented_road_view(augmented_path: Path) -> bool:
    if not augmented_path.exists():
        return False
    if not augmented_path.is_file():
        fail(f"augmented_road_view.py is not a file at {augmented_path}")

    original = augmented_path.read_text()
    lines = original.splitlines()

    render_matches = [
        idx
        for idx, line in enumerate(lines)
        if line.strip() in {"super()._render(self._content_rect)", "super()._render(rect)"}
    ]
    if len(render_matches) != 1:
        fail(
            "expected exactly one augmented road render anchor "
            f"(super()._render(self._content_rect) or super()._render(rect)), found {len(render_matches)}"
        )
    render_idx = render_matches[0]
    camera_call_count = sum(1 for line in lines if line.strip() == CAMERA_EXPORT_CALL)
    if camera_call_count == 0:
        render_indent = lines[render_idx][: line_indent(lines[render_idx])]
        lines.insert(render_idx + 1, f"{render_indent}{CAMERA_EXPORT_CALL}")
    elif camera_call_count != 1:
        fail(f"expected at most one {CAMERA_EXPORT_CALL}, found {camera_call_count}")

    helper_count = sum(1 for line in lines if line.strip().startswith(CAMERA_EXPORT_HELPER))
    if helper_count == 0:
        calibration_idx = find_single(
            lines,
            lambda line: line.lstrip().startswith("def _update_calibration(self") and line.rstrip().endswith(":"),
            "_update_calibration method",
        )
        method_indent = lines[calibration_idx][: line_indent(lines[calibration_idx])]
        body_indent = method_indent + "  "
        helper_block = [
            f"{method_indent}{CAMERA_EXPORT_HELPER}",
            f'{body_indent}exporter = getattr(ui_state, "_commaview_exporter", None)',
            f'{body_indent}if exporter is None or not hasattr(exporter, "set_onroad_camera"):',
            f"{body_indent}  return",
            f"{body_indent}try:",
            f"{body_indent}  exporter.set_onroad_camera(",
            f'{body_indent}    active_camera="wideRoad" if self.stream_type == WIDE_CAM else "road",',
            f"{body_indent}    wide_camera_available=bool(WIDE_CAM in self.available_streams),",
            f"{body_indent}  )",
            f"{body_indent}except Exception:",
            f"{body_indent}  pass",
            "",
        ]
        lines[calibration_idx:calibration_idx] = helper_block
    elif helper_count != 1:
        fail(f"expected at most one {CAMERA_EXPORT_HELPER}, found {helper_count}")

    projection_already_transformed = (
        any(line.strip() == MODEL_TRANSFORM_ASSIGN for line in lines)
        and any(line.strip().endswith(".set_transform(model_transform)") for line in lines)
        and any(PROJECTION_EXPORT_CALL in line for line in lines)
    )
    transform_matches = [(idx, line.strip()) for idx, line in enumerate(lines) if line.strip() in MODEL_TRANSFORM_ANCHORS]
    if projection_already_transformed:
        if transform_matches:
            fail("unexpected raw model renderer transform anchor after projection export transform")
    else:
        if len(transform_matches) != 1:
            fail(
                "expected exactly one model renderer set_transform(video_transform @ calib_transform) anchor, "
                f"found {len(transform_matches)}"
            )
        transform_idx, transform_anchor = transform_matches[0]
        renderer_field = transform_anchor.split(".set_transform", 1)[0]
        model_transform_set = f"{renderer_field}.set_transform(model_transform)"
        transform_indent = lines[transform_idx][: line_indent(lines[transform_idx])]
        projection_block = [
            f"{transform_indent}{MODEL_TRANSFORM_ASSIGN}",
            f"{transform_indent}{model_transform_set}",
            f'{transform_indent}exporter = getattr(ui_state, "_commaview_exporter", None)',
            f'{transform_indent}if exporter is not None and hasattr(exporter, "set_onroad_projection"):',
            f"{transform_indent}  try:",
            f"{transform_indent}    {PROJECTION_EXPORT_CALL}",
            f"{transform_indent}      ui_state=ui_state,",
            f'{transform_indent}      active_camera="wideRoad" if is_wide_camera else "road",',
            f"{transform_indent}      content_rect=self._content_rect,",
            f"{transform_indent}      video_frame_matrix=self._cached_matrix,",
            f"{transform_indent}      model_transform=model_transform,",
            f'{transform_indent}      camera_offset=getattr({renderer_field}, "_camera_offset", 0.0),',
            f"{transform_indent}    )",
            f"{transform_indent}  except Exception:",
            f"{transform_indent}    pass",
        ]
        lines[transform_idx : transform_idx + 1] = projection_block

    return write_lines_if_changed(augmented_path, original, lines)


# Markers only CommaView's patch puts in openpilot's UI files.
COMMAVIEW_MARKERS = ("_commaview_exporter", CAMERA_EXPORT_HELPER, PROJECTION_EXPORT_CALL)


def git_head_text(repo: Path, rel: str) -> str | None:
    """rel as openpilot's HEAD has it, read without touching .git/index; None when HEAD lacks it."""
    result = subprocess.run(
        ["git", "-C", str(repo), "cat-file", "-p", f"HEAD:./{rel}"],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        env={**os.environ, "GIT_OPTIONAL_LOCKS": "0"},
        check=False,
    )
    if result.returncode != 0:
        return None
    try:
        return result.stdout.decode("utf-8")
    except UnicodeDecodeError:
        return None


def patched_from_head(head: str, rel: str) -> str | None:
    """What this transformer makes of HEAD's file, or None when it can't patch it."""
    import contextlib
    import io

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / Path(rel).name
        path.write_text(head)
        try:
            with contextlib.redirect_stderr(io.StringIO()):
                if path.name == "ui_state.py":
                    transform_ui_state(path)
                else:
                    transform_augmented_road_view(path)
        except SystemExit:
            return None
        return path.read_text()


def classify_target(repo: Path, rel: str) -> str:
    """How a managed file stands against openpilot's HEAD:
    clean / untracked / absent: nothing to undo; current: this transformer's patch of HEAD;
    ours: an older CommaView patch; damaged: missing, truncated or no longer compiling (a write cut
    short by a kill or power loss); foreign: someone else's change, left alone."""
    path = repo / rel
    head = git_head_text(repo, rel)
    if head is None:
        return "untracked" if path.exists() else "absent"
    if not path.is_file():
        return "damaged"
    try:
        text = path.read_text()
    except (OSError, UnicodeDecodeError):
        return "damaged"
    if text == head:
        return "clean"
    if compiles(head, path) and not compiles(text, path):
        return "damaged"
    if head.startswith(text):
        return "damaged"
    if text == patched_from_head(head, rel):
        return "current"
    if any(marker in text for marker in COMMAVIEW_MARKERS):
        return "ours"
    return "foreign"


def transform(op_root: Path, flavor: str, platform: str = "auto") -> None:
    if flavor not in {"openpilot", "sunnypilot"}:
        fail(f"unsupported flavor: {flavor}")
    ui_root = source_root(op_root)
    install_helper_source(ui_root, flavor)
    transform_ui_state(ui_root / "selfdrive" / "ui" / "ui_state.py")

    for augmented_path in augmented_paths_for_platform(ui_root, platform):
        transform_augmented_road_view(augmented_path)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--op-root", required=True, type=Path)
    parser.add_argument("--flavor", required=True, choices=("openpilot", "sunnypilot"))
    parser.add_argument("--platform", default="auto", choices=PLATFORM_CHOICES)
    parser.add_argument("--classify", nargs="+", metavar="REL",
                        help="print '<class> <rel>' for each file (relative to --op-root) and change nothing")
    args = parser.parse_args(argv)
    if args.classify:
        for rel in args.classify:
            print(f"{classify_target(args.op_root, rel)} {rel}")
        return 0
    transform(args.op_root, args.flavor, args.platform)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
