import json
import math
import os
import socket
import struct
import sys
import threading
import time
from collections import deque

from opendbc.car import ACCELERATION_DUE_TO_GRAVITY

COMMAVIEW_VENDOR_DIR = os.environ.get("COMMAVIEWD_PYTHON_VENDOR_DIR", "/data/commaview/vendor")
# Appended, not prepended: this runs in openpilot's UI process, and openpilot's own packages win.
if os.path.isdir(COMMAVIEW_VENDOR_DIR) and COMMAVIEW_VENDOR_DIR not in sys.path:
  sys.path.append(COMMAVIEW_VENDOR_DIR)

try:
  import orjson as _orjson
except (ImportError, OSError):
  _orjson = None

COMMAVIEW_RUNTIME_FLAVOR = "STARPILOT"
COMMAVIEW_FRAME_VERSION = 1
COMMAVIEW_UI_STATE_ONROAD_SERVICE_INDEX = 0
COMMAVIEW_SELFDRIVE_STATE_SERVICE_INDEX = 1
COMMAVIEW_CAR_STATE_SERVICE_INDEX = 2
COMMAVIEW_CONTROLS_STATE_SERVICE_INDEX = 3
COMMAVIEW_ONROAD_EVENTS_SERVICE_INDEX = 4
COMMAVIEW_DRIVER_MONITORING_STATE_SERVICE_INDEX = 5
COMMAVIEW_DRIVER_STATE_V2_SERVICE_INDEX = 6
COMMAVIEW_MODEL_V2_SERVICE_INDEX = 7
COMMAVIEW_RADAR_STATE_SERVICE_INDEX = 8
COMMAVIEW_LIVE_CALIBRATION_SERVICE_INDEX = 9
COMMAVIEW_CAR_OUTPUT_SERVICE_INDEX = 10
COMMAVIEW_CAR_CONTROL_SERVICE_INDEX = 11
COMMAVIEW_LIVE_PARAMETERS_SERVICE_INDEX = 12
COMMAVIEW_LONGITUDINAL_PLAN_SERVICE_INDEX = 13
COMMAVIEW_CAR_PARAMS_SERVICE_INDEX = 14
COMMAVIEW_DEVICE_STATE_SERVICE_INDEX = 15
COMMAVIEW_ROAD_CAMERA_STATE_SERVICE_INDEX = 16
COMMAVIEW_PANDA_STATES_SUMMARY_SERVICE_INDEX = 17
COMMAVIEW_ONROAD_PROJECTION_SERVICE_INDEX = 18
COMMAVIEW_WIDE_ROAD_CAMERA_STATE_SERVICE_INDEX = 19
COMMAVIEW_RECIPE_EVENT_SERVICE_INDEX = 20
COMMAVIEW_SOCKET_PATH_DEFAULT = "/data/commaview/run/ui-export.sock"
COMMAVIEW_CONNECT_RETRY_SEC = 1.0
COMMAVIEW_SOCKET_TIMEOUT_SEC = 0.01
COMMAVIEW_DEFAULT_MAX_LAT_ACCEL = 3.0
COMMAVIEW_MS_TO_KPH = 3.6
COMMAVIEW_MS_TO_MPH = 2.23694
COMMAVIEW_KM_TO_MILE = 0.621371
COMMAVIEW_MAX_EXPORT_HZ = 20.0
COMMAVIEW_MIN_EXPORT_INTERVAL_SEC = 1.0 / COMMAVIEW_MAX_EXPORT_HZ
# Params are files. The exporter reads only what the UI has already parsed, except the CameraOffset param,
# which the UI's model renderer itself re-reads every 3 s; the exporter reads it no more often than that.
COMMAVIEW_PARAMS_REFRESH_SEC = 3.0
# openpilot master's comma four lead bar uses leadsV3[0] and [1] at t=0 only.
COMMAVIEW_MAX_MODEL_LEADS = 2

UPSTREAM_SERVICE_ALIASES = {
  "calibration": ("extrinsicsCalibration", "liveCalibration"),
  "road_camera": ("narrowRoadCameraState", "roadCameraState"),
  "vehicle_parameters": ("vehicleParameters", "liveParameters"),
}
# Services only some UIs subscribe to (sunnypilot). A missing one is exported as absent, never an error.
UPSTREAM_OPTIONAL_SERVICE_ALIASES = {
  "torque_parameters": ("lateralTorqueParameters", "liveTorqueParameters"),
}
# Sunnypilot onroad params, exported under carParams.spParams by their param names (offered when one changes):
# (param, sunnypilot ui_state attribute its param thread refreshes, type, params_keys.h default).
SUNNYPILOT_UI_PARAMS = (
  ("BlindSpot", "blindspot", "bool", False),
  ("ShowTurnSignals", "turn_signals", "bool", False),
  ("TorqueBar", "torque_bar", "bool", False),
  ("DevUIInfo", "developer_ui", "int", 0),
  ("HideVEgoUI", "hide_v_ego_ui", "bool", False),
  ("TrueVEgoUI", "true_v_ego_ui", "bool", False),
  ("RoadNameToggle", "road_name_toggle", "bool", False),
  ("RocketFuel", "rocket_fuel", "bool", False),
  ("StandstillTimer", "standstill_timer", "bool", False),
  ("ChevronInfo", "chevron_metrics", "int", 4),
  ("SpeedLimitMode", "speed_limit_mode", "int", 1),
  ("RainbowMode", "rainbow_path", "bool", False),
  ("EnforceTorqueControl", "enforce_torque_control", "bool", False),
  ("CustomTorqueParams", "custom_torque_params", "bool", False),
  ("TorqueParamsOverrideEnabled", "torque_override_enabled", "bool", False),
  ("TorqueParamsOverrideFriction", "torque_override_friction", "float", 0.1),
  ("TorqueParamsOverrideLatAccelFactor", "torque_override_lat_accel_factor", "float", 2.5),
)


def _encode_json(payload: dict) -> bytes:
  if _orjson is not None:
    return _orjson.dumps(payload)
  return json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def _safe_float(value) -> float:
  try:
    result = float(value)
  except (TypeError, ValueError):
    return 0.0
  return result if math.isfinite(result) else 0.0


def _safe_int(value) -> int:
  try:
    if hasattr(value, "raw"):
      value = value.raw
    return int(value)
  except (TypeError, ValueError):
    return 0


def _safe_str(value) -> str:
  return "" if value is None else str(value)


def _text(value) -> str:
  if isinstance(value, bytes):
    return value.decode("utf-8", errors="replace")
  return value if isinstance(value, str) else ""


def _items(values) -> list:
  if values is None or isinstance(values, (bool, int, float, str, bytes)):
    return []
  try:
    return list(values)
  except TypeError:
    return []


def _float_list(values) -> list[float]:
  # Missing or scalar (an absent field read through a fake or older schema) is an empty list.
  if values is None or isinstance(values, (bool, int, float, str, bytes)):
    return []
  try:
    return [_safe_float(v) for v in values]
  except TypeError:
    return []


def _attr(source, *names, default=None):
  # getattr chain that tolerates a missing link (absent cereal field, older schema, None).
  for name in names:
    if source is None:
      return default
    source = getattr(source, name, None)
  return default if source is None else source


def _enum_text(value) -> str:
  # Cereal enumerant name as written in the schema ("preActive", "sccVision", "e2e").
  if value is None or isinstance(value, bool):
    return ""
  name = getattr(value, "name", None)
  if isinstance(name, str):
    return name
  return str(value).split(".")[-1]


def _read_param(params, key: str, kind: str, default):
  if params is None:
    return default
  try:
    if kind == "bool":
      return bool(params.get_bool(key))
    try:
      value = params.get(key, return_default=True)
    except TypeError:
      value = params.get(key)
  except Exception:
    return default
  if isinstance(value, bytes):
    value = value.decode("utf-8", errors="replace")
  return default if value is None else value


def _coerce_param(value, kind: str, default):
  try:
    if kind == "bool":
      if isinstance(value, str):
        return value.strip().lower() in ("1", "true")
      return bool(value)
    if kind == "int":
      return int(float(value))
    if kind == "float":
      result = float(value)
      return result if math.isfinite(result) else default
  except (TypeError, ValueError):
    return default
  return value


def _controls_v_cruise(controls_state) -> float:
  # Set speed in km/h: controlsState.deprecated.vCruise in every current schema, vCruiseDEPRECATED in very old ones.
  value = _attr(controls_state, "deprecated", "vCruise")
  if value is None or isinstance(value, bool):
    value = getattr(controls_state, "vCruiseDEPRECATED", 0.0)
  return _safe_float(value)


def _monitoring_policy(value):
  # driverMonitoringState.activePolicy: 0 wheeltouch, 1 vision; None when the schema has no such field.
  if value is None or isinstance(value, bool):
    return None
  name = _enum_text(value)
  if name in ("wheeltouch", "vision"):
    return 1 if name == "vision" else 0
  try:
    return int(getattr(value, "raw", value))
  except (TypeError, ValueError):
    return None


# driverMonitoringState distraction causes as one bitmask: pose 1, eye 2, phone 4, sleep 8.
_DM_DISTRACTED_BITS = (("pose", 1), ("eye", 2), ("phone", 4), ("sleep", 8))


def _monitoring_distracted_types(driver_monitoring, vision_policy) -> int:
  types = _attr(vision_policy, "distractedTypes")
  if types is not None:
    return sum(bit for name, bit in _DM_DISTRACTED_BITS if bool(getattr(types, name, False)))
  # Older schemas: distractedType's POSE (1 << 0) and BLINK (1 << 1) bits; E2E has no cause to show.
  return _safe_int(getattr(driver_monitoring, "distractedType", 0)) & 3


