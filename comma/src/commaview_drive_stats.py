#!/usr/bin/env python3
"""CommaView drive stats: what this comma drove, from what it already keeps.

A short run, started by the control service; nothing here subscribes to openpilot's messages or
stays running between drives.

  after-drive  Read the drive's own logs (openpilot writes a qlog for every minute of driving, with
               the car's speed ten times a second and its GPS once a second) for its distance and,
               when location is on and the control service didn't catch it, where it ended. Then
               refresh comma's totals.
  totals       Refresh comma's past-week and all-time totals for this comma: sunnypilot's cached
               copy (ApiCache_DriveStats) when it has one, otherwise one request to comma made with
               the comma's own identity, as sunnypilot's Trips page does. What that request answers is
               also kept apart (data/comma-api-stats.json) for the leaderboard, which never takes a
               figure from sunnypilot's copy; with a fresh copy the request is still made, at most
               every 6 hours.
  position     While a drive goes on and location is on: where the car was at the end of the drive's
               last finished log minute. The fallback when the comma's GPS can't be read directly.
  leaderboard-register, leaderboard-statement
               For a paired phone, while the control service waits: this comma's leaderboard key
               signs a registration (for the account service's challenge) or a statement of its
               drives grouped into UTC days, with comma's all-time distance for this comma when the
               runtime's own request to comma has fetched it (data/comma-api-stats.json). The answer
               is one JSON line on stdout.

Files (under ROOT unless noted): data/drives.json, data/drive-stats.json, data/comma-api-stats.json,
and only while location is on data/location-last.json and LIVE_DIR/location-log.json. Positions are
never logged. Once the comma joins the leaderboard: data/leaderboard-key.json (its private key, 0600,
never logged) and data/leaderboard-seq.json (the statement counter).
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import sys
import time
from datetime import datetime, timezone

ROOT = os.environ.get("COMMAVIEW_DRIVE_ROOT", "/data/commaview")
LIVE_DIR = os.environ.get("COMMAVIEW_DRIVE_LIVE_DIR", "/dev/shm/commaview")
PARAMS_DIR = os.environ.get("COMMAVIEW_DRIVE_PARAMS_DIR", "/data/params/d")


def openpilot_log_root(openpilot_dir: str = "/data/openpilot", cache_dir: str = "/cache",
                       media_dir: str = "/data/media/0") -> str:
  """Where loggerd writes drives (system/hardware/hw.py Paths.log_root()). StarPilot moves it while its
  High Bitrate Recording (/cache/use_HD) or Konik (/cache/use_konik) marker exists. Other forks ignore
  those markers, which outlive a fork change on /cache, so they count only under StarPilot
  (commaviewd/include/openpilot_log_root.h)."""
  if os.path.isdir(os.path.join(openpilot_dir, "starpilot")):
    if os.path.isfile(os.path.join(cache_dir, "use_HD")):
      return os.path.join(media_dir, "realdata_HD")
    if os.path.isfile(os.path.join(cache_dir, "use_konik")):
      return os.path.join(media_dir, "realdata_konik")
  return os.path.join(media_dir, "realdata")


LOG_ROOT = os.environ.get("COMMAVIEW_DRIVE_LOG_ROOT") or openpilot_log_root()
# Where openpilot reads which comma this is (system/hardware: "comma tici", "comma tizi", "comma mici").
DEVICE_MODEL_FILE = os.environ.get("COMMAVIEW_DRIVE_DEVICE_MODEL_FILE", "/sys/firmware/devicetree/base/model")

DRIVES_FILE = os.path.join(ROOT, "data", "drives.json")
STATS_FILE = os.path.join(ROOT, "data", "drive-stats.json")
# Only what this runtime's own request to comma answered: the leaderboard's source for commaStats.
COMMA_API_FILE = os.path.join(ROOT, "data", "comma-api-stats.json")
LOCATION_CONFIG_FILE = os.path.join(ROOT, "config", "location.json")
LOCATION_LAST_FILE = os.path.join(ROOT, "data", "location-last.json")
LOCATION_LOG_FILE = os.path.join(LIVE_DIR, "location-log.json")

KEEP_DAYS = 62
DAY_MS = 86_400_000
# A clock before 2024 hasn't been set yet (no GPS or network time since boot).
VALID_WALL_MS = 1_704_067_200_000
# Speed samples further apart than this aren't joined, so a gap in the log never counts as distance.
MAX_SAMPLE_GAP_S = 1.0
GPS_EVENTS = ("gpsLocationExternal", "gpsLocation")


def log(message: str) -> None:
  # stderr: stdout carries the leaderboard's answer to the control service.
  print(f"commaview-drive-stats: {message}", file=sys.stderr, flush=True)


def valid_wall(ms) -> bool:
  return isinstance(ms, int) and not isinstance(ms, bool) and ms >= VALID_WALL_MS


def read_json(path: str):
  try:
    with open(path, encoding="utf-8") as f:
      return json.load(f)
  except (OSError, ValueError):
    return None


def write_json_atomic(path: str, value) -> None:
  os.makedirs(os.path.dirname(path), exist_ok=True)
  tmp = f"{path}.tmp.{os.getpid()}"
  with open(tmp, "w", encoding="utf-8") as f:
    json.dump(value, f, separators=(",", ":"))
  os.replace(tmp, path)


def read_param(key: str) -> str:
  try:
    with open(os.path.join(PARAMS_DIR, key), encoding="utf-8", errors="replace") as f:
      return f.read().strip("\x00\r\n\t ")
  except OSError:
    return ""


def location_enabled() -> bool:
  config = read_json(LOCATION_CONFIG_FILE)
  return isinstance(config, dict) and config.get("enabled") is True


# ---------------------------------------------------------------- The drive's own logs

def route_segments(route: str, log_root: str = None) -> list:
  """The route's segment folders on the comma, in order: <route>--0, <route>--1, ..."""
  log_root = log_root or LOG_ROOT
  if not route or "/" in route or route.startswith("."):
    return []
  pattern = re.compile(re.escape(route) + r"--(\d+)$")
  try:
    names = os.listdir(log_root)
  except OSError:
    return []
  numbered = sorted((int(m.group(1)), name) for name in names if (m := pattern.match(name)))
  return [os.path.join(log_root, name) for _, name in numbered]


