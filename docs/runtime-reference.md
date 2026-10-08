# Runtime reference

[Documentation](README.md) · [Maintenance](maintenance.md) · [Troubleshooting](troubleshooting.md)

This page covers the installed runtime's commands, configuration, and storage behavior.
Paths refer to the comma device unless stated otherwise. API behavior below describes the
current source; check `/commaview/status` capabilities before using features on older releases.

## Processes and telemetry

`start.sh` launches a streaming **bridge** and an HTTP **control** process. The bridge reads
existing encoded camera queues without a msgq subscriber/reader slot. The UI exporter runs
inside the upstream UI, prepares service JSON, and sends it over a Unix socket. The bridge
keeps the latest service payloads and forwards them unchanged to the app in raw frames.
The app decodes those payloads for its overlays.

See the [telemetry overview](../commaviewd/docs/ai/telemetry-raw-only-readme.md) and
[wire contract](../commaviewd/docs/ai/telemetry-raw-only-deep-dive.md) for service indexes,
framing, freshness, and diagnostic markers.

## Runtime CLI

| Command | Purpose |
| --- | --- |
| `commaviewd bridge` | Start video and telemetry streaming. Configuration comes from JSON/environment, with no current bridge CLI flags. |
| `commaviewd control [--port <port>]` | Start the HTTP control API; default port 5002. |
| `commaviewd road-phase` | Print `offroad`, `parked`, or `driving` and its reason; exit 0 for maintenance allowed, 1 for driving. |
| `commaviewd --help`, `-h`, or `help` | Print mode usage. |

## Network ports

| Port | Transport | Purpose |
| --- | --- | --- |
| `5002` | TCP / HTTP | Control API. |
| `5004` | UDP | Local discovery (`COMMAVIEW_DISCOVER_V1`). |
| `8200` | TCP | Road camera. |
| `8201` | TCP | Wide road camera. |
| `8202` | TCP | Driver camera. |
| `8203` | TCP | Telemetry. |

Each camera port streams to one client at a time. A client reconnecting from the address that
holds a camera replaces its earlier connection; a client from another address is turned away
(`client_rejected` in `runtime-run-events.jsonl`) until the camera is free. The bridge sets TCP
keepalive and `TCP_USER_TIMEOUT` on every stream connection, so a client that vanished without
closing it (out of Wi-Fi range, powered off, moved to another network) frees its camera within
about ten seconds.

The discovery response, `/commaview/version`, and `/commaview/status` include `deviceType`
(`tizi`, `mici`, `tici`, or empty if unknown). The app uses it to distinguish comma hardware.

## Common control API endpoints

Requests use `http://<comma-ip>:5002`. Endpoints marked **paired** require the installed
bearer token; the app manages pairing. The table lists common operations, not every route.

| Method and path | Access | Purpose |
| --- | --- | --- |
| `GET /commaview/version` | Read-only | Runtime/device identity. |
| `GET /commaview/status` | Read-only | Phase, maintenance, patch/config state, statistics, and capabilities. |
| `GET /commaview/onroad-ui-export/status` | Read-only | Patch verification and pending UI reload. |
| `GET /commaview/runtime-debug/config` | Read-only | Persisted/effective telemetry configuration. |
| `GET /commaview/support/logs` | Paired | Bounded, redacted support evidence collected on demand. |
| `POST /commaview/runtime/update` | Paired | Queue installation; JSON body `{"tag":"vX.Y.Z"}`; accepted with 202. |
| `POST /commaview/runtime/uninstall` | Paired | Queue removal; accepted with 202. |
| `POST /commaview/reboot` | Paired | Request openpilot-managed reboot while parked/offroad; 202 or 409 while driving. |
| `POST /commaview/onroad-ui-export/repair` | Paired | Repair through the maintenance flow. |
| `POST /commaview/runtime-debug/config` | Paired | Persist a validated configuration. |
| `POST /commaview/runtime-debug/defaults` | Paired | Restore default configuration. |
| `POST /commaview/runtime-debug/apply` | Paired | Apply configuration to the bridge in place. |
| `GET /commaview/drive-stats` | Paired | Cached drive statistics. |
| `GET /commaview/location`, `POST /commaview/location` | Paired | Read/configure opt-in device location. |

The current capability list includes `runtime-update`, `runtime-uninstall`, `reboot`,
`recording-records`, and `drive-retention`. A queued action must be tracked to completion
through status/version; its 202 response is not a completion signal.

