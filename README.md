# commaviewd

The runtime that connects your comma device to [CommaView](https://commaview.com).

CommaView brings your comma's camera views and openpilot or sunnypilot driving overlays to
an Android phone, tablet, or head unit over your network. `commaviewd` runs on the comma;
the app's guided setup installs and pairs it for you.

> CommaView is an unofficial project, not affiliated with or endorsed by comma.ai.

## Supported devices

| Device | Platform | Support |
| --- | --- | --- |
| comma 3X | `tizi` | Supported. |
| comma four | `mici` | Supported. |
| comma 3 | `tici` | Unsupported; the installer refuses it before making changes. |

The supported runtime integrations are openpilot and sunnypilot. Upstream branch coverage
is defined by the [verification target matrix](ci/targets.json); development/nightly checks
provide early warning of upstream changes rather than a promise that every build is compatible.

## Get started

1. Follow the [CommaView install guide](https://commaview.com/docs/install) for guided app setup.
2. Complete pairing and any requested reboot while the car is parked or openpilot is offroad.
3. Open live view in the app when the comma is producing camera video.

For manual installation over SSH, updates, and removal, use the
[installation guide](docs/installation.md). It includes prerequisites, copyable commands,
and explanations of deferred maintenance and installer result codes.

## What runs on the comma

- A streaming bridge reads camera video and forwards UI telemetry to the app.
- A control API handles pairing, status, diagnostics, and supported maintenance requests.
- Device scripts manage startup, updates, removal, and the upstream UI export patch.
- Recording metadata lets paired devices replay a drive while its source files are available.

The runtime uses the comma's existing video archive. Maintenance waits until the car is
parked or openpilot is offroad, and a UI patch applied after openpilot starts may require
a reboot. See the guides below for the details.

## Documentation

| Task | Guide |
| --- | --- |
| Install, update, reinstall, or uninstall | [Installation](docs/installation.md) |
| Fix connection, overlay, update, or recording problems | [Troubleshooting](docs/troubleshooting.md) |
| Understand queued maintenance, boot behavior, and repair | [Maintenance](docs/maintenance.md) |
| Look up ports, commands, configuration, and recording storage | [Runtime reference](docs/runtime-reference.md) |
| Work on the source and verify a change | [Development](docs/development.md) |
| Publish a runtime and manage app compatibility | [Releases](docs/releases.md) |
| Find telemetry references, plans, reports, and release notes | [Documentation index](docs/README.md) |

For protocol work, see the [telemetry overview](commaviewd/docs/ai/telemetry-raw-only-readme.md)
and [wire contract](commaviewd/docs/ai/telemetry-raw-only-deep-dive.md).

Maintainers and coding agents should read [AGENTS.md](AGENTS.md) before making changes.
The companion Android source repository is [RhynoTech/CommaView](https://github.com/RhynoTech/CommaView)
(access required).

## Help and updates

- [What's new](https://commaview.com/whats-new)
- [Discord](https://discord.gg/UuMhz9K8ft)
- [YouTube](https://www.youtube.com/@CommaView) · [X](https://x.com/CommaView)

For a problem report, include the app/runtime versions and use the app's support export.
[Collecting diagnostics](docs/troubleshooting.md#collect-a-support-report) explains what is
included and which details help reproduce a failure.

## Terms

Read [DISCLAIMER.md](DISCLAIMER.md) before use. The source and related materials are
proprietary and **All Rights Reserved** under [LICENSE](LICENSE). Release artifacts are
subject to the disclaimer and their release notes.