def import_log():
  try:
    from cereal import log as capnp_log  # openpilot's layout, as openpilot itself imports it
  except ImportError:
    from openpilot.cereal import log as capnp_log  # sunnypilot's newer package layout
  return capnp_log


def read_qlog(path: str):
  """Events from one qlog.zst, as (which, logMonoTime, message). A log cut short keeps what it has."""
  import zstandard
  capnp_log = import_log()
  chunks = []
  try:
    with open(path, "rb") as f:
      try:
        reader = zstandard.ZstdDecompressor().stream_reader(f, read_across_frames=True)
      except TypeError:
        reader = zstandard.ZstdDecompressor().stream_reader(f)
      while True:
        chunk = reader.read(1 << 20)
        if not chunk:
          break
        chunks.append(chunk)
  except (OSError, zstandard.ZstdError):
    pass
  try:
    for event in capnp_log.Event.read_multiple_bytes(b"".join(chunks)):
      which = event.which()
      yield which, event.logMonoTime, getattr(event, which)
  except Exception:
    return  # the last message of a log cut short


def fix_from_gps(gps):
  """A GPS message as the position this comma reports, or None without a usable fix."""
  try:
    try:
      has_fix = bool(gps.hasFix)
    except Exception:
      has_fix = bool(int(gps.flags) % 2)
    lat, lon = float(gps.latitude), float(gps.longitude)
    if not has_fix or not (-90.0 <= lat <= 90.0 and -180.0 <= lon <= 180.0) or (abs(lat) < 1e-6 and abs(lon) < 1e-6):
      return None
    return {
      "lat": round(lat, 6),
      "lon": round(lon, 6),
      "accuracyM": round(float(gps.horizontalAccuracy), 1),
      "bearingDeg": round(float(gps.bearingDeg), 1),
      "speedMs": round(float(gps.speed), 2),
      "fixMs": int(gps.unixTimestampMillis),
    }
  except Exception:
    return None


