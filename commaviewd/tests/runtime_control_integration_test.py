#!/usr/bin/env python3
"""Control API, end to end, for the things that must never disturb a drive:

- runtime-debug apply reloads the running bridge's config in place (SIGHUP): same process, no
  restart, the new policy in its effective config;
- /commaview/status reports roadPhase offroad / parked / driving from carState and selfdriveState
  read without subscribing, next to the unchanged roadState;
- a Safe Repair asked for while onroad is queued until openpilot is offroad (202, deferred), never
  forced, and the queue shows in /commaview/status;
- a reboot is asked of openpilot (DoReboot) only offroad or parked, never while driving.
"""

import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER = REPO_ROOT / "comma" / "scripts" / "run_when_offroad.sh"
TOKEN = "test-token"


def request(port, path, method="GET", body=None, token=TOKEN):
    data = json.dumps(body, separators=(",", ":")).encode() if body is not None else (b"" if method == "POST" else None)
    headers = {"X-CommaView-Token": token, "Content-Type": "application/json"}
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=10) as response:
            return response.status, json.load(response)
    except urllib.error.HTTPError as error:
        return error.code, json.load(error)


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_until(predicate, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(0.1)
    return False


def start_control(binary, env, port):
    process = subprocess.Popen([str(binary), "control", "--port", str(port)],
                               env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def up():
        try:
            request(port, "/commaview/version")
            return True
        except (OSError, ValueError):
            return False
    if not wait_until(up):
        process.kill()
        raise SystemExit("control API did not start")
    return process


def main():
    binary = Path(os.environ.get("COMMAVIEWD_HOST_BIN") or REPO_ROOT / "dist/commaviewd-host")
    road_tool = os.environ.get("COMMAVIEWD_ROAD_QUEUE_TOOL")
    if not binary.is_file():
        raise SystemExit("build host runtime first")

    with tempfile.TemporaryDirectory(prefix="commaview-runtime-control-test-") as temp:
        root = Path(temp)
        run, config, params, msgq, deferred = (root / name for name in ("run", "config", "params", "shm", "deferred"))
        for folder in (run, config, params, msgq, root / "data"):
            folder.mkdir()
        (params / "IsOffroad").write_text("1")
        env = dict(os.environ)
        env.update(
            COMMAVIEWD_API_TOKEN=TOKEN,
            COMMAVIEWD_RUNTIME_DEBUG_CONFIG=str(config / "runtime-debug.json"),
            COMMAVIEWD_RUNTIME_DEBUG_DEFAULTS=str(REPO_ROOT / "comma" / "runtime-debug.defaults.json"),
            COMMAVIEWD_RUNTIME_DEBUG_EFFECTIVE=str(run / "runtime-debug-effective.json"),
            COMMAVIEWD_RUNTIME_STATS=str(run / "telemetry-stats.json"),
            COMMAVIEWD_TEST_PARAMS_DIR=str(params),
            COMMAVIEWD_TEST_MSGQ_DIR=str(msgq),
            COMMAVIEWD_TEST_DATA_ROOT=str(root / "data"),
            COMMAVIEWD_DEFERRED_DIR=str(deferred),
            COMMAVIEWD_TEST_DEFERRED_RUNNER=str(RUNNER),
            COMMAVIEWD_DEFERRED_REQUIRE_MANAGER="0",
            COMMAVIEWD_PARAMS_DIR=str(params),
            COMMAVIEWD_TEST_INSTALL_SCRIPT=str(root / "install.sh"),
            COMMAVIEWD_TEST_UNINSTALL_SCRIPT=str(root / "uninstall.sh"),
            COMMAVIEWD_ROAD_PHASE_BIN=str(root / "no-commaviewd"),
            COMMAVIEWD_UI_EXPORT_SOCKET=str(run / "ui-export.sock"),
            COMMAVIEWD_RECIPE_DIR=str(root / "recipes"),
        )
        (root / "install.sh").write_text("#!/usr/bin/env bash\nexit 0\n")
        (root / "uninstall.sh").write_text("#!/usr/bin/env bash\necho uninstalling\n")

        def road_phase_cli():
            result = subprocess.run([str(binary), "road-phase"], env=env, capture_output=True, text=True, timeout=10)
            return result.returncode, result.stdout.strip()

        bridge = subprocess.Popen([str(binary), "bridge"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        (run / "bridge.pid").write_text(f"{bridge.pid}\n")
        port = free_port()
        control = start_control(binary, env, port)
        try:
            stats_path = run / "telemetry-stats.json"
            assert wait_until(lambda: stats_path.exists()), "bridge wrote no stats"

            # --- runtime-debug apply: in place, no restart -------------------------------------
            code, body = request(port, "/commaview/runtime-debug/config")
            assert code == 200, body
            config_body = {"configVersion": 1, "instrumentationLevel": "standard",
                           "services": {"carState": {"mode": "sample", "sampleHz": 2}}}
            code, body = request(port, "/commaview/runtime-debug/config", "POST", config_body)
            assert code == 200 and body["ok"], body
            code, body = request(port, "/commaview/runtime-debug/apply", "POST", {})
            assert code == 200, body
            assert body["ok"] and body["applied"] and body["appliedLive"], body
            assert body["restartScheduled"] is False and body["appliesWhenParked"] == [], body
            assert bridge.poll() is None, "the bridge must not restart"
            effective = json.loads((run / "runtime-debug-effective.json").read_text())
            assert effective["services"]["carState"]["mode"] == "sample", effective
            stats = json.loads(stats_path.read_text())
            assert stats["configReloads"] == 1, stats
            assert stats["videoRing"]["msgqReaderSlots"] == 0, stats
            print("PASS: runtime-debug apply reloads the running bridge in place (same pid, no restart)")

            # A pid file naming something that isn't our bridge is never signalled.
            sleeper = subprocess.Popen(["sleep", "30"])
            (run / "bridge.pid").write_text(f"{sleeper.pid}\n")
            code, body = request(port, "/commaview/runtime-debug/apply", "POST", {})
            assert code == 200 and body["ok"] and not body["applied"] and body["appliesOnNextStart"], body
            assert sleeper.poll() is None, "an unrelated process was signalled"
            sleeper.kill()
            (run / "bridge.pid").write_text(f"{bridge.pid}\n")
            print("PASS: runtime-debug apply signals only a pid that really is the bridge")

            # --- roadPhase --------------------------------------------------------------------
            code, status = request(port, "/commaview/status")
            assert status["roadState"] == "offroad" and status["roadPhase"] == "offroad", status
            assert status["deferredMaintenance"] == {"state": "none"}, status
            assert {"runtime-update", "runtime-uninstall", "reboot"} <= set(status["capabilities"]), status
            assert road_phase_cli() == (0, "offroad offroad")
            (params / "IsOffroad").write_text("0")
            code, status = request(port, "/commaview/status")
            assert status["roadState"] == "onroad" and status["roadPhase"] == "driving", status
            assert status["roadPhaseReason"] == "car-state-missing", status
            assert road_phase_cli() == (1, "driving car-state-missing")
            if road_tool:
                def phase(gear, standstill, enabled, age_ms="0"):
                    subprocess.run([road_tool, "--write-road-queues", str(msgq), gear, standstill, enabled, age_ms], check=True)
                    return request(port, "/commaview/status")[1]
                parked = phase("park", "1", "0")
                assert parked["roadPhase"] == "parked" and parked["roadState"] == "onroad", parked
                assert phase("drive", "1", "0")["roadPhase"] == "driving"
                assert phase("unknown", "1", "0")["roadPhaseReason"] == "gear-unknown"
                assert phase("park", "1", "1")["roadPhaseReason"] == "engaged"
                phase("park", "1", "0")
                assert road_phase_cli() == (0, "parked parked")
                # Parked is not driving: a repair runs now (here the helper is missing), not queued.
                code, body = request(port, "/commaview/onroad-ui-export/repair", "POST", {"forceOffroad": True})
                assert code != 202 and body["status"]["state"] != "deferred-until-offroad", (code, body)
                # Parked may reboot, like maintenance.
                code, body = request(port, "/commaview/reboot", "POST", {})
                assert code == 202 and body["rebooting"], (code, body)
                (params / "DoReboot").unlink()
                assert phase("park", "1", "0", "2000")["roadPhaseReason"] == "car-state-stale"
                assert road_phase_cli()[0] == 1
                print("PASS: /commaview/status and commaviewd road-phase report offroad / parked / driving")

                # No car: onroad (a bench supply or harness box turning the ignition on) and card
                # never publishes a carState. Early on that is still driving; once openpilot has
                # been onroad 30 s without one, not engaged and the GPS still, it is parked.
                (msgq / "msgq_carState").unlink()
                (msgq / "msgq_selfdriveState").unlink()
                def started(ago_ms):
                    subprocess.run([road_tool, "--write-device-state", str(msgq), str(ago_ms)], check=True)
                    return request(port, "/commaview/status")[1]
                early = started(10_000)
                assert early["roadPhase"] == "driving" and early["roadPhaseReason"] == "car-state-missing", early
                assert road_phase_cli() == (1, "driving car-state-missing")
                no_car = started(40_000)
                assert no_car["roadPhase"] == "parked" and no_car["roadPhaseReason"] == "no-car", no_car
                assert no_car["roadState"] == "onroad", no_car
                assert road_phase_cli() == (0, "parked no-car")
                subprocess.run([road_tool, "--write-gps", str(msgq), "9.5"], check=True)
                moving = request(port, "/commaview/status")[1]
                assert moving["roadPhase"] == "driving" and moving["roadPhaseReason"] == "moving", moving
                subprocess.run([road_tool, "--write-gps", str(msgq), "0"], check=True)
                assert road_phase_cli() == (0, "parked no-car")
                # A carState this session that stopped is the car: stale, driving (as below).
                assert phase("park", "1", "0", "2000")["roadPhaseReason"] == "car-state-stale"
                assert road_phase_cli()[0] == 1
                print("PASS: onroad with no car at all (no carState) is parked after 30 s, driving before")
            else:
                print("SKIP: roadPhase from queues (set COMMAVIEWD_ROAD_QUEUE_TOOL)")

            # --- Safe Repair while onroad: queued, not forced ---------------------------------
            code, body = request(port, "/commaview/onroad-ui-export/repair", "POST", {"forceOffroad": True})
            assert code == 202, (code, body)
            assert body["ok"] is False and body["deferred"] is True, body
            assert body["status"]["state"] == "deferred-until-offroad", body
            assert body["deferredMaintenance"]["state"] == "waiting" and body["deferredMaintenance"]["action"] == "repair", body
            assert not (params / "OffroadMode").exists(), "nothing may ask openpilot to go offroad"
            code, status = request(port, "/commaview/status")
            assert status["deferredMaintenance"]["state"] == "waiting", status
            code, body = request(port, "/commaview/onroad-ui-export/repair", "POST", {})
            assert body["status"]["state"] == "onroad-blocked", body
            print("PASS: Safe Repair while driving is queued until parked, never forced")

            # --- reboot: openpilot's own DoReboot, never while driving ----------------------
            # Still onroad with the car-state queue stale: driving.
            code, body = request(port, "/commaview/reboot", "POST", {}, token="wrong")
            assert code == 401, (code, body)
            code, body = request(port, "/commaview/reboot", "POST", {})
            assert code == 409 and body["ok"] is False and body["roadPhase"] == "driving", (code, body)
            assert not (params / "DoReboot").exists(), "no reboot while driving"
            (params / "IsOffroad").write_text("1")
            code, body = request(port, "/commaview/reboot", "POST", {})
            assert code == 202 and body["ok"] and body["rebooting"], (code, body)
            assert (params / "DoReboot").read_text() == "1"
            assert not list(params.glob("DoReboot.*")), list(params.iterdir())
            (params / "DoReboot").unlink()
            (params / "IsOffroad").write_text("0")
            print("PASS: /commaview/reboot sets openpilot's DoReboot offroad or parked, never while driving, paired phone only")

            # --- runtime update without SSH ------------------------------------------------
            code, body = request(port, "/commaview/runtime/update", "POST", {"tag": "v0.0.57-alpha.1"}, token="wrong")
            assert code == 401, (code, body)
            for bad in ("", "latest", "v1.2", "v1.2.3;reboot", "v1.2.3-", "../v1.2.3", "v1.2.3-a/b"):
                code, body = request(port, "/commaview/runtime/update", "POST", {"tag": bad})
                assert code == 400, (bad, code, body)
            code, body = request(port, "/commaview/runtime/update", "POST", {"tag": "v0.0.57-alpha.1"})
            assert code == 202 and body["ok"] and body["queued"] and body["tag"] == "v0.0.57-alpha.1", (code, body)
            assert body["deferredMaintenance"]["state"] == "waiting", body
            assert body["deferredMaintenance"]["action"] == "install", body
            args = (deferred / "job" / "args").read_bytes().split(b"\0")
            assert args[:4] == [b"bash", str(deferred / "job" / "install.sh").encode(), b"--tag", b"v0.0.57-alpha.1"], args
            assert (deferred / "job" / "install.sh").read_text() == (root / "install.sh").read_text()
            assert not (params / "OffroadMode").exists()
            print("PASS: /commaview/runtime/update queues the installed installer for the tag, paired phone only")

            # --- runtime uninstall without SSH ---------------------------------------------
            code, body = request(port, "/commaview/runtime/uninstall", "POST", {}, token="wrong")
            assert code == 401, (code, body)
            code, body = request(port, "/commaview/runtime/uninstall", "POST", {})
            assert code == 202 and body["ok"] and body["queued"], (code, body)
            assert body["deferredMaintenance"]["state"] == "waiting", body
            assert body["deferredMaintenance"]["action"] == "uninstall", body
            args = (deferred / "job" / "args").read_bytes().split(b"\0")
            assert args[:2] == [b"bash", str(deferred / "job" / "uninstall.sh").encode()], args
            assert (deferred / "job" / "uninstall.sh").read_text() == (root / "uninstall.sh").read_text()
            print("PASS: /commaview/runtime/uninstall queues the installed uninstaller, paired phone only")

            # --- the stage a queued install has reached ---------------------------------------
            code, status = request(port, "/commaview/status")
            assert not {"stage", "stageDetail", "stageAtMs"} & set(status["deferredMaintenance"]), status
            (deferred / "progress.json").write_text('{"stage":"verifying","detail":"v0.0.57-alpha.1","atMs":1759579200123}\n')
            code, status = request(port, "/commaview/status")
            maintenance = status["deferredMaintenance"]
            assert maintenance["state"] == "waiting" and maintenance["action"] == "uninstall", maintenance
            assert maintenance["stage"] == "verifying" and maintenance["stageDetail"] == "v0.0.57-alpha.1", maintenance
            assert maintenance["stageAtMs"] == 1759579200123, maintenance
            (deferred / "progress.json").write_text('{"stage":"rolled_back","detail":"","atMs":5}\n')
            assert request(port, "/commaview/status")[1]["deferredMaintenance"]["stage"] == "rolled_back"
            (deferred / "progress.json").write_text('{"stage":"no good","atMs":5}\n')
            assert "stage" not in request(port, "/commaview/status")[1]["deferredMaintenance"]
            (deferred / "progress.json").unlink()
            print("PASS: deferredMaintenance carries a queued install's stage, stageDetail and stageAtMs")

            # --- helpers inherit none of control's sockets ---------------------------------------
            fd_report = root / "runner-fds.txt"
            fake_runner = root / "fake-runner.sh"
            fake_runner.write_text(f'#!/usr/bin/env bash\nls -l /proc/$$/fd > "{fd_report}"\necho \'{{"state":"waiting"}}\'\n')
            fake_runner.chmod(0o755)
            fd_port = free_port()
            fd_control = start_control(binary, dict(env, COMMAVIEWD_TEST_DEFERRED_RUNNER=str(fake_runner)), fd_port)
            try:
                code, body = request(fd_port, "/commaview/runtime/uninstall", "POST", {})
                assert code == 202, (code, body)
                links = fd_report.read_text()
                # stdin, stdout and stderr (the pipes control reads), and bash's own script descriptor.
                assert {line.split()[-3] for line in links.splitlines() if "->" in line} <= {"0", "1", "2", "255"}, links
            finally:
                fd_control.terminate()
                fd_control.wait(timeout=5)
            print("PASS: a helper control starts keeps none of its sockets (listener, connection, discovery)")

            # --- one bridge, and it stops promptly on SIGTERM ---------------------------------
            second = subprocess.run([str(binary), "bridge"], env=env, capture_output=True, text=True, timeout=10)
            assert second.returncode == 3 and "another CommaView bridge is running" in second.stderr, second
            assert (run / "ui-export.sock").is_socket(), "a second bridge must not touch the running one's socket"
            stop_started = time.monotonic()
            bridge.send_signal(signal.SIGTERM)
            assert bridge.wait(timeout=3) == 143
            assert time.monotonic() - stop_started < 2
            print("PASS: a second bridge exits untouched, and the bridge stops at once on SIGTERM")
        finally:
            subprocess.run(["bash", str(RUNNER), "cancel"], env=env, check=False, capture_output=True)
            control.terminate()
            control.wait(timeout=5)
            bridge.send_signal(signal.SIGKILL)
            bridge.wait(timeout=5)


if __name__ == "__main__":
    main()