def _monitoring_alert_level(driver_monitoring, active_policy, awareness: float) -> int:
  # driverMonitoringState.alertLevel: 0 none, 1 warning, 2 prompt, 3 terminal. Older schemas have no level,
  # so derive it from awarenessStatus with dmonitoringd's pre/prompt thresholds (seconds till terminal over
  # the total: 8 and 6 of 11 watching the driver, 15 and 6 of 30 on wheel touch).
  level = getattr(driver_monitoring, "alertLevel", None)
  if level is not None and not isinstance(level, bool):
    name = _enum_text(level)
    if name in ("none", "one", "two", "three"):
      return ("none", "one", "two", "three").index(name)
    try:
      return max(0, min(3, int(getattr(level, "raw", level))))
    except (TypeError, ValueError):
      pass
  vision = active_policy == 1 if active_policy is not None else bool(getattr(driver_monitoring, "isActiveMode", False))
  pre, prompt = (8. / 11., 6. / 11.) if vision else (15. / 30., 6. / 30.)
  if awareness <= 0.:
    return 3
  if awareness <= prompt:
    return 2
  return 1 if awareness <= pre else 0


def _projection_caller_layout() -> str:
  # set_onroad_projection is called from the device UI's augmented_road_view. The comma four (mici) UI builds
  # its model transform in content coordinates; the comma 3/3X (big) UI includes the content rect origin.
  try:
    filename = sys._getframe(2).f_code.co_filename.replace("\\", "/")
  except (AttributeError, ValueError):
    return ""
  if "/mici/" in filename:
    return "mici"
  if "/onroad/" in filename:
    return "big"
  return ""


def _matrix3_list(values) -> list[float]:
  if values is None:
    return []
  try:
    if hasattr(values, "reshape"):
      values = values.reshape(-1).tolist()
    elif hasattr(values, "flatten"):
      values = values.flatten().tolist()
  except (TypeError, ValueError):
    pass
  flat = []
  try:
    iterator = list(values)
  except TypeError:
    iterator = []
  for value in iterator:
    if isinstance(value, (list, tuple)):
      flat.extend(_safe_float(v) for v in value)
    else:
      flat.append(_safe_float(value))
  return flat[:9]


def _path_dict(source) -> dict:
  return {
    "x": _float_list(getattr(source, "x", [])),
    "y": _float_list(getattr(source, "y", [])),
    "z": _float_list(getattr(source, "z", [])),
  }


def _lead_dict(source) -> dict:
  # radarState.LeadData presence is `present` in openpilot >= 0.11 and sunnypilot master, `status` in the
  # sunnypilot release. Both keys carry the same flag so older apps (which read `status`) keep working.
  present = bool(getattr(source, "present", False)) or bool(getattr(source, "status", False))
  return {
    "dRel": _safe_float(getattr(source, "dRel", 0.0)),
    "yRel": _safe_float(getattr(source, "yRel", 0.0)),
    "vRel": _safe_float(getattr(source, "vRel", 0.0)),
    "aRel": _safe_float(getattr(source, "aRel", 0.0)),
    "status": present,
    "present": present,
  }


def _lead_v3_dict(source) -> dict:
  # modelV2.leadsV3[i] at t=0 only (each list holds just index 0): x/y metres in the device frame, v m/s, a m/s^2.
  return {
    "prob": _safe_float(getattr(source, "prob", 0.0)),
    "x": _float_list(getattr(source, "x", None))[:1],
    "y": _float_list(getattr(source, "y", None))[:1],
    "v": _float_list(getattr(source, "v", None))[:1],
    "a": _float_list(getattr(source, "a", None))[:1],
  }


def _interp(value: float, xp: list[float], fp: list[float]) -> float:
  if value <= xp[0]:
    return fp[0]
  if value >= xp[-1]:
    return fp[-1]
  span = xp[-1] - xp[0]
  if span <= 0.0:
    return fp[-1]
  return fp[0] + (fp[-1] - fp[0]) * ((value - xp[0]) / span)


class _ServiceResolver:
  def __init__(self):
    self._resolved = {}
    self._optional = {}

  def resolve(self, ui_state, semantic: str) -> str:
    if semantic in self._resolved:
      return self._resolved[semantic]
    aliases = UPSTREAM_SERVICE_ALIASES.get(semantic)
    if aliases is None:
      raise KeyError(f"unknown upstream service semantic: {semantic}")
    service = self._first_present(ui_state, aliases)
    if service is not None:
      self._resolved[semantic] = service
      return service
    raise KeyError(f"missing {semantic} upstream service; expected one of: {', '.join(aliases)}")

  def resolve_optional(self, ui_state, semantic: str):
    # The service this UI subscribes to for an optional semantic, or None. SubMaster services are fixed at
    # start, so the answer is cached either way.
    if semantic in self._optional:
      return self._optional[semantic]
    aliases = UPSTREAM_OPTIONAL_SERVICE_ALIASES.get(semantic)
    if aliases is None:
      raise KeyError(f"unknown optional upstream service semantic: {semantic}")
    service = self._first_present(ui_state, aliases)
    self._optional[semantic] = service
    return service

  def resolve_any(self, ui_state, semantic: str):
    if semantic in UPSTREAM_OPTIONAL_SERVICE_ALIASES:
      return self.resolve_optional(ui_state, semantic)
    return self.resolve(ui_state, semantic)

  @staticmethod
  def _first_present(ui_state, aliases):
    recv_frame = getattr(ui_state.sm, "recv_frame", {})
    for service in aliases:
      try:
        present = service in recv_frame
      except TypeError:
        present = False
      if present:
        return service
    return None

  def message(self, ui_state, semantic: str):
    return ui_state.sm[self.resolve(ui_state, semantic)]

  def recv_frame(self, ui_state, semantic: str) -> int:
    return ui_state.sm.recv_frame[self.resolve(ui_state, semantic)]

  def log_mono_time(self, ui_state, semantic: str) -> int:
    service = self.resolve(ui_state, semantic)
    try:
      return int(ui_state.sm.logMonoTime.get(service, 0) or 0)
    except (AttributeError, TypeError, ValueError):
      return 0

  @property
  def resolved(self) -> dict:
    return dict(self._resolved)


def _torque_bar_value(ui_state, service_resolver=None) -> float:
  service_resolver = service_resolver or _ServiceResolver()
  if ui_state.sm.recv_frame["controlsState"] < ui_state.started_frame:
    return 0.0

  controls_state = ui_state.sm["controlsState"]
  lateral_control = getattr(controls_state, "lateralControlState", None)
  control_mode = lateral_control.which() if lateral_control is not None and hasattr(lateral_control, "which") else ""
  # openpilot/sunnypilot master estimate the bar from lateral accel for curvatureState too (torque_bar.py).
  if control_mode in ("angleState", "curvatureState"):
    if (
      ui_state.sm.recv_frame["carState"] < ui_state.started_frame or
      ui_state.sm.recv_frame["carControl"] < ui_state.started_frame or
      service_resolver.recv_frame(ui_state, "vehicle_parameters") < ui_state.started_frame
    ):
      return 0.0

    car_state = ui_state.sm["carState"]
    car_control = ui_state.sm["carControl"]
    live_parameters = service_resolver.message(ui_state, "vehicle_parameters")
    v_ego = max(_safe_float(car_state.vEgo), 0.0)
    actual_lateral_accel = _safe_float(getattr(controls_state, "curvature", 0.0)) * v_ego ** 2
    desired_lateral_accel = _safe_float(getattr(controls_state, "desiredCurvature", 0.0)) * v_ego ** 2
    roll_compensation = _safe_float(live_parameters.roll) * ACCELERATION_DUE_TO_GRAVITY * _interp(v_ego, [5.0, 15.0], [0.0, 1.0])
    lateral_accel = actual_lateral_accel - roll_compensation
    max_lateral_accel = COMMAVIEW_DEFAULT_MAX_LAT_ACCEL
    if hasattr(ui_state, "CP") and ui_state.CP is not None:
      max_lateral_accel = _safe_float(getattr(ui_state.CP, "maxLateralAccel", COMMAVIEW_DEFAULT_MAX_LAT_ACCEL)) or COMMAVIEW_DEFAULT_MAX_LAT_ACCEL
    if max_lateral_accel <= 0.0 or not bool(car_control.latActive):
      return 0.0
    return max(-1.0, min(1.0, (lateral_accel + (desired_lateral_accel - actual_lateral_accel)) / max_lateral_accel))

  if ui_state.sm.recv_frame["carOutput"] < ui_state.started_frame:
    return 0.0
  return max(-1.0, min(1.0, -_safe_float(ui_state.sm["carOutput"].actuatorsOutput.torque)))


_STATUS_MODE_NAMES = {
  "disengaged": "disengaged",
  "engaged": "engaged",
  "override": "override",
  "lat_only": "latOnly",
  "long_only": "longOnly",
}


def _status_mode_name(status) -> str:
  status_value = str(status.value if hasattr(status, "value") else status)
  return _STATUS_MODE_NAMES.get(status_value, "unknown")


def _panda_states_summary(ui_state) -> tuple[bool, bool, bool]:
  ignition_line = False
  ignition_can = False
  try:
    for panda_state in ui_state.sm["pandaStates"]:
      ignition_line = ignition_line or bool(getattr(panda_state, "ignitionLine", False))
      ignition_can = ignition_can or bool(getattr(panda_state, "ignitionCan", False))
  except (KeyError, TypeError):
    pass
  return ignition_line, ignition_can, ignition_line or ignition_can


def _enum_name(value) -> str:
  if value is None:
    return ""
  raw_name = getattr(value, "name", None)
  if raw_name is not None:
    return _safe_str(raw_name).replace("_", "").lower()
  return _safe_str(value).split(".")[-1].replace("_", "").lower()


def _enum_raw(value) -> int:
  raw = getattr(value, "raw", value)
  return _safe_int(raw)


