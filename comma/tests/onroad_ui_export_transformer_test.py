import difflib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import textwrap
import types
import importlib.util
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
TRANSFORMER = REPO_ROOT / "comma" / "scripts" / "transform_onroad_ui_export.py"
SMOKE_SCRIPT = REPO_ROOT / "comma" / "scripts" / "smoke_onroad_ui_export_helper.py"
APPLY_SCRIPT = REPO_ROOT / "comma" / "scripts" / "apply_onroad_ui_export_patch.sh"
REVERT_SCRIPT = REPO_ROOT / "comma" / "scripts" / "revert_onroad_ui_export_patch.sh"
OPENPILOT_TEMPLATE = REPO_ROOT / "comma" / "src" / "commaview_export.openpilot.py"
SUNNYPILOT_TEMPLATE = REPO_ROOT / "comma" / "src" / "commaview_export.sunnypilot.py"

EXPORT_IMPORT = "from openpilot.selfdrive.ui.commaview_export import _CommaViewSocketExporter, COMMAVIEW_RUNTIME_FLAVOR"
EXPORT_INSTALL = "self._commaview_exporter = _CommaViewSocketExporter(COMMAVIEW_RUNTIME_FLAVOR)"
EXPORT_PUBLISH = "self._commaview_exporter.publish(self)"
HELPER_PATH = Path("selfdrive/ui/commaview_export.py")


def write_upstream_tree(
    tmp_path: Path,
    *,
    ui_state: str,
    augmented_road_view: str | None = None,
    augmented_road_relpath: str = "selfdrive/ui/mici/onroad/augmented_road_view.py",
) -> Path:
    op_root = tmp_path / "openpilot"
    ui_dir = op_root / "selfdrive" / "ui"
    ui_dir.mkdir(parents=True)
    (ui_dir / "ui_state.py").write_text(textwrap.dedent(ui_state).lstrip())
    if augmented_road_view is not None:
        augmented_path = op_root / augmented_road_relpath
        augmented_path.parent.mkdir(parents=True, exist_ok=True)
        augmented_path.write_text(textwrap.dedent(augmented_road_view).lstrip())
    return op_root


def run_transformer(op_root: Path, *extra_args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(TRANSFORMER), "--op-root", str(op_root), "--flavor", "openpilot", *extra_args],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


OLD_UI_STATE = """
import time
from cereal import messaging, car, log
from openpilot.common.filter_simple import FirstOrderFilter
from openpilot.common.params import Params
from openpilot.common.swaglog import cloudlog
from openpilot.selfdrive.ui.lib.prime_state import PrimeState
from openpilot.system.ui.lib.application import gui_app
from openpilot.system.hardware import HARDWARE, PC

PARAM_UPDATE_TIME = 5.0

class UIState:
  def __init__(self):
    self._param_update_time = -PARAM_UPDATE_TIME

  def update(self) -> None:
    self.prime_state.start()
    self.sm.update(0)
    self._update_state()
    self._update_status()
    if time.monotonic() - self._param_update_time >= PARAM_UPDATE_TIME:
      self.update_params()
    device.update()

  def _update_state(self) -> None:
    pass
"""


NEW_UI_STATE = """
import time
import threading
from cereal import messaging, car, log
from openpilot.common.filter_simple import FirstOrderFilter
from openpilot.common.params import Params
from openpilot.common.realtime import drop_realtime
from openpilot.common.swaglog import cloudlog
from openpilot.selfdrive.ui.lib.prime_state import PrimeState
from openpilot.system.ui.lib.application import gui_app
from openpilot.system.hardware import HARDWARE, PC

PARAM_UPDATE_TIME = 1 / 5.0

class UIState:
  def __init__(self):
    self._params_thread = None

  def update(self) -> None:
    self.prime_state.start()
    if self._params_thread is None:
      self._params_thread = threading.Thread(target=self._params_refresh_worker, daemon=True)
      self._params_thread.start()

    self.sm.update(0)
    self._update_state()
    self._update_status()
    device.update()

  def _params_refresh_worker(self):
    drop_realtime()
    while True:
      self.update_params()
      time.sleep(PARAM_UPDATE_TIME)

  def _update_state(self) -> None:
    pass
"""


def assert_ui_state_transformed(ui_state_path: Path) -> None:
    text = ui_state_path.read_text()
    assert text.count(EXPORT_IMPORT) == 1
    assert text.count(EXPORT_INSTALL) == 1
    assert text.count(EXPORT_PUBLISH) == 1
    assert text.index("device.update()") < text.index(EXPORT_INSTALL) < text.index(EXPORT_PUBLISH)
    assert 'cloudlog.exception("commaview ui export publish failed")' in text


def test_transformer_handles_old_inline_params_ui_state_layout(tmp_path):
    op_root = write_upstream_tree(tmp_path, ui_state=OLD_UI_STATE)

    result = run_transformer(op_root)

    assert result.returncode == 0, result.stderr
    assert_ui_state_transformed(op_root / "selfdrive" / "ui" / "ui_state.py")


def test_transformer_installs_openpilot_export_helper_template(tmp_path):
    op_root = write_upstream_tree(tmp_path, ui_state=OLD_UI_STATE)

    result = run_transformer(op_root)

    assert result.returncode == 0, result.stderr
    helper_text = (op_root / HELPER_PATH).read_text()
    assert 'COMMAVIEW_RUNTIME_FLAVOR = "OPENPILOT"' in helper_text
    assert "class _CommaViewSocketExporter:" in helper_text


