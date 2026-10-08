# Install and update commaviewd

[Documentation](README.md) · [Troubleshooting](troubleshooting.md)

The CommaView app's guided setup is the recommended installation path. It installs the
runtime on your comma and pairs the app with it. The [public install guide](https://commaview.com/docs/install)
walks through app setup.

This page covers the alternative SSH procedure and explains what to expect during an update.

## Before you start

- Use a **comma 3X** (`tizi`) or **comma four** (`mici`) running a supported openpilot or
  sunnypilot build. The **comma 3** (`tici`) is unsupported.
- Put the device and your workstation on a network where they can reach each other.
  The comma also needs internet access to download the release.
- For manual installation, enable SSH through the device's developer settings and configure
  your SSH key through the normal comma setup process. Check that `ssh comma@<comma-ip>` works.
- Park the car with openpilot disengaged, or let openpilot become offroad. Read the
  [disclaimer](../DISCLAIMER.md) before use.

Replace `<comma-ip>` in every example with your comma's address. Run the commands below on
your workstation unless the text says otherwise.

## Install the public default runtime

```bash
curl -fsSL https://commaview.com/commaviewd/install.sh \
  | ssh comma@<comma-ip> bash
```

The installer asks `https://commaview.com/api/current-release` for the default runtime paired
with the public app release. If that lookup fails, it falls back to the newest GitHub release.
The default does not change just because a new runtime tag exists.

Release files and companion scripts are downloaded from
`https://commaview.com/commaviewd/<tag>/`. You do not need a source checkout to install.

The installer verifies the bundle's SHA-256 checksum, backs up managed files, applies the UI
export patch, and starts the runtime. An update restores the previous runtime if the new
control API does not answer within its health-check window (30 seconds by default).

## Finish setup and check the installation

1. Read the install output. If it asks for a reboot to load the UI exporter, reboot through
   the app's offered action or the comma's own reboot control while parked or offroad.
2. Complete pairing in the app and open live view when the comma is producing camera video.
3. If setup reports a problem, follow [Troubleshooting](troubleshooting.md) before reinstalling.

For an SSH check:

```bash
ssh comma@<comma-ip> 'cat /data/commaview/version.env'
ssh comma@<comma-ip> 'curl -fsS http://127.0.0.1:5002/commaview/status'
```

The status response includes the runtime version, `roadPhase`, UI export status, runtime
statistics, and any deferred maintenance. A healthy control API confirms that the runtime is
reachable; check video and overlays separately in the app.

## Update from the app

A paired app can request a runtime update without SSH when the installed runtime advertises
`runtime-update`. The request queues maintenance; acceptance means the update was queued,
not that it has finished. Watch the app's update progress and resulting runtime version.

App updates and runtime updates have separate release channels and compatibility ranges.
An alpha runtime is offered to eligible alpha apps; beta apps receive beta runtimes. See
[Releases](releases.md) for the maintainer procedure.

## Install a specific release

Use a runtime tag compatible with your app. Replace `vX.Y.Z` with the full published tag,
including its `-alpha.<n>` or `-beta` suffix.

```bash
curl -fsSL https://commaview.com/commaviewd/install.sh \
  | ssh comma@<comma-ip> bash -s -- --tag vX.Y.Z
```

To reinstall the release already installed on the comma:

```bash
ssh comma@<comma-ip> 'bash /data/commaview/install.sh --current'
```

`--current` reads the installed `version.env`; it does not request the newest runtime.

## If maintenance must wait

Installation, removal, and UI export repair run only while the car is parked or openpilot is
offroad. A normal install refuses while driving with exit code **42**.

To queue an install until the device reaches a suitable state:

```bash
curl -fsSL https://commaview.com/commaviewd/install.sh \
  | ssh comma@<comma-ip> bash -s -- --tag vX.Y.Z --force-offroad
```

Despite its name, `--force-offroad` does **not** force openpilot offroad. It queues the job and
exits **75**, printing `COMMAVIEW_MAINTENANCE_DEFERRED=install`. The queue waits for a stable
parked or offroad period and resumes after a reboot. Only one job is kept; a new queued job
replaces the previous one. Details are in [Maintenance](maintenance.md).

On older runtimes, an app update can remain blocked on a bench device even when a newer
release recognizes that no car is attached. Use the hosted SSH installer above, or wait until
openpilot is offroad. See [bench troubleshooting](troubleshooting.md#an-update-stays-queued-on-a-bench-device).

## Uninstall

A paired app can request removal when the runtime advertises `runtime-uninstall`. To remove
it manually while parked or offroad:

```bash
ssh comma@<comma-ip> 'bash /data/commaview/uninstall.sh'
```

To queue removal until maintenance is allowed:

```bash
ssh comma@<comma-ip> 'bash /data/commaview/uninstall.sh --force-offroad'
```

Uninstall restores the managed upstream UI files first. If restoration fails, it stops so
the failure can be investigated. After successful restoration it removes the runtime,
its boot hook, pairing token, local CommaView data, and queued maintenance. The app loses
access to the runtime when removal completes. The uninstaller also works on a comma 3 to
remove an older installation.

## Maintenance result codes

| Code | Meaning | Next step |
| --- | --- | --- |
| `0` | The command completed successfully. | Check the output for a required reboot. |
| `42` | Maintenance was refused while driving. | Park/disengage, wait for offroad, or queue with `--force-offroad`. |
| `43` | Unsupported comma 3 (`tici`). | Use a supported device; an existing install can still be removed. |
| `44` | The UI patch helper found local changes it will not overwrite automatically. | Review the patch status and [repair procedure](maintenance.md#repairing-the-ui-export-patch). |
| `75` | Maintenance was deferred, or an internal staging step completed. | Check `deferredMaintenance` and wait for the queued job. |

Other nonzero results indicate an error. Keep the command output and collect a support report.
For advanced installer overrides, see the [runtime reference](runtime-reference.md#installer-options-and-overrides).
