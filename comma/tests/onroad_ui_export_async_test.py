import importlib.util
import json
import socket as socket_lib
import sys
import threading
import time
import types
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[2]
TEMPLATES = (
  (REPO_ROOT / "comma" / "src" / "commaview_export.openpilot.py", "OPENPILOT"),
  (REPO_ROOT / "comma" / "src" / "commaview_export.sunnypilot.py", "SUNNYPILOT"),
  (REPO_ROOT / "comma" / "src" / "commaview_export.starpilot.py", "STARPILOT"),
)

WIRE_INDEXES = {
  "COMMAVIEW_UI_STATE_ONROAD_SERVICE_INDEX": 0,
  "COMMAVIEW_SELFDRIVE_STATE_SERVICE_INDEX": 1,
  "COMMAVIEW_CAR_STATE_SERVICE_INDEX": 2,
  "COMMAVIEW_CONTROLS_STATE_SERVICE_INDEX": 3,
  "COMMAVIEW_ONROAD_EVENTS_SERVICE_INDEX": 4,
  "COMMAVIEW_DRIVER_MONITORING_STATE_SERVICE_INDEX": 5,
  "COMMAVIEW_DRIVER_STATE_V2_SERVICE_INDEX": 6,
  "COMMAVIEW_MODEL_V2_SERVICE_INDEX": 7,
  "COMMAVIEW_RADAR_STATE_SERVICE_INDEX": 8,
  "COMMAVIEW_LIVE_CALIBRATION_SERVICE_INDEX": 9,
  "COMMAVIEW_CAR_OUTPUT_SERVICE_INDEX": 10,
  "COMMAVIEW_CAR_CONTROL_SERVICE_INDEX": 11,
  "COMMAVIEW_LIVE_PARAMETERS_SERVICE_INDEX": 12,
  "COMMAVIEW_LONGITUDINAL_PLAN_SERVICE_INDEX": 13,
  "COMMAVIEW_CAR_PARAMS_SERVICE_INDEX": 14,
  "COMMAVIEW_DEVICE_STATE_SERVICE_INDEX": 15,
  "COMMAVIEW_ROAD_CAMERA_STATE_SERVICE_INDEX": 16,
  "COMMAVIEW_PANDA_STATES_SUMMARY_SERVICE_INDEX": 17,
  "COMMAVIEW_ONROAD_PROJECTION_SERVICE_INDEX": 18,
  "COMMAVIEW_WIDE_ROAD_CAMERA_STATE_SERVICE_INDEX": 19,
}


def load_exporter(path: Path):
  sys.modules.setdefault("opendbc", types.ModuleType("opendbc"))
  car_module = types.ModuleType("opendbc.car")
  car_module.ACCELERATION_DUE_TO_GRAVITY = 9.81
  sys.modules["opendbc.car"] = car_module
  spec = importlib.util.spec_from_file_location(f"commaview_export_{path.stem}_{time.time_ns()}", path)
  assert spec is not None and spec.loader is not None
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


class FakeSubMaster:
  def __init__(self, values):
    self.values = values
    self.recv_frame = {key: 10 for key in values}
    self.logMonoTime = {key: 1000 + offset for offset, key in enumerate(values)}
    self.valid = {key: True for key in values}
    self.alive = {key: True for key in values}
    self.updated = {key: True for key in values}

  def __getitem__(self, key):
    return self.values[key]


class FakeUiState:
  def __init__(self, values):
    self.started_frame = 1
    self.sm = FakeSubMaster(values)
    self.CP = types.SimpleNamespace(maxLateralAccel=3.0)