def load_export_template(template_path: Path):
    sys.modules.setdefault("opendbc", types.ModuleType("opendbc"))
    car_module = types.ModuleType("opendbc.car")
    car_module.ACCELERATION_DUE_TO_GRAVITY = 9.81
    sys.modules["opendbc.car"] = car_module
    spec = importlib.util.spec_from_file_location("commaview_export_under_test", template_path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FakeSubMaster:
    def __init__(self, values):
        self.values = values
        self.recv_frame = {key: 10 for key in values}
        self.logMonoTime = {key: 1000 for key in values}
        self.valid = {key: True for key in values}
        self.alive = {key: True for key in values}

    def __getitem__(self, key):
        return self.values[key]


class FakeUiState:
    def __init__(self, values):
        self.started_frame = 1
        self.sm = FakeSubMaster(values)


def export_templates():
    return [
        (OPENPILOT_TEMPLATE, "OPENPILOT"),
        (SUNNYPILOT_TEMPLATE, "SUNNYPILOT"),
    ]


def test_exporter_preserves_top_level_driver_monitoring_schema():
    for template_path, flavor in export_templates():
        module = load_export_template(template_path)
        exporter = module._CommaViewSocketExporter(flavor)
        driver_monitoring = types.SimpleNamespace(
            faceDetected=True,
            isDistracted=True,
            isRHD=True,
            poseYawOffset=-0.12,
            posePitchOffset=0.34,
            poseYawValidCount=12,
            posePitchValidCount=34,
            isLowStd=True,
            isActiveMode=True,
        )
        ui_state = FakeUiState({"driverMonitoringState": driver_monitoring})

        payload = exporter._driver_monitoring_state_payload(ui_state)

        assert payload["faceDetected"] is True
        assert payload["isDistracted"] is True
        assert payload["isRHD"] is True
        assert payload["poseYawOffset"] == -0.12
        assert payload["posePitchOffset"] == 0.34
        assert payload["poseYawValidCount"] == 12
        assert payload["posePitchValidCount"] == 34
        assert payload["isLowStd"] is True
        assert payload["isActiveMode"] is True


def test_exporter_handles_nested_driver_monitoring_schema():
    for template_path, flavor in export_templates():
        module = load_export_template(template_path)
        exporter = module._CommaViewSocketExporter(flavor)
        pose = types.SimpleNamespace(
            pitch=0.11,
            yaw=-0.22,
            pitchCalib=types.SimpleNamespace(offset=0.01, calibratedPercent=88),
            yawCalib=types.SimpleNamespace(offset=-0.02, calibratedPercent=77),
            uncertainty=0.03,
        )
        driver_monitoring = types.SimpleNamespace(
            isRHD=True,
            visionPolicyState=types.SimpleNamespace(
                faceDetected=True,
                isDistracted=True,
                awarenessPercent=65,
                awarenessStep=0.02,
                pose=pose,
            ),
        )
        ui_state = FakeUiState({"driverMonitoringState": driver_monitoring})

        payload = exporter._driver_monitoring_state_payload(ui_state)

        assert payload["faceDetected"] is True
        assert payload["isDistracted"] is True
        assert payload["isRHD"] is True
        assert payload["poseYawOffset"] == -0.02
        assert payload["poseYawValidCount"] == 77


def test_exporter_publish_continues_after_service_payload_error():
    for template_path, flavor in export_templates():
        module = load_export_template(template_path)
        exporter = module._CommaViewSocketExporter(flavor)
        sent = []
        exporter._send_json = lambda service_index, payload: sent.append(service_index)
        for service_index in exporter._payload_builders:
            exporter._payload_builders[service_index] = lambda ui_state: {"exportVersion": 1}
        exporter._payload_builders[module.COMMAVIEW_DRIVER_MONITORING_STATE_SERVICE_INDEX] = (
            lambda ui_state: (_ for _ in ()).throw(AttributeError("schema drift"))
        )
        exporter._offer_payload(module.COMMAVIEW_ONROAD_PROJECTION_SERVICE_INDEX, {"exportVersion": 1})
        exporter.publish(FakeUiState({
            "liveCalibration": object(),
            "roadCameraState": object(),
            "liveParameters": object(),
        }))
        assert exporter.wait_for_idle(1.0)
        exporter.shutdown()

        assert module.COMMAVIEW_DRIVER_MONITORING_STATE_SERVICE_INDEX not in sent
        assert module.COMMAVIEW_DRIVER_STATE_V2_SERVICE_INDEX in sent
        assert module.COMMAVIEW_MODEL_V2_SERVICE_INDEX in sent
        assert module.COMMAVIEW_LIVE_CALIBRATION_SERVICE_INDEX in sent
        assert module.COMMAVIEW_CAR_OUTPUT_SERVICE_INDEX in sent
        assert module.COMMAVIEW_ONROAD_PROJECTION_SERVICE_INDEX in sent


def test_generated_export_helper_smoke_runs_after_transform(tmp_path):
    for flavor in ("openpilot", "sunnypilot"):
        op_root = write_upstream_tree(tmp_path / flavor, ui_state=NEW_UI_STATE, augmented_road_view=AUGMENTED_ROAD_VIEW)

        result = subprocess.run(
            [sys.executable, str(TRANSFORMER), "--op-root", str(op_root), "--flavor", flavor],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        assert result.returncode == 0, result.stderr

        smoke = subprocess.run(
            [sys.executable, str(SMOKE_SCRIPT), str(op_root / HELPER_PATH), flavor.upper()],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

        assert smoke.returncode == 0, smoke.stderr
        smoke_status = json.loads(smoke.stdout)
        assert smoke_status["payloadSmokePassed"] is True
        assert smoke_status["criticalServicesPublishable"] is True
        assert smoke_status["serviceSendCount"] >= 19


def test_transformer_handles_new_background_params_ui_state_layout(tmp_path):
    op_root = write_upstream_tree(tmp_path, ui_state=NEW_UI_STATE)

    result = run_transformer(op_root)

    assert result.returncode == 0, result.stderr
    assert_ui_state_transformed(op_root / "selfdrive" / "ui" / "ui_state.py")


def test_transformer_is_idempotent_for_ui_state(tmp_path):
    op_root = write_upstream_tree(tmp_path, ui_state=NEW_UI_STATE)

    first = run_transformer(op_root)
    assert first.returncode == 0, first.stderr
    ui_state_path = op_root / "selfdrive" / "ui" / "ui_state.py"
    after_first = ui_state_path.read_text()

    second = run_transformer(op_root)
    assert second.returncode == 0, second.stderr

    assert ui_state_path.read_text() == after_first


def test_transformer_fails_when_ui_state_update_lacks_device_update(tmp_path):
    op_root = write_upstream_tree(tmp_path, ui_state=NEW_UI_STATE.replace("    device.update()\n", ""))

    result = run_transformer(op_root)

    assert result.returncode != 0
    assert "device.update" in result.stderr


def test_transformer_fails_when_ui_state_update_has_duplicate_device_update(tmp_path):
    duplicated = NEW_UI_STATE.replace("    device.update()\n", "    device.update()\n    device.update()\n")
    op_root = write_upstream_tree(tmp_path, ui_state=duplicated)

    result = run_transformer(op_root)

    assert result.returncode != 0
    assert "device.update" in result.stderr


AUGMENTED_ROAD_VIEW = """
class AugmentedRoadView:
  def _render(self):
    # Render the base camera view
    super()._render(self._content_rect)

    # Draw all UI overlays
    self._model_renderer.render(self._content_rect)

  def _switch_stream_if_needed(self, sm):
    if sm['selfdriveState'].experimentalMode and WIDE_CAM in self.available_streams:
      target = WIDE_CAM
    else:
      target = ROAD_CAM

    if self.stream_type != target:
      self.switch_stream(target)

  def _update_calibration(self):
    sm = ui_state.sm
    if not (sm.updated["liveCalibration"] and sm.valid['liveCalibration']):
      return

  def _calc_frame_matrix(self, rect):
    is_wide_camera = self.stream_type == WIDE_CAM
    video_transform = make_video_transform()
    calib_transform = make_calib_transform()
    self._cached_matrix = make_cached_matrix()
    self._model_renderer.set_transform(video_transform @ calib_transform)

    return self._cached_matrix
"""


FLAT_AUGMENTED_ROAD_VIEW = """
class AugmentedRoadView:
  def _render(self, rect):
    self._content_rect = rect
    # Render the base camera view
    super()._render(rect)

    # Draw all UI overlays
    self.model_renderer.render(self._content_rect)

  def _switch_stream_if_needed(self, sm):
    if sm['selfdriveState'].experimentalMode and WIDE_CAM in self.available_streams:
      target = WIDE_CAM
    else:
      target = ROAD_CAM

    if self.stream_type != target:
      self.switch_stream(target)

  def _update_calibration(self):
    sm = ui_state.sm
    if not (sm.updated["liveCalibration"] and sm.valid['liveCalibration']):
      return

  def _calc_frame_matrix(self, rect):
    is_wide_camera = self.stream_type == WIDE_CAM
    video_transform = make_video_transform()
    calib_transform = make_calib_transform()
    self._cached_matrix = make_cached_matrix()
    self.model_renderer.set_transform(video_transform @ calib_transform)

    return self._cached_matrix
"""


CAMERA_EXPORT_CALL = "self._update_commaview_camera_export()"
CAMERA_EXPORT_HELPER = "def _update_commaview_camera_export(self):"
PROJECTION_EXPORT_CALL = "exporter.set_onroad_projection("
MODEL_TRANSFORM_ASSIGN = "model_transform = video_transform @ calib_transform"
MODEL_TRANSFORM_SET = "self._model_renderer.set_transform(model_transform)"
FLAT_MODEL_TRANSFORM_SET = "self.model_renderer.set_transform(model_transform)"


def write_augmented_tree(
    tmp_path: Path,
    augmented_road_view: str = AUGMENTED_ROAD_VIEW,
    augmented_road_relpath: str = "selfdrive/ui/mici/onroad/augmented_road_view.py",
) -> Path:
    return write_upstream_tree(
        tmp_path,
        ui_state=NEW_UI_STATE,
        augmented_road_view=augmented_road_view,
        augmented_road_relpath=augmented_road_relpath,
    )


def assert_augmented_transformed(augmented_path: Path) -> None:
    text = augmented_path.read_text()
    assert text.count(CAMERA_EXPORT_CALL) == 1
    assert text.count(CAMERA_EXPORT_HELPER) == 1
    assert text.count(MODEL_TRANSFORM_ASSIGN) == 1
    assert text.count(MODEL_TRANSFORM_SET) + text.count(FLAT_MODEL_TRANSFORM_SET) == 1
    assert text.count(PROJECTION_EXPORT_CALL) == 1
    render_anchor = "super()._render(self._content_rect)" if "super()._render(self._content_rect)" in text else "super()._render(rect)"
    transform_set = MODEL_TRANSFORM_SET if MODEL_TRANSFORM_SET in text else FLAT_MODEL_TRANSFORM_SET
    assert text.index(render_anchor) < text.index(CAMERA_EXPORT_CALL)
    assert text.index(MODEL_TRANSFORM_ASSIGN) < text.index(transform_set) < text.index(PROJECTION_EXPORT_CALL)
    assert "video_frame_matrix=self._cached_matrix" in text
    assert (
        "camera_offset=getattr(self._model_renderer, \"_camera_offset\", 0.0)" in text
        or "camera_offset=getattr(self.model_renderer, \"_camera_offset\", 0.0)" in text
    )


def test_transformer_inserts_augmented_road_camera_and_projection_hooks(tmp_path):
    op_root = write_augmented_tree(tmp_path)

    result = run_transformer(op_root)

    assert result.returncode == 0, result.stderr
    assert_augmented_transformed(op_root / "selfdrive" / "ui" / "mici" / "onroad" / "augmented_road_view.py")


def test_transformer_handles_flat_onroad_augmented_road_view_path(tmp_path):
    op_root = write_augmented_tree(
        tmp_path,
        FLAT_AUGMENTED_ROAD_VIEW,
        augmented_road_relpath="selfdrive/ui/onroad/augmented_road_view.py",
    )

    result = run_transformer(op_root)

    assert result.returncode == 0, result.stderr
    assert_augmented_transformed(op_root / "selfdrive" / "ui" / "onroad" / "augmented_road_view.py")



def test_transformer_platform_tizi_only_patches_flat_onroad_target(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)

    result = run_transformer(op_root, "--platform", "tizi")

    assert result.returncode == 0, result.stderr
    assert_augmented_transformed(op_root / "selfdrive" / "ui" / "onroad" / "augmented_road_view.py")
    assert CAMERA_EXPORT_CALL not in (op_root / "selfdrive" / "ui" / "mici" / "onroad" / "augmented_road_view.py").read_text()


TICI_UNSUPPORTED = "CommaView doesn't support the comma 3 (tici)"


def tree_snapshot(op_root: Path) -> dict[str, str]:
    return {str(path.relative_to(op_root)): path.read_text() for path in sorted(op_root.rglob("*")) if path.is_file() and ".git" not in path.parts}


def test_transformer_refuses_tici_without_touching_the_tree(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    before = tree_snapshot(op_root)

    result = run_transformer(op_root, "--platform", "tici")

    assert result.returncode == 43, result.stderr
    assert TICI_UNSUPPORTED in result.stderr
    assert "comma 3X and comma four are supported" in result.stderr
    assert tree_snapshot(op_root) == before


def fake_device_model(tmp_path: Path, model: str) -> str:
    model_file = tmp_path / "devicetree-model"
    model_file.write_bytes(model.encode() + b"\0")
    return str(model_file)


@pytest.mark.parametrize("platform_args", [("--platform", "tici"), ("--platform", "auto"), ()])
def test_apply_refuses_tici_without_touching_openpilot(tmp_path, platform_args):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)
    state_env = (install_dir / "config" / "onroad-ui-export-patch.env").read_text()
    before = tree_snapshot(op_root)

    applied = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair", *platform_args,
                                   COMMAVIEWD_DEVICE_MODEL_FILE=fake_device_model(tmp_path, "comma tici"))

    assert applied.returncode == 43, applied.stderr
    assert TICI_UNSUPPORTED in applied.stderr
    assert tree_snapshot(op_root) == before
    assert subprocess.run(["git", "status", "--porcelain"], cwd=op_root, text=True, capture_output=True, check=True).stdout == ""
    assert (install_dir / "config" / "onroad-ui-export-patch.env").read_text() == state_env
    assert not (install_dir / "backups").exists()


def test_apply_on_a_comma_3x_still_patches(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)

    applied = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair",
                                   COMMAVIEWD_DEVICE_MODEL_FILE=fake_device_model(tmp_path, "comma tizi"))

    assert applied.returncode == 0, applied.stderr
    assert "ONROAD_UI_EXPORT_UI_PLATFORM=tizi" in (install_dir / "config" / "onroad-ui-export-patch.env").read_text()
    assert_augmented_transformed(op_root / "selfdrive" / "ui" / "onroad" / "augmented_road_view.py")


@pytest.mark.parametrize("platform_args", [("--platform", "tici"), ()])
def test_verify_reports_tici_unsupported(tmp_path, platform_args):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)
    before = tree_snapshot(op_root)

    verified = run_lifecycle_script(REPO_ROOT / "comma" / "scripts" / "verify_onroad_ui_export_patch.sh", install_dir, op_root,
                                    "--json", *platform_args, COMMAVIEWD_DEVICE_MODEL_FILE=fake_device_model(tmp_path, "comma tici"))

    assert verified.returncode == 43, verified.stderr
    status = json.loads(verified.stdout)
    assert status["state"] == "unsupported-device"
    assert status["uiPlatform"] == "tici"
    assert status["deviceModel"] == "tici"
    assert status["patchVerified"] is False
    assert status["repairNeeded"] is False
    assert TICI_UNSUPPORTED in status["reason"]
    assert json.loads((install_dir / "run" / "onroad-ui-export-status.json").read_text())["state"] == "unsupported-device"
    assert tree_snapshot(op_root) == before


