import importlib.util
import json
import os
import time
from pathlib import Path
from types import SimpleNamespace

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "comma" / "src" / "commaview_drive_stats.py"
NOW = 1_780_000_000_000  # a set clock
DAY = 86_400_000
ROUTE = "0000002a--8f1e2d3c4b"


@pytest.fixture
def stats(tmp_path, monkeypatch):
  spec = importlib.util.spec_from_file_location(f"commaview_drive_stats_{time.time_ns()}", SCRIPT)
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  monkeypatch.setattr(module, "DRIVES_FILE", str(tmp_path / "data" / "drives.json"))
  monkeypatch.setattr(module, "DRIVES_DIR", str(tmp_path / "data" / "drives"))
  monkeypatch.setattr(module, "STATS_FILE", str(tmp_path / "data" / "drive-stats.json"))
  monkeypatch.setattr(module, "COMMA_API_FILE", str(tmp_path / "data" / "comma-api-stats.json"))
  monkeypatch.setattr(module, "LOCATION_CONFIG_FILE", str(tmp_path / "config" / "location.json"))
  monkeypatch.setattr(module, "LOCATION_LOG_FILE", str(tmp_path / "live" / "location-log.json"))
  monkeypatch.setattr(module, "PARAMS_DIR", str(tmp_path / "params"))
  monkeypatch.setattr(module, "LOG_ROOT", str(tmp_path / "realdata"))
  (tmp_path / "params").mkdir()
  (tmp_path / "realdata").mkdir()
  return module


def gps(lat=37.7749, lon=-122.4194, has_fix=True):
  return SimpleNamespace(latitude=lat, longitude=lon, hasFix=has_fix, horizontalAccuracy=4.2, bearingDeg=90.0,
                         speed=12.5, unixTimestampMillis=NOW, flags=1 if has_fix else 0)


def car(speed):
  return SimpleNamespace(vEgo=speed)


def minute(start_s, speed, lat=None, hz=10):
  """One logged minute: the speed ten times a second and, if given, a GPS fix once a second."""
  events = []
  for i in range(60 * hz):
    t = start_s + i / hz
    events.append(("carState", int(t * 1e9), car(speed)))
    if lat is not None and i % hz == 0:
      events.append(("gpsLocationExternal", int(t * 1e9), gps(lat=lat + i / 1e5)))
  events.append(("deviceState", int((start_s + 59) * 1e9), SimpleNamespace()))
  return events


def make_route(stats, logs, route=ROUTE):
  """Segment folders for [logs] (one list of events each), and a reader that serves them."""
  by_path = {}
  for i, events in enumerate(logs):
    folder = Path(stats.LOG_ROOT) / f"{route}--{i}"
    folder.mkdir()
    by_path[str(folder / "qlog.zst")] = events
  return lambda path: iter(by_path.get(path, []))


def enable_location(stats, enabled=True):
  stats.write_json_atomic(stats.LOCATION_CONFIG_FILE, {"enabled": enabled})


def test_only_a_real_fix_becomes_a_position(stats):
  assert stats.fix_from_gps(gps()) == {
    "lat": 37.7749, "lon": -122.4194, "accuracyM": 4.2, "bearingDeg": 90.0, "speedMs": 12.5, "fixMs": NOW,
  }
  assert stats.fix_from_gps(gps(has_fix=False)) is None
  assert stats.fix_from_gps(gps(lat=0.0, lon=0.0)) is None
  assert stats.fix_from_gps(gps(lat=123.0)) is None
  older = SimpleNamespace(latitude=1.0, longitude=2.0, flags=1, horizontalAccuracy=3.0, bearingDeg=0.0,
                          speed=0.0, unixTimestampMillis=NOW)
  assert stats.fix_from_gps(older)["lat"] == 1.0  # schemas before hasFix use flags


def test_segments_are_this_routes_in_order(stats):
  root = Path(stats.LOG_ROOT)
  for name in (f"{ROUTE}--10", f"{ROUTE}--2", f"{ROUTE}--0", f"{ROUTE}5--1", "0000002b--0000000000--0", f"{ROUTE}--x"):
    (root / name).mkdir()
  assert [Path(p).name for p in stats.route_segments(ROUTE)] == [f"{ROUTE}--0", f"{ROUTE}--2", f"{ROUTE}--10"]
  assert stats.route_segments("") == [] and stats.route_segments("../etc") == []


def write_drive(stats, drive, name=None):
  folder = Path(stats.DRIVES_DIR)
  folder.mkdir(parents=True, exist_ok=True)
  (folder / (name or f"{drive['startMs']}.json")).write_text(json.dumps(drive))


def test_the_drive_list_is_the_control_services_folder(stats):
  assert stats.read_drives() == []
  first = {"startMs": NOW - DAY, "endMs": NOW - DAY + 600_000, "durationS": 600, "distanceM": 9000, "route": ROUTE}
  going_on = {"startMs": NOW - 60_000, "endMs": NOW, "durationS": 60, "distanceM": 700}
  write_drive(stats, going_on)
  write_drive(stats, first)
  assert stats.read_drives() == [first, going_on]
  # Anything else in the folder isn't a drive: a file being written, notes, a drive before the clock was set.
  write_drive(stats, first, name=f"{first['startMs']}.json.tmp")
  write_drive(stats, {"startMs": 5, "endMs": NOW}, name="5.json")
  (Path(stats.DRIVES_DIR) / "7.json").write_text("{not json")
  write_drive(stats, first, name="notes.json")
  assert stats.read_drives() == [first, going_on]