def alias_values(current: bool):
  calibration = "extrinsicsCalibration" if current else "liveCalibration"
  road_camera = "narrowRoadCameraState" if current else "roadCameraState"
  vehicle_parameters = "vehicleParameters" if current else "liveParameters"
  return {
    calibration: types.SimpleNamespace(
      rpyCalib=[0.1, 0.2, 0.3],
      height=[1.22],
      calStatus=2,
      calPerc=87,
      wideFromDeviceEuler=[0.4, 0.5, 0.6],
    ),
    road_camera: types.SimpleNamespace(sensor="ar0231", frameId=4242, timestampEof=9999),
    vehicle_parameters: types.SimpleNamespace(roll=0.125),
  }


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_wire_service_indexes_remain_stable(template, flavor):
  module = load_exporter(template)
  assert {name: getattr(module, name) for name in WIRE_INDEXES} == WIRE_INDEXES


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_json_encoder_falls_back_to_stdlib_and_preserves_compact_unicode(template, flavor, monkeypatch):
  module = load_exporter(template)
  monkeypatch.setattr(module, "_orjson", None)

  assert module._encode_json({"label": "café", "ok": True}) == b'{"label":"caf\xc3\xa9","ok":true}'


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_json_encoder_uses_orjson_when_available(template, flavor, monkeypatch):
  module = load_exporter(template)
  calls = []

  class FakeOrjson:
    @staticmethod
    def dumps(payload):
      calls.append(payload)
      return b'{"fast":true}'

  monkeypatch.setattr(module, "_orjson", FakeOrjson())

  assert module._encode_json({"fast": True}) == b'{"fast":true}'
  assert calls == [{"fast": True}]


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_release_and_current_aliases_emit_equivalent_wire_semantics(template, flavor):
  module = load_exporter(template)
  legacy_exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  current_exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  legacy = FakeUiState(alias_values(current=False))
  current = FakeUiState(alias_values(current=True))

  assert legacy_exporter._live_calibration_payload(legacy) == current_exporter._live_calibration_payload(current)
  assert legacy_exporter._road_camera_state_payload(legacy) == current_exporter._road_camera_state_payload(current)
  assert legacy_exporter._live_parameters_payload(legacy) == current_exporter._live_parameters_payload(current)
  assert module.COMMAVIEW_LIVE_CALIBRATION_SERVICE_INDEX == 9
  assert module.COMMAVIEW_LIVE_PARAMETERS_SERVICE_INDEX == 12
  assert module.COMMAVIEW_ROAD_CAMERA_STATE_SERVICE_INDEX == 16


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_current_vehicle_parameters_support_angle_control_torque(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  values = alias_values(current=True)
  values.update({
    "controlsState": types.SimpleNamespace(
      lateralControlState=types.SimpleNamespace(which=lambda: "angleState"),
      curvature=0.02,
      desiredCurvature=0.03,
    ),
    "carState": types.SimpleNamespace(vEgo=10.0),
    "carControl": types.SimpleNamespace(latActive=True),
  })

  torque = module._torque_bar_value(FakeUiState(values), exporter._service_resolver)

  assert torque == pytest.approx(0.795625, abs=1e-4)


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_current_projection_uses_narrow_camera_and_extrinsics_metadata(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  values = alias_values(current=True)
  values.update({
    "wideRoadCameraState": types.SimpleNamespace(frameId=5252, timestampEof=8888),
    "modelV2": types.SimpleNamespace(frameId=6262, timestampEof=7777),
  })
  ui_state = FakeUiState(values)

  exporter.set_onroad_projection(
    ui_state,
    "road",
    types.SimpleNamespace(x=1, y=2, width=3, height=4),
    [1.0] * 9,
    [2.0] * 9,
  )

  payload = exporter._pending_payload(module.COMMAVIEW_ONROAD_PROJECTION_SERVICE_INDEX)
  assert payload["roadFrameId"] == 4242
  assert payload["roadTimestampEof"] == 9999
  assert payload["liveCalibrationLogMonoTime"] == ui_state.sm.logMonoTime["extrinsicsCalibration"]
  assert payload["logMonoTime"] == max(ui_state.sm.logMonoTime.values())


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_missing_semantic_alias_fails_explicitly(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)

  with pytest.raises(KeyError, match="calibration.*extrinsicsCalibration.*liveCalibration"):
    exporter._service_resolver.resolve(FakeUiState({}), "calibration")


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_publish_path_performs_no_json_or_socket_io_on_calling_thread(template, flavor, monkeypatch):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor)
  ui_thread = threading.get_ident()
  violations = []
  original_dumps = json.dumps

  def guarded_dumps(*args, **kwargs):
    if threading.get_ident() == ui_thread:
      violations.append("json.dumps")
    return original_dumps(*args, **kwargs)

  class GuardedSocket:
    def __init__(self, *args, **kwargs):
      if threading.get_ident() == ui_thread:
        violations.append("socket.socket")
      raise OSError("receiver absent")

  monkeypatch.setattr(module.json, "dumps", guarded_dumps)
  monkeypatch.setattr(module.socket, "socket", GuardedSocket)
  values = alias_values(current=True)
  for service_index, _, _ in exporter._service_specs:
    exporter._payload_builders[service_index] = lambda _ui_state: {"exportVersion": 1}

  ui_state = FakeUiState(values)
  exporter.set_onroad_projection(ui_state, "road", types.SimpleNamespace(), [1.0] * 9, [1.0] * 9)
  exporter.publish(ui_state)
  assert exporter.wait_for_idle(1.0)
  exporter.shutdown()

  assert violations == []


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_latest_value_mailbox_is_bounded_and_drops_stale_generations(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)

  started = time.perf_counter()
  for generation in range(10_000):
    exporter._offer_payload(7, {"generation": generation})
    exporter._offer_payload(8, {"generation": generation})
  elapsed = time.perf_counter() - started

  assert len(exporter._pending) == 2
  assert exporter._pending_payload(7) == {"generation": 9_999}
  assert exporter.stats()["maxPending"] == 2
  assert exporter.stats()["overwritten"] == 19_998
  assert elapsed < 0.25


@pytest.mark.parametrize("receiver_mode", ("never-reads", "slow-reader", "disconnect-mid-frame"))
@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_socket_backpressure_cannot_block_latest_value_offer(template, flavor, receiver_mode):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor)
  sender, receiver = socket_lib.socketpair()
  sender.setsockopt(socket_lib.SOL_SOCKET, socket_lib.SO_SNDBUF, 4096)
  sender.settimeout(0.05)
  exporter._sock = sender
  stop_reader = threading.Event()
  reader = None

  if receiver_mode == "disconnect-mid-frame":
    receiver.close()
  elif receiver_mode == "slow-reader":
    def read_slowly():
      while not stop_reader.is_set():
        try:
          if not receiver.recv(256):
            return
        except OSError:
          return
        time.sleep(0.005)
    reader = threading.Thread(target=read_slowly, daemon=True)
    reader.start()

  oversized = {"generation": 0, "blob": "x" * 1_000_000}
  exporter._offer_payload(7, oversized)
  time.sleep(0.01)
  started = time.perf_counter()
  for generation in range(1, 1_001):
    exporter._offer_payload(7, {"generation": generation, "blob": oversized["blob"]})
  offer_elapsed = time.perf_counter() - started

  assert offer_elapsed < 0.1
  assert exporter.stats()["pending"] <= 1
  assert exporter.stats()["maxPending"] <= 1
  assert exporter.wait_for_idle(2.0)
  exporter.shutdown()
  stop_reader.set()
  try:
    receiver.close()
  except OSError:
    pass
  if reader is not None:
    reader.join(0.2)


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_worker_survives_serialization_error_and_sends_next_service(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor)
  packets = []

  class SinkSocket:
    def sendall(self, packet):
      packets.append(packet)

    def close(self):
      pass

  exporter._sock = SinkSocket()
  exporter._offer_payload(1, {"bad": {object()}})
  exporter._offer_payload(2, {"good": True})

  assert exporter.wait_for_idle(1.0)
  stats = exporter.stats()
  exporter.shutdown()

  assert stats["workerExceptions"] == 1
  assert stats["sent"] == 1
  assert len(packets) == 1
  assert packets[0][5] == 2


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_repeated_disconnects_and_receiver_restart_do_not_kill_worker(template, flavor, monkeypatch):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor)
  created = []

  class RestartingSocket:
    def __init__(self):
      self.number = len(created)
      self.sent = []
      created.append(self)

    def settimeout(self, _timeout):
      pass

    def connect(self, _path):
      if self.number == 0:
        raise OSError("receiver absent")

    def sendall(self, packet):
      if self.number == 1:
        raise OSError("receiver restarted mid-frame")
      self.sent.append(packet)

    def close(self):
      pass

  monkeypatch.setattr(module.socket, "socket", lambda *_args: RestartingSocket())
  monkeypatch.setattr(module, "COMMAVIEW_CONNECT_RETRY_SEC", 0.0)

  for generation in range(3):
    exporter._offer_payload(7, {"generation": generation})
    assert exporter.wait_for_idle(1.0)
  exporter.shutdown()

  assert len(created) == 3
  assert len(created[2].sent) == 1


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_source_recipe_is_always_enabled_for_onroad_export(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  values = alias_values(current=True)
  values.update({
    "wideRoadCameraState": types.SimpleNamespace(frameId=5, timestampEof=6),
    "modelV2": types.SimpleNamespace(frameId=7, timestampEof=8),
  })
  exporter.set_onroad_projection(FakeUiState(values), "road", types.SimpleNamespace(), [1.0] * 9, [1.0] * 9)
  assert [event["kind"] for event in exporter._recipe_pending] == ["camera_switch"]


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_source_recipe_keeps_rapid_camera_switches_despite_live_conflation(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  values = alias_values(current=True)
  values.update({
    "wideRoadCameraState": types.SimpleNamespace(frameId=5, timestampEof=9998),
    "modelV2": types.SimpleNamespace(frameId=7, timestampEof=9997),
  })
  ui_state = FakeUiState(values)
  for camera in ("road", "wideRoad", "road"):
    exporter.set_onroad_projection(
      ui_state, camera, types.SimpleNamespace(), [1.0] * 9, [1.0] * 9,
    )
  assert [event["projection"]["camera"] for event in exporter._recipe_pending] == [
    "road", "wideRoad", "road",
  ]
  assert [event["sequence"] for event in exporter._recipe_pending] == [1, 2, 3]
  assert exporter._pending_payload(module.COMMAVIEW_ONROAD_PROJECTION_SERVICE_INDEX)["camera"] == "road"


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_projection_uses_one_latest_value_path_without_publish_duplication(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  values = alias_values(current=True)
  values.update({
    "wideRoadCameraState": types.SimpleNamespace(frameId=5, timestampEof=6),
    "modelV2": types.SimpleNamespace(frameId=7, timestampEof=8),
  })
  ui_state = FakeUiState(values)

  for road_frame in (100, 101):
    values["narrowRoadCameraState"].frameId = road_frame
    exporter.set_onroad_projection(ui_state, "road", types.SimpleNamespace(), [1.0] * 9, [1.0] * 9)
    time.sleep(module.COMMAVIEW_MIN_EXPORT_INTERVAL_SEC)
  before_publish = exporter.stats().copy()
  for service_index in exporter._payload_builders:
    exporter._payload_builders[service_index] = lambda _ui_state: {"exportVersion": 1}
  exporter.publish(ui_state)

  assert exporter._pending_payload(module.COMMAVIEW_ONROAD_PROJECTION_SERVICE_INDEX)["roadFrameId"] == 101
  assert before_publish["offers"] == 2
  assert before_publish["overwritten"] == 1
  assert exporter.stats()["overwritten"] == 1


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_unchanged_publish_path_stays_below_host_timing_gate(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  ui_state = FakeUiState(alias_values(current=True))
  for service_index in exporter._payload_builders:
    exporter._payload_builders[service_index] = lambda _ui_state: {"exportVersion": 1}

  for _ in range(500):
    exporter.publish(ui_state)

  stats = exporter.stats()
  assert stats["snapshotP99Ms"] < 1.0
  assert stats["snapshotMaxMs"] < 5.0
  assert stats["maxPending"] <= len(exporter._service_specs)


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_car_params_are_offered_once_when_the_car_is_known_and_again_only_when_it_changes(template, flavor):
  # The UI loads CarParams from params a moment after it starts. Offered only at start, the car
  # never reached an app (no car picture); offered with every deviceState, it cost the comma work
  # for nothing. commaviewd keeps the latest frame for clients that connect later.
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  values = alias_values(current=True)
  values["deviceState"] = types.SimpleNamespace(started=False, deviceType="mici")
  ui_state = FakeUiState(values)
  ui_state.CP = None

  exporter.publish(ui_state)
  assert exporter._pending_payload(module.COMMAVIEW_CAR_PARAMS_SERVICE_INDEX)["carFingerprint"] == ""

  exporter._pending.clear()
  ui_state.CP = types.SimpleNamespace(
    carFingerprint="HYUNDAI_IONIQ_5", carName="hyundai", carVin="KMHKN81AFNU000000",
    openpilotLongitudinalControl=True, maxLateralAccel=2.5,
  )
  time.sleep(module.COMMAVIEW_MIN_EXPORT_INTERVAL_SEC)
  exporter.publish(ui_state)
  payload = exporter._pending_payload(module.COMMAVIEW_CAR_PARAMS_SERVICE_INDEX)
  assert payload is not None
  assert payload["carFingerprint"] == "HYUNDAI_IONIQ_5"
  assert payload["carName"] == "hyundai"
  exporter._pending.clear()

  # Other services moving on (deviceState at 2 Hz) don't offer the unchanged car again.
  for _ in range(3):
    ui_state.sm.recv_frame["deviceState"] += 1
    time.sleep(module.COMMAVIEW_MIN_EXPORT_INTERVAL_SEC)
    exporter.publish(ui_state)
    assert exporter._pending_payload(module.COMMAVIEW_CAR_PARAMS_SERVICE_INDEX) is None
    exporter._pending.clear()

  # A reloaded CarParams with the same content is the same car.
  ui_state.CP = types.SimpleNamespace(**vars(ui_state.CP))
  time.sleep(module.COMMAVIEW_MIN_EXPORT_INTERVAL_SEC)
  exporter.publish(ui_state)
  assert exporter._pending_payload(module.COMMAVIEW_CAR_PARAMS_SERVICE_INDEX) is None

  ui_state.CP = types.SimpleNamespace(**{**vars(ui_state.CP), "carFingerprint": "KIA_EV6"})
  time.sleep(module.COMMAVIEW_MIN_EXPORT_INTERVAL_SEC)
  exporter.publish(ui_state)
  assert exporter._pending_payload(module.COMMAVIEW_CAR_PARAMS_SERVICE_INDEX)["carFingerprint"] == "KIA_EV6"

class CountingParams:
  def __init__(self, values):
    self.values = values
    self.reads = []

  def get_bool(self, key):
    self.reads.append(key)
    return bool(self.values.get(key, False))

  def get(self, key, return_default=False):
    self.reads.append(key)
    return self.values.get(key)


def started_ui_state(values, **attrs):
  ui_state = FakeUiState(values)
  ui_state.started = True
  ui_state.ignition = True
  ui_state.status = "engaged"
  ui_state.is_metric = True
  ui_state.started_time = 12.5
  ui_state.params = CountingParams({})
  for name, value in attrs.items():
    setattr(ui_state, name, value)
  return ui_state


def union(which, **members):
  return types.SimpleNamespace(which=lambda: which, **members)


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_radar_lead_presence_reads_present_on_current_schema_and_status_on_release(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  current = started_ui_state({"radarState": types.SimpleNamespace(
    leadOne=types.SimpleNamespace(dRel=20.0, yRel=0.5, vRel=-1.0, aRel=0.0, present=True),
    leadTwo=types.SimpleNamespace(dRel=0.0, yRel=0.0, vRel=0.0, aRel=0.0, present=False),
  )})
  release = started_ui_state({"radarState": types.SimpleNamespace(
    leadOne=types.SimpleNamespace(dRel=20.0, yRel=0.5, vRel=-1.0, aRel=0.0, status=True),
    leadTwo=types.SimpleNamespace(dRel=0.0, yRel=0.0, vRel=0.0, aRel=0.0, status=False),
  )})

  for ui_state in (current, release):
    payload = exporter._radar_state_payload(ui_state)
    assert payload["leadOne"]["present"] is True
    assert payload["leadOne"]["status"] is True
    assert payload["leadTwo"]["present"] is False
    assert payload["leadTwo"]["status"] is False


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_driver_monitoring_exports_active_policy_awareness_and_pose(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  driver_monitoring = types.SimpleNamespace(
    isRHD=False,
    activePolicy="vision",
    visionPolicyState=types.SimpleNamespace(
      faceDetected=True,
      isDistracted=False,
      awarenessPercent=62,
      pose=types.SimpleNamespace(
        pitch=0.12, yaw=-0.25,
        pitchCalib=types.SimpleNamespace(offset=0.01, calibratedPercent=88),
        yawCalib=types.SimpleNamespace(offset=-0.02, calibratedPercent=77),
      ),
    ),
  )
  payload = exporter._driver_monitoring_state_payload(started_ui_state({"driverMonitoringState": driver_monitoring}))

  assert payload["activePolicy"] == 1
  assert payload["isActiveMode"] is True
  assert payload["awarenessPercent"] == 62
  assert payload["posePitch"] == pytest.approx(0.12)
  assert payload["poseYaw"] == pytest.approx(-0.25)
  # The calibration offsets are still exported under their old names.
  assert payload["poseYawOffset"] == pytest.approx(-0.02)

  driver_monitoring.activePolicy = types.SimpleNamespace(raw=0)
  payload = exporter._driver_monitoring_state_payload(started_ui_state({"driverMonitoringState": driver_monitoring}))
  assert payload["activePolicy"] == 0
  assert payload["isActiveMode"] is False


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_driver_monitoring_without_active_policy_keeps_legacy_flag_and_marks_policy_unknown(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  legacy = types.SimpleNamespace(faceDetected=True, isRHD=False, isActiveMode=True)
  payload = exporter._driver_monitoring_state_payload(started_ui_state({"driverMonitoringState": legacy}))

  assert payload["activePolicy"] == -1
  assert payload["isActiveMode"] is True
  assert payload["awarenessPercent"] == 100


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_driver_monitoring_exports_alert_level_causes_and_the_active_policy_attention(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  driver_monitoring = types.SimpleNamespace(
    isRHD=False,
    activePolicy="vision",
    alertLevel="two",
    visionPolicyState=types.SimpleNamespace(
      faceDetected=True, isDistracted=True, awarenessPercent=41,
      distractedTypes=types.SimpleNamespace(pose=False, eye=True, phone=True, sleep=False),
    ),
    wheeltouchPolicyState=types.SimpleNamespace(awarenessPercent=90),
  )
  payload = exporter._driver_monitoring_state_payload(started_ui_state({"driverMonitoringState": driver_monitoring}))
  assert payload["alertLevel"] == 2
  assert payload["distractedTypes"] == 2 | 4
  assert payload["attentionPercent"] == 41

  # On wheel touch the attention is the wheel-touch policy's.
  driver_monitoring.activePolicy = "wheeltouch"
  driver_monitoring.alertLevel = types.SimpleNamespace(raw=0)
  payload = exporter._driver_monitoring_state_payload(started_ui_state({"driverMonitoringState": driver_monitoring}))
  assert payload["alertLevel"] == 0
  assert payload["attentionPercent"] == 90


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_driver_monitoring_without_alert_level_derives_it_from_awareness(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  legacy = types.SimpleNamespace(faceDetected=True, isRHD=False, isActiveMode=True, awarenessStatus=0.6, distractedType=3 | 4)
  payload = exporter._driver_monitoring_state_payload(started_ui_state({"driverMonitoringState": legacy}))
  # Watching the driver: warning at 8 s of 11 till terminal, prompt at 6.
  assert payload["alertLevel"] == 1
  assert payload["attentionPercent"] == 60
  # POSE and BLINK; E2E (1 << 2) has no cause to show.
  assert payload["distractedTypes"] == 3

  legacy.awarenessStatus = 0.5
  assert exporter._driver_monitoring_state_payload(started_ui_state({"driverMonitoringState": legacy}))["alertLevel"] == 2
  legacy.awarenessStatus = 0.0
  assert exporter._driver_monitoring_state_payload(started_ui_state({"driverMonitoringState": legacy}))["alertLevel"] == 3
  legacy.isActiveMode = False
  legacy.awarenessStatus = 0.6
  assert exporter._driver_monitoring_state_payload(started_ui_state({"driverMonitoringState": legacy}))["alertLevel"] == 0


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_controls_state_set_speed_reads_deprecated_group_and_dev_ui_steering(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  values = alias_values(current=True)
  values.update({
    "controlsState": types.SimpleNamespace(
      lateralControlState=union("angleState", angleState=types.SimpleNamespace(steeringAngleDeg=-12.5)),
      curvature=0.0, desiredCurvature=0.0,
      deprecated=types.SimpleNamespace(vCruise=88.0),
    ),
    "carState": types.SimpleNamespace(vEgo=0.0),
    "carControl": types.SimpleNamespace(latActive=False),
    "carOutput": types.SimpleNamespace(actuatorsOutput=types.SimpleNamespace(torque=0.0)),
  })
  payload = exporter._controls_state_payload(started_ui_state(values))

  assert payload["vCruise"] == 88.0
  assert payload["vCruiseDEPRECATED"] == 88.0
  assert payload["angleStateSteeringAngleDeg"] == -12.5
  assert payload["pidStateSteeringAngleDesiredDeg"] == 0.0

  values["controlsState"] = types.SimpleNamespace(
    lateralControlState=union("pidState", pidState=types.SimpleNamespace(steeringAngleDesiredDeg=4.25)),
    curvature=0.0, desiredCurvature=0.0, vCruiseDEPRECATED=55.0,
  )
  payload = exporter._controls_state_payload(started_ui_state(values))
  assert payload["vCruise"] == 55.0
  assert payload["pidStateSteeringAngleDesiredDeg"] == 4.25


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_speed_limit_pre_active_arrow_falls_back_to_controls_deprecated_v_cruise(template, flavor):
  module = load_exporter(template)
  values = {
    "longitudinalPlanSP": types.SimpleNamespace(speedLimit=types.SimpleNamespace(
      assist=types.SimpleNamespace(state="preActive"),
      resolver=types.SimpleNamespace(speedLimitFinalLast=25.0),  # 90 km/h
    )),
    "carState": types.SimpleNamespace(vCruiseCluster=0.0),
    "controlsState": types.SimpleNamespace(deprecated=types.SimpleNamespace(vCruise=100.0)),
  }
  pre_active, icon = module._speed_limit_pre_active_state(started_ui_state(values))

  assert pre_active is True
  assert icon == "down"


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_torque_bar_uses_lateral_accel_for_curvature_state(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  values = alias_values(current=True)
  values.update({
    "controlsState": types.SimpleNamespace(lateralControlState=union("curvatureState"), curvature=0.02, desiredCurvature=0.03),
    "carState": types.SimpleNamespace(vEgo=10.0),
    "carControl": types.SimpleNamespace(latActive=True),
  })

  assert module._torque_bar_value(FakeUiState(values), exporter._service_resolver) == pytest.approx(0.795625, abs=1e-4)


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_car_state_and_car_control_export_the_hud_inputs(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  car_state = types.SimpleNamespace(
    vEgo=10.0, vEgoCluster=10.2, vCruiseCluster=255.0, standstill=False, steeringAngleDeg=1.0, steeringPressed=False,
    leftBlinker=False, rightBlinker=True, leftBlindspot=True, rightBlindspot=False,
    aEgo=-1.25, steeringTorqueEps=-42.0, brakePressed=True, gasPressed=False,
    cruiseState=types.SimpleNamespace(enabled=True, available=True, speed=27.0, speedCluster=27.5),
  )
  car_control = types.SimpleNamespace(
    enabled=True, latActive=True, longActive=False,
    cruiseControl=types.SimpleNamespace(override=True, cancel=False, resume=False),
    hudControl=types.SimpleNamespace(setSpeed=70.83, speedVisible=True),
  )
  ui_state = started_ui_state({"carState": car_state, "carControl": car_control})

  car = exporter._car_state_payload(ui_state)
  assert car["aEgo"] == -1.25
  assert car["steeringTorqueEps"] == -42.0
  assert car["brakePressed"] is True
  assert car["gasPressed"] is False
  assert car["vCruiseCluster"] == 255.0
  assert car["cruiseState"] == {"speedCluster": 27.5}

  control = exporter._car_control_payload(ui_state)
  assert control["enabled"] is True
  assert control["cruiseControl"] == {"override": True}


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_longitudinal_plan_carries_plan_source_and_sunnypilot_planner_and_map(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  plan_sp = types.SimpleNamespace(
    longitudinalPlanSource="sccVision",
    speedLimit=types.SimpleNamespace(
      resolver=types.SimpleNamespace(
        speedLimit=22.2, speedLimitLast=22.2, speedLimitFinal=23.0, speedLimitFinalLast=23.0, speedLimitOffset=0.8,
        speedLimitValid=True, speedLimitLastValid=True, distToSpeedLimit=120.0, source="map",
      ),
      assist=types.SimpleNamespace(state="preActive", enabled=True, active=False),
    ),
    smartCruiseControl=types.SimpleNamespace(
      vision=types.SimpleNamespace(state="entering", enabled=True, active=True),
      map=types.SimpleNamespace(state="disabled", enabled=False, active=False),
    ),
    e2eAlerts=types.SimpleNamespace(greenLightAlert=True, leadDepartAlert=False),
  )
  map_data = types.SimpleNamespace(
    speedLimitValid=True, speedLimit=22.2, speedLimitAheadValid=True, speedLimitAhead=13.9,
    speedLimitAheadDistance=340.0, roadName="Main Street",
  )
  ui_state = started_ui_state({
    "longitudinalPlan": types.SimpleNamespace(allowThrottle=True, longitudinalPlanSource="e2e"),
    "longitudinalPlanSP": plan_sp,
    "liveMapDataSP": map_data,
  })

  payload = exporter._longitudinal_plan_payload(ui_state)

  assert payload["longitudinalPlanSource"] == "e2e"
  assert payload["logMonoTime"] == max(ui_state.sm.logMonoTime.values())
  sp = payload["longitudinalPlanSP"]
  assert sp["speedLimit"]["resolver"] == {
    "speedLimit": 22.2, "speedLimitLast": 22.2, "speedLimitFinalLast": 23.0, "speedLimitOffset": 0.8,
    "speedLimitValid": True, "speedLimitLastValid": True, "source": "map",
  }
  assert sp["speedLimit"]["assist"] == {"state": "preActive", "active": False}
  assert sp["smartCruiseControl"]["vision"] == {"enabled": True, "active": True}
  assert sp["e2eAlerts"] == {"greenLightAlert": True, "leadDepartAlert": False}
  assert payload["liveMapDataSP"]["roadName"] == "Main Street"
  assert payload["liveMapDataSP"]["speedLimitAheadDistance"] == 340.0
  json.dumps(payload)

  openpilot_like = started_ui_state({"longitudinalPlan": types.SimpleNamespace(allowThrottle=False, longitudinalPlanSource="cruise")})
  payload = exporter._longitudinal_plan_payload(openpilot_like)
  assert payload["longitudinalPlanSource"] == "cruise"
  assert "longitudinalPlanSP" not in payload
  assert "liveMapDataSP" not in payload


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_selfdrive_state_carries_mads_only_when_sunnypilot_publishes_it(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  selfdrive_state = types.SimpleNamespace(
    enabled=False, active=False, engageable=True, alertText1="", alertText2="", alertType="", alertStatus=0,
    alertSize=0, alertHudVisual=0, experimentalMode=False,
  )
  mads = types.SimpleNamespace(state="paused", enabled=True, active=False, available=True)
  payload = exporter._selfdrive_state_payload(started_ui_state({
    "selfdriveState": selfdrive_state, "selfdriveStateSP": types.SimpleNamespace(mads=mads),
  }))
  assert payload["mads"] == {"state": "paused", "enabled": True, "active": False, "available": True}

  payload = exporter._selfdrive_state_payload(started_ui_state({"selfdriveState": selfdrive_state}))
  assert "mads" not in payload


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_model_v2_exports_leads_v3(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  lead = types.SimpleNamespace(prob=0.9, probTime=0.0, x=[30.0, 31.0], y=[0.4, 0.5], v=[20.0, 20.5], a=[0.1, 0.2])
  model = types.SimpleNamespace(
    frameId=1, frameIdExtra=1, frameAge=0, frameDropPerc=0.0, timestampEof=1,
    position=types.SimpleNamespace(x=[0.0], y=[0.0], z=[0.0]), laneLines=[], roadEdges=[],
    leadsV3=[lead, lead, lead, lead],
  )
  payload = exporter._model_v2_payload(started_ui_state({"modelV2": model}))

  # Only what the lead bar reads: the first two leads at t=0.
  assert len(payload["leadsV3"]) == module.COMMAVIEW_MAX_MODEL_LEADS == 2
  assert payload["leadsV3"][0] == {"prob": 0.9, "x": [30.0], "y": [0.4], "v": [20.0], "a": [0.1]}


@pytest.mark.parametrize("current", (False, True))
@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_live_parameters_export_validity_and_optional_torque_parameters(template, flavor, current):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  values = alias_values(current=current)
  vehicle_parameters = "vehicleParameters" if current else "liveParameters"
  values[vehicle_parameters].valid = True
  if current:
    values["lateralTorqueParameters"] = types.SimpleNamespace(valid=True, frictionCoefficientFiltered=0.12, latAccelFactorFiltered=2.3)
  else:
    values["liveTorqueParameters"] = types.SimpleNamespace(liveValid=True, frictionCoefficientFiltered=0.12, latAccelFactorFiltered=2.3)
  ui_state = started_ui_state(values)
  ui_state.sm.valid[vehicle_parameters] = False

  payload = exporter._live_parameters_payload(ui_state)

  assert payload["roll"] == 0.125
  assert payload["valid"] is True
  assert payload["serviceValid"] is False
  assert payload["torqueParameters"]["frictionCoefficientFiltered"] == pytest.approx(0.12)
  assert payload["torqueParameters"]["latAccelFactorFiltered"] == pytest.approx(2.3)
  assert payload["torqueParameters"]["liveValid"] is True
  assert payload["torqueParameters"]["serviceValid"] is True

  # openpilot subscribes to neither torque service: the payload and publish() must still work.
  plain = started_ui_state(alias_values(current=current))
  assert "torqueParameters" not in exporter._live_parameters_payload(plain)
  exporter.publish(plain)
  assert exporter._pending_payload(module.COMMAVIEW_LIVE_PARAMETERS_SERVICE_INDEX) is not None


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_car_params_export_pcm_cruise_speed_with_upstream_default(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  ui_state = started_ui_state({})
  assert exporter._car_params_payload(ui_state)["pcmCruiseSpeed"] is True
  ui_state.CP_SP = types.SimpleNamespace(pcmCruiseSpeed=False)
  assert exporter._car_params_payload(ui_state)["pcmCruiseSpeed"] is False


SUNNYPILOT_UI_ATTRS = {
  "blindspot": True, "turn_signals": True, "torque_bar": True, "developer_ui": 3, "hide_v_ego_ui": False,
  "true_v_ego_ui": True, "road_name_toggle": True, "rocket_fuel": False, "standstill_timer": True,
  "chevron_metrics": 2, "speed_limit_mode": 3, "rainbow_path": True, "enforce_torque_control": True,
  "custom_torque_params": True, "torque_override_enabled": False, "torque_override_friction": 0.15,
  "torque_override_lat_accel_factor": 2.1,
}


def ui_state_for_params(module, param_values, **attrs):
  values = alias_values(current=True)
  values.update({
    "deviceState": types.SimpleNamespace(started=True, deviceType="tizi"),
    "selfdriveState": types.SimpleNamespace(experimentalMode=False),
    "carState": types.SimpleNamespace(vEgo=0.0),
  })
  ui_state = started_ui_state(values, **attrs)
  ui_state.params = CountingParams(param_values)
  return ui_state


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_car_params_carry_sunnypilot_params_only_for_sunnypilot(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  ui_state = ui_state_for_params(module, {"CameraOffset": 0.12}, active_bundle={"name": "custom"}, always_on_dm=True, **SUNNYPILOT_UI_ATTRS)

  onroad = exporter._ui_state_onroad_payload(ui_state)
  car_params = exporter._car_params_payload(ui_state)

  assert onroad["startedTime"] == 12.5
  assert onroad["alwaysOnDm"] is True
  assert "spParams" not in onroad
  assert car_params["upstreamServices"] == {}  # nothing resolved yet: publish() resolves before building
  if flavor != "SUNNYPILOT":
    assert "spParams" not in car_params
    assert onroad["rainbowPathEnabled"] is False
    assert ui_state.params.reads == []
    return
  assert onroad["rainbowPathEnabled"] is True
  assert car_params["spParams"] == {
    "BlindSpot": True, "ShowTurnSignals": True, "TorqueBar": True, "DevUIInfo": 3, "HideVEgoUI": False,
    "TrueVEgoUI": True, "RoadNameToggle": True, "RocketFuel": False, "StandstillTimer": True, "ChevronInfo": 2,
    "SpeedLimitMode": 3, "RainbowMode": True, "EnforceTorqueControl": True, "CustomTorqueParams": True,
    "TorqueParamsOverrideEnabled": False, "TorqueParamsOverrideFriction": 0.15,
    "TorqueParamsOverrideLatAccelFactor": 2.1, "CameraOffset": 0.12,
  }
  json.dumps(car_params)


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_sunnypilot_params_default_like_params_keys_and_camera_offset_needs_a_model_bundle(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter("SUNNYPILOT", start_worker=False)
  # Before the sunnypilot param thread's first pass, ChevronInfo/SpeedLimitMode are None and the
  # torque override floats are not set at all.
  ui_state = ui_state_for_params(module, {"CameraOffset": 0.2}, active_bundle=None, chevron_metrics=None, speed_limit_mode=None)

  params = exporter._car_params_payload(ui_state)["spParams"]

  assert params["ChevronInfo"] == 4
  assert params["SpeedLimitMode"] == 1
  assert params["TorqueParamsOverrideFriction"] == 0.1
  assert params["CameraOffset"] == 0.0
  assert ui_state.params.reads == []  # no model bundle: the CameraOffset file is not read either


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_sunnypilot_params_are_offered_only_when_they_change_and_read_no_files_per_frame(template, flavor, monkeypatch):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter("SUNNYPILOT", start_worker=False)
  ui_state = ui_state_for_params(module, {"CameraOffset": 0.1}, active_bundle={"x": 1}, always_on_dm=False, **SUNNYPILOT_UI_ATTRS)
  ui_state.CP = types.SimpleNamespace(carFingerprint="KIA_EV6", carName="hyundai", carVin="", openpilotLongitudinalControl=True, maxLateralAccel=2.5)
  clock = [100.0]
  monkeypatch.setattr(module.time, "monotonic", lambda: clock[0])

  def publish_frame():
    clock[0] += module.COMMAVIEW_MIN_EXPORT_INTERVAL_SEC
    for service in ("deviceState", "selfdriveState", "carState"):
      ui_state.sm.recv_frame[service] += 1
    exporter.publish(ui_state)
    payload = exporter._pending_payload(module.COMMAVIEW_CAR_PARAMS_SERVICE_INDEX)
    exporter._pending.clear()
    return payload

  assert publish_frame()["spParams"]["CameraOffset"] == 0.1
  # Frames where nothing changed offer nothing, and the only file read is the CameraOffset param at the
  # model renderer's own 3 s cadence.
  for _ in range(40):
    assert publish_frame() is None
  assert ui_state.params.reads == ["CameraOffset"]

  ui_state.torque_bar = False  # the sunnypilot param thread picked up a toggle
  assert publish_frame()["spParams"]["TorqueBar"] is False

  ui_state.params.values["CameraOffset"] = -0.05
  clock[0] += module.COMMAVIEW_PARAMS_REFRESH_SEC
  assert publish_frame()["spParams"]["CameraOffset"] == -0.05
  assert ui_state.params.reads == ["CameraOffset", "CameraOffset"]


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_projection_reports_whether_model_transform_includes_the_content_origin(template, flavor):
  module = load_exporter(template)
  values = alias_values(current=True)
  values.update({
    "wideRoadCameraState": types.SimpleNamespace(frameId=5, timestampEof=6),
    "modelV2": types.SimpleNamespace(frameId=7, timestampEof=8),
  })
  caller = "def call(exporter, ui_state, rect):\n  exporter.set_onroad_projection(ui_state, 'road', rect, [1.0] * 9, [1.0] * 9)\n"
  expected = {
    "/data/openpilot/selfdrive/ui/mici/onroad/augmented_road_view.py": ("mici", "content"),
    "/data/openpilot/selfdrive/ui/onroad/augmented_road_view.py": ("big", "screen"),
    "/somewhere/else.py": ("", ""),
  }
  for filename, (layout, origin) in expected.items():
    exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
    namespace = {}
    exec(compile(caller, filename, "exec"), namespace)
    namespace["call"](exporter, FakeUiState(values), types.SimpleNamespace(x=30, y=30, width=2100, height=1020))
    payload = exporter._pending_payload(module.COMMAVIEW_ONROAD_PROJECTION_SERVICE_INDEX)
    assert (payload["layout"], payload["transformOrigin"]) == (layout, origin), filename


@pytest.mark.parametrize("template,flavor", TEMPLATES)
def test_full_publish_of_a_sunnypilot_shaped_state_offers_every_payload(template, flavor):
  module = load_exporter(template)
  exporter = module._CommaViewSocketExporter(flavor, start_worker=False)
  ui_state = ui_state_for_params(module, {}, active_bundle=None, **SUNNYPILOT_UI_ATTRS)
  ui_state.sm.values.update({
    "longitudinalPlan": types.SimpleNamespace(allowThrottle=True, longitudinalPlanSource="cruise"),
    "selfdriveStateSP": types.SimpleNamespace(mads=types.SimpleNamespace(state="enabled", enabled=True, active=True, available=True)),
    "longitudinalPlanSP": types.SimpleNamespace(),
    "liveMapDataSP": types.SimpleNamespace(roadName="A1"),
  })
  for service in ("longitudinalPlan", "selfdriveStateSP", "longitudinalPlanSP", "liveMapDataSP"):
    ui_state.sm.recv_frame[service] = 10
    ui_state.sm.logMonoTime[service] = 5000
  exporter.publish(ui_state)

  plan = exporter._pending_payload(module.COMMAVIEW_LONGITUDINAL_PLAN_SERVICE_INDEX)
  assert plan["liveMapDataSP"]["roadName"] == "A1"
  assert plan["longitudinalPlanSP"]["speedLimit"]["assist"]["state"] == ""
  json.dumps(plan)
  car_params = exporter._pending_payload(module.COMMAVIEW_CAR_PARAMS_SERVICE_INDEX)
  assert car_params["upstreamServices"]["calibration"] == "extrinsicsCalibration"
  assert ("spParams" in car_params) == (flavor == "SUNNYPILOT")
  assert "spParams" not in exporter._pending_payload(module.COMMAVIEW_UI_STATE_ONROAD_SERVICE_INDEX)


@pytest.mark.parametrize("camera", ["driver", "none", "road", "wideRoad"])
def test_starpilot_camera_and_mode_snapshot(camera):
  module = load_exporter(REPO_ROOT / "comma/src/commaview_export.starpilot.py")
  exporter = module._CommaViewSocketExporter("STARPILOT", start_worker=False)
  ui = FakeUiState({
    "selfdriveState": types.SimpleNamespace(enabled=False),
    "starpilotCarState": types.SimpleNamespace(pauseLateral=False),
    "carState": types.SimpleNamespace(gearShifter="drive"),
  })
  ui.started = ui.ignition = True
  ui.is_metric = True
  ui.status = "disengaged"
  ui.always_on_lateral_active = True
  exporter.set_onroad_camera(camera, camera == "wideRoad")
  payload = exporter._ui_state_onroad_payload(ui)
  assert payload["runtimeFlavor"] == "STARPILOT"
  assert payload["activeCamera"] == camera
  assert payload["status"] == "latOnly"
  assert payload["starpilot"]["drawRoadOverlays"] == (camera in ("road", "wideRoad"))
  assert payload["starpilot"]["drawHudControls"] == (camera != "driver")
  ui.sm.values["selfdriveState"].enabled = True
  ui.sm.values["starpilotCarState"].pauseLateral = True
  ui.always_on_lateral_active = False
  assert exporter._ui_state_onroad_payload(ui)["status"] == "longOnly"
  ui.sm.values["carState"].gearShifter = "reverse"
  assert not exporter._ui_state_onroad_payload(ui)["starpilot"]["drawRoadOverlays"]


def test_starpilot_widths_and_logging_status():
  module = load_exporter(REPO_ROOT / "comma/src/commaview_export.starpilot.py")
  exporter = module._CommaViewSocketExporter("STARPILOT", start_worker=False)
  ui = FakeUiState({})
  ui.is_metric = True
  ui.status = "disengaged"
  ui.always_on_lateral_active = True
  class Params:
    def get_bool(self, name, default=False):
      return name in {"ModelUI", "DynamicPathWidth"}
    def get_float(self, name, default=0):
      return {"PathWidth": 2.0, "LaneLinesWidth": 10.0, "RoadEdgesWidth": 20.0}.get(name, default)
  ui.ui_params = Params()
  ui.starpilot_toggles = {"no_logging": True}
  state = exporter._starpilot_payload(ui, "road")
  assert state["pathHalfWidthM"] == 0.75
  assert state["laneLineHalfWidthM"] == 0.05
  assert state["roadEdgeHalfWidthM"] == 0.1
  assert "Disable Logging" in state["videoDisabledReason"]


def test_starpilot_color_failure_keeps_the_onroad_payload(monkeypatch):
  module = load_exporter(REPO_ROOT / "comma/src/commaview_export.starpilot.py")
  exporter = module._CommaViewSocketExporter("STARPILOT", start_worker=False)
  status = types.ModuleType("openpilot.selfdrive.ui.lib.starpilot_status")
  def fail(_state):
    raise RuntimeError("capnp read failed")
  status.get_border_color = status.get_screen_edge_color = fail
  monkeypatch.setitem(sys.modules, "openpilot.selfdrive.ui.lib.starpilot_status", status)
  ui = FakeUiState({})
  ui.started = ui.ignition = True
  ui.is_metric = True
  ui.status = "disengaged"
  ui.always_on_lateral_active = False
  state = exporter._ui_state_onroad_payload(ui)["starpilot"]
  assert state["borderColor"] == state["screenEdgeColor"] == 0xFF122839
  assert "modeBanner" not in state  # StarPilot shows its modes only as colors