def test_transformer_platform_mici_only_patches_mici_target(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)

    result = run_transformer(op_root, "--platform", "mici")

    assert result.returncode == 0, result.stderr
    assert_augmented_transformed(op_root / "selfdrive" / "ui" / "mici" / "onroad" / "augmented_road_view.py")
    assert CAMERA_EXPORT_CALL not in (op_root / "selfdrive" / "ui" / "onroad" / "augmented_road_view.py").read_text()


def test_transformer_handles_nested_openpilot_package_root(tmp_path):
    repo_root = tmp_path / "repo"
    source_parent = tmp_path / "source"
    ui_root = write_upstream_tree(
        source_parent,
        ui_state=NEW_UI_STATE,
        augmented_road_view=FLAT_AUGMENTED_ROAD_VIEW,
        augmented_road_relpath="selfdrive/ui/onroad/augmented_road_view.py",
    )
    repo_root.mkdir()
    nested = repo_root / "openpilot"
    ui_root.rename(nested)

    result = run_transformer(repo_root, "--platform", "tizi")

    assert result.returncode == 0, result.stderr
    assert (nested / HELPER_PATH).exists()
    assert_ui_state_transformed(nested / "selfdrive" / "ui" / "ui_state.py")
    assert_augmented_transformed(nested / "selfdrive" / "ui" / "onroad" / "augmented_road_view.py")


def test_apply_platform_tizi_records_platform_and_patches_flat_target(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)

    applied = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair", "--platform", "tizi")

    assert applied.returncode == 0, applied.stderr
    state_env = (install_dir / "config" / "onroad-ui-export-patch.env").read_text()
    assert "ONROAD_UI_EXPORT_UI_PLATFORM=tizi" in state_env
    assert_augmented_transformed(op_root / "selfdrive" / "ui" / "onroad" / "augmented_road_view.py")
    assert CAMERA_EXPORT_CALL not in (op_root / "selfdrive" / "ui" / "mici" / "onroad" / "augmented_road_view.py").read_text()

    verified = run_lifecycle_script(REPO_ROOT / "comma" / "scripts" / "verify_onroad_ui_export_patch.sh", install_dir, op_root, "--json", "--platform", "tizi")
    assert verified.returncode == 0, verified.stdout
    status = json.loads(verified.stdout)
    assert status["uiPlatform"] == "tizi"
    assert status["selectedAugmentedRoadTargets"] == ["selfdrive/ui/onroad/augmented_road_view.py"]

