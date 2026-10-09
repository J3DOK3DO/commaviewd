#!/usr/bin/env python3
"""Synthetic smoke test for generated CommaView onroad UI export helpers.

This intentionally avoids real openpilot imports/sockets. It loads a generated
commaview_export.py helper, feeds it old/new upstream-shaped objects, and proves
critical overlay services remain publishable.
"""

from __future__ import annotations

import importlib.util
import json
import sys
import types
from collections import defaultdict
from pathlib import Path


CRITICAL_SERVICE_NAMES = (
  "COMMAVIEW_DRIVER_MONITORING_STATE_SERVICE_INDEX",
  "COMMAVIEW_DRIVER_STATE_V2_SERVICE_INDEX",
  "COMMAVIEW_MODEL_V2_SERVICE_INDEX",
  "COMMAVIEW_LIVE_CALIBRATION_SERVICE_INDEX",
  "COMMAVIEW_CAR_OUTPUT_SERVICE_INDEX",
  "COMMAVIEW_ONROAD_PROJECTION_SERVICE_INDEX",
)


class Dot(types.SimpleNamespace):
  def __getattr__(self, name):
    if name.endswith("Prob") or name.endswith("Perc") or name.endswith("Count") or name.endswith("Frame"):
      return 0
    return False


class LateralControlState(Dot):
  def which(self):
    return "torqueState"


class Params:
  def get_bool(self, _key: str) -> bool:
    return False


class FakeSubMaster:
  def __init__(self, values: dict[str, object]):
    self.values = values
    self.recv_frame = defaultdict(lambda: 10, {key: 10 for key in values})
    self.logMonoTime = defaultdict(lambda: 1000, {key: 1000 for key in values})
    self.valid = defaultdict(lambda: True, {key: True for key in values})
    self.alive = defaultdict(lambda: True, {key: True for key in values})
    self.updated = defaultdict(lambda: True, {key: True for key in values})

  def __getitem__(self, key: str):
    return self.values.get(key, Dot())


class FakeUiState:
  def __init__(self, values: dict[str, object]):
    self.started = True
    self.ignition = True
    self.status = "engaged"
    self.is_metric = False
    self.started_frame = 1
    self.started_time = 123.0
    self.params = Params()
    self.CP = Dot(
      openpilotLongitudinalControl=True,
      maxLateralAccel=3.0,
      carFingerprint="mock",
      carName="mock",
      carVin="VIN",
    )
    self.sm = FakeSubMaster(values)


def path(x=(0.0, 1.0, 2.0), y=(0.0, 0.1, 0.2), z=(0.0, 0.0, 0.0)) -> Dot:
  return Dot(x=list(x), y=list(y), z=list(z))


def driver_data() -> Dot:
  return Dot(
    faceOrientation=[0.0, 0.0, 0.0],
    faceOrientationStd=[0.1, 0.1, 0.1],
    facePosition=[0.0, 0.0],
    facePositionStd=[0.1, 0.1],
    faceProb=0.9,
    leftEyeProb=0.8,
    rightEyeProb=0.8,
    leftBlinkProb=0.0,
    rightBlinkProb=0.0,
    sunglassesProb=0.0,
    phoneProb=0.0,
  )