class DriveSummary:
  """Distance from the logged speed, and the last GPS fix, over a drive's events in order."""

  def __init__(self):
    self.distance_m = 0.0
    self.last_speed = None
    self.last_mono = None
    self.last_fix = None

  def add(self, which: str, mono_ns: int, message) -> None:
    if which == "carState":
      speed, mono = float(message.vEgo), mono_ns / 1e9
      if self.last_mono is not None and mono <= self.last_mono:
        return
      if self.last_mono is not None and self.last_speed is not None and 0.0 < mono - self.last_mono <= MAX_SAMPLE_GAP_S:
        self.distance_m += max(0.0, (self.last_speed + speed) / 2.0) * (mono - self.last_mono)
      self.last_speed, self.last_mono = speed, mono
    elif which in GPS_EVENTS:
      fix = fix_from_gps(message)
      if fix is not None:
        self.last_fix = fix


def summarize_route(route: str, reader=read_qlog) -> DriveSummary:
  summary = DriveSummary()
  for segment in route_segments(route):
    for which, mono, message in reader(os.path.join(segment, "qlog.zst")):
      summary.add(which, mono, message)
  return summary


# ---------------------------------------------------------------- The drive list

class DriveLedger:
  """The drives this comma has made since it started keeping them; phones group them into days."""

  def __init__(self, saved=None):
    saved = saved if isinstance(saved, dict) else {}
    self.since_ms = saved.get("sinceMs") if valid_wall(saved.get("sinceMs")) else None
    drives = saved.get("drives") if isinstance(saved.get("drives"), list) else []
    self.drives = [d for d in drives if isinstance(d, dict) and valid_wall(d.get("startMs")) and valid_wall(d.get("endMs"))]

  def add(self, drive: dict, now_ms: int) -> None:
    if self.since_ms is None and valid_wall(now_ms):
      self.since_ms = min(now_ms, drive["startMs"])
    # The same drive summarized again replaces itself.
    self.drives = [d for d in self.drives if not (drive.get("route") and d.get("route") == drive["route"])]
    self.drives.append(drive)
    self.drives.sort(key=lambda d: d["startMs"])

  def prune(self, now_ms: int) -> None:
    if valid_wall(now_ms):
      oldest = now_ms - KEEP_DAYS * DAY_MS
      self.drives = [d for d in self.drives if d["endMs"] >= oldest]

  def to_json(self) -> dict:
    return {"version": 1, "sinceMs": self.since_ms, "drives": self.drives}


def place_drive(start_ms: int, end_ms: int, duration_s: int, now_ms: int):
  """The drive's start and end on the wall clock, even when the clock was set during the drive."""
  if valid_wall(end_ms):
    end = end_ms
  elif valid_wall(now_ms):
    end = now_ms
  else:
    return None
  start = start_ms if valid_wall(start_ms) and start_ms <= end else end - duration_s * 1000
  return start, end


def after_drive(route: str, start_ms: int, end_ms: int, duration_s: int, now_ms: int, reader=read_qlog) -> dict | None:
  placed = place_drive(start_ms, end_ms, duration_s, now_ms)
  if placed is None:
    log("the clock isn't set; this drive can't be placed")
    return None
  summary = summarize_route(route, reader)
  drive = {"startMs": placed[0], "endMs": placed[1], "durationS": int(duration_s), "distanceM": int(round(summary.distance_m))}
  if route:
    drive["route"] = route
  ledger = DriveLedger(read_json(DRIVES_FILE))
  ledger.add(drive, now_ms)
  ledger.prune(now_ms)
  write_json_atomic(DRIVES_FILE, ledger.to_json())
  if summary.last_fix is not None and location_enabled():
    saved = read_json(LOCATION_LAST_FILE)
    # The control service keeps where the drive ended from the comma's own GPS; the log fills in
    # only when it couldn't.
    from_comma = isinstance(saved, dict) and valid_wall(saved.get("endedMs")) and saved["endedMs"] >= placed[0]
    if not from_comma:
      write_json_atomic(LOCATION_LAST_FILE, dict(summary.last_fix, endedMs=placed[1], source="log"))
  try:
    os.remove(LOCATION_LOG_FILE)  # the drive is over; its position now lives in location-last
  except OSError:
    pass
  return drive