def test_transformer_is_idempotent_for_augmented_road_view(tmp_path):
    op_root = write_augmented_tree(tmp_path)
    augmented_path = op_root / "selfdrive" / "ui" / "mici" / "onroad" / "augmented_road_view.py"

    first = run_transformer(op_root)
    assert first.returncode == 0, first.stderr
    after_first = augmented_path.read_text()

    second = run_transformer(op_root)
    assert second.returncode == 0, second.stderr

    assert augmented_path.read_text() == after_first


def test_transformer_fails_when_augmented_render_anchor_is_missing(tmp_path):
    op_root = write_augmented_tree(tmp_path, AUGMENTED_ROAD_VIEW.replace("    super()._render(self._content_rect)\n", ""))

    result = run_transformer(op_root)

    assert result.returncode != 0
    assert "super()._render" in result.stderr


def test_transformer_fails_when_augmented_render_anchor_is_duplicated(tmp_path):
    duplicated = AUGMENTED_ROAD_VIEW.replace(
        "    super()._render(self._content_rect)\n",
        "    super()._render(self._content_rect)\n    super()._render(self._content_rect)\n",
    )
    op_root = write_augmented_tree(tmp_path, duplicated)

    result = run_transformer(op_root)

    assert result.returncode != 0
    assert "super()._render" in result.stderr


def test_transformer_fails_when_augmented_transform_anchor_is_missing(tmp_path):
    op_root = write_augmented_tree(
        tmp_path,
        AUGMENTED_ROAD_VIEW.replace("    self._model_renderer.set_transform(video_transform @ calib_transform)\n", ""),
    )

    result = run_transformer(op_root)

    assert result.returncode != 0
    assert "set_transform(video_transform @ calib_transform)" in result.stderr


def test_transformer_fails_when_augmented_transform_anchor_is_duplicated(tmp_path):
    duplicated = AUGMENTED_ROAD_VIEW.replace(
        "    self._model_renderer.set_transform(video_transform @ calib_transform)\n",
        "    self._model_renderer.set_transform(video_transform @ calib_transform)\n    self._model_renderer.set_transform(video_transform @ calib_transform)\n",
    )
    op_root = write_augmented_tree(tmp_path, duplicated)

    result = run_transformer(op_root)

    assert result.returncode != 0
    assert "set_transform(video_transform @ calib_transform)" in result.stderr


