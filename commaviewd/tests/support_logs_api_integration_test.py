#!/usr/bin/env python3
"""The support bundle, end to end: GET /commaview/support/logs answers only a paired phone, and
gathers openpilot's process state (managerState, read without subscribing), the swaglog lines
about stopped or killed processes and the kernel's out-of-memory lines at request time, plus the
process watcher's log of what went down as it happened, capped and redacted before they leave the
comma.
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

DONGLE = "0123456789abcdef"


def request(port, path, token=None):
    headers = {"X-CommaView-Token": token} if token else {}
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=5) as response:
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


def stop(process):
    process.terminate()
    process.wait(timeout=5)


def swaglog_line(msg):
    return json.dumps({"msg$s": msg, "ctx": {"dongle_id": DONGLE, "branch": "release3"},
                       "levelname": "INFO", "created$f": 1727830000.5})


def main():
    binary = Path(os.environ["COMMAVIEWD_HOST_BIN"]) if os.environ.get("COMMAVIEWD_HOST_BIN") else \
        Path(__file__).resolve().parents[2] / "dist/commaviewd-host"
    if not binary.is_file():
        raise SystemExit("build host runtime first")
    queue_tool = os.environ.get("COMMAVIEWD_MANAGER_STATE_QUEUE_TOOL")

    with tempfile.TemporaryDirectory(prefix="commaview-support-logs-test-") as temp:
        root = Path(temp)
        params, msgq, swaglog = root / "params", root / "shm", root / "log"
        for folder in (params, msgq, swaglog, root / "install", root / "live", root / "shm-params"):
            folder.mkdir()
        (params / "IsOffroad").write_text("1")

        (swaglog / "swaglog.0000000041").write_text("\n".join([
            swaglog_line("killing modeld"),
            swaglog_line("modeld started"),
        ]) + "\n")
        (swaglog / "swaglog.0000000042").write_text("\n".join([
            json.dumps({"msg$s": {"event$s": "process_not_running", "not_running$a": ["modeld"]},
                        "ctx": {"dongle_id": DONGLE}}),
            swaglog_line("gps fix latitude=37.774929 longitude=-122.419416"),
            swaglog_line("modeld is dead with -9 (peer 8.8.8.8, ssid=Home WiFi)"),
        ]) + "\n")
        (swaglog / "unrelated.txt").write_text("killing should-not-appear\n")
        kernel_log = root / "kmsg.txt"
        kernel_log.write_text(
            "<6>[  10.0] wlan0: associated with aa:bb:cc:dd:ee:ff\n"
            "<4>[ 812.0] modeld invoked oom-killer: gfp_mask=0x6200ca\n"
            "<3>[ 812.1] Out of memory: Killed process 4242 (modeld) total-vm:1834000kB\n")
        if queue_tool:
            subprocess.check_call([queue_tool, "--write-manager-state-queue", str(msgq / "msgq_managerState")])
        # The process watcher's log from before a reboot: kept, and redacted on the way out like the rest.
        events_log = root / "install" / "logs" / "process-events.jsonl"
        events_log.parent.mkdir(parents=True)
        events_log.with_name("process-events.jsonl.1").write_text(json.dumps(
            {"t": 1727830000000, "event": "kernel", "line": f"Killed process 77 (micd) dongle_id={DONGLE} from 8.8.8.8"}) + "\n")

        env = dict(os.environ, COMMAVIEWD_TEST_PARAMS_DIR=str(params), COMMAVIEWD_TEST_MSGQ_DIR=str(msgq),
                   COMMAVIEWD_TEST_SWAGLOG_DIR=str(swaglog), COMMAVIEWD_TEST_KERNEL_LOG_FILE=str(kernel_log),
                   COMMAVIEWD_TEST_DATA_ROOT=str(root / "install"), COMMAVIEWD_TEST_LIVE_DIR=str(root / "live"),
                   COMMAVIEWD_TEST_MEM_PARAMS_DIR=str(root / "shm-params"),
                   COMMAVIEWD_TEST_PROCESS_EVENTS_LOG=str(events_log), COMMAVIEWD_PROCESS_WATCH_MS="500")

        # A runtime without an API token (never paired) hands its logs to no one.
        port = free_port()
        unpaired_env = dict(env, COMMAVIEWD_API_TOKEN_FILE=str(root / "missing.token"))
        del unpaired_env["COMMAVIEWD_TEST_PROCESS_EVENTS_LOG"]  # no watcher: its log is for the run below
        unpaired = start_control(binary, unpaired_env, port)
        try:
            assert request(port, "/commaview/support/logs")[0] == 401
        finally:
            stop(unpaired)

        port = free_port()
        process = start_control(binary, dict(env, COMMAVIEWD_API_TOKEN="test-token-1234567890"), port)
        try:
            assert request(port, "/commaview/support/logs")[0] == 401
            assert request(port, "/commaview/support/logs", token="wrong")[0] == 401
            # The watcher's first look at the (stale) process table: modeld and camerad down, manager silent.
            watch = {}
            for _ in range(40):
                watch = request(port, "/commaview/status")[1]["processEvents"]
                if not queue_tool or watch.get("events", 0) >= 3:
                    break
                time.sleep(0.1)
            assert watch["watching"] is True and watch["intervalMs"] == 500 and watch["kernelLog"] is False, watch
            if queue_tool:
                assert watch["downCount"] == 2 and watch["down"] == ["modeld", "camerad"], watch
                assert watch["managerSilent"] is True and watch["lastEvent"]["event"] == "manager-silent", watch

            status, body = request(port, "/commaview/support/logs", token="test-token-1234567890")
            assert status == 200 and body["ok"] is True, body
            assert "public-ip" in body["redacted"] and "gps" in body["redacted"], body["redacted"]
            files = {entry["name"]: entry for entry in body["files"]}
            names = [entry["name"] for entry in body["files"]]
            for name in ("openpilot-manager-state.json", "openpilot-process-events.log", "kernel-oom-events.log",
                         "runtime-debug-apply.log", "telemetry-stats.json", "commaviewd-control.log"):
                assert name in files, (name, names)
            # The small openpilot sections come before CommaView's rolling logs, so the total cap can't drop them.
            assert names.index("kernel-oom-events.log") < names.index("runtime-run-events.jsonl"), names

            swag = files["openpilot-process-events.log"]
            assert swag["source"] == "openpilot-swaglog" and swag["exists"] is True, swag
            assert swag["content"].startswith("# "), swag
            text = "\n".join(line for line in swag["content"].splitlines() if not line.startswith("# "))
            assert text.index("killing modeld") < text.index("process_not_running") < text.index("is dead with -9"), text
            assert "modeld started" not in text and "latitude" not in text and "should-not-appear" not in text, text
            assert DONGLE not in text and "******abcdef" in text, text
            assert "8.8.8.8" not in text and "Home WiFi" not in text, text

            kernel = files["kernel-oom-events.log"]
            assert kernel["source"] == "kernel-log" and kernel["exists"] is True, kernel
            assert "invoked oom-killer" in kernel["content"] and "Killed process 4242" in kernel["content"], kernel
            assert "associated" not in kernel["content"] and "aa:bb:cc" not in kernel["content"], kernel

            manager = files["openpilot-manager-state.json"]
            assert manager["source"] == "openpilot-manager-state", manager
            state = json.loads(manager["content"])
            if queue_tool:
                assert manager["exists"] is True and state["available"] is True, state
                stuck = {p["name"]: p for p in state["shouldRunButNotRunning"]}
                assert "modeld" in stuck and stuck["modeld"]["exitCode"] == -9 and stuck["modeld"]["exitSignal"] == 9, state
                assert "controlsd" not in stuck, state
            else:
                assert manager["exists"] is False and state["available"] is False, state

            events = files["process-events.jsonl"]
            if queue_tool:
                lines = [json.loads(line) for line in events["content"].splitlines()]
                kinds = [(line["event"], line.get("proc")) for line in lines]
                assert kinds == [("watch-start", None), ("down-first-seen", "modeld"), ("down-first-seen", "camerad"),
                                 ("manager-silent", None)], kinds
                assert lines[1]["exitCode"] == -9 and lines[1]["exitSignal"] == 9, lines
                assert lines[1]["down"] == ["modeld", "camerad"], lines
                assert lines[1]["onroad"] is False and lines[1]["phase"] == "offroad", lines
            previous = files["process-events.jsonl.1"]
            assert previous["exists"] is True and "Killed process 77 (micd)" in previous["content"], previous
            assert "8.8.8.8" not in previous["content"] and "******abcdef" in previous["content"], previous
            assert names.index("process-events.jsonl") < names.index("runtime-run-events.jsonl"), names

            # Nothing private in the whole response, whatever file it came from.
            raw = json.dumps(body)
            assert DONGLE not in raw and "test-token-1234567890" not in raw
        finally:
            stop(process)

        # openpilot not running (no queue, no swaglog dir, no kernel log): still a bundle, saying so.
        for path in swaglog.iterdir():
            path.unlink()
        swaglog.rmdir()
        kernel_log.unlink()
        (msgq / "msgq_managerState").unlink(missing_ok=True)
        port = free_port()
        process = start_control(binary, dict(env, COMMAVIEWD_API_TOKEN="test-token-1234567890"), port)
        try:
            status, body = request(port, "/commaview/support/logs", token="test-token-1234567890")
            assert status == 200, body
            files = {entry["name"]: entry for entry in body["files"]}
            assert files["openpilot-process-events.log"]["exists"] is False
            assert "unavailable" in files["openpilot-process-events.log"]["content"]
            assert files["kernel-oom-events.log"]["exists"] is False
            assert "unavailable" in files["kernel-oom-events.log"]["content"]
            assert json.loads(files["openpilot-manager-state.json"]["content"])["available"] is False
        finally:
            stop(process)

    print("PASS: support logs API gathers openpilot and kernel diagnostics on request, redacted")


if __name__ == "__main__":
    main()
