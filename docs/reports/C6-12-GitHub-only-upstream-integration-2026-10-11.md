# C6-12 GitHub-only upstream feature integration — 2026-10-11

## Scope
- Branch: `release/c612-v0.0.58-beta` in `J3DOK3DO/commaviewd`.
- Integrated upstream `main` through **`9e6e2fde274bb51852398a12b749389556af26f5`**.
- Preserved StarPilot C6-12 compatibility, including `roadEncodeData`, `driverEncodeData`, alternate video message queue names from the newer source, conditional `impl_zmq.cc` linkage and `$ORIGIN/lib` runtime linking.
- Preserved `J3DOK3DO/openpilot` recognition as the custom openpilot flavor. Retained upstream recognition of `firestar5683/StarPilot` as StarPilot.
- Included current upstream drive recording/ledger, session reconnect, recording archive recovery and metadata safeguards, live leaderboard, runtime installation/discovery, and updated tests/documentation.

## Release boundary — important
This branch is a **new integration candidate**, NOT the original published and on-device tested `v0.0.58-beta` binary. The immutable `v0.0.58-beta` Git tag and its release archive continue to point to original commit `66b62f98ada9154d6da802df3c77572b5c0c461b`. Do not retag, rebuild or overwrite that published beta from this newer branch.

**No update to the comma device** is authorized or performed as part of this integration. There has been no Panda firmware change or C6-12 driving code change. Do not deploy this branch to the comma until the user finishes testing the current published beta and explicitly requests deployment.

## Verification
- Python regression suite: **362 passed, 1 skipped** on Ubuntu (final post-hardening run).
- Focused StarPilot/UI suite: **83 passed, 1 skipped**.
- C6-12 isolated ARM64 build: **passed** using C6-12 `cereal/services.py` and C6-12 message-queue sources.
- Build has an ARM64 `ELF` output with RUNPATH `$ORIGIN/lib`; isolated video bridge startup selected `roadEncodeData:8200`, `wideRoadEncodeData:8201`, `driverEncodeData:8202`, and telemetry `:8203`.
- Release workflow contract: passed; standard source/build and pinned-beta conditions remain separate.
- Node release-publication tests: 9 passed.
- C++ unit and integration suite: **passed** on native x86-64 test build against isolated C6-12 schema, using the identical source and prebuilt executable (Python regression run separately). Tested message-queue/road phase, bounded source archive, offroad/parked access checks, drive stats, control API/pairing, runtime updates, installer, and release contracts.
- Safety note: Source-archive test on ARM64 intentionally cannot override vehicle offroad parameters; it returned HTTP 403 as designed. That same test passed on x86-64, where the isolated test override is permitted. No safety guard was modified.
- Independent read-only Codex review completed. Tightened all GitHub remote host checks to reject lookalikes and exercised StarPilot in the exporter tests.

## Next decision
Finish testing the already installed `v0.0.58-beta` on the comma. If stable, select a separate **future** integration release/tag, rebuild exact source, review Android compatibility, and obtain explicit permission before any deployment.