def _speed_limit_pre_active_state(ui_state) -> tuple[bool, str]:
  try:
    speed_limit = ui_state.sm["longitudinalPlanSP"].speedLimit
    assist_state = speed_limit.assist.state
  except (AttributeError, KeyError, TypeError, IndexError):
    return False, "none"

  speed_limit_pre_active = _enum_raw(assist_state) == 2 or _enum_name(assist_state) == "preactive"
  speed_limit_pre_active_icon = "none"
  if speed_limit_pre_active:
    speed_conv = COMMAVIEW_MS_TO_KPH if ui_state.is_metric else COMMAVIEW_MS_TO_MPH
    speed_limit_final_last = ui_state.sm["longitudinalPlanSP"].speedLimit.resolver.speedLimitFinalLast
    speed_limit_round = round(_safe_float(speed_limit_final_last) * speed_conv)

    try:
      v_cruise_cluster = _safe_float(ui_state.sm["carState"].vCruiseCluster)
    except (AttributeError, KeyError, TypeError, IndexError):
      v_cruise_cluster = 0.0
    try:
      controls_v_cruise = _controls_v_cruise(ui_state.sm["controlsState"])
    except (AttributeError, KeyError, TypeError, IndexError):
      controls_v_cruise = 0.0

    set_speed = controls_v_cruise if v_cruise_cluster == 0.0 else v_cruise_cluster
    if not ui_state.is_metric:
      set_speed *= COMMAVIEW_KM_TO_MILE
    set_speed_round = round(set_speed)

    if set_speed_round < speed_limit_round:
      speed_limit_pre_active_icon = "up"
    elif set_speed_round > speed_limit_round:
      speed_limit_pre_active_icon = "down"

  return speed_limit_pre_active, speed_limit_pre_active_icon


