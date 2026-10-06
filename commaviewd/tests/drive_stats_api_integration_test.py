#!/usr/bin/env python3
"""Drive stats and location, served to paired phones only, from what the comma already keeps.

The control service notices each drive from the onroad flag and runs the drive stats script after
it; location is off until turned on, prefers the comma's timestamped GPS read from openpilot's
msgq ring (without subscribing), falls back to saved positions, and is forgotten when turned off.
"""

import json
import os
from pathlib import Path
import socket
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
        base_env = dict(os.environ, COMMAVIEWD_TEST_PARAMS_DIR=str(params), COMMAVIEWD_TEST_DATA_ROOT=str(install),
                        COMMAVIEWD_TEST_LIVE_DIR=str(live), COMMAVIEWD_TEST_MEM_PARAMS_DIR=str(mem_params),
                        COMMAVIEWD_TEST_MSGQ_DIR=str(msgq), COMMAVIEWD_TEST_DRIVE_SCRIPT=str(script),
                        COMMAVIEWD_TEST_DRIVE_POLL_MS="50", RUNS_FILE=str(runs))

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

        port = free_port()
        process = start_control(binary, dict(base_env, COMMAVIEWD_API_TOKEN="test-token"), port)
        token = "test-token"
        try:
            assert request(port, "/commaview/drive-stats")[0] == 401
            assert request(port, "/commaview/drive-stats", token="wrong")[0] == 401
            assert request(port, "/commaview/drive-stats", token=token) == \
                (200, {"ok": True, "onroad": False, "current": None, "ledger": None, "totals": None})
            # No totals yet and parked: the phone's visit asks the script for comma's totals.
            wait_for(lambda: runs.exists() and "totals" in runs.read_text(), "a totals run")

            ledger = {"version": 1, "sinceMs": 1780000000000,
                      "drives": [{"startMs": 1780000000000, "endMs": 1780000600000, "durationS": 600, "distanceM": 12000}]}
            totals = {"version": 1, "source": "comma", "fetchedAtMs": 1780000700000,
                      "all": {"routes": 1349, "distanceMi": 13027.4, "minutes": 26000},
                      "week": {"routes": 28, "distanceMi": 105.25, "minutes": 372}}
            (install / "data" / "drives.json").write_text(json.dumps(ledger))
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
            now_ms = int(time.time() * 1000)
            write_gps_queue(msgq / "msgq_gpsLocationExternal", [now_ms - 2000, now_ms])
            (params / "IsOffroad").write_text("0")
            (params / "CurrentRoute").write_text("0000002a--8f1e2d3c4b")
            wait_for(lambda: request(port, "/commaview/drive-stats", token=token)[1]["current"] is not None, "the drive to start")
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

            # The drive ends: where it ended is kept from the comma's GPS, and the script runs.
            time.sleep(11)  # shorter drives are ignition blips
            (params / "IsOffroad").write_text("1")
            wait_for(lambda: runs.exists() and "after-drive" in runs.read_text(), "the after-drive run")
            args = runs.read_text().split("after-drive", 1)[1].split()
            assert args[0:2] == ["--route", "0000002a--8f1e2d3c4b"] and args[4] == "--end-ms" and args[6] == "--duration-s"
            assert int(args[7]) >= 10
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
            print("PASS: drive stats and location come from what the comma keeps, for paired phones only")
        finally:
            process.terminate()
            process.wait(timeout=5)


if __name__ == "__main__":
    main()
