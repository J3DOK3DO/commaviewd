# Runtime maintenance and UI export repair

[Documentation](README.md) · [Installation](installation.md)

This guide explains how installation, removal, repair, and reboot interact with openpilot.
It is intended for maintainers and people diagnosing a queued operation.

## When maintenance can run

The runtime distinguishes three phases:

| Phase | Meaning | Installation, removal, or repair |
| --- | --- | --- |
| `offroad` | Openpilot reports `IsOffroad=1`. | Allowed. |
| `parked` | Onroad, in Park, at a standstill, and openpilot not engaged; or a recognized no-car bench session. | Allowed. |
| `driving` | Driving, engaged, or insufficient evidence to establish a safe maintenance state. | Refused or deferred. |

Sunnypilot's MADS steering can remain enabled while the parked check passes; the gate uses
selfdrive engagement. A runtime without `road-phase` treats onroad as driving.

For a no-car bench session, `road-phase` requires at least 30 seconds onroad, no car state
from this session, no engagement, and no fresh GPS reading showing movement. A car state
from this session, even if now stale, means the normal car checks apply. Early startup
returns `driving car-state-missing`.

The binary reads the relevant queues without creating a subscriber or taking a msgq reader
slot. It prints a phase and reason, for example `parked no-car`, and exits 0 for offroad or
parked and 1 for driving:

```bash
ssh comma@<comma-ip> '/data/commaview/commaviewd road-phase'
```

## Deferred maintenance

`comma/scripts/run_when_offroad.sh` keeps one job under `/data/commaview-deferred`. A new job
replaces the previous one. `start.sh` resumes the waiter after reboot. A job runs only when
openpilot's manager is up and the device has been offroad for 60 consecutive seconds or
parked for 20 consecutive seconds. If the job finds driving at execution time (exit 42),
it waits again.

| Queue command | Purpose |
| --- | --- |
| `queue ACTION [--file PATH]... -- COMMAND...` | Save a job and any files it needs. |
| `resume` | Start/resume the waiter. |
| `status` | Show the queued job/result. |
| `cancel` | Cancel pending maintenance. |

The control API exposes the queue as `deferredMaintenance` in `/commaview/status`. Runtime
update/removal requests return **202** when queued. The installer's `--force-offroad` means
defer until permitted; it never sets `OffroadMode` or asks openpilot to become offroad.

### Which release decides whether an update is allowed

An install is judged by the release **being installed**. If the device appears to be driving,
the installer can download, verify, and unpack that release at low priority before asking
its staged `road-phase` binary. Nothing installed is changed during staging.

If an install must wait, the queue keeps the release's binary, libraries, and tag under
`/data/commaview-deferred/road-phase`. An app-queued update without this kit first invokes
`install.sh --stage-only` under the maintenance lock. That step exits 75 and installs
nothing. The waiter then uses the retained release's phase rules. A failed staging attempt
is retried after 600 seconds by default.

The kit is used only for an install of its matching tag and is removed when the install
succeeds or the queue is cancelled. Removal and repair do not stage a release. Older
installed queue implementations may still use their own phase rules; see
[bench troubleshooting](troubleshooting.md#an-update-stays-queued-on-a-bench-device).

Queue path/timing overrides for tests are `COMMAVIEWD_DEFERRED_DIR`,
`COMMAVIEWD_PARAMS_DIR`, `COMMAVIEWD_PROC_ROOT`, `COMMAVIEWD_DEFERRED_POLL_SEC` (10),
`COMMAVIEWD_DEFERRED_OFFROAD_STABLE_SEC` (60), `COMMAVIEWD_DEFERRED_PARKED_STABLE_SEC` (20),
`COMMAVIEWD_ROAD_PHASE_BIN`, `COMMAVIEWD_ROAD_PHASE_KIT`, and
`COMMAVIEWD_DEFERRED_STAGE_RETRY_SEC` (600).

## The boot hook and required reboots

Installation adds `/data/commaview/start.sh --before-openpilot` to `/data/continue.sh`
immediately before `exec ./launch_openpilot.sh`. It replaces the older background
`/data/commaview/start.sh &` hook.

The hook prepares the UI export patch synchronously, then starts the runtime in the
background and returns so openpilot can launch. Preparation is bounded by
`COMMAVIEWD_ONROAD_UI_EXPORT_PREPARE_TIMEOUT_SEC` (60 seconds by default). If an upstream
update is staged, preparation targets `/data/safe_staging/finalized`, the tree about to run.

Patching files while openpilot is already running does not reload its UI. Some upstream
managers do not restart an exited UI at all; others fork from modules already imported by
manager. Neither path reliably loads the edited files. CommaView therefore never signals
the UI or manager to make the patch take effect.

Patch status reports `uiReloadPending: true`, `uiReloadAction: "reboot"`, and a
`uiReloadReason` until a reboot or a manager started after patching has loaded the new code.
These fields reach the app through `onroadUiExport` and the patch status/repair endpoints.

The app's authenticated reboot request sets openpilot's own `DoReboot` parameter, as the
device's Reboot button does. It returns 202 when accepted, or 409 while driving.

## Repairing the UI export patch

The transformer installs `comma/src/commaview_export.<flavor>.py` into the upstream UI
and adds the hooks that call it. This changes upstream files, so automatic repair is
conservative.

- A verified patch requires no changes; ordinary apply exits successfully.
- If target files have local modifications, ordinary apply refuses with exit 44.
- Apply backs up targets and restores them if transformation or verification fails.
- `--force-repair` explicitly backs up targets under
  `/data/commaview/backups/onroad-ui-export/<timestamp>`, then resets/reapplies the managed
  patch. The installer uses this path during installation.
- Unsupported comma 3 devices are refused with exit 43 by apply and verify.

Inspect first, using SSH or the app's patch status:

```bash
ssh comma@<comma-ip> 'bash /data/commaview/scripts/verify_onroad_ui_export_patch.sh --json'
```

Use the app's safe repair flow to queue work when needed. Passing `--force-offroad` directly
to the apply/revert helpers is accepted for older callers but does not queue work or bypass
the gate. The queue belongs to the surrounding app/install/uninstall flow.

The helper command and override reference is in
[Runtime reference](runtime-reference.md#ui-export-patch-helpers).