def fake_values(current_layout: bool = False) -> dict[str, object]:
  nested_driver_monitoring = Dot(
    isRHD=False,
    visionPolicyState=Dot(
      faceDetected=True,
      isDistracted=False,
      pose=Dot(
        yawCalib=Dot(offset=-0.02, calibratedPercent=77),
        pitchCalib=Dot(offset=0.01, calibratedPercent=88),
      ),
    ),
  )
  values = {
    "selfdriveState": Dot(enabled=True, active=True, engageable=True, alertText1="", alertText2="", alertType="none", alertStatus=0, alertSize=0, experimentalMode=False),
    "carState": Dot(vEgo=12.0, vEgoCluster=12.0, vCruiseCluster=25.0, standstill=False, steeringAngleDeg=1.5, steeringPressed=False, leftBlinker=False, rightBlinker=False, leftBlindspot=False, rightBlindspot=False),
    "controlsState": Dot(enabled=True, active=True, engageable=True, alertText1="", alertText2="", alertType="none", alertStatus=0, alertSize=0, experimentalMode=False, vCruiseDEPRECATED=25.0, lateralControlState=LateralControlState(), curvature=0.01, desiredCurvature=0.02),
    "onroadEvents": [],
    "driverMonitoringState": nested_driver_monitoring,
    "driverStateV2": Dot(frameId=1, modelExecutionTime=0.01, gpuExecutionTime=0.02, wheelOnRightProb=0.0, leftDriverData=driver_data(), rightDriverData=driver_data()),
    "modelV2": Dot(frameId=1, frameIdExtra=2, frameAge=0, frameDropPerc=0.0, timestampEof=12345, position=path(), laneLines=[path(), path()], laneLineProbs=[0.8, 0.9], laneLineStds=[0.1, 0.1], roadEdges=[path(), path()], roadEdgeStds=[0.2, 0.2], meta=Dot(disengagePredictions=Dot(t=[0.0], brakeDisengageProbs=[0.1], gasDisengageProbs=[0.1], steerOverrideProbs=[0.1], brake3MetersPerSecondSquaredProbs=[0.1], brake4MetersPerSecondSquaredProbs=[0.1], brake5MetersPerSecondSquaredProbs=[0.1], gasPressProbs=[0.1], brakePressProbs=[0.1])), acceleration=Dot(x=[0.0])),
    "radarState": Dot(leadOne=Dot(dRel=10.0, yRel=0.0, vRel=0.0, aRel=0.0, status=True), leadTwo=Dot(dRel=20.0, yRel=0.0, vRel=0.0, aRel=0.0, status=False)),
    "liveCalibration": Dot(rpyCalib=[0.0, 0.0, 0.0], height=[1.22], calStatus=1, calPerc=100, wideFromDeviceEuler=[0.0, 0.0, 0.0]),
    "carOutput": Dot(actuatorsOutput=Dot(torque=0.2)),
    "carControl": Dot(latActive=True, longActive=True, hudControl=Dot(setSpeed=25.0, speedVisible=True)),
    "liveParameters": Dot(roll=0.01),
    "longitudinalPlan": Dot(allowThrottle=True),
    "longitudinalPlanSP": Dot(speedLimit=Dot(resolver=Dot(speedLimitFinalLast=20.0))),
    "deviceState": Dot(started=True, deviceType="mici"),
    "roadCameraState": Dot(sensor="road", frameId=1),
    "wideRoadCameraState": Dot(sensor="wide", frameId=1),
    "pandaStates": [],
  }
  if current_layout:
    values["extrinsicsCalibration"] = values.pop("liveCalibration")
    values["narrowRoadCameraState"] = values.pop("roadCameraState")
    values["vehicleParameters"] = values.pop("liveParameters")
  return values


def install_import_stubs() -> None:
  sys.modules.setdefault("opendbc", types.ModuleType("opendbc"))
  car_module = types.ModuleType("opendbc.car")
  car_module.ACCELERATION_DUE_TO_GRAVITY = 9.81
  sys.modules["opendbc.car"] = car_module


def load_module(helper_path: Path):
  install_import_stubs()
  spec = importlib.util.spec_from_file_location("commaview_export_smoke", helper_path)
  if spec is None or spec.loader is None:
    raise RuntimeError(f"cannot load helper: {helper_path}")
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


def main() -> int:
  if len(sys.argv) != 3:
    print("usage: smoke_onroad_ui_export_helper.py <commaview_export.py> <OPENPILOT|SUNNYPILOT|STARPILOT>", file=sys.stderr)
    return 2
  helper_path = Path(sys.argv[1])
  flavor = sys.argv[2]
  module = load_module(helper_path)
  sent: list[int] = []
  resolved_layouts = {}
  for layout_name, current_layout in (("legacy", False), ("current", True)):
    exporter = module._CommaViewSocketExporter(flavor)
    exporter._send_json = lambda service_index, payload: sent.append(int(service_index))
    ui_state = FakeUiState(fake_values(current_layout=current_layout))
    exporter.set_onroad_projection(
      ui_state,
      active_camera="road",
      content_rect=Dot(x=0, y=0, width=1928, height=1208),
      video_frame_matrix=[1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0],
      model_transform=[1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0],
      camera_offset=0.0,
    )
    exporter.publish(ui_state)
    exporter.wait_for_idle(1.0)
    resolved_layouts[layout_name] = exporter._service_resolver.resolved
    exporter.shutdown()
  critical = [int(getattr(module, name)) for name in CRITICAL_SERVICE_NAMES]
  missing = [service for service in critical if service not in sent]
  status = {
    "payloadSmokePassed": not missing,
    "criticalServicesPublishable": not missing,
    "serviceSendCount": len(sent),
    "sentServices": sent,
    "missingCriticalServices": missing,
    "resolvedLayouts": resolved_layouts,
  }
  print(json.dumps(status, separators=(",", ":")))
  return 0 if not missing else 1


if __name__ == "__main__":
  raise SystemExit(main())
