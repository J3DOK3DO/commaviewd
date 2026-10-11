# CommaView v0.0.58-beta — StarPilot C6-12 compatibility edition

This release packages the **working C6-12-specific CommaView bridge binary** rather than the standard upstream Sunnypilot runtime.

## What was fixed

- The video bridge now subscribes to C6-12's `roadEncodeData`, `wideRoadEncodeData`, and `driverEncodeData` instead of the incompatible `narrowRoadEncodeData` / `cabinEncodeData` names.
- C6-12's legacy MSGQ ZeroMQ implementation (`impl_zmq.cc`) is included when compiling the dedicated compatibility build.
- The ARM64 executable resolves its bundled Cap'n Proto/KJ libraries through `$ORIGIN/lib` and uses the device's existing `libzmq.so.5`.
- The original video/telemetry transport ports and Android framing protocol are preserved.

## Verification and scope

- Verified on comma running frozen C6-12 `7b4db9a26010ba9899eef242d3ca49b33cbcca9c`.
- Device bridge and control listeners verified after the successful replacement; ports 8200, 8201, 8202, 8203, and 5002.
- Exact binary SHA-256: `6f86757274921d8a8ab57cd6bbeaac05961b3fb3bf79cd517252ae6c774244b0`.
- Isolated ARM64 compile, service-schema assertions, dependency checks, and bridge startup smoke passed.
- Seven source-level video/transport contract checks passed.
- The owner reports the video-streaming fix is working **so far**. Prolonged road-use, every video mode, and every Android device have **not** been exhaustively qualified.

## Release safety

This beta is specific to the C6-12 StarPilot video schema. It is **not a generic replacement** for all Sunnypilot/openpilot installations and is not promoted as the public/default CommaView installer runtime.

To avoid disturbing a currently working installation, do not automatically reinstall on the vehicle. The working bridge uses the same executable bytes as the asset `commaviewd-c612-aarch64`; its on-device version label can be updated separately after additional parked verification.

On-device rollback backup:
`/data/commaview/backups/c612-video-compat-20261011-before/commaviewd`

No changes to C6-12 driving controls, Panda firmware, or device UI files are included.

## Provenance

- Source parent: `8e4fc8b5244413435420f1a490f531c25d512bbc` (C6-12 support).
- Release diff: two legacy-ZMQ linker inputs plus ARM64 host-builder `$ORIGIN/lib` RUNPATH integration.
- The tested binary was produced with the isolated ARM64 container builder, then post-linked with RUNPATH, without touching C6-12.
- The normal repository `main` branch has newer changes and is intentionally not overwritten by this C6-12-specific tag.

## Assets

- `commaviewd-c612-aarch64`: verified working bridge binary.
- `commaviewd-c612-aarch64.sha256`: per-binary integrity check.
- `commaview-comma-starpilot-c6-12-v0.0.58-beta.tar.gz`: comma runtime kit for this edition.
- `commaview-comma-starpilot-c6-12-v0.0.58-beta.tar.gz.sha256`: bundle integrity check.

**Note:** The associated Git tag uses `starpilot-c6-12-v0.0.58-beta` so it does not trigger the normal `v*` release workflow, which otherwise builds against a different video schema.