def latest_position(route: str, reader=read_qlog):
  """Where the car was at the end of the drive's last finished minute (the newest log is still open)."""
  segments = route_segments(route)
  for segment in reversed(segments[:-1]):
    summary = DriveSummary()
    for which, mono, message in reader(os.path.join(segment, "qlog.zst")):
      summary.add(which, mono, message)
    if summary.last_fix is not None:
      return dict(summary.last_fix, segment=os.path.basename(segment), source="log")
  return None


def position(route: str, reader=read_qlog):
  if not location_enabled():
    return None
  fix = latest_position(route, reader)
  if fix is not None and location_enabled():
    write_json_atomic(LOCATION_LOG_FILE, fix)
  return fix


# ---------------------------------------------------------------- comma's totals

def normalize_stats(data) -> dict:
  """comma's /v1.1/devices/:dongle_id/stats body as {all, week}: routes, miles, minutes."""
  def part(value):
    value = value if isinstance(value, dict) else {}
    return {
      "routes": int(value.get("routes") or 0),
      "distanceMi": round(float(value.get("distance") or 0.0), 2),
      "minutes": int(value.get("minutes") or 0),
    }
  data = data if isinstance(data, dict) else {}
  return {"all": part(data.get("all")), "week": part(data.get("week"))}


def cached_totals():
  """sunnypilot's own cached copy of comma's totals, and when it was saved."""
  path = os.path.join(PARAMS_DIR, "ApiCache_DriveStats")
  data = read_json(path)
  if not isinstance(data, dict) or not isinstance(data.get("all"), dict):
    return None
  try:
    saved_ms = int(os.path.getmtime(path) * 1000)
  except OSError:
    return None
  return normalize_stats(data), saved_ms


def fetch_totals_from_comma() -> dict:
  dongle_id = read_param("DongleId")
  if not dongle_id or dongle_id == "UnregisteredDevice":
    raise RuntimeError("unregistered")
  from openpilot.common.api import Api, api_get
  token = Api(dongle_id).get_token()
  response = api_get(f"v1.1/devices/{dongle_id}/stats", timeout=15, access_token=token)
  if response.status_code != 200:
    raise RuntimeError(f"http {response.status_code}")
  return normalize_stats(response.json())


# sunnypilot refreshes its copy every 30 s while its UI is up; an hour-old copy still beats a request.
CACHE_FRESH_MS = 60 * 60 * 1000
# The leaderboard's figure comes only from this runtime's own request (anyone on the comma can edit
# sunnypilot's copy): made at least this often while sunnypilot's copy is fresh, when comma answers,
COMMA_API_REFRESH_MS = 6 * 60 * 60 * 1000
# and tried again no sooner than this after it failed.
COMMA_API_RETRY_MS = 60 * 60 * 1000
COMMA_API_SOURCE = "comma-api"


def save_comma_api(totals: dict, fetched_ms: int) -> None:
  write_json_atomic(COMMA_API_FILE, {"version": 1, "source": COMMA_API_SOURCE, "fetchedAtMs": fetched_ms,
                                     "all": totals["all"]})


def comma_api_due(now_ms: int) -> bool:
  """Whether the runtime's own figure is old enough (or missing) to ask comma for it again."""
  own = read_json(COMMA_API_FILE)
  own = own if isinstance(own, dict) else {}
  fetched, tried = own.get("fetchedAtMs"), own.get("lastAttemptAtMs")
  if valid_wall(fetched) and 0 <= now_ms - fetched < COMMA_API_REFRESH_MS:
    return False
  return not (valid_wall(tried) and 0 <= now_ms - tried < COMMA_API_RETRY_MS)


def note_comma_api_failure(now_ms: int) -> None:
  own = read_json(COMMA_API_FILE)
  own = own if isinstance(own, dict) else {"version": 1}
  own["lastAttemptAtMs"] = now_ms
  write_json_atomic(COMMA_API_FILE, own)