def init_git_repo(op_root: Path) -> None:
    subprocess.run(["git", "init"], cwd=op_root, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    subprocess.run(["git", "config", "user.email", "commaview-test@example.invalid"], cwd=op_root, check=True)
    subprocess.run(["git", "config", "user.name", "CommaView Test"], cwd=op_root, check=True)
    subprocess.run(["git", "add", "."], cwd=op_root, check=True)
    subprocess.run(["git", "commit", "-m", "baseline"], cwd=op_root, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def prepare_lifecycle_install_dir(tmp_path: Path, op_root: Path) -> Path:
    install_dir = tmp_path / "commaview"
    (install_dir / "scripts").mkdir(parents=True)
    (install_dir / "src").mkdir(parents=True)
    (install_dir / "config").mkdir(parents=True)
    shutil.copy2(TRANSFORMER, install_dir / "scripts" / "transform_onroad_ui_export.py")
    shutil.copy2(SMOKE_SCRIPT, install_dir / "scripts" / "smoke_onroad_ui_export_helper.py")
    shutil.copy2(APPLY_SCRIPT, install_dir / "scripts" / "apply_onroad_ui_export_patch.sh")
    shutil.copy2(REVERT_SCRIPT, install_dir / "scripts" / "revert_onroad_ui_export_patch.sh")
    shutil.copy2(REPO_ROOT / "comma" / "scripts" / "verify_onroad_ui_export_patch.sh", install_dir / "scripts" / "verify_onroad_ui_export_patch.sh")
    shutil.copy2(OPENPILOT_TEMPLATE, install_dir / "src" / "commaview_export.openpilot.py")
    shutil.copy2(SUNNYPILOT_TEMPLATE, install_dir / "src" / "commaview_export.sunnypilot.py")
    (install_dir / "config" / "onroad-ui-export-patch.env").write_text(
        f"ONROAD_UI_EXPORT_FLAVOR=openpilot\nONROAD_UI_EXPORT_OP_ROOT={op_root}\n"
    )
    return install_dir


def lifecycle_env(install_dir: Path, op_root: Path, **extra: str) -> dict[str, str]:
    return {
        "COMMAVIEWD_INSTALL_DIR": str(install_dir),
        "COMMAVIEWD_OP_ROOT": str(op_root),
        "COMMAVIEWD_SKIP_OPENPILOT_UI_RESTART": "1",
        "COMMAVIEWD_MAINT_LOCK": str(install_dir.parent / "maint.lock"),
        **extra,
    }


def run_lifecycle_script(script: Path, install_dir: Path, op_root: Path, *args: str, **extra_env: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["bash", str(script), *args],
        env={**lifecycle_env(install_dir, op_root, **extra_env)},
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def write_full_augmented_tree(tmp_path: Path) -> Path:
    op_root = write_upstream_tree(tmp_path, ui_state=NEW_UI_STATE)
    mici_path = op_root / "selfdrive" / "ui" / "mici" / "onroad" / "augmented_road_view.py"
    flat_path = op_root / "selfdrive" / "ui" / "onroad" / "augmented_road_view.py"
    mici_path.parent.mkdir(parents=True, exist_ok=True)
    flat_path.parent.mkdir(parents=True, exist_ok=True)
    mici_path.write_text(textwrap.dedent(AUGMENTED_ROAD_VIEW).lstrip())
    flat_path.write_text(textwrap.dedent(FLAT_AUGMENTED_ROAD_VIEW).lstrip())
    return op_root


def test_apply_script_force_repair_resets_dirty_targets_then_reapplies_transformer(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)
    ui_state = op_root / "selfdrive" / "ui" / "ui_state.py"
    dirty_marker = "# user dirty change"
    ui_state.write_text(ui_state.read_text() + f"\n{dirty_marker}\n")

    refused = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root)

    assert refused.returncode == 44
    assert "refusing to modify dirty upstream files without --force-repair" in refused.stderr

    repaired = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair")

    assert repaired.returncode == 0, repaired.stderr
    backup_paths = re.findall(r"backups written to (\S+)", repaired.stderr)
    assert backup_paths
    assert any(
        dirty_marker in (Path(backup_path) / "selfdrive" / "ui" / "ui_state.py").read_text()
        for backup_path in backup_paths
        if (Path(backup_path) / "selfdrive" / "ui" / "ui_state.py").exists()
    )
    assert_ui_state_transformed(ui_state)
    assert_augmented_transformed(op_root / "selfdrive" / "ui" / "mici" / "onroad" / "augmented_road_view.py")
    assert_augmented_transformed(op_root / "selfdrive" / "ui" / "onroad" / "augmented_road_view.py")
    assert (op_root / HELPER_PATH).exists()


def failing_mktemp_path(tmp_path: Path) -> str:
    fakebin = tmp_path / "fakebin"
    fakebin.mkdir()
    fake_mktemp = fakebin / "mktemp"
    fake_mktemp.write_text("#!/bin/sh\necho forced mktemp failure >&2\nexit 77\n")
    fake_mktemp.chmod(0o755)
    return f"{fakebin}:{os.environ['PATH']}"


def failing_git_checkout_path(tmp_path: Path) -> str:
    """git that fails to read a file from HEAD (git cat-file -p), how targets are restored."""
    real_git = shutil.which("git")
    assert real_git
    fakebin = tmp_path / "fakegitbin"
    fakebin.mkdir()
    fake_git = fakebin / "git"
    fake_git.write_text(
        "#!/bin/sh\n"
        "is_cat=0\n"
        "for arg in \"$@\"; do\n"
        "  if [ \"$arg\" = cat-file ]; then is_cat=1; fi\n"
        "  if [ \"$is_cat\" = 1 ] && [ \"$arg\" = -p ]; then\n"
        "    echo forced git cat-file failure >&2\n"
        "    exit 88\n"
        "  fi\n"
        "done\n"
        f"exec {shlex.quote(real_git)} \"$@\"\n"
    )
    fake_git.chmod(0o755)
    return f"{fakebin}:{os.environ['PATH']}"


def failing_git_checkout_after_mutating_path(tmp_path: Path, op_root: Path, relpath: str, replacement: str) -> str:
    real_git = shutil.which("git")
    assert real_git
    fakebin = tmp_path / "fakepartialgitbin"
    fakebin.mkdir()
    fake_git = fakebin / "git"
    fake_git.write_text(
        "#!/bin/sh\n"
        "git_root=\"\"\n"
        "is_checkout=0\n"
        "prev=\"\"\n"
        "for arg in \"$@\"; do\n"
        "  if [ \"$prev\" = -C ]; then git_root=\"$arg\"; fi\n"
        "  if [ \"$arg\" = -p ] && [ \"$prev\" = cat-file ]; then is_checkout=1; fi\n"
        f"  if [ \"$is_checkout\" = 1 ] && [ \"$git_root\" = {shlex.quote(str(op_root))} ] && [ \"$arg\" = {shlex.quote('HEAD:./' + relpath)} ]; then\n"
        f"    printf '%s\\n' {shlex.quote(replacement)} > {shlex.quote(str(op_root / relpath))}\n"
        "    echo forced partial git checkout failure >&2\n"
        "    exit 88\n"
        "  fi\n"
        "  prev=\"$arg\"\n"
        "done\n"
        f"exec {shlex.quote(real_git)} \"$@\"\n"
    )
    fake_git.chmod(0o755)
    return f"{fakebin}:{os.environ['PATH']}"


def failing_rm_helper_path(tmp_path: Path, op_root: Path) -> str:
    real_rm = shutil.which("rm")
    assert real_rm
    helper = op_root / HELPER_PATH
    fakebin = tmp_path / "fakermbin"
    fakebin.mkdir()
    fake_rm = fakebin / "rm"
    fake_rm.write_text(
        "#!/bin/sh\n"
        f"helper={shlex.quote(str(helper))}\n"
        "for arg in \"$@\"; do\n"
        "  if [ \"$arg\" = \"$helper\" ]; then\n"
        "    echo forced rm helper failure >&2\n"
        "    exit 89\n"
        "  fi\n"
        "done\n"
        f"exec {shlex.quote(real_rm)} \"$@\"\n"
    )
    fake_rm.chmod(0o755)
    return f"{fakebin}:{os.environ['PATH']}"


def test_apply_script_force_repair_backup_failure_stops_before_reset(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)
    ui_state = op_root / "selfdrive" / "ui" / "ui_state.py"
    dirty_marker = "# user dirty change before failed backup"
    ui_state.write_text(ui_state.read_text() + f"\n{dirty_marker}\n")

    result = run_lifecycle_script(
        APPLY_SCRIPT,
        install_dir,
        op_root,
        "--force-repair",
        PATH=failing_mktemp_path(tmp_path),
    )

    assert result.returncode != 0
    assert "failed to back up managed onroad UI export transformer targets; refusing force repair" in result.stderr
    assert dirty_marker in ui_state.read_text()
    assert not (op_root / HELPER_PATH).exists()


def test_apply_script_force_repair_restores_backup_when_reset_checkout_partially_fails(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)

    applied = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair")
    assert applied.returncode == 0, applied.stderr
    helper_text = (op_root / HELPER_PATH).read_text()
    ui_state = op_root / "selfdrive" / "ui" / "ui_state.py"
    dirty_marker = "# force repair must restore this dirty managed target"
    ui_state.write_text(ui_state.read_text() + f"\n{dirty_marker}\n")
    dirty_ui_state = ui_state.read_text()

    result = run_lifecycle_script(
        APPLY_SCRIPT,
        install_dir,
        op_root,
        "--force-repair",
        PATH=failing_git_checkout_after_mutating_path(
            tmp_path,
            op_root,
            "selfdrive/ui/ui_state.py",
            "# partially reset by failing checkout",
        ),
    )

    assert result.returncode != 0
    assert "failed to reset managed onroad UI export transformer targets" in result.stderr
    assert "restored managed targets from" in result.stderr
    assert "rollback failed or incomplete" not in result.stderr
    assert ui_state.read_text() == dirty_ui_state
    assert (op_root / HELPER_PATH).read_text() == helper_text


def test_apply_script_transform_backup_failure_stops_before_transform(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)
    ui_state = op_root / "selfdrive" / "ui" / "ui_state.py"
    original_ui_state = ui_state.read_text()

    result = run_lifecycle_script(
        APPLY_SCRIPT,
        install_dir,
        op_root,
        PATH=failing_mktemp_path(tmp_path),
    )

    assert result.returncode != 0
    assert "failed to back up managed onroad UI export transformer targets before transform" in result.stderr
    assert ui_state.read_text() == original_ui_state
    assert not (op_root / HELPER_PATH).exists()


def test_revert_script_removes_helper_and_restores_tracked_transformer_targets(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)
    originals = {
        rel: (op_root / rel).read_text()
        for rel in (
            Path("selfdrive/ui/ui_state.py"),
            Path("selfdrive/ui/mici/onroad/augmented_road_view.py"),
            Path("selfdrive/ui/onroad/augmented_road_view.py"),
        )
    }

    applied = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair")
    assert applied.returncode == 0, applied.stderr
    assert (op_root / HELPER_PATH).exists()

    reverted = run_lifecycle_script(
        REVERT_SCRIPT,
        install_dir,
        op_root,
        COMMAVIEWD_BACKUP_ROOT=str(tmp_path / "backups"),
    )

    assert reverted.returncode == 0, reverted.stderr
    assert not (op_root / HELPER_PATH).exists()
    for rel, text in originals.items():
        assert (op_root / rel).read_text() == text
    status = subprocess.check_output(
        ["git", "status", "--short", "--", "selfdrive/ui/commaview_export.py", "selfdrive/ui/ui_state.py", "selfdrive/ui/mici/onroad/augmented_road_view.py", "selfdrive/ui/onroad/augmented_road_view.py"],
        cwd=op_root,
        text=True,
    )
    assert status == ""


def test_revert_script_backup_failure_stops_before_reset(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)

    applied = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair")
    assert applied.returncode == 0, applied.stderr
    managed_before_revert = {
        rel: (op_root / rel).read_text()
        for rel in (
            HELPER_PATH,
            Path("selfdrive/ui/ui_state.py"),
            Path("selfdrive/ui/mici/onroad/augmented_road_view.py"),
            Path("selfdrive/ui/onroad/augmented_road_view.py"),
        )
    }

    result = run_lifecycle_script(
        REVERT_SCRIPT,
        install_dir,
        op_root,
        PATH=failing_mktemp_path(tmp_path),
        COMMAVIEWD_BACKUP_ROOT=str(tmp_path / "backups"),
    )

    assert result.returncode != 0
    assert "failed to back up managed onroad UI export transformer targets; refusing revert" in result.stderr
    for rel, text in managed_before_revert.items():
        assert (op_root / rel).read_text() == text


def test_revert_script_rolls_back_when_reset_checkout_fails(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)

    applied = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair")
    assert applied.returncode == 0, applied.stderr
    run_dir = install_dir / "run"
    run_dir.mkdir(exist_ok=True)
    state_json = run_dir / "onroad-ui-export-status.json"
    restart_marker = run_dir / "onroad-ui-export-ui-restart-needed"
    state_json.write_text('{"patchVerified": true}\n')
    restart_marker.write_text("pending\n")
    managed_before_revert = {
        rel: (op_root / rel).read_text()
        for rel in (
            HELPER_PATH,
            Path("selfdrive/ui/ui_state.py"),
            Path("selfdrive/ui/mici/onroad/augmented_road_view.py"),
            Path("selfdrive/ui/onroad/augmented_road_view.py"),
        )
    }
    state_env_text = (install_dir / "config" / "onroad-ui-export-patch.env").read_text()

    result = run_lifecycle_script(
        REVERT_SCRIPT,
        install_dir,
        op_root,
        PATH=failing_git_checkout_path(tmp_path),
        COMMAVIEWD_BACKUP_ROOT=str(tmp_path / "backups"),
    )

    assert result.returncode != 0
    assert "failed to restore tracked managed target from HEAD" in result.stderr
    assert "revert failed; restored managed targets" in result.stderr
    for rel, text in managed_before_revert.items():
        assert (op_root / rel).read_text() == text
    assert (install_dir / "config" / "onroad-ui-export-patch.env").read_text() == state_env_text
    assert state_json.read_text() == '{"patchVerified": true}\n'
    assert restart_marker.read_text() == "pending\n"


def test_revert_script_preflight_only_rejects_onroad_without_mutating_targets(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)
    params_dir = tmp_path / "params" / "d"
    params_dir.mkdir(parents=True)
    (params_dir / "IsOnroad").write_text("1")
    backup_root = tmp_path / "backups"

    applied = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair")
    assert applied.returncode == 0, applied.stderr
    managed_after_apply = {
        rel: (op_root / rel).read_text()
        for rel in (
            HELPER_PATH,
            Path("selfdrive/ui/ui_state.py"),
            Path("selfdrive/ui/mici/onroad/augmented_road_view.py"),
            Path("selfdrive/ui/onroad/augmented_road_view.py"),
        )
    }

    result = run_lifecycle_script(
        REVERT_SCRIPT,
        install_dir,
        op_root,
        "--preflight-only",
        COMMAVIEWD_BACKUP_ROOT=str(backup_root),
        COMMAVIEWD_PARAMS_DIR=str(params_dir),
    )

    assert result.returncode == 42
    assert "transformer revert blocked while driving" in result.stderr
    assert not backup_root.exists()
    for rel, text in managed_after_apply.items():
        assert (op_root / rel).read_text() == text


def test_revert_script_preflight_only_success_does_not_mutate_backups_state_or_restart(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)
    params_dir = tmp_path / "params" / "d"
    params_dir.mkdir(parents=True)
    (params_dir / "IsOnroad").write_text("0")
    backup_root = tmp_path / "backups"

    applied = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair")
    assert applied.returncode == 0, applied.stderr
    run_dir = install_dir / "run"
    run_dir.mkdir(exist_ok=True)
    state_json = run_dir / "onroad-ui-export-status.json"
    restart_marker = run_dir / "onroad-ui-export-ui-restart-needed"
    state_json.write_text('{"patchVerified": true}\n')
    restart_marker.write_text("pending\n")
    managed_after_apply = {
        rel: (op_root / rel).read_text()
        for rel in (
            HELPER_PATH,
            Path("selfdrive/ui/ui_state.py"),
            Path("selfdrive/ui/mici/onroad/augmented_road_view.py"),
            Path("selfdrive/ui/onroad/augmented_road_view.py"),
        )
    }
    state_env_text = (install_dir / "config" / "onroad-ui-export-patch.env").read_text()

    result = run_lifecycle_script(
        REVERT_SCRIPT,
        install_dir,
        op_root,
        "--preflight-only",
        COMMAVIEWD_BACKUP_ROOT=str(backup_root),
        COMMAVIEWD_PARAMS_DIR=str(params_dir),
    )

    assert result.returncode == 0, result.stderr
    assert not backup_root.exists()
    for rel, text in managed_after_apply.items():
        assert (op_root / rel).read_text() == text
    assert (install_dir / "config" / "onroad-ui-export-patch.env").read_text() == state_env_text
    assert state_json.read_text() == '{"patchVerified": true}\n'
    assert restart_marker.read_text() == "pending\n"


def test_apply_script_rolls_back_partial_transformer_failure(tmp_path):
    broken_augmented = AUGMENTED_ROAD_VIEW.replace(
        "    self._model_renderer.set_transform(video_transform @ calib_transform)\n",
        "",
    )
    op_root = write_augmented_tree(tmp_path, broken_augmented)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)
    ui_state = op_root / "selfdrive" / "ui" / "ui_state.py"
    augmented_path = op_root / "selfdrive" / "ui" / "mici" / "onroad" / "augmented_road_view.py"
    original_ui_state = ui_state.read_text()
    original_augmented = augmented_path.read_text()

    result = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root)

    assert result.returncode != 0
    assert "transformer failed; restored managed targets" in result.stderr
    assert ui_state.read_text() == original_ui_state
    assert augmented_path.read_text() == original_augmented
    assert not (op_root / HELPER_PATH).exists()
    status = subprocess.check_output(
        ["git", "status", "--short", "--", "selfdrive/ui/commaview_export.py", "selfdrive/ui/ui_state.py", "selfdrive/ui/mici/onroad/augmented_road_view.py"],
        cwd=op_root,
        text=True,
    )
    assert status == ""