class _CommaViewSocketExporter:
  def __init__(self, flavor: str, start_worker: bool = True):
    self._flavor = flavor
    self._socket_path = os.environ.get("COMMAVIEWD_UI_EXPORT_SOCKET") or COMMAVIEW_SOCKET_PATH_DEFAULT
    self._sock = None
    self._next_connect_attempt = 0.0
    self._active_camera = "road"
    self._active_camera_override = None
    self._driver_video_frame_matrix = []
    self._wide_camera_available_override = None
    self._latest_onroad_projection = None
    self._service_resolver = _ServiceResolver()
    self._pending = {}
    self._recipe_pending = deque()
    self._recipe_sequence = 0
    self._recipe_camera = None
    self._last_recipe_anchor_time = 0.0
    self._condition = threading.Condition()
    self._stopping = False
    self._worker_busy = False
    self._worker = None
    self._worker_enabled = start_worker
    self._last_generation = {}
    self._last_offer_time = {}
    self._last_projection_offer_time = 0.0
    self._param_cache = {}
    self._snapshot_durations_ns = deque(maxlen=512)
    self._worker_lag_ns = deque(maxlen=512)
    self._stats = {
      "offers": 0,
      "overwritten": 0,
      "rateLimited": 0,
      "snapshotExceptions": 0,
      "workerExceptions": 0,
      "socketExceptions": 0,
      "reconnects": 0,
      "sent": 0,
      "bytesSent": 0,
      "maxPending": 0,
      "recipeOverflow": 0,
      "recipeSendFailures": 0,
    }
    self._payload_builders = {
      COMMAVIEW_UI_STATE_ONROAD_SERVICE_INDEX: self._ui_state_onroad_payload,
      COMMAVIEW_SELFDRIVE_STATE_SERVICE_INDEX: self._selfdrive_state_payload,
      COMMAVIEW_CAR_STATE_SERVICE_INDEX: self._car_state_payload,
      COMMAVIEW_CONTROLS_STATE_SERVICE_INDEX: self._controls_state_payload,
      COMMAVIEW_ONROAD_EVENTS_SERVICE_INDEX: self._onroad_events_payload,
      COMMAVIEW_DRIVER_MONITORING_STATE_SERVICE_INDEX: self._driver_monitoring_state_payload,
      COMMAVIEW_DRIVER_STATE_V2_SERVICE_INDEX: self._driver_state_v2_payload,
      COMMAVIEW_MODEL_V2_SERVICE_INDEX: self._model_v2_payload,
      COMMAVIEW_RADAR_STATE_SERVICE_INDEX: self._radar_state_payload,
      COMMAVIEW_LIVE_CALIBRATION_SERVICE_INDEX: self._live_calibration_payload,
      COMMAVIEW_CAR_OUTPUT_SERVICE_INDEX: self._car_output_payload,
      COMMAVIEW_CAR_CONTROL_SERVICE_INDEX: self._car_control_payload,
      COMMAVIEW_LIVE_PARAMETERS_SERVICE_INDEX: self._live_parameters_payload,
      COMMAVIEW_LONGITUDINAL_PLAN_SERVICE_INDEX: self._longitudinal_plan_payload,
      COMMAVIEW_CAR_PARAMS_SERVICE_INDEX: self._car_params_payload,
      COMMAVIEW_DEVICE_STATE_SERVICE_INDEX: self._device_state_payload,
      COMMAVIEW_ROAD_CAMERA_STATE_SERVICE_INDEX: self._road_camera_state_payload,
      COMMAVIEW_PANDA_STATES_SUMMARY_SERVICE_INDEX: self._panda_states_summary_payload,
      COMMAVIEW_WIDE_ROAD_CAMERA_STATE_SERVICE_INDEX: self._wide_road_camera_state_payload,
    }
    self._service_specs = (
      (COMMAVIEW_UI_STATE_ONROAD_SERVICE_INDEX, self._ui_state_onroad_payload, ("selfdriveState", "carState", "deviceState", "pandaStates", "wideRoadCameraState", "starpilotCarState", "starpilotPlan")),
      (COMMAVIEW_SELFDRIVE_STATE_SERVICE_INDEX, self._selfdrive_state_payload, ("selfdriveState", "selfdriveStateSP")),
      (COMMAVIEW_CAR_STATE_SERVICE_INDEX, self._car_state_payload, ("carState",)),
      (COMMAVIEW_CONTROLS_STATE_SERVICE_INDEX, self._controls_state_payload, ("controlsState", "carOutput", "carControl", "@vehicle_parameters", "carState", "longitudinalPlanSP")),
      (COMMAVIEW_ONROAD_EVENTS_SERVICE_INDEX, self._onroad_events_payload, ("onroadEvents",)),
      (COMMAVIEW_DRIVER_MONITORING_STATE_SERVICE_INDEX, self._driver_monitoring_state_payload, ("driverMonitoringState",)),
      (COMMAVIEW_DRIVER_STATE_V2_SERVICE_INDEX, self._driver_state_v2_payload, ("driverStateV2",)),
      (COMMAVIEW_MODEL_V2_SERVICE_INDEX, self._model_v2_payload, ("modelV2",)),
      (COMMAVIEW_RADAR_STATE_SERVICE_INDEX, self._radar_state_payload, ("radarState",)),
      (COMMAVIEW_LIVE_CALIBRATION_SERVICE_INDEX, self._live_calibration_payload, ("@calibration",)),
      (COMMAVIEW_CAR_OUTPUT_SERVICE_INDEX, self._car_output_payload, ("carOutput",)),
      (COMMAVIEW_CAR_CONTROL_SERVICE_INDEX, self._car_control_payload, ("carControl",)),
      (COMMAVIEW_LIVE_PARAMETERS_SERVICE_INDEX, self._live_parameters_payload, ("@vehicle_parameters", "@torque_parameters")),
      # The sunnypilot planner and map services ride along with longitudinalPlan (absent on openpilot).
      (COMMAVIEW_LONGITUDINAL_PLAN_SERVICE_INDEX, self._longitudinal_plan_payload, ("longitudinalPlan", "longitudinalPlanSP", "liveMapDataSP")),
      # CarParams has no message of its own here: the UI loads it from params a moment after it
      # starts. It is offered when what it says changes (loaded, or another car), and commaviewd
      # keeps the latest for each client that connects later.
      (COMMAVIEW_CAR_PARAMS_SERVICE_INDEX, self._car_params_payload, ("#carParams",)),
      (COMMAVIEW_DEVICE_STATE_SERVICE_INDEX, self._device_state_payload, ("deviceState",)),
      (COMMAVIEW_ROAD_CAMERA_STATE_SERVICE_INDEX, self._road_camera_state_payload, ("@road_camera",)),
      (COMMAVIEW_PANDA_STATES_SUMMARY_SERVICE_INDEX, self._panda_states_summary_payload, ("pandaStates",)),
      (COMMAVIEW_WIDE_ROAD_CAMERA_STATE_SERVICE_INDEX, self._wide_road_camera_state_payload, ("wideRoadCameraState",)),
    )

  def set_onroad_camera(self, active_camera: str, wide_camera_available: bool, driver_video_frame_matrix=None) -> None:
    camera = _safe_str(active_camera)
    self._active_camera_override = camera if camera in ("driver", "none") else "wideRoad" if camera in ("wideRoad", "wide") else "road"
    self._wide_camera_available_override = bool(wide_camera_available)
    if driver_video_frame_matrix is not None:
      self._driver_video_frame_matrix = _matrix3_list(driver_video_frame_matrix)

  def set_onroad_projection(
    self,
    ui_state,
    active_camera: str,
    content_rect,
    video_frame_matrix,
    model_transform,
    camera_offset: float = 0.0,
    layout: str | None = None,
  ) -> None:
    now = time.monotonic()
    camera = _safe_str(active_camera)
    camera = "wideRoad" if camera in ("wideRoad", "wide") else "road"
    source_switch = camera != self._recipe_camera
    if not source_switch and self._last_projection_offer_time and now - self._last_projection_offer_time < COMMAVIEW_MIN_EXPORT_INTERVAL_SEC:
      self._stats["rateLimited"] += 1
      return
    layout = layout if layout in ("big", "mici") else _projection_caller_layout()
    transform_origin = {"big": "screen", "mici": "content"}.get(layout, "")
    road_service = self._service_resolver.resolve(ui_state, "road_camera")
    calibration_service = self._service_resolver.resolve(ui_state, "calibration")
    road_camera = self._service_msg(ui_state, road_service)
    wide_camera = self._service_msg(ui_state, "wideRoadCameraState")
    model = self._service_msg(ui_state, "modelV2")
    road_log_mono = self._service_log_mono(ui_state, road_service)
    wide_log_mono = self._service_log_mono(ui_state, "wideRoadCameraState")
    model_log_mono = self._service_log_mono(ui_state, "modelV2")
    live_calib_log_mono = self._service_log_mono(ui_state, calibration_service)
    self._latest_onroad_projection = {
      "valid": True,
      "camera": camera,
      "contentRect": {
        "x": _safe_float(getattr(content_rect, "x", 0.0)),
        "y": _safe_float(getattr(content_rect, "y", 0.0)),
        "width": _safe_float(getattr(content_rect, "width", 0.0)),
        "height": _safe_float(getattr(content_rect, "height", 0.0)),
      },
      "videoFrameMatrix": _matrix3_list(video_frame_matrix),
      "modelTransform": _matrix3_list(model_transform),
      # "big": comma 3/3X UI, "mici": comma four UI, "" unknown (older hook or caller).
      "layout": layout,
      # "screen": modelTransform maps to device screen px, so it already includes contentRect.x/y (big UI).
      # "content": modelTransform maps to content-rect-local px (mici UI). "": unknown, assume "screen".
      "transformOrigin": transform_origin,
      "cameraOffset": [0.0, _safe_float(camera_offset), 0.0],
      "roadFrameId": _safe_int(getattr(road_camera, "frameId", 0)),
      "roadTimestampEof": _safe_int(getattr(road_camera, "timestampEof", 0)),
      "wideFrameId": _safe_int(getattr(wide_camera, "frameId", 0)),
      "wideTimestampEof": _safe_int(getattr(wide_camera, "timestampEof", 0)),
      "modelFrameId": _safe_int(getattr(model, "frameId", 0)),
      "modelTimestampEof": _safe_int(getattr(model, "timestampEof", 0)),
      "liveCalibrationLogMonoTime": live_calib_log_mono,
      "logMonoTime": max(road_log_mono, wide_log_mono, model_log_mono, live_calib_log_mono),
    }
    self._last_projection_offer_time = now
    kind = "camera_switch" if source_switch else "anchor"
    if source_switch or now - self._last_recipe_anchor_time >= 5.0:
      self._offer_recipe_event(kind, self._latest_onroad_projection)
      self._recipe_camera = camera
      self._last_recipe_anchor_time = now
    self._offer_payload(COMMAVIEW_ONROAD_PROJECTION_SERVICE_INDEX, self._latest_onroad_projection)

  def publish(self, ui_state) -> None:
    started_ns = time.monotonic_ns()
    for semantic in UPSTREAM_SERVICE_ALIASES:
      self._service_resolver.resolve(ui_state, semantic)
    now = time.monotonic()
    for service_index, _, dependencies in self._service_specs:
      generation = self._generation(ui_state, dependencies)
      if self._last_generation.get(service_index) == generation:
        continue
      last_offer = self._last_offer_time.get(service_index, 0.0)
      if service_index in self._last_generation and now - last_offer < COMMAVIEW_MIN_EXPORT_INTERVAL_SEC:
        self._stats["rateLimited"] += 1
        continue
      payload_fn = self._payload_builders[service_index]
      try:
        payload = payload_fn(ui_state)
      except Exception:
        self._stats["snapshotExceptions"] += 1
        continue
      self._last_generation[service_index] = generation
      self._last_offer_time[service_index] = now
      self._offer_payload(service_index, payload)
    self._snapshot_durations_ns.append(time.monotonic_ns() - started_ns)

  def _generation(self, ui_state, dependencies) -> tuple:
    generation = []
    for dependency in dependencies:
      if dependency == "#carParams":
        generation.append((dependency, self._car_params_identity(ui_state)))
        continue
      service = self._service_resolver.resolve_any(ui_state, dependency[1:]) if dependency.startswith("@") else dependency
      if service is None:
        generation.append((dependency, -1, 0))
        continue
      try:
        recv_frame = int(ui_state.sm.recv_frame.get(service, -1))
      except (AttributeError, TypeError, ValueError):
        recv_frame = -1
      try:
        log_mono_time = int(ui_state.sm.logMonoTime.get(service, 0) or 0)
      except (AttributeError, TypeError, ValueError):
        log_mono_time = 0
      generation.append((service, recv_frame, log_mono_time))
    return tuple(generation)

  def _offer_payload(self, service_index: int, payload: dict) -> None:
    offered_ns = time.monotonic_ns()
    with self._condition:
      if self._worker_enabled and self._worker is None:
        self._worker = threading.Thread(target=self._worker_main, name="commaview-export", daemon=True)
        self._worker.start()
      if service_index in self._pending:
        self._stats["overwritten"] += 1
      self._pending[service_index] = (payload, offered_ns)
      self._stats["offers"] += 1
      self._stats["maxPending"] = max(self._stats["maxPending"], len(self._pending))
      self._condition.notify()

  def _offer_recipe_event(self, kind: str, projection: dict) -> None:
    # Preserve source camera changes separately from conflated live telemetry.
    # The UI thread only queues a small object; socket/JSON work stays on the worker.
    with self._condition:
      self._recipe_sequence += 1
      if len(self._recipe_pending) >= 128:
        self._stats["recipeOverflow"] += 1
        return
      if self._worker_enabled and self._worker is None:
        self._worker = threading.Thread(target=self._worker_main, name="commaview-export", daemon=True)
        self._worker.start()
      self._recipe_pending.append({
        "sequence": self._recipe_sequence, "kind": kind, "projection": projection,
      })
      self._condition.notify()

  def _pending_payload(self, service_index: int):
    with self._condition:
      pending = self._pending.get(service_index)
      return None if pending is None else pending[0]

  def _worker_main(self) -> None:
    while True:
      with self._condition:
        while not self._pending and not self._recipe_pending and not self._stopping:
          self._condition.wait()
        if self._stopping:
          break
        recipe_events = list(self._recipe_pending)
        self._recipe_pending.clear()
        batch = self._pending
        self._pending = {}
        self._worker_busy = True
      for event in recipe_events:
        try:
          if not self._send_json(COMMAVIEW_RECIPE_EVENT_SERVICE_INDEX, event):
            self._stats["recipeSendFailures"] += 1
        except Exception:
          self._stats["recipeSendFailures"] += 1
      for service_index, pending in batch.items():
        with self._condition:
          pending = self._pending.pop(service_index, pending)
        payload, offered_ns = pending
        self._worker_lag_ns.append(max(0, time.monotonic_ns() - offered_ns))
        try:
          self._send_json(service_index, payload)
        except OSError:
          self._stats["socketExceptions"] += 1
        except Exception:
          self._stats["workerExceptions"] += 1
      with self._condition:
        self._worker_busy = False
        self._condition.notify_all()
    self._close()
    with self._condition:
      self._worker_busy = False
      self._condition.notify_all()

  def wait_for_idle(self, timeout: float = 1.0) -> bool:
    deadline = time.monotonic() + timeout
    with self._condition:
      while self._pending or self._recipe_pending or self._worker_busy:
        remaining = deadline - time.monotonic()
        if remaining <= 0.0:
          return False
        self._condition.wait(remaining)
      return True

  def shutdown(self, timeout: float = 1.0) -> None:
    if self._worker is None:
      return
    with self._condition:
      self._stopping = True
      self._pending.clear()
      self._recipe_pending.clear()
      self._condition.notify_all()
    self._worker.join(timeout)

  def stats(self) -> dict:
    def percentile_ms(values, percentile):
      if not values:
        return 0.0
      ordered = sorted(values)
      index = min(len(ordered) - 1, int((len(ordered) - 1) * percentile))
      return ordered[index] / 1_000_000.0

    with self._condition:
      result = dict(self._stats)
      result["pending"] = len(self._pending)
      result["recipePending"] = len(self._recipe_pending)
    result.update({
      "snapshotP50Ms": percentile_ms(self._snapshot_durations_ns, 0.50),
      "snapshotP95Ms": percentile_ms(self._snapshot_durations_ns, 0.95),
      "snapshotP99Ms": percentile_ms(self._snapshot_durations_ns, 0.99),
      "snapshotMaxMs": percentile_ms(self._snapshot_durations_ns, 1.0),
      "workerLagP95Ms": percentile_ms(self._worker_lag_ns, 0.95),
    })
    return result

  def _connect(self) -> bool:
    if self._sock is not None:
      return True
    now = time.monotonic()
    if now < self._next_connect_attempt:
      return False
    sock = None
    try:
      sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
      sock.settimeout(COMMAVIEW_SOCKET_TIMEOUT_SEC)
      sock.connect(self._socket_path)
    except OSError:
      if sock is not None:
        try:
          sock.close()
        except OSError:
          pass
      self._next_connect_attempt = now + COMMAVIEW_CONNECT_RETRY_SEC
      return False
    self._sock = sock
    self._next_connect_attempt = 0.0
    self._stats["reconnects"] += 1
    return True

  def _close(self) -> None:
    if self._sock is not None:
      try:
        self._sock.close()
      except OSError:
        pass
    self._sock = None

  def _send_json(self, service_index: int, payload: dict) -> bool:
    if not self._connect():
      return False
    raw = _encode_json(payload)
    frame = bytes((COMMAVIEW_FRAME_VERSION, service_index)) + raw
    packet = struct.pack(">I", len(frame)) + frame
    try:
      self._sock.sendall(packet)
      self._stats["sent"] += 1
      self._stats["bytesSent"] += len(packet)
      return True
    except OSError:
      self._close()
      raise

  def _service_msg(self, ui_state, service: str):
    try:
      return ui_state.sm[service]
    except (KeyError, TypeError, IndexError):
      return None

  def _started_service_msg(self, ui_state, service):
    # The latest message of an optional service received during this drive, else None (not subscribed:
    # openpilot has no sunnypilot services; not received yet).
    if service is None:
      return None
    try:
      recv_frame = int(ui_state.sm.recv_frame.get(service, -1))
    except (AttributeError, TypeError, ValueError):
      return None
    if recv_frame < 0 or recv_frame < ui_state.started_frame:
      return None
    return self._service_msg(ui_state, service)

  def _service_valid(self, ui_state, service) -> bool:
    try:
      return bool(ui_state.sm.valid[service])
    except (AttributeError, KeyError, TypeError):
      return False

  def _param_value(self, ui_state, key: str, kind: str, default, attr: str | None = None):
    # The value the UI already parsed (sunnypilot refreshes these on its own param thread). Without such an
    # attribute the param file is read at most once per COMMAVIEW_PARAMS_REFRESH_SEC.
    if attr:
      value = getattr(ui_state, attr, None)
      if value is not None:
        return _coerce_param(value, kind, default)
    now = time.monotonic()
    cached = self._param_cache.get(key)
    if cached is not None and now - cached[0] < COMMAVIEW_PARAMS_REFRESH_SEC:
      return cached[1]
    value = _coerce_param(_read_param(getattr(ui_state, "params", None), key, kind, default), kind, default)
    self._param_cache[key] = (now, value)
    return value

  def _sunnypilot_params(self, ui_state) -> dict | None:
    if self._flavor != "SUNNYPILOT":
      return None
    values = {}
    for key, attr, kind, default in SUNNYPILOT_UI_PARAMS:
      value = getattr(ui_state, attr, None)
      values[key] = default if value is None else _coerce_param(value, kind, default)
    # What the device's model renderer adds to model y (path, lanes, edges, lead): the CameraOffset param,
    # applied only while a custom model bundle is active (onroad/model_renderer.py, re-read every 3 s there).
    camera_offset = 0.0
    if getattr(ui_state, "active_bundle", None):
      camera_offset = self._param_value(ui_state, "CameraOffset", "float", 0.0)
    values["CameraOffset"] = camera_offset
    return values

  def _service_log_mono(self, ui_state, *services: str) -> int:
    values = []
    for service in services:
      try:
        values.append(int(ui_state.sm.logMonoTime.get(service, 0) or 0))
      except (AttributeError, TypeError, ValueError):
        values.append(0)
    return max(values) if values else 0

  def _wide_camera_available(self, ui_state) -> bool:
    if self._wide_camera_available_override is not None:
      return bool(self._wide_camera_available_override)
    try:
      return bool(ui_state.sm.alive["wideRoadCameraState"]) and bool(ui_state.sm.valid["wideRoadCameraState"])
    except (AttributeError, KeyError, TypeError):
      return False

  def _active_camera_name(self, ui_state) -> str:
    wide_available = self._wide_camera_available(ui_state)
    if self._active_camera_override is not None:
      self._active_camera = "road" if self._active_camera_override == "wideRoad" and not wide_available else self._active_camera_override
      return self._active_camera

    if not wide_available:
      self._active_camera = "road"
      return self._active_camera

    if (
      ui_state.sm.recv_frame["selfdriveState"] < ui_state.started_frame or
      ui_state.sm.recv_frame["carState"] < ui_state.started_frame
    ):
      self._active_camera = "road"
      return self._active_camera

    selfdrive_state = ui_state.sm["selfdriveState"]
    car_state = ui_state.sm["carState"]
    experimental_mode = bool(getattr(selfdrive_state, "experimentalMode", False))
    v_ego = _safe_float(getattr(car_state, "vEgo", 0.0))

    if not experimental_mode:
      target = "road"
    elif v_ego < 5.0:
      target = "wideRoad"
    elif v_ego > 10.0:
      target = "road"
    else:
      target = self._active_camera

    self._active_camera = "wideRoad" if target == "wideRoad" else "road"
    return self._active_camera

  def _ui_state_onroad_payload(self, ui_state) -> dict:
    _, _, ignition = _panda_states_summary(ui_state)
    wide_camera_available = self._wide_camera_available(ui_state)
    active_camera = self._active_camera_name(ui_state)
    starpilot = self._starpilot_payload(ui_state, active_camera)
    status = _status_mode_name(ui_state.status)
    if status != "override":
      if starpilot["longitudinalOnlyActive"]:
        status = "longOnly"
      elif starpilot["alwaysOnLateralActive"]:
        status = "latOnly"
    return {
      "exportVersion": 1,
      "started": bool(ui_state.started),
      "ignition": bool(getattr(ui_state, "ignition", ignition)),
      "status": status,
      "isMetric": bool(ui_state.is_metric),
      "alwaysOnDm": bool(self._param_value(ui_state, "AlwaysOnDM", "bool", False, "always_on_dm")),
      "hasLongitudinalControl": bool(getattr(ui_state, "has_longitudinal_control", False)),
      "startedFrame": _safe_int(getattr(ui_state, "started_frame", 0)),
      # time.monotonic() seconds when the UI went onroad: the clock of every logMonoTime / 1e9.
      "startedTime": _safe_float(getattr(ui_state, "started_time", 0.0)),
      "activeCamera": active_camera,
      "wideCameraAvailable": wide_camera_available,
      "runtimeFlavor": self._flavor,
      "rainbowPathEnabled": starpilot["rainbowPathEnabled"],
      "starpilot": starpilot,
      "logMonoTime": self._service_log_mono(ui_state, "deviceState", "pandaStates", "selfdriveState"),
    }

  def _starpilot_payload(self, ui_state, active_camera: str) -> dict:
    car_state = self._started_service_msg(ui_state, "carState")
    star_car = self._started_service_msg(ui_state, "starpilotCarState")
    selfdrive = self._started_service_msg(ui_state, "selfdriveState")
    enabled = bool(_attr(selfdrive, "enabled", default=False))
    lateral = bool(getattr(ui_state, "always_on_lateral_active", False))
    long_only = enabled and bool(_attr(star_car, "pauseLateral", default=False))
    reverse = _enum_text(_attr(car_state, "gearShifter", default="")).lower() == "reverse"
    params = getattr(ui_state, "ui_params", None)

    def param(name, kind, default):
      if params is None:
        return default
      try:
        return getattr(params, "get_" + kind)(name, default=default)
      except (AttributeError, TypeError, ValueError):
        return default

    # Export resolved upstream colors, rather than duplicate StarPilot's status priorities in
    # every client. Imports occur only when its UI is fully initialized; failure stays UI-safe.
    def argb(color):
      return (int(color.a) << 24) | (int(color.r) << 16) | (int(color.g) << 8) | int(color.b)

    border = path_edge = screen_edge = 0xFF122839
    banner = ""
    try:
      from openpilot.selfdrive.ui.lib.starpilot_status import (
        get_border_color, get_path_edge_color, get_screen_edge_color, get_mode_transition_banner_text,
      )
      border = argb(get_border_color(ui_state))
      path_edge = argb(get_path_edge_color(ui_state))
      screen_edge = argb(get_screen_edge_color(ui_state))
      banner = get_mode_transition_banner_text(ui_state) or ""
    except (ImportError, AttributeError, KeyError, TypeError, ValueError):
      pass

    model_ui = bool(param("ModelUI", "bool", True))
    metric = bool(ui_state.is_metric)
    path_width = 0.9
    lane_width = edge_width = None
    if model_ui:
      value = _safe_float(param("PathWidth", "float", 6.1))
      if not math.isclose(value, 6.1, rel_tol=1e-5, abs_tol=1e-8):
        path_width = max(0.0, value) * (1.0 if metric else 0.3048) / 2.0
      for name, default in (("LaneLinesWidth", 4.0), ("RoadEdgesWidth", 2.0)):
        value = _safe_float(param(name, "float", default))
        if not math.isclose(value, default, rel_tol=1e-5, abs_tol=1e-8):
          width = max(0.0, value) * (0.01 if metric else 0.0254) / 2.0
          if name == "LaneLinesWidth":
            lane_width = width
          else:
            edge_width = width
      if param("DynamicPathWidth", "bool", False):
        path_width *= 1.0 if _status_mode_name(ui_state.status) == "engaged" else 0.75 if lateral else 0.5
    path_color = 0xFF30FF9C
    try:
      import pyray as rl
      from openpilot.selfdrive.ui.lib.starpilot_theme import get_visual_color
      path_color = argb(get_visual_color(params, "PathColor", "Path", rl.Color(48, 255, 156, 255)))
    except (ImportError, AttributeError, KeyError, TypeError, ValueError):
      pass
    toggles = getattr(ui_state, "starpilot_toggles", {}) or {}
    return {
      "received": star_car is not None,
      "controlsEnabled": enabled,
      "driverVideoFrameMatrix": self._driver_video_frame_matrix,
      "alwaysOnLateralActive": lateral,
      "longitudinalOnlyActive": long_only,
      "trafficModeEnabled": bool(getattr(ui_state, "traffic_mode_enabled", False)),
      "switchbackModeEnabled": bool(getattr(ui_state, "switchback_mode_enabled", False)),
      "conditionalStatus": _safe_int(getattr(ui_state, "conditional_status", 0)),
      "modeBanner": banner,
      "borderColor": border,
      "pathEdgeColor": path_edge,
      "screenEdgeColor": screen_edge,
      "reverse": reverse,
      "drawRoadOverlays": not reverse and active_camera not in ("driver", "none"),
      "drawHudControls": active_camera == "none" or (not reverse and active_camera != "driver"),
      "pathHalfWidthM": path_width,
      "laneLineHalfWidthM": lane_width,
      "roadEdgeHalfWidthM": edge_width,
      "rainbowPathEnabled": bool(param("RainbowPath", "bool", False)),
      "accelerationPath": bool(param("AccelerationPath", "bool", True)),
      "pathColor": path_color,
      "videoDisabledReason": "StarPilot Disable Logging stops the video encoder. Turn off Disable Logging to use Live View." if toggles.get("no_logging", False) else "",
    }

  def _selfdrive_state_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "enabled": False,
      "active": False,
      "engageable": False,
      "alertText1": "",
      "alertText2": "",
      "alertType": "none",
      "alertStatus": 0,
      "alertSize": 0,
      "alertHudVisual": 0,
      "experimentalMode": False,
      "logMonoTime": self._service_log_mono(ui_state, "selfdriveState", "selfdriveStateSP"),
    }
    if ui_state.sm.recv_frame["selfdriveState"] >= ui_state.started_frame:
      selfdrive_state = ui_state.sm["selfdriveState"]
      payload.update({
        "enabled": bool(selfdrive_state.enabled),
        "active": bool(selfdrive_state.active),
        "engageable": bool(selfdrive_state.engageable),
        "alertText1": _safe_str(selfdrive_state.alertText1),
        "alertText2": _safe_str(selfdrive_state.alertText2),
        "alertType": _safe_str(selfdrive_state.alertType),
        "alertStatus": _safe_int(selfdrive_state.alertStatus),
        "alertSize": _safe_int(selfdrive_state.alertSize),
        "alertHudVisual": _safe_int(getattr(selfdrive_state, "alertHudVisual", 0)),
        "experimentalMode": bool(selfdrive_state.experimentalMode),
      })
    # sunnypilot MADS (selfdriveStateSP.mads); absent on openpilot.
    mads = _attr(self._started_service_msg(ui_state, "selfdriveStateSP"), "mads")
    if mads is not None:
      payload["mads"] = {
        "state": _enum_text(getattr(mads, "state", None)),
        "enabled": bool(getattr(mads, "enabled", False)),
        "active": bool(getattr(mads, "active", False)),
        "available": bool(getattr(mads, "available", False)),
      }
    return payload

  def _car_state_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "vEgo": 0.0,
      "vEgoCluster": 0.0,
      "vCruiseCluster": 0.0,
      "standstill": False,
      "steeringAngleDeg": 0.0,
      "steeringPressed": False,
      "leftBlinker": False,
      "rightBlinker": False,
      "leftBlindspot": False,
      "rightBlindspot": False,
      "aEgo": 0.0,
      "steeringTorqueEps": 0.0,
      "brakePressed": False,
      "gasPressed": False,
      "cruiseState": {"speedCluster": 0.0},
      "logMonoTime": self._service_log_mono(ui_state, "carState"),
    }
    if ui_state.sm.recv_frame["carState"] >= ui_state.started_frame:
      car_state = ui_state.sm["carState"]
      cruise_state = getattr(car_state, "cruiseState", None)
      payload.update({
        "vEgo": _safe_float(car_state.vEgo),
        "vEgoCluster": _safe_float(car_state.vEgoCluster),
        "vCruiseCluster": _safe_float(getattr(car_state, "vCruiseCluster", 0.0)),
        "standstill": bool(car_state.standstill),
        "steeringAngleDeg": _safe_float(car_state.steeringAngleDeg),
        "steeringPressed": bool(car_state.steeringPressed),
        "leftBlinker": bool(car_state.leftBlinker),
        "rightBlinker": bool(car_state.rightBlinker),
        "leftBlindspot": bool(car_state.leftBlindspot),
        "rightBlindspot": bool(car_state.rightBlindspot),
        "aEgo": _safe_float(getattr(car_state, "aEgo", 0.0)),
        "steeringTorqueEps": _safe_float(getattr(car_state, "steeringTorqueEps", 0.0)),
        "brakePressed": bool(getattr(car_state, "brakePressed", False)),
        "gasPressed": bool(getattr(car_state, "gasPressed", False)),
        # Set speed as shown on the car's cluster, m/s (sunnypilot ICBM).
        "cruiseState": {"speedCluster": _safe_float(getattr(cruise_state, "speedCluster", 0.0))},
      })
    return payload

  def _controls_state_payload(self, ui_state) -> dict:
    speed_limit_pre_active, speed_limit_pre_active_icon = _speed_limit_pre_active_state(ui_state)
    payload = {
      "exportVersion": 1,
      "enabled": False,
      "active": False,
      "engageable": False,
      "alertText1": "",
      "alertText2": "",
      "alertType": "none",
      "alertStatus": 0,
      "alertSize": 0,
      "alertHudVisual": 0,
      "experimentalMode": False,
      "speedLimitPreActive": speed_limit_pre_active,
      "speedLimitPreActiveIcon": speed_limit_pre_active_icon,
      "vCruise": 0.0,
      "vCruiseDEPRECATED": 0.0,
      "lateralControlStateWhich": "",
      "curvature": 0.0,
      "desiredCurvature": 0.0,
      "angleStateSteeringAngleDeg": 0.0,
      "pidStateSteeringAngleDesiredDeg": 0.0,
      "torqueBarValue": 0.0,
      "logMonoTime": max(
        self._service_log_mono(ui_state, "controlsState", "carOutput", "carControl", "carState", "longitudinalPlanSP"),
        self._service_resolver.log_mono_time(ui_state, "vehicle_parameters"),
      ),
    }
    if ui_state.sm.recv_frame["controlsState"] >= ui_state.started_frame:
      controls_state = ui_state.sm["controlsState"]
      lateral_control = getattr(controls_state, "lateralControlState", None)
      control_mode = lateral_control.which() if lateral_control is not None and hasattr(lateral_control, "which") else ""
      v_cruise = _controls_v_cruise(controls_state)
      payload.update({
        "enabled": bool(getattr(controls_state, "enabled", False)),
        "active": bool(getattr(controls_state, "active", False)),
        "engageable": bool(getattr(controls_state, "engageable", False)),
        "alertText1": _safe_str(getattr(controls_state, "alertText1", "")),
        "alertText2": _safe_str(getattr(controls_state, "alertText2", "")),
        "alertType": _safe_str(getattr(controls_state, "alertType", "none")),
        "alertStatus": _safe_int(getattr(controls_state, "alertStatus", 0)),
        "alertSize": _safe_int(getattr(controls_state, "alertSize", 0)),
        "alertHudVisual": _safe_int(getattr(controls_state, "alertHudVisual", 0)),
        "experimentalMode": bool(getattr(controls_state, "experimentalMode", False)),
        "vCruise": v_cruise,
        # Same value under the old key, for apps that predate "vCruise".
        "vCruiseDEPRECATED": v_cruise,
        "lateralControlStateWhich": _safe_str(control_mode),
        "curvature": _safe_float(getattr(controls_state, "curvature", 0.0)),
        "desiredCurvature": _safe_float(getattr(controls_state, "desiredCurvature", 0.0)),
      })
      # Only the active union member may be read.
      try:
        if control_mode == "angleState":
          payload["angleStateSteeringAngleDeg"] = _safe_float(getattr(lateral_control.angleState, "steeringAngleDeg", 0.0))
        elif control_mode == "pidState":
          payload["pidStateSteeringAngleDesiredDeg"] = _safe_float(getattr(lateral_control.pidState, "steeringAngleDesiredDeg", 0.0))
      except Exception:
        pass
    payload["torqueBarValue"] = _safe_float(_torque_bar_value(ui_state, self._service_resolver))
    return payload

  def _onroad_events_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "events": [],
      "logMonoTime": self._service_log_mono(ui_state, "onroadEvents"),
    }
    if ui_state.sm.recv_frame["onroadEvents"] >= ui_state.started_frame:
      payload["events"] = [
        {
          "name": _safe_int(onroad_event.name.raw if hasattr(onroad_event.name, "raw") else onroad_event.name),
          "enable": bool(onroad_event.enable),
          "noEntry": bool(onroad_event.noEntry),
          "warning": bool(onroad_event.warning),
          "userDisable": bool(onroad_event.userDisable),
          "softDisable": bool(onroad_event.softDisable),
          "immediateDisable": bool(onroad_event.immediateDisable),
          "preEnable": bool(onroad_event.preEnable),
          "permanent": bool(onroad_event.permanent),
          "overrideLateral": bool(onroad_event.overrideLateral),
          "overrideLongitudinal": bool(onroad_event.overrideLongitudinal),
        }
        for onroad_event in ui_state.sm["onroadEvents"]
      ]
    return payload

  def _driver_monitoring_state_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "faceDetected": False,
      "isDistracted": False,
      "isRHD": False,
      "poseYawOffset": 0.0,
      "posePitchOffset": 0.0,
      "poseYawValidCount": 0,
      "posePitchValidCount": 0,
      "isLowStd": False,
      "isActiveMode": False,
      "activePolicy": -1,
      "awarenessPercent": 100,
      "attentionPercent": -1,
      "alertLevel": -1,
      "distractedTypes": 0,
      "posePitch": 0.0,
      "poseYaw": 0.0,
      "logMonoTime": self._service_log_mono(ui_state, "driverMonitoringState"),
    }
    if ui_state.sm.recv_frame["driverMonitoringState"] >= ui_state.started_frame:
      driver_monitoring = ui_state.sm["driverMonitoringState"]
      vision_policy = getattr(driver_monitoring, "visionPolicyState", None)
      pose = getattr(vision_policy, "pose", None)
      yaw_calib = getattr(pose, "yawCalib", None)
      pitch_calib = getattr(pose, "pitchCalib", None)
      active_policy = _monitoring_policy(getattr(driver_monitoring, "activePolicy", None))
      if active_policy is not None:
        # Current schema: the UI dims the face unless activePolicy == vision. Older apps read isActiveMode.
        payload.update({
          "activePolicy": active_policy,
          "isActiveMode": active_policy == 1,
          "awarenessPercent": _safe_int(getattr(vision_policy, "awarenessPercent", 100)),
          "posePitch": _safe_float(getattr(pose, "pitch", 0.0)),
          "poseYaw": _safe_float(getattr(pose, "yaw", 0.0)),
        })
      else:
        payload["isActiveMode"] = bool(getattr(driver_monitoring, "isActiveMode", False))
      payload.update({
        "faceDetected": bool(getattr(driver_monitoring, "faceDetected", getattr(vision_policy, "faceDetected", False))),
        "isDistracted": bool(getattr(driver_monitoring, "isDistracted", getattr(vision_policy, "isDistracted", False))),
        "isRHD": bool(getattr(driver_monitoring, "isRHD", False)),
        "poseYawOffset": _safe_float(getattr(driver_monitoring, "poseYawOffset", getattr(yaw_calib, "offset", 0.0))),
        "posePitchOffset": _safe_float(getattr(driver_monitoring, "posePitchOffset", getattr(pitch_calib, "offset", 0.0))),
        "poseYawValidCount": _safe_int(getattr(driver_monitoring, "poseYawValidCount", getattr(yaw_calib, "calibratedPercent", 0))),
        "posePitchValidCount": _safe_int(getattr(driver_monitoring, "posePitchValidCount", getattr(pitch_calib, "calibratedPercent", 0))),
        "isLowStd": bool(getattr(driver_monitoring, "isLowStd", False)),
      })
      # The active policy's awareness (the vision or wheel-touch one; awarenessStatus on older schemas), how
      # far dmonitoringd has escalated, and why the driver counts as distracted.
      if active_policy is not None:
        policy_state = vision_policy if active_policy == 1 else getattr(driver_monitoring, "wheeltouchPolicyState", None)
        attention = _safe_int(getattr(policy_state, "awarenessPercent", 100)) / 100.
      else:
        attention = _safe_float(getattr(driver_monitoring, "awarenessStatus", 1.0))
      attention = max(0., min(1., attention))
      payload.update({
        "attentionPercent": int(round(attention * 100)),
        "alertLevel": _monitoring_alert_level(driver_monitoring, active_policy, attention),
        "distractedTypes": _monitoring_distracted_types(driver_monitoring, vision_policy),
      })
    return payload

  def _driver_data_payload(self, driver_data) -> dict:
    return {
      "faceOrientation": _float_list(getattr(driver_data, "faceOrientation", [])),
      "faceOrientationStd": _float_list(getattr(driver_data, "faceOrientationStd", [])),
      "facePosition": _float_list(getattr(driver_data, "facePosition", [])),
      "facePositionStd": _float_list(getattr(driver_data, "facePositionStd", [])),
      "faceProb": _safe_float(getattr(driver_data, "faceProb", 0.0)),
      "leftEyeProb": _safe_float(getattr(driver_data, "leftEyeProb", 0.0)),
      "rightEyeProb": _safe_float(getattr(driver_data, "rightEyeProb", 0.0)),
      "leftBlinkProb": _safe_float(getattr(driver_data, "leftBlinkProb", 0.0)),
      "rightBlinkProb": _safe_float(getattr(driver_data, "rightBlinkProb", 0.0)),
      "sunglassesProb": _safe_float(getattr(driver_data, "sunglassesProb", 0.0)),
      "phoneProb": _safe_float(getattr(driver_data, "phoneProb", 0.0)),
    }

  def _driver_state_v2_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "frameId": 0,
      "modelExecutionTime": 0.0,
      "gpuExecutionTime": 0.0,
      "wheelOnRightProb": 0.0,
      "leftDriverData": self._driver_data_payload(None),
      "rightDriverData": self._driver_data_payload(None),
      "logMonoTime": self._service_log_mono(ui_state, "driverStateV2"),
    }
    if ui_state.sm.recv_frame["driverStateV2"] >= ui_state.started_frame:
      driver_state = ui_state.sm["driverStateV2"]
      payload.update({
        "frameId": _safe_int(getattr(driver_state, "frameId", 0)),
        "modelExecutionTime": _safe_float(getattr(driver_state, "modelExecutionTime", 0.0)),
        "gpuExecutionTime": _safe_float(getattr(driver_state, "gpuExecutionTime", 0.0)),
        "wheelOnRightProb": _safe_float(getattr(driver_state, "wheelOnRightProb", 0.0)),
        "leftDriverData": self._driver_data_payload(driver_state.leftDriverData),
        "rightDriverData": self._driver_data_payload(driver_state.rightDriverData),
      })
    return payload

  def _model_v2_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "frameId": 0,
      "frameIdExtra": 0,
      "frameAge": 0,
      "frameDropPerc": 0.0,
      "timestampEof": 0,
      "position": {"x": [], "y": [], "z": []},
      "laneLines": [],
      "laneLineProbs": [],
      "laneLineStds": [],
      "roadEdges": [],
      "roadEdgeStds": [],
      "meta": {
        "disengagePredictions": {
          "t": [],
          "brakeDisengageProbs": [],
          "gasDisengageProbs": [],
          "steerOverrideProbs": [],
          "brake3MetersPerSecondSquaredProbs": [],
          "brake4MetersPerSecondSquaredProbs": [],
          "brake5MetersPerSecondSquaredProbs": [],
          "gasPressProbs": [],
          "brakePressProbs": [],
        },
      },
      "acceleration": {"x": []},
      "leadsV3": [],
      "logMonoTime": self._service_log_mono(ui_state, "modelV2"),
    }
    if ui_state.sm.recv_frame["modelV2"] >= ui_state.started_frame:
      model = ui_state.sm["modelV2"]
      disengage_predictions = getattr(getattr(model, "meta", None), "disengagePredictions", None)
      payload.update({
        "frameId": _safe_int(model.frameId),
        "frameIdExtra": _safe_int(model.frameIdExtra),
        "frameAge": _safe_int(model.frameAge),
        "frameDropPerc": _safe_float(model.frameDropPerc),
        "timestampEof": _safe_int(model.timestampEof),
        "position": _path_dict(model.position),
        "laneLines": [_path_dict(line) for line in model.laneLines],
        "laneLineProbs": _float_list(getattr(model, "laneLineProbs", [])),
        "laneLineStds": _float_list(getattr(model, "laneLineStds", [])),
        "roadEdges": [_path_dict(edge) for edge in model.roadEdges],
        "roadEdgeStds": _float_list(getattr(model, "roadEdgeStds", [])),
        "meta": {
          "disengagePredictions": {
            "t": _float_list(getattr(disengage_predictions, "t", [])),
            "brakeDisengageProbs": _float_list(getattr(disengage_predictions, "brakeDisengageProbs", [])),
            "gasDisengageProbs": _float_list(getattr(disengage_predictions, "gasDisengageProbs", [])),
            "steerOverrideProbs": _float_list(getattr(disengage_predictions, "steerOverrideProbs", [])),
            "brake3MetersPerSecondSquaredProbs": _float_list(getattr(disengage_predictions, "brake3MetersPerSecondSquaredProbs", [])),
            "brake4MetersPerSecondSquaredProbs": _float_list(getattr(disengage_predictions, "brake4MetersPerSecondSquaredProbs", [])),
            "brake5MetersPerSecondSquaredProbs": _float_list(getattr(disengage_predictions, "brake5MetersPerSecondSquaredProbs", [])),
            "gasPressProbs": _float_list(getattr(disengage_predictions, "gasPressProbs", [])),
            "brakePressProbs": _float_list(getattr(disengage_predictions, "brakePressProbs", [])),
          },
        },
        "acceleration": {
          "x": _float_list(getattr(getattr(model, "acceleration", None), "x", [])),
        },
        # openpilot master's comma four lead bar uses leadsV3[0..1] (prob, x[0], y[0]) in e2e plans.
        "leadsV3": [_lead_v3_dict(lead) for lead in _items(getattr(model, "leadsV3", None))[:COMMAVIEW_MAX_MODEL_LEADS]],
      })
    return payload

  def _radar_state_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "valid": bool(ui_state.sm.valid["radarState"]),
      "leadOne": _lead_dict(None),
      "leadTwo": _lead_dict(None),
      "logMonoTime": self._service_log_mono(ui_state, "radarState"),
    }
    if ui_state.sm.recv_frame["radarState"] >= ui_state.started_frame:
      radar_state = ui_state.sm["radarState"]
      payload.update({
        "leadOne": _lead_dict(radar_state.leadOne),
        "leadTwo": _lead_dict(radar_state.leadTwo),
      })
    return payload

  def _live_calibration_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "rpyCalib": [],
      "height": [],
      "calStatus": 0,
      "calPerc": 0,
      "wideFromDeviceEuler": [],
      "logMonoTime": self._service_resolver.log_mono_time(ui_state, "calibration"),
    }
    if self._service_resolver.recv_frame(ui_state, "calibration") >= ui_state.started_frame:
      live_calibration = self._service_resolver.message(ui_state, "calibration")
      payload.update({
        "rpyCalib": _float_list(live_calibration.rpyCalib),
        "height": _float_list(live_calibration.height),
        "calStatus": _safe_int(live_calibration.calStatus),
        "calPerc": _safe_int(live_calibration.calPerc),
        "wideFromDeviceEuler": _float_list(getattr(live_calibration, "wideFromDeviceEuler", [])),
      })
    return payload

  def _car_output_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "actuatorsOutput": {"torque": 0.0},
      "logMonoTime": self._service_log_mono(ui_state, "carOutput"),
    }
    if ui_state.sm.recv_frame["carOutput"] >= ui_state.started_frame:
      payload["actuatorsOutput"] = {
        "torque": _safe_float(ui_state.sm["carOutput"].actuatorsOutput.torque),
      }
    return payload

  def _car_control_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "enabled": False,
      "latActive": False,
      "longActive": False,
      "cruiseControl": {"override": False},
      "hudControl": {
        "setSpeed": 0.0,
        "speedVisible": False,
      },
      "logMonoTime": self._service_log_mono(ui_state, "carControl"),
    }
    if ui_state.sm.recv_frame["carControl"] >= ui_state.started_frame:
      car_control = ui_state.sm["carControl"]
      hud_control = getattr(car_control, "hudControl", None)
      cruise_control = getattr(car_control, "cruiseControl", None)
      payload.update({
        "enabled": bool(getattr(car_control, "enabled", False)),
        "latActive": bool(car_control.latActive),
        "longActive": bool(car_control.longActive),
        "cruiseControl": {
          "override": bool(getattr(cruise_control, "override", False)),
        },
        "hudControl": {
          "setSpeed": _safe_float(getattr(hud_control, "setSpeed", 0.0)),
          "speedVisible": bool(getattr(hud_control, "speedVisible", False)),
        },
      })
    return payload

  def _live_parameters_payload(self, ui_state) -> dict:
    vehicle_parameters = self._service_resolver.resolve(ui_state, "vehicle_parameters")
    torque_parameters = self._service_resolver.resolve_optional(ui_state, "torque_parameters")
    payload = {
      "exportVersion": 1,
      "roll": 0.0,
      "valid": False,
      # SubMaster validity of the service (what the sunnypilot developer UI checks before using roll).
      "serviceValid": self._service_valid(ui_state, vehicle_parameters),
      "logMonoTime": self._service_log_mono(ui_state, vehicle_parameters, torque_parameters),
    }
    if self._service_resolver.recv_frame(ui_state, "vehicle_parameters") >= ui_state.started_frame:
      message = self._service_resolver.message(ui_state, "vehicle_parameters")
      payload["roll"] = _safe_float(message.roll)
      payload["valid"] = bool(getattr(message, "valid", False))
    # sunnypilot developer UI FRIC. / L.A.F. (liveTorqueParameters on the release, lateralTorqueParameters on master).
    torque = self._started_service_msg(ui_state, torque_parameters)
    if torque is not None:
      live_valid = getattr(torque, "liveValid", None)
      payload["torqueParameters"] = {
        "frictionCoefficientFiltered": _safe_float(getattr(torque, "frictionCoefficientFiltered", 0.0)),
        "latAccelFactorFiltered": _safe_float(getattr(torque, "latAccelFactorFiltered", 0.0)),
        # liveValid (release) / valid (master): the estimate is live, drawn green.
        "liveValid": bool(live_valid if live_valid is not None else getattr(torque, "valid", False)),
        "serviceValid": self._service_valid(ui_state, torque_parameters),
        "logMonoTime": self._service_log_mono(ui_state, torque_parameters),
      }
    return payload

  def _longitudinal_plan_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "allowThrottle": False,
      "longitudinalPlanSource": "",
      "logMonoTime": self._service_log_mono(ui_state, "longitudinalPlan", "longitudinalPlanSP", "liveMapDataSP"),
    }
    if ui_state.sm.recv_frame["longitudinalPlan"] >= ui_state.started_frame:
      plan = ui_state.sm["longitudinalPlan"]
      payload["allowThrottle"] = bool(getattr(plan, "allowThrottle", False))
      payload["longitudinalPlanSource"] = _enum_text(getattr(plan, "longitudinalPlanSource", None))
    plan_sp = self._started_service_msg(ui_state, "longitudinalPlanSP")
    if plan_sp is not None:
      payload["longitudinalPlanSP"] = self._longitudinal_plan_sp(ui_state, plan_sp)
    map_data = self._started_service_msg(ui_state, "liveMapDataSP")
    if map_data is not None:
      payload["liveMapDataSP"] = {
        "speedLimitValid": bool(getattr(map_data, "speedLimitValid", False)),
        "speedLimit": _safe_float(getattr(map_data, "speedLimit", 0.0)),
        "speedLimitAheadValid": bool(getattr(map_data, "speedLimitAheadValid", False)),
        "speedLimitAhead": _safe_float(getattr(map_data, "speedLimitAhead", 0.0)),
        "speedLimitAheadDistance": _safe_float(getattr(map_data, "speedLimitAheadDistance", 0.0)),
        "roadName": _text(getattr(map_data, "roadName", "")),
        "logMonoTime": self._service_log_mono(ui_state, "liveMapDataSP"),
      }
    return payload

  def _longitudinal_plan_sp(self, ui_state, plan_sp) -> dict:
    # Speeds are m/s as published; the UI multiplies by its km/h or mph factor.
    resolver = _attr(plan_sp, "speedLimit", "resolver")
    assist = _attr(plan_sp, "speedLimit", "assist")
    vision = _attr(plan_sp, "smartCruiseControl", "vision")
    map_ = _attr(plan_sp, "smartCruiseControl", "map")
    e2e_alerts = _attr(plan_sp, "e2eAlerts")
    # Only what the sunnypilot onroad UI reads (speed_limit.py, smart_cruise_control.py, circular_alerts.py,
    # hud_renderer.py), since this rides along at the plan's rate.
    return {
      "speedLimit": {
        "resolver": {
          "speedLimit": _safe_float(getattr(resolver, "speedLimit", 0.0)),
          "speedLimitLast": _safe_float(getattr(resolver, "speedLimitLast", 0.0)),
          "speedLimitFinalLast": _safe_float(getattr(resolver, "speedLimitFinalLast", 0.0)),
          "speedLimitOffset": _safe_float(getattr(resolver, "speedLimitOffset", 0.0)),
          "speedLimitValid": bool(getattr(resolver, "speedLimitValid", False)),
          "speedLimitLastValid": bool(getattr(resolver, "speedLimitLastValid", False)),
          "source": _enum_text(getattr(resolver, "source", None)),
        },
        "assist": {
          "state": _enum_text(getattr(assist, "state", None)),
          "active": bool(getattr(assist, "active", False)),
        },
      },
      "smartCruiseControl": {
        "vision": {
          "enabled": bool(getattr(vision, "enabled", False)),
          "active": bool(getattr(vision, "active", False)),
        },
        "map": {
          "enabled": bool(getattr(map_, "enabled", False)),
          "active": bool(getattr(map_, "active", False)),
        },
      },
      "e2eAlerts": {
        "greenLightAlert": bool(getattr(e2e_alerts, "greenLightAlert", False)),
        "leadDepartAlert": bool(getattr(e2e_alerts, "leadDepartAlert", False)),
      },
      "logMonoTime": self._service_log_mono(ui_state, "longitudinalPlanSP"),
    }

  def _car_params_identity(self, ui_state) -> tuple:
    # Everything carParams carries that can change: the car, and the slow-changing UI settings that ride along
    # (sunnypilot params, CarParamsSP, the service names this UI resolved). Built from values the UI already
    # holds, so it is cheap to compute every frame; the payload is offered only when this changes.
    car_params = getattr(ui_state, "CP", None)
    car = () if car_params is None else tuple(_safe_str(getattr(car_params, field, "")) for field in (
      "carFingerprint", "carName", "carVin", "openpilotLongitudinalControl", "maxLateralAccel"))
    sunnypilot_params = self._sunnypilot_params(ui_state)
    return (
      car,
      self._pcm_cruise_speed(ui_state),
      tuple(sunnypilot_params.items()) if sunnypilot_params is not None else (),
      tuple(sorted(self._service_resolver.resolved.items())),
    )

  @staticmethod
  def _pcm_cruise_speed(ui_state) -> bool:
    # CarParamsSP.pcmCruiseSpeed (sunnypilot ICBM "MAX" swap); True, the UI's default, without CarParamsSP.
    car_params_sp = getattr(ui_state, "CP_SP", None)
    return True if car_params_sp is None else bool(getattr(car_params_sp, "pcmCruiseSpeed", True))

  def _car_params_payload(self, ui_state) -> dict:
    car_params = getattr(ui_state, "CP", None)
    payload = {
      "exportVersion": 1,
      "openpilotLongitudinalControl": bool(getattr(car_params, "openpilotLongitudinalControl", False)),
      "maxLateralAccel": _safe_float(getattr(car_params, "maxLateralAccel", COMMAVIEW_DEFAULT_MAX_LAT_ACCEL)),
      "carFingerprint": _safe_str(getattr(car_params, "carFingerprint", "")),
      "carName": _safe_str(getattr(car_params, "carName", "")),
      "carVin": _safe_str(getattr(car_params, "carVin", "")),
      "pcmCruiseSpeed": self._pcm_cruise_speed(ui_state),
      # Which name this UI subscribes to for each renamed service ("liveCalibration" = sunnypilot release schema).
      "upstreamServices": self._service_resolver.resolved,
      "logMonoTime": 0,
    }
    sunnypilot_params = self._sunnypilot_params(ui_state)
    if sunnypilot_params is not None:
      payload["spParams"] = sunnypilot_params
    return payload

  def _device_state_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "started": False,
      "deviceType": "",
      "logMonoTime": self._service_log_mono(ui_state, "deviceState"),
    }
    if ui_state.sm.recv_frame["deviceState"] >= 0:
      device_state = ui_state.sm["deviceState"]
      payload.update({
        "started": bool(device_state.started),
        "deviceType": _safe_str(device_state.deviceType),
      })
    return payload

  def _road_camera_state_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "sensor": "",
      "frameId": 0,
      "logMonoTime": self._service_resolver.log_mono_time(ui_state, "road_camera"),
    }
    if self._service_resolver.recv_frame(ui_state, "road_camera") >= 0:
      road_camera_state = self._service_resolver.message(ui_state, "road_camera")
      payload.update({
        "sensor": _safe_str(getattr(road_camera_state, "sensor", "")),
        "frameId": _safe_int(getattr(road_camera_state, "frameId", 0)),
      })
    return payload

  def _wide_road_camera_state_payload(self, ui_state) -> dict:
    payload = {
      "exportVersion": 1,
      "sensor": "",
      "frameId": 0,
      "logMonoTime": self._service_log_mono(ui_state, "wideRoadCameraState"),
    }
    if ui_state.sm.recv_frame["wideRoadCameraState"] >= 0:
      wide_camera_state = ui_state.sm["wideRoadCameraState"]
      payload.update({
        "sensor": _safe_str(getattr(wide_camera_state, "sensor", "")),
        "frameId": _safe_int(getattr(wide_camera_state, "frameId", 0)),
      })
    return payload

  def _panda_states_summary_payload(self, ui_state) -> dict:
    ignition_line, ignition_can, ignition = _panda_states_summary(ui_state)
    return {
      "exportVersion": 1,
      "ignitionLine": ignition_line,
      "ignitionCan": ignition_can,
      "ignition": ignition,
      "logMonoTime": self._service_log_mono(ui_state, "pandaStates"),
    }