def refresh_totals(now_ms: int, fetch=fetch_totals_from_comma, cache=cached_totals) -> dict:
  saved = read_json(STATS_FILE)
  saved = saved if isinstance(saved, dict) else {"version": 1}
  cached = cache()
  if cached is not None and now_ms - cached[1] <= CACHE_FRESH_MS:
    saved = {"version": 1, "source": "sunnypilot", "fetchedAtMs": cached[1], **cached[0]}
    if comma_api_due(now_ms):
      # Driving insights keeps sunnypilot's copy; this answer is only the leaderboard's.
      try:
        save_comma_api(fetch(), now_ms)
      except Exception as e:
        note_comma_api_failure(now_ms)
        log(f"comma's totals unavailable: {str(e)[:120]}")
  else:
    try:
      totals = fetch()
      saved = {"version": 1, "source": "comma", "fetchedAtMs": now_ms, **totals}
      save_comma_api(totals, now_ms)
    except Exception as e:  # offline, unregistered, or comma said no: keep the last totals
      if cached is not None and cached[1] > int(saved.get("fetchedAtMs") or 0):
        saved = {"version": 1, "source": "sunnypilot", "fetchedAtMs": cached[1], **cached[0]}
      saved["lastError"] = str(e)[:120]
      saved["lastErrorAtMs"] = now_ms
      log(f"comma's totals unavailable: {saved['lastError']}")
  write_json_atomic(STATS_FILE, saved)
  return saved


# ---------------------------------------------------------------- The leaderboard
#
# The contract is docs/plans/leaderboard.md in RhynoTech/commaview-web ("The contract"): this
# comma's own Ed25519 key signs compact JWS (EdDSA) over canonical JSON, and the phone only relays
# them. The private key never leaves this file's two functions that read and write it: it is never
# printed, logged or answered, and the control service leaves it out of support bundles.
#
# A statement's payload: {"v": 1, "seq", "issuedAtMs", "days": [{day, distanceM, durationS, drives}],
# and optionally "deviceType", "runtimeVersion" and "commaStats": {"allTimeM", "fetchedAtMs", "source":
# "comma-api"}} -- comma's all-time distance for this comma in whole metres, from the runtime's own
# request to comma, and when that request was made, for the lifetime board; left out when there's no
# such figure. Awards and weekly totals come from `days` only.

LEADERBOARD_KEY_FILE = os.path.join(ROOT, "data", "leaderboard-key.json")
LEADERBOARD_SEQ_FILE = os.path.join(ROOT, "data", "leaderboard-seq.json")
LEADERBOARD_LOCK_FILE = os.path.join(ROOT, "data", "leaderboard.lock")
VERSION_FILE = os.path.join(ROOT, "VERSION")

LEADERBOARD_MAX_DAYS = 62
LEADERBOARD_MAX_SEQ = 2_147_483_647
LEADERBOARD_TEXT_MAX = 40
METRES_PER_MILE = 1609.344
# Over a million miles (and still a 32-bit int): anything more isn't a comma's all-time distance.
COMMA_ALL_TIME_MAX_M = 2_000_000_000
DEVICE_HASH_PREFIX = "commaview-leaderboard-device/v1:"
# The device types openpilot knows (cereal InitData.DeviceType): comma 3, comma 3X, comma four.
DEVICE_TYPES = ("tici", "tizi", "mici")
B64URL_32 = re.compile(r"[A-Za-z0-9_-]{43}")

# Exit codes the control service turns into its answers (anything else is a 500).
EXIT_BAD_REQUEST = 3  # 400 challenge required
EXIT_NO_KEY = 4       # 409 no key
EXIT_NO_CRYPTO = 5    # 503 crypto unavailable


class LeaderboardError(Exception):
  def __init__(self, code: int, error: str):
    super().__init__(error)
    self.code = code
    self.error = error


def b64u(data: bytes) -> str:
  return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def b64u_32(text) -> bytes | None:
  """32 bytes written the one canonical way as base64url without padding (43 characters), or None."""
  if not isinstance(text, str) or not B64URL_32.fullmatch(text):
    return None
  raw = base64.urlsafe_b64decode(text + "=")
  return raw if len(raw) == 32 and b64u(raw) == text else None


def canonical(obj) -> bytes:
  return json.dumps(obj, separators=(",", ":"), sort_keys=True, ensure_ascii=True).encode("ascii")


def ed25519():
  """cryptography's Ed25519 key class and serialization module, or None when this Python lacks them."""
  try:
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    Ed25519PrivateKey.from_private_bytes(bytes(32))  # the OpenSSL underneath may lack Ed25519
  except Exception:
    return None
  return Ed25519PrivateKey, serialization