def test_drives_still_in_the_old_list_are_read_once(stats):
  old = {"startMs": NOW - 2 * DAY, "endMs": NOW - 2 * DAY + 60_000, "durationS": 60, "distanceM": 1000}
  moved = {"startMs": NOW - DAY, "endMs": NOW - DAY + 60_000, "durationS": 60, "distanceM": 2000}
  stats.write_json_atomic(stats.DRIVES_FILE, {"version": 1, "sinceMs": old["startMs"], "drives": [old, moved, {"startMs": 1}]})
  write_drive(stats, dict(moved, distanceM=2500))
  assert stats.read_drives() == [old, dict(moved, distanceM=2500)]


def test_the_position_while_driving_is_the_last_finished_minute(stats):
  enable_location(stats)
  # Minute 2 is still being written; minute 1 has no fix (a tunnel); minute 0 has one.
  reader = make_route(stats, [minute(100, 20.0, lat=37.70), minute(160, 20.0), minute(220, 20.0, lat=37.72)])
  fix = stats.position(ROUTE, reader)
  assert fix["segment"] == f"{ROUTE}--0" and fix["lat"] == pytest.approx(37.7059, abs=1e-5)
  assert json.loads(Path(stats.LOCATION_LOG_FILE).read_text()) == fix
  enable_location(stats, False)
  Path(stats.LOCATION_LOG_FILE).unlink()
  assert stats.position(ROUTE, reader) is None
  assert not Path(stats.LOCATION_LOG_FILE).exists()


def test_comma_totals_are_normalized(stats):
  assert stats.normalize_stats({"all": {"routes": 1349, "distance": 13027.4, "minutes": 26000},
                                "week": {"routes": 28, "distance": 105.25, "minutes": 372}}) == {
    "all": {"routes": 1349, "distanceMi": 13027.4, "minutes": 26000},
    "week": {"routes": 28, "distanceMi": 105.25, "minutes": 372},
  }
  assert stats.normalize_stats(None)["week"] == {"routes": 0, "distanceMi": 0.0, "minutes": 0}


TOTALS = {"all": {"routes": 2, "distanceMi": 3.0, "minutes": 4}, "week": {"routes": 1, "distanceMi": 1.0, "minutes": 2}}


def test_sunnypilots_fresh_cache_is_read_instead_of_asking_comma(stats):
  cache_path = Path(stats.PARAMS_DIR) / "ApiCache_DriveStats"
  cache_path.write_text(json.dumps({"all": {"routes": 9, "distance": 12.5, "minutes": 30}, "week": {"routes": 1, "distance": 2, "minutes": 5}}))
  saved_s = NOW / 1000 - 600
  os.utime(cache_path, (saved_s, saved_s))

  # The runtime's own figure (the leaderboard's) is under 6 hours old: comma isn't asked.
  stats.write_json_atomic(stats.COMMA_API_FILE, {"version": 1, "source": "comma-api", "fetchedAtMs": NOW - 3_600_000,
                                                 "all": TOTALS["all"]})

  def fetch():
    raise AssertionError("asked comma although sunnypilot's copy and the runtime's own are fresh")

  saved = stats.refresh_totals(NOW, fetch=fetch)
  assert saved["source"] == "sunnypilot" and saved["fetchedAtMs"] == NOW - 600_000
  assert saved["all"] == {"routes": 9, "distanceMi": 12.5, "minutes": 30}
  # Kept apart: sunnypilot's copy never becomes the runtime's own figure.
  assert json.loads(Path(stats.COMMA_API_FILE).read_text())["all"] == TOTALS["all"]


def test_totals_come_from_comma_and_survive_a_failed_refresh(stats):
  saved = stats.refresh_totals(NOW, fetch=lambda: TOTALS, cache=lambda: None)
  assert saved == {"version": 1, "source": "comma", "fetchedAtMs": NOW, **TOTALS}
  assert json.loads(Path(stats.COMMA_API_FILE).read_text()) == \
    {"version": 1, "source": "comma-api", "fetchedAtMs": NOW, "all": TOTALS["all"]}

  def offline():
    raise RuntimeError("offline")

  saved = stats.refresh_totals(NOW + 60_000, fetch=offline, cache=lambda: None)
  assert saved["all"] == TOTALS["all"] and saved["fetchedAtMs"] == NOW
  assert saved["lastError"] == "offline" and saved["lastErrorAtMs"] == NOW + 60_000
  # A stale sunnypilot copy newer than ours still wins when comma can't be reached.
  newer = ({"all": {"routes": 3, "distanceMi": 4.0, "minutes": 5}, "week": TOTALS["week"]}, NOW + 30_000)
  saved = stats.refresh_totals(NOW + 5 * 3_600_000, fetch=offline, cache=lambda: newer)
  assert saved["source"] == "sunnypilot" and saved["all"]["routes"] == 3


def test_an_unregistered_comma_does_not_ask(stats):
  (Path(stats.PARAMS_DIR) / "DongleId").write_text("UnregisteredDevice")
  with pytest.raises(RuntimeError, match="unregistered"):
    stats.fetch_totals_from_comma()


def test_nothing_runs_after_a_drive_any_more(stats):
  with pytest.raises(SystemExit):
    stats.main(["after-drive", "--route", ROUTE, "--duration-s", "60"])


def test_starpilot_markers_move_the_log_root_only_under_starpilot(stats, tmp_path):
  op, cache, media = tmp_path / "openpilot", tmp_path / "cache", tmp_path / "media"
  op.mkdir()
  cache.mkdir()
  (cache / "use_konik").write_text("1")

  def root():
    return stats.openpilot_log_root(str(op), str(cache), str(media))

  assert root() == str(media / "realdata")  # markers left behind by StarPilot
  (op / "starpilot").mkdir()
  assert root() == str(media / "realdata_konik")
  (cache / "use_HD").write_text("1")  # hw.py checks High Bitrate Recording first
  assert root() == str(media / "realdata_HD")
