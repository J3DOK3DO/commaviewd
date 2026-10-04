#!/usr/bin/env python3
"""Recording records, kept on the comma for every paired device (recording_records.h).

A paired device saves what it kept of a recording (a small JSON record and a thumbnail) and every
paired device lists them, so a drive recorded on the car's head unit can be made into a replay from
a phone at home. Only paired devices see or change them, and a save is taken while driving too.
"""

import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

JPEG = b"\xff\xd8" + b"thumbnail" + b"\xff\xd9"
RECORD = {"schemaVersion": 1, "sessionPrefix": "source_1700000000000_42", "routeId": "000004b4--75e1f0ba8f",
          "status": "stopped", "device": {"dongleId": "abc", "name": "Car", "hardwareSerial": "", "ip": ""}}


def request(port, path, method="GET", token=None, body=None, content_type="application/json"):
    data = body if isinstance(body, (bytes, type(None))) else json.dumps(body).encode()
    headers = {"Content-Type": content_type}
    if token:
        headers["X-CommaView-Token"] = token
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=2) as response:
            return response.status, response.headers.get("Content-Type"), response.read()
    except urllib.error.HTTPError as error:
        return error.code, error.headers.get("Content-Type"), error.read()


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


def main():
    binary = Path(os.environ["COMMAVIEWD_HOST_BIN"]) if os.environ.get("COMMAVIEWD_HOST_BIN") else \
        Path(__file__).resolve().parents[2] / "dist/commaviewd-host"
    if not binary.is_file():
        raise SystemExit("build host runtime first")
    with tempfile.TemporaryDirectory(prefix="commaview-recording-records-test-") as temp:
        root = Path(temp)
        params, install = root / "params", root / "install"
        for folder in (params, install / "logs"):
            folder.mkdir(parents=True)
        # The car is being driven: records are still saved (a recording is stopped on the road).
        (params / "IsOffroad").write_text("0")
        base_env = dict(os.environ, COMMAVIEWD_TEST_PARAMS_DIR=str(params), COMMAVIEWD_TEST_DATA_ROOT=str(install))

        # A runtime that hasn't been paired keeps no records for anyone.
        unpaired_port = free_port()
        unpaired = start_control(binary, dict(base_env, COMMAVIEWD_API_TOKEN_FILE=str(root / "missing.token")),
                                 unpaired_port)
        try:
            assert request(unpaired_port, "/commaview/recordings")[0] == 401
            assert request(unpaired_port, "/commaview/recordings/journal?id=source_1_1", "POST", body=RECORD)[0] == 401
        finally:
            unpaired.terminate()
            unpaired.wait(timeout=5)

        port = free_port()
        token = "test-token"
        process = start_control(binary, dict(base_env, COMMAVIEWD_API_TOKEN=token), port)
        try:
            status, _, body = request(port, "/commaview/status")
            assert status == 200 and "recording-records" in json.loads(body)["capabilities"]

            for path, method in (("/commaview/recordings", "GET"),
                                 ("/commaview/recordings/thumb?id=source_1_1", "GET"),
                                 ("/commaview/recordings/journal?id=source_1_1", "POST"),
                                 ("/commaview/recordings/delete?id=source_1_1", "POST")):
                assert request(port, path, method, body=b"{}" if method == "POST" else None)[0] == 401, path
                assert request(port, path, method, token="wrong", body=b"{}" if method == "POST" else None)[0] == 401

            status, _, body = request(port, "/commaview/recordings", token=token)
            assert status == 200 and json.loads(body) == {"ok": True, "recordings": [], "truncated": False}

            record_id = RECORD["sessionPrefix"]
            assert request(port, f"/commaview/recordings/journal?id={record_id}", "POST", token, RECORD)[0] == 200
            assert request(port, f"/commaview/recordings/thumb?id={record_id}", "POST", token, JPEG,
                           "image/jpeg")[0] == 200
            assert request(port, "/commaview/recordings/journal?id=../escape", "POST", token, RECORD)[0] == 400
            assert request(port, "/commaview/recordings/thumb?id=x", "POST", token, b"not a jpeg")[0] == 400

            saved = install / "data" / "recordings"
            assert sorted(p.name for p in saved.iterdir()) == [f"{record_id}.jpg", f"{record_id}.json"]
            assert (saved.stat().st_mode & 0o777) == 0o700

            status, _, body = request(port, "/commaview/recordings", token=token)
            listing = json.loads(body)
            assert status == 200 and listing["truncated"] is False
            assert [(r["id"], r["thumb"], r["journal"]) for r in listing["recordings"]] == [(record_id, True, RECORD)]
            assert listing["recordings"][0]["savedAtMs"] > 0

            status, content_type, body = request(port, f"/commaview/recordings/thumb?id={record_id}", token=token)
            assert status == 200 and content_type == "image/jpeg" and body == JPEG

            assert request(port, f"/commaview/recordings/delete?id={record_id}", "POST", token, b"")[0] == 200
            status, _, body = request(port, "/commaview/recordings", token=token)
            assert json.loads(body)["recordings"] == []
            assert list(saved.iterdir()) == []

            # How long drives have before openpilot deletes them: not while driving.
            assert "drive-retention" in json.loads(request(port, "/commaview/status")[2])["capabilities"]
            assert request(port, "/commaview/drive-retention?routes=000004b4--75e1f0ba8f")[0] == 401
            assert request(port, "/commaview/drive-retention?routes=000004b4--75e1f0ba8f", token=token)[0] == 403
        finally:
            process.terminate()
            process.wait(timeout=5)

        # Offroad: each asked-for drive, present or not, with the drive folder's figures.
        realdata = root / "realdata"
        for segment in range(2):
            folder = realdata / f"000004b4--75e1f0ba8f--{segment}"
            folder.mkdir(parents=True)
            (folder / "fcamera.hevc").write_bytes(b"x" * 8192)
        (params / "IsOffroad").write_text("1")
        port = free_port()
        process = start_control(binary, dict(base_env, COMMAVIEWD_API_TOKEN=token,
                                             COMMAVIEWD_SOURCE_ARCHIVE_ROOT=str(realdata)), port)
        try:
            status, _, body = request(port, "/commaview/drive-retention?routes=000004b4--75e1f0ba8f,000004b5--gone",
                                      token=token)
            retention = json.loads(body)
            assert status == 200 and retention["ok"] is True, body
            assert retention["deleteBelowBytes"] >= 5 * 1024 ** 3 and retention["bytesPerMinute"] > 0
            present, gone = retention["routes"]
            assert present["route"] == "000004b4--75e1f0ba8f" and present["present"] is True
            assert present["segments"] == 2 and isinstance(present["drivingMinutesLeft"], int)
            assert gone == {"route": "000004b5--gone", "present": False}
            assert request(port, "/commaview/drive-retention?routes=../etc", token=token)[0] == 400
        finally:
            process.terminate()
            process.wait(timeout=5)
    print("recording records API integration test passed")


if __name__ == "__main__":
    main()