def require_crypto():
  crypto = ed25519()
  if crypto is None:
    raise LeaderboardError(EXIT_NO_CRYPTO, "crypto unavailable")
  return crypto


def private_key(seed: bytes):
  key_class, _ = require_crypto()
  return key_class.from_private_bytes(seed)


def public_x(key) -> str:
  _, serialization = require_crypto()
  return b64u(key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw))


def key_id(x: str) -> str:
  """The RFC 7638 thumbprint of the public key."""
  return b64u(hashlib.sha256(canonical({"crv": "Ed25519", "kty": "OKP", "x": x})).digest())


def device_hash(identity: str) -> str:
  return b64u(hashlib.sha256((DEVICE_HASH_PREFIX + identity).encode("utf-8")).digest())


def jws(key, header: dict, payload: dict) -> str:
  signing_input = b64u(canonical(header)) + "." + b64u(canonical(payload))
  return signing_input + "." + b64u(key.sign(signing_input.encode("ascii")))


def device_identity() -> str:
  """The dongle id, or the serial when the comma isn't registered with comma. It's only ever hashed."""
  dongle_id = read_param("DongleId")
  if dongle_id and dongle_id != "UnregisteredDevice":
    return dongle_id
  return "serial:" + read_param("HardwareSerial")


def optional_text(value: str) -> str | None:
  value = value.strip()
  return value[:LEADERBOARD_TEXT_MAX] if value else None


def device_type() -> str | None:
  """Which comma this is, as openpilot tells (HARDWARE.get_device_type(): the devicetree model after
  "comma "): tici, tizi or mici; None on anything else (a PC, a device this script doesn't know)."""
  try:
    with open(DEVICE_MODEL_FILE, encoding="ascii", errors="replace") as f:
      model = f.read(64).strip("\x00\r\n\t ")
  except OSError:
    return None
  name = model.split("comma ")[-1].strip()
  return name if name in DEVICE_TYPES else None


def runtime_version() -> str | None:
  try:
    with open(VERSION_FILE, encoding="utf-8", errors="replace") as f:
      return optional_text(f.readline())
  except OSError:
    return None


def whole(value) -> int:
  """A count from the drive list as the non-negative integer the contract wants."""
  if isinstance(value, bool) or not isinstance(value, (int, float)) or value != value or value in (float("inf"), float("-inf")):
    return 0
  return max(0, int(round(value)))


def utc_day(ms: int):
  return datetime.fromtimestamp(ms / 1000, timezone.utc).date()


def drive_days(drives: list, signed_ms: int) -> list:
  """The drives grouped by the UTC day they started: only days with a drive, oldest first, the newest 62.
  A day after the day it's signed (a clock that went back) can't be right, so it's left out."""
  signed_day = utc_day(signed_ms) if valid_wall(signed_ms) else None
  days = {}
  for drive in drives:
    start_ms = drive.get("startMs") if isinstance(drive, dict) else None
    if not valid_wall(start_ms):
      continue
    day = utc_day(start_ms)
    if signed_day is not None and day > signed_day:
      continue
    total = days.setdefault(day, [0, 0, 0])
    total[0] += whole(drive.get("distanceM"))
    total[1] += whole(drive.get("durationS"))
    total[2] += 1
  newest = sorted(days)[-LEADERBOARD_MAX_DAYS:]
  return [{"day": day.isoformat(), "distanceM": days[day][0], "durationS": days[day][1], "drives": days[day][2]}
          for day in newest]


def write_private_json(path: str, value) -> None:
  """Owner-only (0600) and on disk before it's used: a counter must never go back after a power cut."""
  directory = os.path.dirname(path)
  os.makedirs(directory, mode=0o700, exist_ok=True)
  tmp = f"{path}.tmp.{os.getpid()}"
  fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
  try:
    os.fchmod(fd, 0o600)
    os.write(fd, json.dumps(value, separators=(",", ":")).encode("ascii"))
    os.fsync(fd)
  except BaseException:
    os.close(fd)
    try:
      os.unlink(tmp)
    except OSError:
      pass
    raise
  os.close(fd)
  os.replace(tmp, path)
  dir_fd = os.open(directory, os.O_RDONLY)
  try:
    os.fsync(dir_fd)
  finally:
    os.close(dir_fd)