def test_apply_script_rolls_back_when_post_transform_verify_fails(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)
    verify_script = install_dir / "scripts" / "verify_onroad_ui_export_patch.sh"
    verify_script.write_text("#!/usr/bin/env bash\necho forced verify failure >&2\nexit 77\n")
    verify_script.chmod(0o755)
    managed_paths = [
        Path("selfdrive/ui/ui_state.py"),
        Path("selfdrive/ui/mici/onroad/augmented_road_view.py"),
        Path("selfdrive/ui/onroad/augmented_road_view.py"),
    ]
    originals = {rel: (op_root / rel).read_text() for rel in managed_paths}
    original_state_env = (install_dir / "config" / "onroad-ui-export-patch.env").read_text()

    result = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root)

    assert result.returncode == 77
    assert "forced verify failure" in result.stderr
    assert "verification failed; restored managed targets" in result.stderr
    for rel, original in originals.items():
        assert (op_root / rel).read_text() == original
    assert not (op_root / HELPER_PATH).exists()
    assert (install_dir / "config" / "onroad-ui-export-patch.env").read_text() == original_state_env
    status = subprocess.check_output(
        [
            "git",
            "status",
            "--short",
            "--",
            "selfdrive/ui/commaview_export.py",
            "selfdrive/ui/ui_state.py",
            "selfdrive/ui/mici/onroad/augmented_road_view.py",
            "selfdrive/ui/onroad/augmented_road_view.py",
        ],
        cwd=op_root,
        text=True,
    )
    assert status == ""


def test_apply_script_attempts_rollback_restore_when_reset_fails_after_transformer_failure(tmp_path):
    broken_augmented = AUGMENTED_ROAD_VIEW.replace(
        "    self._model_renderer.set_transform(video_transform @ calib_transform)\n",
        "",
    )
    op_root = write_augmented_tree(tmp_path, broken_augmented)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)
    ui_state = op_root / "selfdrive" / "ui" / "ui_state.py"
    augmented_path = op_root / "selfdrive" / "ui" / "mici" / "onroad" / "augmented_road_view.py"
    original_ui_state = ui_state.read_text()
    original_augmented = augmented_path.read_text()

    result = run_lifecycle_script(
        APPLY_SCRIPT,
        install_dir,
        op_root,
        PATH=failing_git_checkout_path(tmp_path),
    )

    assert result.returncode != 0
    assert "WARN: failed to restore tracked managed target from HEAD" in result.stderr
    assert "WARN: reset of managed targets had errors before rollback restore" in result.stderr
    assert "transformer failed; restored managed targets" in result.stderr
    assert "transformer failed and rollback failed" not in result.stderr
    assert ui_state.read_text() == original_ui_state
    assert augmented_path.read_text() == original_augmented
    assert not (op_root / HELPER_PATH).exists()


