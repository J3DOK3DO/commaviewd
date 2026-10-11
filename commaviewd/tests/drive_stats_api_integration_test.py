#!/usr/bin/env python3
"""Drive stats and location, served to paired phones only, from what the comma already keeps.

The control service notices each drive from the onroad flag, measures it from carState read from
openpilot's msgq ring (without subscribing) while it goes on, and keeps it in its own file as it goes,
so a comma that loses power with the car still has it; nothing runs after a drive. Location is off
until turned on, prefers the comma's timestamped GPS read the same way, falls back to saved
positions, and is forgotten when turned off.
"""

import json
import os
from pathlib import Path
import socket
import stat
import struct
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

# A cereal Event holding a gpsLocationExternal fix (37.7749, -122.4194, hasFix, 4.5 m, 90°, 12.5 m/s)
# whose unixTimestampMillis is the sentinel 0x0123456789ABCDEF, swapped for the time the test runs.
# It and the queue below follow openpilot's schema and msgq (15 reader slots). Forks differ in both,
# so the pipeline passes COMMAVIEWD_GPS_QUEUE_TOOL, which writes the queue with the target's own.
GPS_EVENT_HEX = (
    "000000000d0000000000000002000100e8030000000000002f000000000000000000000008000100000000000000"
    "4841d0d556ec2fe3424050fc1873d79a5ec000000000000000000000b44200009040efcdab8967452301000000000000"
    "000000000000010000000000000000000000"
)
SENTINEL = struct.pack("<q", 0x0123456789ABCDEF)
MSGQ_HEADER_BYTES = 8 * (3 + 3 * 15)  # msgq_header_t with NUM_READERS 15


def gps_event(fix_ms):
    event = bytes.fromhex(GPS_EVENT_HEX)
    assert event.count(SENTINEL) == 1
    return event.replace(SENTINEL, struct.pack("<q", fix_ms))


def write_msgq_queue(path, messages, data_size=65536):
    """A queue file laid out and written the way openpilot's msgq writes one."""
    data = bytearray(data_size)
    offset = 0
    for message in messages:
        data[offset:offset + 8] = struct.pack("<q", len(message))
        data[offset + 8:offset + 8 + len(message)] = message
        offset = (offset + 8 + len(message) + 7) & ~7
    header = bytearray(MSGQ_HEADER_BYTES)
    header[8:16] = struct.pack("<Q", offset)  # lap 0, write pointer after the last message
    path.write_bytes(bytes(header) + bytes(data))


def write_gps_queue(path, fix_times):
    """The comma's GPS queue holding a fix at each time, as its own msgq and schema would write it."""
    tool = os.environ.get("COMMAVIEWD_GPS_QUEUE_TOOL")
    if tool:
        subprocess.check_call([tool, "--write-gps-queue", str(path)] + [str(t) for t in fix_times])
    else:
        write_msgq_queue(path, [gps_event(t) for t in fix_times])


