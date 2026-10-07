#!/usr/bin/env python3
"""Exercise private, bounded source archive reads through the real HTTP server."""

import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


def request(url, token=None):
  headers = {"X-CommaView-Token": token} if token else {}
  try:
    with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=5) as reply:
      return reply.status, reply.read(), dict(reply.headers)
  except urllib.error.HTTPError as failure:
    return failure.code, failure.read(), dict(failure.headers)


def main():
  root = Path(__file__).resolve().parents[2]
  binary = Path(os.environ.get("COMMAVIEWD_HOST_BIN", root / "dist/commaviewd-host"))
  assert binary.is_file(), binary
  with tempfile.TemporaryDirectory(prefix="commaview-archive-api-") as temp:
    temp = Path(temp)
    route = "000004b4--75e1f0ba8f"
    segment = temp / "realdata" / f"{route}--0"
    segment.mkdir(parents=True)
    (segment / "fcamera.hevc").write_bytes(b"0123456789")
    (segment / "ecamera.hevc").write_bytes(b"wide")
    (segment / "rlog.zst").write_bytes(b"log")
    recipes = temp / "recipes"
    recipes.mkdir()
    (recipes / "ui-source-1.jsonl").write_text(
        f'{{"routeId":"{route}","sequence":1}}\n', encoding="ascii")
    (recipes / f"ui-snapshot-{route}.jsonl").write_bytes(b"snapshot\n")
    with socket.socket() as probe:
      probe.bind(("127.0.0.1", 0))
      port = probe.getsockname()[1]
    env = os.environ.copy()
    params = temp / "params"
    params.mkdir()
    msgq = temp / "shm"
    msgq.mkdir()
    (params / "IsOffroad").write_text("1", encoding="ascii")
    env.update(COMMAVIEWD_API_TOKEN="source-test-token",
               COMMAVIEWD_TEST_PARAMS_DIR=str(params),
               COMMAVIEWD_TEST_MSGQ_DIR=str(msgq),
               COMMAVIEWD_SOURCE_ARCHIVE_ROOT=str(temp / "realdata"),
               COMMAVIEWD_RECIPE_DIR=str(recipes))
    process = subprocess.Popen([str(binary), "control", "--port", str(port)], env=env,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
      base = f"http://127.0.0.1:{port}"
      for _ in range(50):
        try:
          if request(base + "/commaview/version")[0] == 200:
            break
        except (OSError, TimeoutError):
          time.sleep(0.1)
      else:
        raise AssertionError("control server did not start")
      media = (base + "/commaview/source-recording/range?route=" + route +
               "&segment=0&kind=road&offset=2&length=4")
      assert request(media)[0] == 401
      assert request(media, "wrong-token")[0] == 401
      status, body, headers = request(media, "source-test-token")
      assert (status, body, headers["X-Source-Size"]) == (200, b"2345", "10")
      recipe = base + "/commaview/source-recording/recipe?route=" + route
      status, body, _ = request(recipe, "source-test-token")
      assert status == 200 and b'"sequence":1' in body
      manifest = base + "/commaview/source-recording/manifest?route=" + route
      assert request(manifest)[0] == 401
      status, body, _ = request(manifest, "source-test-token")
      assert status == 200 and json.loads(body) == {
          "routeId": route, "segments": [0], "incompleteSegments": []}, (status, body)
      (segment / "ecamera.hevc").unlink()
      status, body, _ = request(manifest, "source-test-token")
      assert status == 200 and json.loads(body) == {
          "routeId": route, "segments": [],
          "incompleteSegments": [{"segment": 0, "missing": ["wide"]}]}, (status, body)
      (segment / "ecamera.hevc").write_bytes(b"wide")
      snapshot = (base + "/commaview/source-recording/range?route=" + route +
                  "&segment=0&kind=snapshot&offset=0&length=8")
      assert request(snapshot)[0] == 401
      assert request(snapshot, "source-test-token")[:2] == (200, b"snapshot")
      assert request(media.replace("length=4", "length=262145"), "source-test-token")[0] == 400
      marker = recipes / f"ui-invalid-{route}"
      marker.write_text("recording metadata incomplete\n", encoding="ascii")
      for endpoint in (media, snapshot, recipe, manifest):
        assert request(endpoint)[0] == 401
        status, body, _ = request(endpoint, "source-test-token")
        assert status == 409 and json.loads(body)["error"] == "recording metadata damaged", (status, body)
      assert (segment / "fcamera.hevc").read_bytes() == b"0123456789"
      assert (segment / "ecamera.hevc").read_bytes() == b"wide"
      assert (segment / "rlog.zst").read_bytes() == b"log"
      marker.unlink()
      print("PASS: authenticated archive HTTP, incomplete manifests, durable damage refusal and preserved originals")

      # Onroad, a finished drive is served only while parked: never while the car is driven.
      current = base + "/commaview/source-recording/current"
      assert request(current)[0] == 401
      assert request(current, "source-test-token")[0] == 403
      (params / "IsOffroad").write_text("0", encoding="ascii")
      status, body, headers = request(current, "source-test-token")
      startup = json.loads(body)
      assert status == 404 and startup["error"] == "route unavailable", (status, body)
      assert "recordingStatus" in startup and startup["routeChanged"] is False
      (params / "CurrentRoute").write_text(route, encoding="ascii")
      status, body, headers = request(current, "source-test-token")
      ready_route = json.loads(body)
      assert status == 200 and ready_route["routeId"] == route, (status, body)
      assert ready_route["recordingStatus"]["roadState"] == "onroad"
      assert ready_route["routeChanged"] is False and headers["Cache-Control"] == "no-store"
      print("PASS: current-route HTTP startup evidence, onroad guard and embedded status")
      status, body, _ = request(manifest, "source-test-token")
      assert status == 403 and b"parked or offroad required" in body, (status, body)
      road_tool = os.environ.get("COMMAVIEWD_ROAD_QUEUE_TOOL")
      if road_tool:
        def phase(gear, standstill, enabled):
          subprocess.run([road_tool, "--write-road-queues", str(msgq), gear, standstill, enabled, "0"], check=True)
        phase("park", "1", "0")
        assert request(manifest, "source-test-token")[0] == 200
        assert request(media, "source-test-token")[:2] == (200, b"2345")
        phase("drive", "0", "1")
        assert request(manifest, "source-test-token")[0] == 403
        print("PASS: source archive served offroad and parked, refused while driving")
      else:
        print("SKIP: parked source archive (set COMMAVIEWD_ROAD_QUEUE_TOOL)")
    finally:
      process.terminate()
      process.wait(timeout=5)


if __name__ == "__main__":
  main()