def test_apply_script_reports_incomplete_rollback_when_absent_helper_remove_fails(tmp_path):
    broken_augmented = AUGMENTED_ROAD_VIEW.replace(
        "    self._model_renderer.set_transform(video_transform @ calib_transform)\n",
        "",
    )
    op_root = write_augmented_tree(tmp_path, broken_augmented)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)

    result = run_lifecycle_script(
        APPLY_SCRIPT,
        install_dir,
        op_root,
        PATH=failing_rm_helper_path(tmp_path, op_root),
    )

    assert result.returncode != 0
    assert "WARN: failed to remove untracked managed target: selfdrive/ui/commaview_export.py" in result.stderr
    assert "WARN: failed to remove managed rollback target absent from backup: selfdrive/ui/commaview_export.py" in result.stderr
    assert "WARN: managed rollback incomplete; dirty targets remain:" in result.stderr
    assert "transformer failed and rollback failed or incomplete" in result.stderr
    assert "transformer failed; restored managed targets" not in result.stderr
    assert (op_root / HELPER_PATH).exists()
    status = subprocess.check_output(
        ["git", "status", "--short", "--", "selfdrive/ui/commaview_export.py"],
        cwd=op_root,
        text=True,
    )
    assert "selfdrive/ui/commaview_export.py" in status


def test_apply_script_force_repair_and_transform_failure_use_distinct_backups(tmp_path):
    broken_augmented = AUGMENTED_ROAD_VIEW.replace(
        "    self._model_renderer.set_transform(video_transform @ calib_transform)\n",
        "",
    )
    op_root = write_augmented_tree(tmp_path, broken_augmented)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)
    ui_state = op_root / "selfdrive" / "ui" / "ui_state.py"
    ui_state.write_text(ui_state.read_text() + "\n# dirty local user change\n")

    fakebin = tmp_path / "fakebin"
    fakebin.mkdir()
    fake_date = fakebin / "date"
    fake_date.write_text("#!/bin/sh\necho 20260526-132500\n")
    fake_date.chmod(0o755)

    result = run_lifecycle_script(
        APPLY_SCRIPT,
        install_dir,
        op_root,
        "--force-repair",
        PATH=f"{fakebin}:{os.environ['PATH']}",
    )

    assert result.returncode != 0
    backup_paths = re.findall(r"backups written to (\S+)", result.stderr)
    restore_paths = re.findall(r"restored managed targets from (\S+)", result.stderr)
    assert backup_paths
    assert restore_paths
    all_paths = backup_paths + restore_paths
    assert len(set(all_paths)) >= 2, result.stderr


def test_revert_script_writes_backup_outside_install_tree(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)
    backup_root = tmp_path / "surviving-backups"

    applied = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair")
    assert applied.returncode == 0, applied.stderr

    reverted = run_lifecycle_script(
        REVERT_SCRIPT,
        install_dir,
        op_root,
        COMMAVIEWD_BACKUP_ROOT=str(backup_root),
    )

    assert reverted.returncode == 0, reverted.stderr
    shutil.rmtree(install_dir)
    backups = list((backup_root / "onroad-ui-export-revert").glob("*"))
    assert backups
    assert any((backup / "selfdrive" / "ui" / "ui_state.py").exists() for backup in backups)


def test_apply_and_verify_accept_starpilot_fork_remote_as_openpilot_flavor(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    subprocess.run(["git", "remote", "add", "origin", "https://github.com/J3DOK3DO/openpilot.git"], cwd=op_root, check=True)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)

    applied = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair")

    assert applied.returncode == 0, applied.stderr
    assert_ui_state_transformed(op_root / "selfdrive" / "ui" / "ui_state.py")
    state_env = (install_dir / "config" / "onroad-ui-export-patch.env").read_text()
    assert "ONROAD_UI_EXPORT_FLAVOR=openpilot" in state_env
    assert "ONROAD_UI_EXPORT_UPSTREAM_REMOTE=https://github.com/J3DOK3DO/openpilot.git" in state_env

    verified = run_lifecycle_script(
        REPO_ROOT / "comma" / "scripts" / "verify_onroad_ui_export_patch.sh",
        install_dir,
        op_root,
        "--json",
    )
    assert verified.returncode == 0, verified.stdout
    status = json.loads(verified.stdout)
    assert status["patchVerified"] is True
    assert status["flavor"] == "openpilot"


def test_apply_accepts_starpilot_upstream_ssh_remote_as_openpilot_flavor(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    subprocess.run(["git", "remote", "add", "origin", "git@github.com:firestar5683/StarPilot.git"], cwd=op_root, check=True)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)

    result = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair")

    assert result.returncode == 0, result.stderr
    assert "ONROAD_UI_EXPORT_FLAVOR=openpilot" in (install_dir / "config" / "onroad-ui-export-patch.env").read_text()


def test_apply_rejects_starpilot_lookalike_host(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    subprocess.run(["git", "remote", "add", "origin", "https://evilgithub.com/J3DOK3DO/openpilot.git"], cwd=op_root, check=True)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)

    result = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root)

    assert result.returncode == 1
    assert "unsupported upstream remote" in result.stderr


def test_verify_rejects_starpilot_lookalike_host(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    subprocess.run(["git", "remote", "add", "origin", "https://evilgithub.com/firestar5683/StarPilot.git"], cwd=op_root, check=True)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)

    result = run_lifecycle_script(
        REPO_ROOT / "comma" / "scripts" / "verify_onroad_ui_export_patch.sh",
        install_dir,
        op_root,
        "--json",
    )

    assert result.returncode == 1
    assert "unsupported upstream remote" in result.stdout


def test_apply_accepts_starpilot_remote_with_git_trailing_slash(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    subprocess.run(["git", "remote", "add", "origin", "https://github.com/J3DOK3DO/openpilot.git/"], cwd=op_root, check=True)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)

    result = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair")

    assert result.returncode == 0, result.stderr
    assert "ONROAD_UI_EXPORT_FLAVOR=openpilot" in (install_dir / "config" / "onroad-ui-export-patch.env").read_text()


def test_verify_accepts_starpilot_remote_with_git_trailing_slash(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    subprocess.run(["git", "remote", "add", "origin", "https://github.com/firestar5683/StarPilot.git/"], cwd=op_root, check=True)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)

    applied = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair")
    assert applied.returncode == 0, applied.stderr

    result = run_lifecycle_script(
        REPO_ROOT / "comma" / "scripts" / "verify_onroad_ui_export_patch.sh",
        install_dir,
        op_root,
        "--json",
    )

    assert result.returncode == 0, result.stdout
    status = json.loads(result.stdout)
    assert status["patchVerified"] is True
    assert status["flavor"] == "openpilot"


def test_apply_script_accepts_sunnypilot_openpilot_remote(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    subprocess.run(["git", "remote", "add", "origin", "https://github.com/sunnypilot/openpilot.git"], cwd=op_root, check=True)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)

    result = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair")

    assert result.returncode == 0, result.stderr
    assert_ui_state_transformed(op_root / "selfdrive" / "ui" / "ui_state.py")
    assert "ONROAD_UI_EXPORT_FLAVOR=sunnypilot" in (install_dir / "config" / "onroad-ui-export-patch.env").read_text()


def test_apply_records_upstream_head_and_verify_reports_matching_checkout(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    subprocess.run(["git", "remote", "add", "origin", "https://github.com/commaai/openpilot.git"], cwd=op_root, check=True)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)
    expected_head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=op_root, text=True).strip()

    applied = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair")

    assert applied.returncode == 0, applied.stderr
    state_env = (install_dir / "config" / "onroad-ui-export-patch.env").read_text()
    assert f"ONROAD_UI_EXPORT_UPSTREAM_HEAD={expected_head}" in state_env

    verified = run_lifecycle_script(REPO_ROOT / "comma" / "scripts" / "verify_onroad_ui_export_patch.sh", install_dir, op_root, "--json")

    assert verified.returncode == 0, verified.stdout
    status = json.loads(verified.stdout)
    assert status["patchVerified"] is True
    assert status["upstreamHead"] == expected_head
    assert status["appliedUpstreamHead"] == expected_head
    assert status["upstreamChangedSinceApply"] is False


