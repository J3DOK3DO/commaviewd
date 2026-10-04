#!/usr/bin/env python3
"""The runtime reports which comma it runs on as deviceType ("tici", "tizi", "mici"; "" when it
can't tell) in /commaview/version, /commaview/status and the local discovery reply, read from the
devicetree model the way openpilot reads it. The comma 3 (tici) is reported as itself, never as
the comma 3X (tizi): CommaView doesn't support it, and the app says so from this field. deviceModel
and the other identity fields are unchanged (deviceModel is "comma" without a HardwareModel param)."""

import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

TOKEN = "test-token"
DISCOVERY_PORT = 5004


def request(port, path):
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", headers={"X-CommaView-Token": TOKEN})
    with urllib.request.urlopen(req, timeout=10) as response:
        return json.load(response)


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def discover():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(0.5)
        for _ in range(10):
            sock.sendto(b"COMMAVIEW_DISCOVER_V1", ("127.0.0.1", DISCOVERY_PORT))
            try:
                return json.loads(sock.recv(4096))
            except socket.timeout:
                continue
    raise AssertionError("no discovery reply")


def run_case(binary, root, model, want):
    params = root / "params"
    params.mkdir(exist_ok=True)
    (params / "IsOffroad").write_text("1")
    model_file = root / "model"
    if model is None:
        model_file.unlink(missing_ok=True)
    else:
        model_file.write_bytes(model)
    port = free_port()
    env = dict(os.environ, COMMAVIEWD_TEST_PARAMS_DIR=str(params), COMMAVIEWD_API_TOKEN=TOKEN,
               COMMAVIEWD_TEST_DEVICE_MODEL_FILE=str(model_file))
    process = subprocess.Popen([str(binary), "control", "--port", str(port)],
                               env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        version = None
        for _ in range(100):
            try:
                version = request(port, "/commaview/version")
                break
            except (OSError, ValueError, urllib.error.URLError):
                time.sleep(0.1)
        assert version is not None, "control API did not start"
        assert version["deviceType"] == want, (model, version)
        assert version["deviceModel"] == "comma", version
        assert version["device"] == "comma", version
        status = request(port, "/commaview/status")
        assert status["deviceType"] == want, (model, status.get("deviceType"))
        reply = discover()
        assert reply["type"] == "commaview.discovery.v1", reply
        assert reply["deviceType"] == want, (model, reply)
        assert reply["deviceModel"] == "comma", reply
    finally:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def main():
    binary = Path(os.environ.get("COMMAVIEWD_HOST_BIN") or Path(__file__).resolve().parents[2] / "dist/commaviewd-host")
    if not binary.is_file():
        raise SystemExit("build host runtime first")
    with tempfile.TemporaryDirectory(prefix="commaview-device-type-test-") as temp:
        root = Path(temp)
        # The devicetree model ends in a NUL, as on the device.
        for model, want in ((b"comma tici\0", "tici"), (b"comma tizi\0", "tizi"), (b"comma mici\0", "mici"),
                            (b"comma tici\n", "tici"), (b"Qualcomm Technologies, Inc. SDM845\0", ""), (None, "")):
            run_case(binary, root, model, want)
    print("PASS: deviceType reports tici, tizi and mici as themselves in version, status and discovery")


if __name__ == "__main__":
    main()