def request(port, path, method="GET", token=None, body=None):
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Content-Type": "application/json"}
    if token:
        headers["X-CommaView-Token"] = token
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=2) as response:
            return response.status, json.load(response)
    except urllib.error.HTTPError as error:
        return error.code, json.load(error)


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def start_control(binary, env, port):
    process = subprocess.Popen([str(binary), "control", "--port", str(port)],
                               env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(50):
        try:
            request(port, "/commaview/version")
            return process
        except (OSError, ValueError):
            time.sleep(0.1)
    process.terminate()
    raise SystemExit("control API did not start")


def wait_for(check, what, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {what}")


def drive_files(folder):
    return sorted(folder.glob("*.json")) if folder.is_dir() else []


def main():
    binary = Path(os.environ.get("COMMAVIEWD_HOST_BIN", "")) if os.environ.get("COMMAVIEWD_HOST_BIN") else \
        Path(__file__).resolve().parents[2] / "dist/commaviewd-host"
    if not binary.is_file():
        raise SystemExit("build host runtime first")
    with tempfile.TemporaryDirectory(prefix="commaview-drive-stats-test-") as temp:
        root = Path(temp)
        params, mem_params, msgq, live = root / "params", root / "shm-params", root / "shm", root / "live"
        install = root / "install"
        for folder in (params, mem_params, msgq, live, install / "data", install / "logs"):
            folder.mkdir(parents=True)
        (params / "IsOffroad").write_text("1")
        runs = root / "runs.txt"
        script = root / "fake-drive-stats.sh"
        script.write_text('#!/bin/sh\necho "$@" >> "$RUNS_FILE"\n')
        script.chmod(0o755)
        # A drive's file is rewritten every minute here, so what the test sees are the writes made
        # when the car stops and when the drive ends.
        base_env = dict(os.environ, COMMAVIEWD_TEST_PARAMS_DIR=str(params), COMMAVIEWD_TEST_DATA_ROOT=str(install),
                        COMMAVIEWD_TEST_LIVE_DIR=str(live), COMMAVIEWD_TEST_MEM_PARAMS_DIR=str(mem_params),
                        COMMAVIEWD_TEST_MSGQ_DIR=str(msgq), COMMAVIEWD_TEST_DRIVE_SCRIPT=str(script),
                        COMMAVIEWD_TEST_DRIVE_POLL_MS="50", COMMAVIEWD_TEST_DRIVE_CHECKPOINT_MS="60000",
                        RUNS_FILE=str(runs))
        drives = install / "data" / "drives"
        road_tool = os.environ.get("COMMAVIEWD_ROAD_QUEUE_TOOL")

        def car_state(speed, count, interval_ms=10):
            """carStates published at speed m/s, interval_ms apart and ending now, as card would."""
            if road_tool:
                subprocess.check_call([road_tool, "--append-car-state", str(msgq / "msgq_carState"),
                                       str(speed), str(count), str(interval_ms)])

        # A runtime that hasn't been paired never shows drives or location.
        unpaired_port = free_port()
        unpaired = start_control(binary, dict(base_env, COMMAVIEWD_API_TOKEN_FILE=str(root / "missing.token")), unpaired_port)
        try:
            assert request(unpaired_port, "/commaview/drive-stats")[0] == 401
            assert request(unpaired_port, "/commaview/location")[0] == 401
            assert request(unpaired_port, "/commaview/location", "POST", body={"enabled": True})[0] == 401
        finally:
            unpaired.terminate()
            unpaired.wait(timeout=5)
        runs.unlink(missing_ok=True)

        # The single drive list earlier runtimes kept moves into the folder when the service starts.
        old_start = (int(time.time()) - 3 * 86400) * 1000
        old_drive = {"startMs": old_start, "endMs": old_start + 600000, "durationS": 600, "distanceM": 12000}
        ledger = {"version": 1, "sinceMs": old_start - 10000000, "drives": [old_drive]}
        (install / "data" / "drives.json").write_text(json.dumps(ledger))

        port = free_port()
        process = start_control(binary, dict(base_env, COMMAVIEWD_API_TOKEN="test-token"), port)
        token = "test-token"
        try:
            assert request(port, "/commaview/drive-stats")[0] == 401
            assert request(port, "/commaview/drive-stats", token="wrong")[0] == 401
            assert request(port, "/commaview/drive-stats", token=token) == \
                (200, {"ok": True, "onroad": False, "current": None, "ledger": ledger, "totals": None})
            assert not (install / "data" / "drives.json").exists() and (install / "data" / "drives.json.moved").exists()
            assert json.loads((drives / f"{old_start}.json").read_text()) == old_drive
            assert json.loads((install / "data" / "drives-since.json").read_text()) == {"sinceMs": ledger["sinceMs"]}
            # No totals yet and parked: the phone's visit asks the script for comma's totals.
            wait_for(lambda: runs.exists() and "totals" in runs.read_text(), "a totals run")

            totals = {"version": 1, "source": "comma", "fetchedAtMs": 1780000700000,
                      "all": {"routes": 1349, "distanceMi": 13027.4, "minutes": 26000},
                      "week": {"routes": 28, "distanceMi": 105.25, "minutes": 372}}
            (install / "data" / "drive-stats.json").write_text(json.dumps(totals))
            status, body = request(port, "/commaview/drive-stats", token=token)
            assert status == 200 and body["ledger"] == ledger and body["totals"] == totals
            (install / "data" / "drive-stats.json").write_text("[not an object]")
            assert request(port, "/commaview/drive-stats", token=token)[1]["totals"] is None

            # Location is off by default: positions sunnypilot or the comma keep are never served.
            (params / "LastGPSPositionLLK").write_text(json.dumps({"latitude": 37.5, "longitude": -122.2, "altitude": 10.0}))
            assert request(port, "/commaview/location", token=token) == \
                (200, {"ok": True, "enabled": False, "onroad": False, "live": None, "last": None})
            assert request(port, "/commaview/location", "POST", token, {"enabled": "yes"})[0] == 400
            assert request(port, "/commaview/location", "POST", None, {"enabled": True})[0] == 401
            assert request(port, "/commaview/location", "POST", token, {"enabled": True}) == (200, {"ok": True, "enabled": True})
            assert json.loads((install / "config" / "location.json").read_text()) == {"enabled": True}

            # Parked: where the last drive ended, here from the position sunnypilot saves.
            status, body = request(port, "/commaview/location", token=token)
            assert status == 200 and body["live"] is None
            assert body["last"]["lat"] == 37.5 and body["last"]["source"] == "sunnypilot"

            # A drive: the car is on, loggerd names the route, and the comma's GPS is in msgq.
            runs.unlink(missing_ok=True)
            car_state(0.0, 0)  # card's queue, before it publishes
            now_ms = int(time.time() * 1000)
            write_gps_queue(msgq / "msgq_gpsLocationExternal", [now_ms - 2000, now_ms])
            (params / "IsOffroad").write_text("0")
            (params / "CurrentRoute").write_text("0000002a--8f1e2d3c4b")
            wait_for(lambda: request(port, "/commaview/drive-stats", token=token)[1]["current"] is not None, "the drive to start")
            drive_started = time.monotonic()
            car_state(20.0, 101)  # a second at 20 m/s: 20 m
            status, body = request(port, "/commaview/location", token=token)
            assert body["onroad"] is True
            assert body["live"]["lat"] == 37.7749 and body["live"]["source"] == "comma" and body["live"]["fixMs"] == now_ms
            # A timestamped GPS fix wins over sunnypilot's saved coordinates, even after the
            # cache writer refreshes the file mtime. The original fix time and accuracy survive.
            (mem_params / "LastGPSPosition").write_text(json.dumps({"latitude": 37.8, "longitude": -122.3, "bearing": 45.0}))
            live_now = request(port, "/commaview/location", token=token)[1]["live"]
            assert live_now["lat"] == 37.7749 and live_now["source"] == "comma"
            assert live_now["fixMs"] == now_ms and live_now["accuracyM"] == 4.5
            for _ in range(3):
                time.sleep(1.05)
                (mem_params / "LastGPSPosition").write_text(json.dumps({"latitude": 37.8, "longitude": -122.3, "bearing": 45.0}))
                live_now = request(port, "/commaview/location", token=token)[1]["live"]
                assert live_now["source"] == "comma" and live_now["fixMs"] == now_ms
                assert live_now["lat"] == 37.7749 and live_now["accuracyM"] == 4.5
            assert int(time.time() * 1000) - live_now["fixMs"] > 3000
            # A new GPS acquisition recovers normally while the cached position still exists.
            recovered_ms = int(time.time() * 1000)
            write_gps_queue(msgq / "msgq_gpsLocationExternal", [recovered_ms])
            recovered = request(port, "/commaview/location", token=token)[1]["live"]
            assert recovered["source"] == "comma" and recovered["fixMs"] == recovered_ms
            assert recovered["lat"] == 37.7749 and recovered["accuracyM"] == 4.5
            (mem_params / "LastGPSPosition").unlink()

            # The first ten seconds are an ignition blip, kept nowhere.
            assert drive_files(drives) == [drives / f"{old_start}.json"]
            time.sleep(max(0.0, drive_started + 11 - time.monotonic()))
            # The car slows to a stop: the drive is on disk then, as far as it has gone, before the
            # power can go. Its file is named by its start; the phone sees it only once it has ended.
            car_state(10.0, 51)  # half a second at 10 m/s: 5 m
            time.sleep(1.2)  # stopped a moment later (samples over a second apart aren't joined)
            car_state(0.0, 5)
            wait_for(lambda: len(drive_files(drives)) == 2, "the drive written when the car stopped")
            path = next(p for p in drive_files(drives) if p.name != f"{old_start}.json")
            written = json.loads(path.read_text())
            assert path.name == f"{written['startMs']}.json" and written["route"] == "0000002a--8f1e2d3c4b"
            assert abs(written["startMs"] - now_ms) < 5000 and written["durationS"] >= 10
            assert written["distanceM"] == (25 if road_tool else 0), written
            assert stat.S_IMODE(path.stat().st_mode) == 0o600
            status, body = request(port, "/commaview/drive-stats", token=token)
            assert body["ledger"] == ledger and body["current"]["startMs"] == written["startMs"]

            # The drive ends: where it ended is kept from the comma's GPS, its file has its last
            # seconds, and nothing runs after it.
            (params / "IsOffroad").write_text("1")
            wait_for(lambda: request(port, "/commaview/drive-stats", token=token)[1]["current"] is None, "the drive to end")
            ended = json.loads(path.read_text())
            assert ended["startMs"] == written["startMs"] and ended["distanceM"] == written["distanceM"]
            assert ended["durationS"] >= written["durationS"] and ended["endMs"] >= written["endMs"]
            assert request(port, "/commaview/drive-stats", token=token)[1]["ledger"]["drives"] == [old_drive, ended]
            assert not (runs.exists() and "after-drive" in runs.read_text())
            last = json.loads((install / "data" / "location-last.json").read_text())
            assert last["lat"] == 37.7749 and last["source"] == "comma" and last["endedMs"] >= now_ms
            status, body = request(port, "/commaview/location", token=token)
            assert body["live"] is None and body["last"]["source"] == "comma"

            # Turning it off forgets where the comma was, right away.
            (live / "location-log.json").write_text(json.dumps({"lat": 1.0, "lon": 2.0, "fixMs": now_ms, "source": "log"}))
            assert request(port, "/commaview/location", "POST", token, {"enabled": False}) == (200, {"ok": True, "enabled": False})
            assert not (live / "location-log.json").exists()
            assert not (install / "data" / "location-last.json").exists()
            assert request(port, "/commaview/location", token=token)[1] == \
                {"ok": True, "enabled": False, "onroad": False, "live": None, "last": None}

            # A drive the control service restarts in the middle of (an update, a crash) carries on in
            # the same file. Meanwhile, comma's totals are fetched while driving.
            (params / "CurrentRoute").write_text("0000002b--1111111111")
            (params / "IsOffroad").write_text("0")
            wait_for(lambda: request(port, "/commaview/drive-stats", token=token)[1]["current"] is not None, "a second drive")
            second_started = time.monotonic()
            car_state(15.0, 101)  # 15 m
            time.sleep(max(0.0, second_started + 11 - time.monotonic()))
            car_state(0.0, 5)
            wait_for(lambda: len(drive_files(drives)) == 3, "the second drive written when the car stopped")
            second = max(drive_files(drives), key=lambda p: json.loads(p.read_text())["startMs"])
            before = json.loads(second.read_text())
            assert before["route"] == "0000002b--1111111111" and before["distanceM"] == (15 if road_tool else 0)
        finally:
            process.terminate()
            process.wait(timeout=5)

        runs.unlink(missing_ok=True)
        port = free_port()
        process = start_control(binary, dict(base_env, COMMAVIEWD_API_TOKEN="test-token",
                                             COMMAVIEWD_TEST_TOTALS_FIRST_MS="300"), port)
        try:
            wait_for(lambda: request(port, "/commaview/drive-stats", token=token)[1]["current"] is not None, "the drive to carry on")
            restarted = time.monotonic()
            assert request(port, "/commaview/drive-stats", token=token)[1]["current"]["startMs"] == before["startMs"]
            wait_for(lambda: runs.exists() and "totals" in runs.read_text(), "comma's totals while driving")
            time.sleep(max(0.0, restarted + 3.5 - time.monotonic()))
            car_state(10.0, 101)  # 10 m more
            time.sleep(1.2)
            car_state(0.0, 5)
            wait_for(lambda: json.loads(second.read_text())["endMs"] > before["endMs"], "the carried-on drive written")
            after = json.loads(second.read_text())
            assert len(drive_files(drives)) == 3 and after["startMs"] == before["startMs"]
            assert after["distanceM"] == (25 if road_tool else 0) and after["durationS"] >= before["durationS"] + 3
            (params / "IsOffroad").write_text("1")
            wait_for(lambda: request(port, "/commaview/drive-stats", token=token)[1]["current"] is None, "the second drive to end")
            assert [d["startMs"] for d in request(port, "/commaview/drive-stats", token=token)[1]["ledger"]["drives"]] == \
                [old_drive["startMs"], ended["startMs"], before["startMs"]]
            if not road_tool:
                print("SKIP: distance from carState (set COMMAVIEWD_ROAD_QUEUE_TOOL)")
            print("PASS: drive stats and location come from what the comma keeps, for paired phones only")
        finally:
            process.terminate()
            process.wait(timeout=5)


if __name__ == "__main__":
    main()