def test_verify_requests_repair_when_upstream_head_changed_since_apply(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    subprocess.run(["git", "remote", "add", "origin", "https://github.com/commaai/openpilot.git"], cwd=op_root, check=True)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)
    applied = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair")
    assert applied.returncode == 0, applied.stderr
    applied_head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=op_root, text=True).strip()
    (op_root / "CHANGELOG.md").write_text("upstream changed\n")
    subprocess.run(["git", "add", "CHANGELOG.md"], cwd=op_root, check=True)
    subprocess.run(["git", "commit", "-m", "upstream update"], cwd=op_root, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    current_head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=op_root, text=True).strip()

    result = run_lifecycle_script(REPO_ROOT / "comma" / "scripts" / "verify_onroad_ui_export_patch.sh", install_dir, op_root, "--json")

    assert result.returncode == 1
    status = json.loads(result.stdout)
    assert status["patchVerified"] is False
    assert status["repairNeeded"] is True
    assert status["state"] == "repair-needed"
    assert status["upstreamHead"] == current_head
    assert status["appliedUpstreamHead"] == applied_head
    assert status["upstreamChangedSinceApply"] is True
    assert "upstream checkout changed since patch was applied" in status["reason"]


def test_apply_script_rejects_unsupported_upstream_fork_even_with_state_flavor(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    subprocess.run(["git", "remote", "add", "origin", "https://github.com/example/openpilot.git"], cwd=op_root, check=True)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)

    result = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root)

    assert result.returncode == 1
    assert "unsupported upstream remote" in result.stderr
    assert "validated StarPilot remotes" in result.stderr


def test_verify_script_rejects_unsupported_upstream_fork_even_with_state_flavor(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    subprocess.run(["git", "remote", "add", "origin", "git@github.com:example/sunnypilot.git"], cwd=op_root, check=True)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)

    result = run_lifecycle_script(REPO_ROOT / "comma" / "scripts" / "verify_onroad_ui_export_patch.sh", install_dir, op_root, "--json")

    assert result.returncode == 1
    assert "unsupported upstream remote" in result.stdout
    assert "validated StarPilot remotes" in result.stdout


def test_verify_script_fails_when_existing_flat_augmented_road_target_is_stale(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)
    flat_augmented_path = op_root / "selfdrive" / "ui" / "onroad" / "augmented_road_view.py"

    applied = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair")
    assert applied.returncode == 0, applied.stderr
    assert_augmented_transformed(op_root / "selfdrive" / "ui" / "mici" / "onroad" / "augmented_road_view.py")
    assert_augmented_transformed(flat_augmented_path)

    subprocess.run(
        ["git", "checkout", "HEAD", "--", "selfdrive/ui/onroad/augmented_road_view.py"],
        cwd=op_root,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    result = run_lifecycle_script(REPO_ROOT / "comma" / "scripts" / "verify_onroad_ui_export_patch.sh", install_dir, op_root, "--json")

    assert result.returncode == 1
    status = json.loads(result.stdout)
    assert status["patchVerified"] is False
    assert status["repairNeeded"] is True


EXPECTED_OPENPILOT_ONLY_LINES = [
    'COMMAVIEW_RUNTIME_FLAVOR = "OPENPILOT"',
]
EXPECTED_SUNNYPILOT_ONLY_LINES = [
    'COMMAVIEW_RUNTIME_FLAVOR = "SUNNYPILOT"',
]


def test_flavor_templates_differ_only_by_known_flavor_hunks():
    openpilot = OPENPILOT_TEMPLATE.read_text().splitlines()
    sunnypilot = SUNNYPILOT_TEMPLATE.read_text().splitlines()
    opcodes = [op for op in difflib.SequenceMatcher(a=openpilot, b=sunnypilot, autojunk=False).get_opcodes() if op[0] != "equal"]
    openpilot_only = [line for _, i1, i2, _, _ in opcodes for line in openpilot[i1:i2]]
    sunnypilot_only = [line for _, _, _, j1, j2 in opcodes for line in sunnypilot[j1:j2]]
    # The two templates are the same exporter: sunnypilot-only exports are gated on the flavor constant and
    # on the sunnypilot services being subscribed, so only the constant may differ.
    assert openpilot_only == EXPECTED_OPENPILOT_ONLY_LINES
    assert sunnypilot_only == EXPECTED_SUNNYPILOT_ONLY_LINES
    assert len(opcodes) == 1


def write_car_schema(op_root: Path) -> Path:
    # The fields the status reads, at their numbers in every openpilot schema (carFingerprint @1, carVin @38).
    fields = ["  brand @0 :Text;", "  carFingerprint @1 :Text;"]
    fields += [f"  unused{n} @{n} :Bool;" for n in range(2, 38)]
    fields += ["  carVin @38 :Text;"]
    schema = op_root / "opendbc_repo" / "opendbc" / "car" / "car.capnp"
    schema.parent.mkdir(parents=True, exist_ok=True)
    schema.write_text("@0xd3c6d7a2f3b5e8a1;\nstruct CarParams {\n" + "\n".join(fields) + "\n}\n")
    return schema


def test_verify_reports_the_car_from_car_params_persistent(tmp_path):
    capnp = pytest.importorskip("capnp")
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)
    (install_dir / "run").mkdir(exist_ok=True)
    schema = write_car_schema(op_root)
    capnp.remove_import_hook()
    car = capnp.load(str(schema))
    params = tmp_path / "params"
    params.mkdir()
    (params / "CarParamsPersistent").write_bytes(
        car.CarParams.new_message(brand="toyota", carFingerprint="TOYOTA_RAV4_TSS2", carVin="2T3ZZZ").to_bytes())
    verify = REPO_ROOT / "comma" / "scripts" / "verify_onroad_ui_export_patch.sh"

    status = json.loads(run_lifecycle_script(verify, install_dir, op_root, "--json", COMMAVIEWD_TEST_PARAMS_DIR=str(params)).stdout)

    assert status["carFingerprint"] == "TOYOTA_RAV4_TSS2"
    assert status["carVin"] == "2T3ZZZ"
    # Cached until the param changes, then read again.
    assert json.loads((install_dir / "run" / "car-identity.json").read_text())["identity"]["carFingerprint"] == "TOYOTA_RAV4_TSS2"
    (params / "CarParamsPersistent").write_bytes(car.CarParams.new_message(carFingerprint="HONDA_CIVIC_BOSCH").to_bytes() + b"\0" * 8)
    status = json.loads(run_lifecycle_script(verify, install_dir, op_root, "--json", COMMAVIEWD_TEST_PARAMS_DIR=str(params)).stdout)
    assert status["carFingerprint"] == "HONDA_CIVIC_BOSCH"


def test_verify_without_car_params_reports_no_car(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)
    verify = REPO_ROOT / "comma" / "scripts" / "verify_onroad_ui_export_patch.sh"
    status = json.loads(run_lifecycle_script(verify, install_dir, op_root, "--json", COMMAVIEWD_TEST_PARAMS_DIR=str(tmp_path / "none")).stdout)
    assert "carFingerprint" not in status


@pytest.mark.parametrize(
    "remote",
    [
        "https://github.com/J3DOK3DO/openpilot.git",
        "git@github.com:J3DOK3DO/openpilot.git",
        "ssh://git@github.com/J3DOK3DO/openpilot.git",
        "https://github.com/firestar5683/StarPilot.git",
        "git@github.com:firestar5683/StarPilot.git",
        "ssh://git@github.com/firestar5683/StarPilot.git",
    ],
)
def test_apply_and_verify_accept_all_validated_starpilot_remote_forms(tmp_path, remote):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    subprocess.run(["git", "remote", "add", "origin", remote], cwd=op_root, check=True)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)

    applied = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair")
    assert applied.returncode == 0, applied.stderr
    assert "ONROAD_UI_EXPORT_FLAVOR=openpilot" in (install_dir / "config" / "onroad-ui-export-patch.env").read_text()

    verified = run_lifecycle_script(
        REPO_ROOT / "comma" / "scripts" / "verify_onroad_ui_export_patch.sh",
        install_dir,
        op_root,
        "--json",
    )
    assert verified.returncode == 0, verified.stdout
    status = json.loads(verified.stdout)
    assert status["patchVerified"] is True
    assert status["flavor"] == "openpilot"