## Runtime environment

These defaults are used by the installed scripts or runtime:

| Variable | Default | Purpose |
| --- | --- | --- |
| `COMMAVIEWD_RUNTIME_DEBUG_DEFAULTS` | `/data/commaview/runtime-debug.defaults.json` | Seed file copied by `start.sh` when config is missing/empty; the binary does not read this seed. |
| `COMMAVIEWD_RUNTIME_DEBUG_CONFIG` | `/data/commaview/config/runtime-debug.json` | Persisted editable configuration. |
| `COMMAVIEWD_RUNTIME_DEBUG_EFFECTIVE` | `/data/commaview/run/runtime-debug-effective.json` | Effective configuration output. |
| `COMMAVIEWD_RUNTIME_STATS` | `/data/commaview/run/telemetry-stats.json` | Telemetry statistics output. |
| `COMMAVIEWD_RESTART_REASON` | `startup` | Reason recorded in status/logs. |
| `COMMAVIEWD_API_TOKEN` | Unset | Direct bearer token override. |
| `COMMAVIEWD_API_TOKEN_FILE` | `/data/commaview/api/auth.token` when launched by `start.sh` | Control API token file. |
| `COMMAVIEWD_UI_EXPORT_SOCKET` | `/data/commaview/run/ui-export.sock` | Exporter/bridge Unix socket. |
| `COMMAVIEWD_RECIPE_DIR` | `/data/commaview/recording-recipes` | Recording metadata recipes/snapshots. |
| `COMMAVIEWD_SOURCE_ARCHIVE_ROOT` | `/data/media/0/realdata` | Source route segment archive. |
| `COMMAVIEWD_CURRENT_ROUTE_FILE` | `/data/params/d/CurrentRoute` | Drive identifier for recordings. |
| `COMMAVIEW_VIDEO_SOURCE` | `full` | Camera source; see below. |

Per-service policies are `pass`, `sample` at `sampleHz`, or `off`. The checked-in
[defaults](../comma/runtime-debug.defaults.json) pass all listed services with standard
instrumentation. Persisted and effective settings can differ until configuration is applied;
status reports warnings and safe fallback use.

### Experimental video source

The normal `full` source uses HEVC encoded services. `COMMAVIEW_VIDEO_SOURCE=livestream`
selects the stock H.264 livestream services on the same camera ports. It does not start
`encoderd --stream` or change the archival encoder. It requires an H.264-capable client
and a separately guarded stock livestream encoder; the current Android HEVC preview does
not support this mode. Unknown source values fail startup.

## Lifecycle commands

| Installed command | Behavior |
| --- | --- |
| `bash /data/commaview/start.sh [--before-openpilot]` | Prepare/verify the export patch when permitted, stop stale runtime processes, and launch bridge/control. The boot flag prepares the patch before openpilot launches. |
| `bash /data/commaview/stop.sh` | Stop tracked processes and stray installed runtime processes. No flags. |
| `bash /data/commaview/uninstall.sh [--force-offroad]` | Restore the UI files, then remove the runtime, boot hook, and queue. Refuse/defer while driving. |

See [Maintenance](maintenance.md) for the queue, boot ordering, and upstream UI reload rules.

## UI export patch helpers

Installed helpers live in `/data/commaview/scripts/`. Run them with `bash`.

| Helper | Options and behavior |
| --- | --- |
| `verify_onroad_ui_export_patch.sh` | `[--json] [--platform auto\|mici\|tizi]`; always prints JSON, including UI reload state. Detecting/explicitly selecting unsupported `tici` returns 43. |
| `apply_onroad_ui_export_patch.sh` | `[--force-offroad] [--force-repair] [--before-openpilot] [--platform auto\|mici\|tizi]`; ordinary apply preserves local modifications (44), and driving is refused (42). `--force-repair` backs up/reset/reapplies. |
| `revert_onroad_ui_export_patch.sh` | `[--force-offroad] [--preflight-only]`; restore upstream targets or check restoration without changing them. Driving is refused (42). |

Apply/revert accept `--force-offroad` for compatibility but do not defer or bypass the gate.
`--before-openpilot` skips stale onroad parameters only when no manager/UI is running and
selects the finalized staging tree when needed. When openpilot is running, the gate remains.

