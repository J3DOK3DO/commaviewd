#!/usr/bin/env python3
"""The leaderboard endpoints, end to end (docs/plans/leaderboard.md in RhynoTech/commaview-web, "The
comma"): POST /commaview/leaderboard/register and /statement answer only a paired phone, run the real
drive stats script to sign with this comma's own key, and the key never leaves its 0600 file: not in
an answer, the script's log, or a support bundle. While the drive list and the key are unchanged, a
statement is the last one again, from its 0600 cache, without running the script; while a drive goes
on, its changing file makes a new statement only every 5 minutes.
"""

import base64
import hashlib
import json
import os
from pathlib import Path
import socket
import stat
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "comma" / "src" / "commaview_drive_stats.py"
DONGLE = "0123456789abcdef"
CHALLENGE = "qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqo"
TOKEN = "test-token"


def request(port, path, method="POST", token=None, body=None):
    data = json.dumps(body).encode() if body is not None else (b"{}" if method == "POST" else None)
    headers = {"Content-Type": "application/json"}
    if token:
        headers["X-CommaView-Token"] = token
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=30) as response:
            return response.status, json.load(response)
    except urllib.error.HTTPError as error:
        return error.code, json.load(error)


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def start_control(binary, env):
    port = free_port()
    process = subprocess.Popen([str(binary), "control", "--port", str(port)],
                               env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(50):
        try:
            request(port, "/commaview/version", "GET")
            return process, port
        except (OSError, ValueError):
            time.sleep(0.1)
    process.terminate()
    raise SystemExit("control API did not start")


def stop(process):
    process.terminate()
    process.wait(timeout=5)


def b64u_decode(text):
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


def b64u(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def parts(token):
    header, payload, signature = token.split(".")
    return json.loads(b64u_decode(header)), json.loads(b64u_decode(payload)), b64u_decode(signature)


def verify(token, x):
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
    signing_input, signature = token.rsplit(".", 1)
    Ed25519PublicKey.from_public_bytes(b64u_decode(x)).verify(b64u_decode(signature), signing_input.encode("ascii"))


def key_id(x):
    return b64u(hashlib.sha256(('{"crv":"Ed25519","kty":"OKP","x":"%s"}' % x).encode()).digest())


def main():
    binary = Path(os.environ["COMMAVIEWD_HOST_BIN"]) if os.environ.get("COMMAVIEWD_HOST_BIN") else \
        REPO_ROOT / "dist/commaviewd-host"
    if not binary.is_file():
        raise SystemExit("build host runtime first")

    with tempfile.TemporaryDirectory(prefix="commaview-lb-test-") as temp:
        root = Path(temp)
        params, install, swaglog, msgq, no_crypto = (root / "params", root / "install", root / "log", root / "shm",
                                                     root / "no-crypto")
        for folder in (params, install / "data", install / "logs", swaglog, msgq, no_crypto / "cryptography"):
            folder.mkdir(parents=True)
        (params / "IsOffroad").write_text("1")
        (params / "DongleId").write_text(DONGLE)
        (root / "devicetree-model").write_bytes(b"comma tizi\x00")  # a comma 3X, as openpilot reads it
        (params / "HardwareSerial").write_text("c0ffee12")
        (install / "VERSION").write_text("v0.0.60\n")
        (root / "kernel.log").write_text("")
        # A Python without cryptography (as an openpilot that dropped it would be).
        (no_crypto / "cryptography" / "__init__.py").write_text("raise ImportError('no cryptography here')\n")
        wrapper = root / "drive-stats.sh"
        runs = root / "script-runs.txt"
        wrapper.write_text(f'#!/bin/sh\necho "$1" >> "{runs}"\nexec "{sys.executable}" "{SCRIPT}" "$@"\n')
        wrapper.chmod(0o755)
        key_file = install / "data" / "leaderboard-key.json"
        drive_dir = install / "data" / "drives"
        cache_file = install / "data" / "leaderboard-statement.json"

        def statement_runs():
            return runs.read_text().split().count("leaderboard-statement") if runs.exists() else 0
        script_log = install / "logs" / "commaview-drive-stats.log"

        base_env = dict(os.environ, COMMAVIEWD_TEST_PARAMS_DIR=str(params), COMMAVIEWD_TEST_DATA_ROOT=str(install),
                        COMMAVIEWD_TEST_LIVE_DIR=str(root / "live"), COMMAVIEWD_TEST_MEM_PARAMS_DIR=str(root / "shm-params"),
                        COMMAVIEWD_TEST_MSGQ_DIR=str(msgq), COMMAVIEWD_TEST_SWAGLOG_DIR=str(swaglog),
                        COMMAVIEWD_TEST_KERNEL_LOG_FILE=str(root / "kernel.log"),
                        COMMAVIEWD_TEST_DRIVE_SCRIPT=str(wrapper), COMMAVIEW_DRIVE_ROOT=str(install),
                        COMMAVIEW_DRIVE_PARAMS_DIR=str(params),
                        COMMAVIEW_DRIVE_DEVICE_MODEL_FILE=str(root / "devicetree-model"),
                        COMMAVIEWD_TEST_DRIVE_POLL_MS="50")
        base_env.pop("PYTHONPATH", None)

        # A runtime that hasn't been paired never signs.
        unpaired, port = start_control(binary, dict(base_env, COMMAVIEWD_API_TOKEN_FILE=str(root / "missing.token")))
        try:
            assert request(port, "/commaview/leaderboard/register", body={"challenge": CHALLENGE})[0] == 401
            assert request(port, "/commaview/leaderboard/statement")[0] == 401
            assert not key_file.exists()
        finally:
            stop(unpaired)

        process, port = start_control(binary, dict(base_env, COMMAVIEWD_API_TOKEN=TOKEN))
        try:
            # The pairing token, like every private route.
            for token in (None, "wrong"):
                assert request(port, "/commaview/leaderboard/register", token=token, body={"challenge": CHALLENGE}) == \
                    (401, {"ok": False, "error": "unauthorized"})
                assert request(port, "/commaview/leaderboard/statement", token=token)[0] == 401
            assert request(port, "/commaview/leaderboard/statement", "GET", TOKEN)[0] == 404

            # No statement before the first registration.
            assert request(port, "/commaview/leaderboard/statement", token=TOKEN) == (409, {"ok": False, "error": "no key"})

            # A challenge is 32 bytes as base64url, 43 characters.
            for body in ({}, {"challenge": ""}, {"challenge": "qqq"}, {"challenge": CHALLENGE + "q"},
                         {"challenge": CHALLENGE[:-1] + "+"}, {"challenge": CHALLENGE[:-1] + "r"}, {"challenge": 7}):
                assert request(port, "/commaview/leaderboard/register", token=TOKEN, body=body) == \
                    (400, {"ok": False, "error": "challenge required"}), body
            assert not key_file.exists()

            # The first registration makes the key.
            status, answer = request(port, "/commaview/leaderboard/register", token=TOKEN, body={"challenge": CHALLENGE})
            assert status == 200 and set(answer) == {"ok", "registration", "keyId"} and answer["ok"] is True, answer
            header, payload, _ = parts(answer["registration"])
            assert header == {"alg": "EdDSA", "kid": answer["keyId"], "typ": "cv-lb-reg+jwt"}
            assert key_id(payload["publicKey"]) == answer["keyId"]
            verify(answer["registration"], payload["publicKey"])
            device_hash = b64u(hashlib.sha256(("commaview-leaderboard-device/v1:" + DONGLE).encode()).digest())
            assert payload["challenge"] == CHALLENGE and payload["deviceHash"] == device_hash
            assert payload["deviceType"] == "tizi" and "model" not in payload
            assert payload["runtimeVersion"] == "v0.0.60" and payload["v"] == 1
            assert stat.S_IMODE(key_file.stat().st_mode) == 0o600
            seed = json.loads(key_file.read_text())["seed"]
            public_x = payload["publicKey"]

            # Again (a retry, or a second phone): the same key.
            dashed = b64u(b"\xf8" + bytes(31))
            status, again = request(port, "/commaview/leaderboard/register", token=TOKEN, body={"challenge": dashed})
            assert status == 200 and again["keyId"] == answer["keyId"] and parts(again["registration"])[1]["challenge"] == dashed

            # Statements: the drive list by UTC day, seq one higher each time.
            day_ms = 86_400_000
            today = int(time.time() * 1000) // day_ms * day_ms
            drives = [{"startMs": today - 2 * day_ms + 3_600_000, "endMs": today - 2 * day_ms + 4_200_000,
                       "durationS": 600, "distanceM": 9000, "route": "0000002a--8f1e2d3c4b"},
                      {"startMs": today - 2 * day_ms + 7_200_000, "endMs": today - 2 * day_ms + 7_500_000,
                       "durationS": 300, "distanceM": 3000}]
            def write_drives(listed):
                """The drive list as the drive watcher keeps it: one file per drive."""
                drive_dir.mkdir(exist_ok=True)
                for old in drive_dir.iterdir():
                    old.unlink()
                for listed_drive in listed:
                    (drive_dir / f"{listed_drive['startMs']}.json").write_text(json.dumps(listed_drive))

            def driving(expected):
                for _ in range(100):
                    current = request(port, "/commaview/drive-stats", "GET", TOKEN)[1]["current"]
                    if (current is not None) == expected:
                        return
                    time.sleep(0.05)
                raise AssertionError(f"the drive watcher never saw driving={expected}")

            write_drives(drives)
            runs_before = statement_runs()  # the "no key" answer above ran it once
            day = time.strftime("%Y-%m-%d", time.gmtime((today - 2 * day_ms) / 1000))
            status, statement = request(port, "/commaview/leaderboard/statement", token=TOKEN)
            assert status == 200 and set(statement) == {"ok", "statement", "keyId", "seq"}, statement
            assert statement["seq"] == 1 and statement["keyId"] == answer["keyId"]
            header, payload, _ = parts(statement["statement"])
            assert header == {"alg": "EdDSA", "kid": answer["keyId"], "typ": "cv-lb-stats+jwt"}
            assert payload["seq"] == 1 and payload["v"] == 1 and payload["runtimeVersion"] == "v0.0.60"
            assert payload["deviceType"] == "tizi"
            assert payload["days"] == [{"day": day, "distanceM": 12000, "durationS": 900, "drives": 2}]
            verify(statement["statement"], public_x)
            assert statement_runs() == runs_before + 1
            assert stat.S_IMODE(cache_file.stat().st_mode) == 0o600

            # Nothing changed on the comma (mid-drive, say): the same statement, and no script runs.
            for _ in range(3):
                assert request(port, "/commaview/leaderboard/statement", token=TOKEN) == (200, statement)
            assert statement_runs() == runs_before + 1
            # The cache never answers without the pairing token.
            assert request(port, "/commaview/leaderboard/statement", token="wrong")[0] == 401

            # Another drive (the drive watcher wrote its file): signed again, seq one higher.
            drives = drives + [{"startMs": today - day_ms + 3_600_000, "endMs": today - day_ms + 3_900_000,
                                "durationS": 300, "distanceM": 2500}]
            write_drives(drives)
            status, statement = request(port, "/commaview/leaderboard/statement", token=TOKEN)
            assert status == 200 and statement["seq"] == 2 and statement_runs() == runs_before + 2
            payload = parts(statement["statement"])[1]
            assert payload["seq"] == 2 and payload["days"][0] == {"day": day, "distanceM": 12000, "durationS": 900, "drives": 2}
            assert payload["days"][1]["distanceM"] == 2500
            verify(statement["statement"], public_x)
            assert request(port, "/commaview/leaderboard/statement", token=TOKEN) == (200, statement)
            assert statement_runs() == runs_before + 2
            assert json.loads((install / "data" / "leaderboard-seq.json").read_text())["seq"] == 2
            assert "commaStats" not in payload  # no comma totals saved yet

            # comma's totals from sunnypilot's cache (Driving insights): never in a statement, and not
            # a reason to sign again.
            (install / "data" / "drive-stats.json").write_text(json.dumps(
                {"version": 1, "source": "sunnypilot", "fetchedAtMs": today - day_ms,
                 "all": {"routes": 1349, "distanceMi": 99999.0, "minutes": 26000},
                 "week": {"routes": 28, "distanceMi": 105.25, "minutes": 372}}))
            assert request(port, "/commaview/leaderboard/statement", token=TOKEN) == (200, statement)
            assert statement_runs() == runs_before + 2

            # The runtime's own request to comma answered: signed again, with comma's all-time
            # distance in metres and when it asked; the days are as they were.
            fetched_ms = today - day_ms
            (install / "data" / "comma-api-stats.json").write_text(json.dumps(
                {"version": 1, "source": "comma-api", "fetchedAtMs": fetched_ms,
                 "all": {"routes": 1349, "distanceMi": 13027.4, "minutes": 26000}}))
            days_before = payload["days"]
            status, statement = request(port, "/commaview/leaderboard/statement", token=TOKEN)
            assert status == 200 and statement["seq"] == 3 and statement_runs() == runs_before + 3
            payload = parts(statement["statement"])[1]
            assert payload["commaStats"] == {"allTimeM": 20965568, "fetchedAtMs": fetched_ms, "source": "comma-api"}
            assert payload["days"] == days_before
            verify(statement["statement"], public_x)
            assert request(port, "/commaview/leaderboard/statement", token=TOKEN) == (200, statement)
            assert statement_runs() == runs_before + 3

            # A drive going on: its file grows every few seconds, and the statement covering it is
            # signed again only once the last one is 5 minutes old.
            (params / "IsOffroad").write_text("0")
            driving(True)
            write_drives(drives[:-1] + [dict(drives[-1], distanceM=4000, durationS=420)])
            for _ in range(3):
                assert request(port, "/commaview/leaderboard/statement", token=TOKEN) == (200, statement)
            assert statement_runs() == runs_before + 3
            aged = time.time() - 301
            os.utime(cache_file, (aged, aged))
            status, statement = request(port, "/commaview/leaderboard/statement", token=TOKEN)
            assert status == 200 and statement["seq"] == 4 and statement_runs() == runs_before + 4
            assert parts(statement["statement"])[1]["days"][1]["distanceM"] == 4000
            write_drives(drives[:-1] + [dict(drives[-1], distanceM=5000, durationS=480)])
            assert request(port, "/commaview/leaderboard/statement", token=TOKEN) == (200, statement)
            # The comma's totals (or key) changing is signed at once, drive or not.
            (install / "data" / "comma-api-stats.json").write_text(json.dumps(
                {"version": 1, "source": "comma-api", "fetchedAtMs": fetched_ms + 1000,
                 "all": {"routes": 1350, "distanceMi": 13030.0, "minutes": 26010}}))
            status, statement = request(port, "/commaview/leaderboard/statement", token=TOKEN)
            assert status == 200 and statement["seq"] == 5 and statement_runs() == runs_before + 5
            assert parts(statement["statement"])[1]["days"][1]["distanceM"] == 5000
            # Parked, any change is signed at once again.
            (params / "IsOffroad").write_text("1")
            driving(False)
            write_drives(drives[:-1] + [dict(drives[-1], distanceM=5100, durationS=490)])
            status, statement = request(port, "/commaview/leaderboard/statement", token=TOKEN)
            assert status == 200 and statement["seq"] == 6 and statement_runs() == runs_before + 6

            # The key gone (an uninstall that kept the drive list): no key, as before, and no cache left.
            key_file.rename(root / "key-aside.json")
            assert request(port, "/commaview/leaderboard/statement", token=TOKEN) == (409, {"ok": False, "error": "no key"})
            assert not cache_file.exists()
            (root / "key-aside.json").rename(key_file)
            status, statement = request(port, "/commaview/leaderboard/statement", token=TOKEN)
            assert status == 200 and statement["seq"] == 7

            # Rotating makes a new key, whose counter starts again.
            status, rotated = request(port, "/commaview/leaderboard/register", token=TOKEN,
                                      body={"challenge": CHALLENGE, "rotate": True})
            assert status == 200 and rotated["keyId"] != answer["keyId"]
            assert stat.S_IMODE(key_file.stat().st_mode) == 0o600
            new_seed = json.loads(key_file.read_text())["seed"]
            assert new_seed != seed
            status, statement = request(port, "/commaview/leaderboard/statement", token=TOKEN)
            assert status == 200 and statement["seq"] == 1 and statement["keyId"] == rotated["keyId"], statement
            verify(statement["statement"], parts(rotated["registration"])[1]["publicKey"])

            # A support bundle never carries the key, even from a log that somehow held it.
            (swaglog / "swaglog.0000000001").write_text("\n".join([
                json.dumps({"msg$s": f"killing modeld {new_seed}"}),
                json.dumps({"msg$s": "killing camerad", "seed": seed}),
            ]) + "\n")
            status, bundle = request(port, "/commaview/support/logs", "GET", TOKEN)
            assert status == 200 and bundle["ok"] is True
            text = json.dumps(bundle)
            assert "killing modeld" in text and "killing camerad" in text
            assert new_seed not in text and seed not in text
            assert not any("leaderboard-key" in entry["path"] + entry["name"] for entry in bundle["files"])
            assert not any("/data/commaview/data/" in entry["path"] for entry in bundle["files"])

            # The answers and the script's own log never held it either.
            log_text = script_log.read_text() if script_log.exists() else ""
            for secret in (seed, new_seed):
                assert secret not in json.dumps([answer, again, rotated, statement]) and secret not in log_text
        finally:
            stop(process)

        # Without cryptography in openpilot's Python, or without that Python, the runtime says so when
        # it has something new to sign (the last statement needs neither).
        write_drives(drives[:1])
        for env in (dict(base_env, COMMAVIEWD_API_TOKEN=TOKEN, PYTHONPATH=str(no_crypto)),
                    dict(base_env, COMMAVIEWD_API_TOKEN=TOKEN, COMMAVIEWD_TEST_DRIVE_SCRIPT=str(root / "missing.sh"))):
            process, port = start_control(binary, env)
            try:
                assert request(port, "/commaview/leaderboard/register", token=TOKEN, body={"challenge": CHALLENGE}) == \
                    (503, {"ok": False, "error": "crypto unavailable"})
                assert request(port, "/commaview/leaderboard/statement", token=TOKEN) == \
                    (503, {"ok": False, "error": "crypto unavailable"})
            finally:
                stop(process)
        assert json.loads(key_file.read_text())["seed"] == new_seed  # kept for when it's back

    print("PASS: leaderboard endpoints sign with the comma's own key, for paired phones only")


if __name__ == "__main__":
    main()