def load_seed() -> bytes | None:
  """The key's 32-byte seed, or None when there's no usable key."""
  saved = read_json(LEADERBOARD_KEY_FILE)
  if not isinstance(saved, dict) or saved.get("version") != 1:
    return None
  seed = b64u_32(saved.get("seed"))
  if seed is not None:
    try:
      if os.stat(LEADERBOARD_KEY_FILE).st_mode & 0o077:
        os.chmod(LEADERBOARD_KEY_FILE, 0o600)
    except OSError:
      pass
  return seed


def new_seed(now_ms: int) -> bytes:
  """A new key, saved before it signs anything; its counter starts again at 0."""
  seed = os.urandom(32)
  write_private_json(LEADERBOARD_KEY_FILE, {"version": 1, "seed": b64u(seed), "createdMs": now_ms})
  write_private_json(LEADERBOARD_SEQ_FILE, {"version": 1, "keyId": key_id(public_x(private_key(seed))), "seq": 0})
  return seed


def last_seq(kid: str) -> int:
  """The last seq this key signed; a counter left from another key (a crash mid-rotation) is 0."""
  saved = read_json(LEADERBOARD_SEQ_FILE)
  if not isinstance(saved, dict) or saved.get("keyId") != kid:
    return 0
  seq = saved.get("seq")
  return seq if isinstance(seq, int) and not isinstance(seq, bool) and 0 <= seq <= LEADERBOARD_MAX_SEQ else 0