Path overrides are `COMMAVIEWD_INSTALL_DIR` (`/data/commaview`), `COMMAVIEWD_OP_ROOT`
(`/data/openpilot`), `COMMAVIEWD_PARAMS_DIR` (`/data/params/d`), `COMMAVIEWD_PROC_ROOT`
(`/proc`), `COMMAVIEWD_DEVICE_MODEL_FILE`, and `COMMAVIEWD_STAGING_ROOT`
(`/data/safe_staging`). `COMMAVIEWD_SKIP_OPENPILOT_UI_RESTART=1` skips UI reload bookkeeping;
it does not make the running upstream UI reload.

## Installer options and overrides

The public installer shim dispatches to the requested release's own installer. A release
normally fetches its matching scripts; using a moving source ref is a development override.

| Option | Purpose |
| --- | --- |
| `--tag <release-tag>` | Install a specific published tag. |
| `--current` | Reinstall the release in the installed `version.env`. |
| `--force-offroad` | Queue maintenance while driving; exit 75 instead of refusing with 42. |
| `--stage-only` | Internal queue step: download/verify/unpack and retain `road-phase`; install nothing, exit 75. |
| `-h`, `--help` | Print usage. |

| Variable | Purpose |
| --- | --- |
| `COMMAVIEWD_RELEASE_REPO` | Release repository; default `CommaView/commaviewd` (`RhynoTech/commaviewd`, its previous name, works the same). |
| `COMMAVIEWD_RELEASE_TAG` | Explicit release tag. |
| `COMMAVIEWD_DEFAULT_TAG` | Default tag before network lookup. |
| `COMMAVIEWD_CURRENT_RELEASE_URL` | Current-release lookup; default `https://commaview.com/api/current-release` for this release repository. Other repositories skip it unless explicitly set. |
| `COMMAVIEWD_RELEASES_API_URL` | GitHub releases API used when the current-release lookup fails: the newest beta by version, never an alpha (another repository: its newest release). With neither, the install stops and changes nothing. The shim reads it too. |
| `COMMAVIEWD_RELEASES_ORIGIN` | Hosted release origin; default `https://commaview.com/commaviewd`. |
| `COMMAVIEWD_INSTALLER_REF` | Companion script ref; default resolved release tag. |
| `COMMAVIEWD_ASSET_NAME` | Bundle filename override. |
| `COMMAVIEWD_BASE_URL` | Bundle download base URL override. |
| `COMMAVIEWD_INSTALLER_RAW_BASE` | Companion file base URL override. |
| `COMMAVIEWD_VERSION` | Displayed/stamped version override. |

## Recordings and source retention

The comma keeps the original drive segments. CommaView adds two kinds of metadata:

| Data | Location | Purpose |
| --- | --- | --- |
| Source drive segments | `/data/media/0/realdata` | The comma's own video/log archive, managed by openpilot. |
| UI recipes, snapshots, and damage markers | `/data/commaview/recording-recipes` | Overlay timeline and integrity evidence needed for replay; removed with the drive's segments. |
| Recording journals and thumbnails | `/data/commaview/data/recordings` | Small records shared among the comma's paired devices, kept across runtime updates. |

The app saves a journal (which drive, interval, and look) plus a thumbnail; a journal is not
a rendered video or a copy of the source segments. Another paired device can make the replay
while the required source archive and metadata remain available.

### UI recipes and metadata retention

Each runtime process writes one recipe file per drive:
`ui-source-<route>.<process start ms>-<pid>.jsonl`. Runtime v0.0.58-alpha.1 and earlier wrote one
`ui-source-<start ms>-<pid>.jsonl` per process for every drive.
`GET /commaview/source-recording/recipe` returns the drive's own files, plus the lines for that
drive in those older files, in the order the processes ran, so a drive recorded across runtime
restarts stays complete. Other drives' files are never read. A drive's recipe returns 413 only
when that drive's own events pass 2 MiB, the most the app accepts.

The runtime removes the recipes, snapshot and damage marker of a drive whose segments openpilot
has deleted, because nothing can be replayed from it. Removal runs when the runtime starts and
when each new drive begins. The current drive is never removed, and nothing is removed while
the source archive can't be listed or holds no drive at all. Older per-process files are read at
startup and removed once every drive they name is gone.

### Recording record API

All of these endpoints require the paired token; small journal/thumbnail writes are permitted
while driving. The directory is owner-only, excluded from support bundles, and retains the
newest 300 records.

