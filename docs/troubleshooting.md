# Troubleshooting

[Documentation](README.md) · [Installation](installation.md)

Start with the symptom below. Keep the app version, runtime version, device model, upstream
fork/version, and approximate time of the problem; these make a support report much easier
to interpret.

## The app cannot find or connect to the comma

1. Confirm both devices can reach each other. A guest Wi-Fi network or client isolation can
   prevent discovery even when both devices have internet access.
2. Try the comma's current IP address if automatic discovery fails. Discovery uses UDP 5004;
   the control API uses TCP 5002. Camera and telemetry streams use TCP 8200–8203.
3. Check that guided setup completed and the app is paired with the device.
4. With SSH access, check the local control API:

   ```bash
   ssh comma@<comma-ip> 'curl -fsS http://127.0.0.1:5002/commaview/status'
   ```

If this succeeds but the app cannot connect, investigate network reachability and pairing.
If it fails, inspect the runtime logs listed below. See the [port reference](runtime-reference.md#network-ports).

## Live view keeps reconnecting

The app reaches the comma but no video arrives. Check these in the support report's
`commaviewd-bridge.log` and `runtime-run-events.jsonl`:

| What the logs show | Meaning |
| --- | --- |
| `client rejected: <ip> (the camera is streaming to <other ip>)` | Another device has that camera open. Close live view there. |
| `client connected` and then nothing for that client | No camera video: openpilot is offroad, or encoderd isn't running. |
| `peer disconnect phase=video_chunk_send` | The network can't carry the video; look at `videoSend` in the status statistics. |

A comma 3X streams 1928×1208 video at about 10 Mbit/s per camera, twice a comma four's, so a
weak Wi-Fi link that carries a comma four can fall short with a 3X.

## Video works but overlays are missing

Check `/commaview/status` in this order:

| Field | What to look for |
| --- | --- |
| `onroadUiExport` | Whether the patch is verified and whether `uiReloadPending` is true. |
| `runtimeStats.uiExportSocket.connected` | Whether the upstream UI exporter has connected to the bridge. |
| `runtimeStats.services.<service>.emittedCount` | Whether telemetry is being forwarded while an app is connected. |
| `effectiveConfig` and `warnings` | A service disabled by configuration, an invalid configuration, or fallback settings. |

If `uiReloadPending` is true, the patched files take effect after a reboot. CommaView leaves
the running openpilot UI alone; restarting only commaviewd does not reload that UI.

If the patch is missing or incompatible, use the app's repair flow while parked or offroad.
For modified upstream files, review the [repair rules](maintenance.md#repairing-the-ui-export-patch)
before choosing a forced repair. If counters grow but overlays remain absent, include the
app's diagnostics too: the receiving app may be failing to decode or display the payload.

The [telemetry overview](../commaviewd/docs/ai/telemetry-raw-only-readme.md) lists startup
markers, and the [wire contract](../commaviewd/docs/ai/telemetry-raw-only-deep-dive.md) describes
service indexes and forwarding policy.

## An update or repair is waiting

Look at `roadPhase`, `roadPhaseReason`, and `deferredMaintenance` in the status response.
`roadState` can still be `onroad` while `roadPhase` is `parked`; maintenance uses the latter
distinction. A queued operation waits for 20 seconds of parked readings or 60 seconds of
offroad readings by default, with openpilot's manager running.

For an SSH view of the queue:

```bash
ssh comma@<comma-ip> 'bash /data/commaview/scripts/run_when_offroad.sh status'
```

Exit **75** from the installer means deferred work, not a completed update. A **202** response
to an app update request also means accepted/queued. Check the finished job's result and
runtime version. If staging failed, include the deferred maintenance log in your report.

## An update stays queued on a bench device

A powered comma can be onroad without a car. Current maintenance logic recognizes this
as `parked no-car` after at least 30 seconds onroad, when there has been no car state from
this session, no engagement, and no fresh GPS reading showing movement.

An installed runtime from **v0.0.56-alpha.11 or earlier** can gate its own app update using
older rules. Run the [hosted SSH install for the desired tag](installation.md#install-a-specific-release),
which uses that release's maintenance logic, or wait until openpilot is offroad. Do not
alter the device's onroad parameters to bypass the maintenance gate.

## A recording will not start or render

A recording needs the comma's source video and a usable UI metadata timeline. The runtime
reports the current drive's recording status to the app, and refuses an archive known to
have damaged metadata with **409**. A route change during initialization can also invalidate
a preflight response; a route identifier alone is not evidence that metadata is healthy.
Runtime v0.0.58-alpha.1 and earlier could mark a finished drive damaged when the runtime
restarted while parked (for example, after an update) and the car was started again. That
drive keeps returning 409. Later drives are not affected.

Collect diagnostics soon after a failed button press and include its approximate time. The
report should let a maintainer compare recording counters/faults, exporter connectivity,
runtime restarts, and the app's error. Current counters describe the current drive/run;
persistent logs provide evidence across restarts, subject to rotation.

Processing a replay does **not** remove the comma's original segments. Openpilot's own
storage cleanup can remove them later. A saved recording journal or thumbnail does not keep
its source drive alive. If a recording was made on another paired device, it can be replayed
only while its source segments and metadata remain available.

See [recording behavior](runtime-reference.md#recordings-and-source-retention) for the storage
model, integrity fields, and retention estimate.

## Collect a support report

Use the app's support export/share flow while connected to the comma. The runtime part is
collected on request through authenticated `GET /commaview/support/logs`. It includes bounded
runtime log tails, telemetry/configuration snapshots, available upstream process events,
and memory-pressure evidence. It redacts credentials and identifying/location details;
private recording records and leaderboard key files are excluded.

The log-content budget is 512 KiB per file and 2 MiB total, before JSON encoding. Entries
report whether they exist or were truncated. Missing or truncated evidence should be treated
as a limit of the report, not proof that no error occurred.

When sharing a report, include:

- The action that failed and the exact visible error.
- App/runtime versions, comma model, and upstream fork/version.
- The approximate time, whether the comma had restarted, and whether the car was driving,
  parked, or offroad.
- Whether the problem repeats and whether another paired device behaves differently.

## Inspect logs over SSH

These commands read local evidence; they do not restart or repair anything:

```bash
ssh comma@<comma-ip> 'tail -n 100 /data/commaview/logs/commaviewd-bridge.log'
ssh comma@<comma-ip> 'tail -n 100 /data/commaview/logs/commaviewd-control.log'
ssh comma@<comma-ip> 'tail -n 100 /data/commaview/logs/onroad-ui-export-startup.log'
ssh comma@<comma-ip> 'tail -n 100 /data/commaview/logs/runtime-run-events.jsonl'
```

Logs under `/data/commaview/logs/` survive a restart but rotate; they are not retained forever.
Use the app's redacted export when sharing evidence instead of posting raw local logs.
See [runtime logging](runtime-reference.md#logging-and-process-supervision) for retention and
supervision defaults.