class LeaderboardLock:
  """One signing at a time, so two statements never share a seq."""

  def __enter__(self):
    import fcntl
    os.makedirs(os.path.dirname(LEADERBOARD_LOCK_FILE), mode=0o700, exist_ok=True)
    self.fd = os.open(LEADERBOARD_LOCK_FILE, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    fcntl.flock(self.fd, fcntl.LOCK_EX)
    return self

  def __exit__(self, *exc):
    os.close(self.fd)  # releases the lock


def registration_token(key, challenge: str, issued_ms: int) -> str:
  x = public_x(key)
  payload = {"v": 1, "challenge": challenge, "publicKey": x, "deviceHash": device_hash(device_identity()),
             "issuedAtMs": issued_ms}
  kind = device_type()
  if kind:
    payload["deviceType"] = kind
  version = runtime_version()
  if version:
    payload["runtimeVersion"] = version
  return jws(key, {"alg": "EdDSA", "kid": key_id(x), "typ": "cv-lb-reg+jwt"}, payload)


def comma_stats(issued_ms: int) -> dict | None:
  """comma's all-time distance for this comma, as the runtime's own request to comma last answered it
  (data/comma-api-stats.json; never sunnypilot's ApiCache_DriveStats, which anyone on the comma can
  edit): {allTimeM, fetchedAtMs, source}, or None when there's no such figure. Read from that file
  only, so signing stays fast and offline, and the control service's cached statement (keyed on that
  file too) is signed again when it changes. However old the figure is, it's sent with when it was
  fetched: the account service and the app decide what's fresh enough."""
  saved = read_json(COMMA_API_FILE)
  if not isinstance(saved, dict) or saved.get("source") != COMMA_API_SOURCE:
    return None
  fetched_ms, all_time = saved.get("fetchedAtMs"), saved.get("all")
  if not valid_wall(fetched_ms) or not (valid_wall(issued_ms) and fetched_ms <= issued_ms):
    return None
  miles = all_time.get("distanceMi") if isinstance(all_time, dict) else None
  if isinstance(miles, bool) or not isinstance(miles, (int, float)) or not 0 <= miles < float("inf"):
    return None  # also NaN
  metres = int(round(miles * METRES_PER_MILE))
  if metres > COMMA_ALL_TIME_MAX_M:
    return None
  return {"allTimeM": metres, "fetchedAtMs": fetched_ms, "source": COMMA_API_SOURCE}


def statement_token(key, seq: int, issued_ms: int, days: list) -> str:
  payload = {"v": 1, "seq": seq, "issuedAtMs": issued_ms, "days": days}
  # Optional: comma's lifetime figure for the lifetime board. Awards and weekly totals stay with `days`.
  stats = comma_stats(issued_ms)
  if stats is not None:
    payload["commaStats"] = stats
  kind = device_type()  # also here, so a comma registered before it was sent gets it on its next statement
  if kind:
    payload["deviceType"] = kind
  version = runtime_version()
  if version:
    payload["runtimeVersion"] = version
  return jws(key, {"alg": "EdDSA", "kid": key_id(public_x(key)), "typ": "cv-lb-stats+jwt"}, payload)


def leaderboard_register(challenge: str, rotate: bool, now_ms: int) -> dict:
  """Signs the account service's challenge with this comma's key, made first if it has none (or
  `rotate`): {ok, registration, keyId}."""
  if b64u_32(challenge) is None:
    raise LeaderboardError(EXIT_BAD_REQUEST, "challenge required")
  require_crypto()
  with LeaderboardLock():
    seed = None if rotate else load_seed()
    if seed is None:
      seed = new_seed(now_ms)
    key = private_key(seed)
    return {"ok": True, "registration": registration_token(key, challenge, now_ms), "keyId": key_id(public_x(key))}


def leaderboard_statement(now_ms: int) -> dict:
  """This comma's drives by UTC day, and comma's all-time distance when known, signed with the next
  seq (saved before it's signed): {ok, statement, keyId, seq}."""
  with LeaderboardLock():
    seed = load_seed()
    if seed is None:
      raise LeaderboardError(EXIT_NO_KEY, "no key")
    key = private_key(seed)
    kid = key_id(public_x(key))
    seq = last_seq(kid) + 1
    if seq > LEADERBOARD_MAX_SEQ:
      raise LeaderboardError(1, "seq exhausted")
    write_private_json(LEADERBOARD_SEQ_FILE, {"version": 1, "keyId": kid, "seq": seq})
    days = drive_days(DriveLedger(read_json(DRIVES_FILE)).drives, now_ms)
    return {"ok": True, "statement": statement_token(key, seq, now_ms, days), "keyId": kid, "seq": seq}


def leaderboard_main(run) -> int:
  """One JSON line on stdout for the control service; failures say only what went wrong, never a key."""
  try:
    answer, code = run(), 0
  except LeaderboardError as e:
    answer, code = {"ok": False, "error": e.error}, e.code
  except Exception as e:
    log(f"leaderboard: {type(e).__name__}")
    answer, code = {"ok": False, "error": "leaderboard failed"}, 1
  sys.stdout.write(json.dumps(answer, separators=(",", ":")) + "\n")
  sys.stdout.flush()
  return code


# ---------------------------------------------------------------- Entry

def main(argv=None) -> int:
  os.umask(0o077)  # drives and positions are this comma owner's alone
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  sub = parser.add_subparsers(dest="mode", required=True)
  drive = sub.add_parser("after-drive")
  drive.add_argument("--route", default="")
  drive.add_argument("--start-ms", type=int, default=0)
  drive.add_argument("--end-ms", type=int, default=0)
  drive.add_argument("--duration-s", type=int, required=True)
  sub.add_parser("totals")
  pos = sub.add_parser("position")
  pos.add_argument("--route", required=True)
  register = sub.add_parser("leaderboard-register")
  register.add_argument("--challenge", required=True)
  register.add_argument("--rotate", action="store_true")
  sub.add_parser("leaderboard-statement")
  args = parser.parse_args(argv)

  now_ms = int(time.time() * 1000)
  if args.mode == "leaderboard-register":
    return leaderboard_main(lambda: leaderboard_register(args.challenge, args.rotate, now_ms))
  if args.mode == "leaderboard-statement":
    return leaderboard_main(lambda: leaderboard_statement(now_ms))
  if args.mode == "after-drive":
    after_drive(args.route, args.start_ms, args.end_ms, args.duration_s, now_ms)
    refresh_totals(now_ms)
  elif args.mode == "totals":
    refresh_totals(now_ms)
  elif args.mode == "position":
    position(args.route)
  return 0


if __name__ == "__main__":
  sys.exit(main())