| Method and path | Limit or result |
| --- | --- |
| `POST /commaview/recordings/journal?id=<id>` | JSON object, up to 32 KiB. |
| `POST /commaview/recordings/thumb?id=<id>` | JPEG, up to 256 KiB. |
| `GET /commaview/recordings` | Newest first, journals included, at most 2 MiB. |
| `GET /commaview/recordings/thumb?id=<id>` | Fetch a thumbnail. |
| `POST /commaview/recordings/delete?id=<id>` | Remove that recording record. |

### Metadata integrity

Authenticated `GET /commaview/source-recording/current` requires onroad and brackets status
collection with fresh route/onroad checks. It retains `routeId` for older clients and includes
`recordingStatus` containing the runtime status. Its
`runtimeStats.uiExportSocket.metadataRoute` and `routeGeneration` fields associate readiness
with the correct drive. Source archive endpoints require parked/offroad.

Queued recipes/snapshots carry the producer's route generation. The writer discards stale
records. When the old route is a drive this runtime process was recording (it wrote an onroad
snapshot or a recipe event for it), the writer marks that route damaged in a synced private
`ui-invalid-<route>` sidecar and recipe fault row. Once an observed onroad drive is finalized
offroad, later UI traffic for it is ignored. A missing route parameter during an established
drive preserves its counters and marks damage; initial startup waits for a known route.

A route the process only found in `CurrentRoute` is never marked damaged. This happens when the
runtime starts or restarts while parked after a drive, or at boot, before openpilot clears the
previous boot's value. Nothing for that route was recorded in this process, so the offroad
traffic discarded at the next ignition does not affect the finished drive.

The sidecar restores `metadataDamaged` after restart and damaged archives return 409.
Other integrity counters reset on a new route/restart. Damage writes use bounded retries
that continue offroad until durable. Original source segments are never removed by this
processing.

Faults use the persistent logger from the metadata writer. An eight-entry queue and 32 fixed
reason/errno counters emit occurrences 1, 2, 4, 8, and so on per cause, favor new first causes,
and report suppression through `recordingFaultsSuppressed` and coalesced overflow messages.
This adds no per-frame disk I/O or periodic logging. Support exports redact recording identifiers.

### Retention estimates

Authenticated `GET /commaview/drive-retention?routes=<route>,...` estimates how long the
specified drives have before openpilot deletes them to make room. It is allowed only while
parked/offroad. Openpilot's cleanup removes segment folders when free space falls below
5 GiB or 10%, following its deletion order.

The estimate reads folder names, samples the newest 30 segments to estimate a minute's size,
and caches the listing for five minutes. It is an estimate, not a reservation of storage.
Rendering a replay does not delete source segments, and saving a recording record does not
prevent openpilot from deleting them.

## Logging and process supervision

`start.sh` writes runtime logs under `/data/commaview/logs/`, which survives reboots. Defaults
for its managed log set are 8 MiB per file, up to 14 rotations, 14-day pruning for rotated
logs, a 256 MiB total budget, and a rotation/pruning check every 600 seconds. These are periodic
retention controls, not an instantaneous hard cap on all files in the logs directory.

Overrides are `COMMAVIEWD_LOG_MAX_BYTES`, `COMMAVIEWD_LOG_MAX_FILES_PER_LOG`,
`COMMAVIEWD_LOG_MAX_AGE_DAYS`, `COMMAVIEWD_LOG_TOTAL_MAX_BYTES`, and
`COMMAVIEWD_LOG_ROTATE_INTERVAL_SEC`. The process watcher and deferred maintenance logs have
their own bounded rotation.

A crashed bridge/control process is restarted with backoff of 5, 10, 20… seconds, capped at
300 seconds, with at most eight restarts in a row. A healthy 10-minute run resets the count.
Clean exits, SIGTERM, and deliberately stopped supervisors do not restart. Scheduled restarts
are recorded in `runtime-run-events.jsonl`. The supervisor overrides are
`COMMAVIEWD_SUPERVISOR_FIRST_BACKOFF_SEC`, `COMMAVIEWD_SUPERVISOR_MAX_BACKOFF_SEC`,
`COMMAVIEWD_SUPERVISOR_MAX_RESTARTS`, and `COMMAVIEWD_SUPERVISOR_HEALTHY_SEC`.

For redacted on-demand export and evidence limits, see
[Collect a support report](troubleshooting.md#collect-a-support-report).
