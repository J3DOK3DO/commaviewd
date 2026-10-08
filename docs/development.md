# Development and verification

[Documentation](README.md) · [Runtime reference](runtime-reference.md) · [Releases](releases.md)

This guide is for maintainers with source access. Read [AGENTS.md](../AGENTS.md) before
changing the repository. Run commands from the repository root unless stated otherwise.

## Repository layout

| Path | Contents |
| --- | --- |
| `commaviewd/` | C++ runtime source, tests, build/verification scripts, and telemetry references. |
| `comma/` | Device lifecycle scripts, defaults, exporter sources, transformer/patch helpers, and version metadata. |
| `comma4/install.sh` | Installer shim used by older app versions and the hosted install entry point; retain it. |
| `scripts/` | Host toolchain setup, local CI, upstream helpers, telemetry guard, and release control-plane scripts. |
| `ci/` | Upstream refs, target matrices, and the shared target planner. |
| `tools/release/` | Bundle construction and hosted release publishing. |
| `tools/bench/` | On-device benchmarks and validation helpers. |
| `tools/contribute/` | Drive contribution sanitizer and instructions. |
| `docs/` | Current guides, historical plans/reports, and per-tag release notes. |
| `.github/workflows/` | CI, canaries, release/rollout, and device-test automation. |

## Start a change

Use one topic per branch from current `main`:

```bash
git fetch origin main
git switch -c <topic> origin/main
```

Do not commit or push directly to `main`. Open a **draft** pull request as soon as there is
something worth reviewing, and keep it draft while iterating. Draft PRs run no verification CI.
Mark it ready only after the full local check passes and the branch is up to date with its
base; record the commands/results in the PR description. Maintainers merge unless they
explicitly ask an agent to merge.

## Prepare a build environment

The verification environment is Ubuntu 24.04 with host and ARM64 dependencies. Use a
disposable container/VM or a dedicated build host for the toolchain installer: it rewrites
APT sources and adds the ARM64 package architecture.

```bash
bash scripts/install-commaviewd-toolchain.sh
```

The script needs `sudo` and package network access. It installs compilers, Cap'n Proto,
ZeroMQ, pytest, and ARM64 runtime libraries, and prints `ARM_CAPNP_SO`/`ARM_KJ_SO` paths.
Build scripts normally detect those libraries themselves.

## Run local CI

```bash
scripts/ci-local.sh --list
scripts/ci-local.sh
scripts/ci-local.sh openpilot-release-mici sunnypilot-release-tizi
scripts/ci-local.sh --all
```

With no arguments, the script checks `sunnypilot-release-pin`, the upstream source used by
release builds. Before a PR is ready, run `--all` for the full standard target matrix.
For a canary matrix:

```bash
scripts/ci-local.sh --targets ci/canary-sunnypilot.json --all
```

The shared planner resolves targets, and `scripts/ci-local-checkout.sh` caches upstream source
with initialized submodules under `~/.cache/commaviewd-ci-src` (`CI_LOCAL_SRC_DIR` overrides
this). Each target checks interface and UI patch applicability. Targets sharing an upstream
commit share one full verification build; the remaining targets check patch applicability.
The telemetry-hardening guard runs too.

## Build against an existing upstream checkout

Set `OP_ROOT` explicitly to avoid accidentally using a different local tree. The checkout
must include the cereal schemas and initialized msgq/car-schema submodules required by the
build script.

```bash
OP_ROOT=/path/to/openpilot-src commaviewd/scripts/build-ubuntu.sh
OP_ROOT=/path/to/openpilot-src commaviewd/scripts/run-verification.sh
```

Build output defaults to `dist/commaviewd-host`, `dist/commaviewd-aarch64`, and `dist/lib/`.
`DIST_DIR` overrides the output directory. `HOST_CXX`/`CXX`, `CROSS_CXX`, `ARM_CAPNP_SO`,
`ARM_KJ_SO`, and `PATCHED_MSGQ_LOCAL` override toolchain inputs. `COMMAVIEWD_SKIP_ARM=1`
allows a host-only build; it does not satisfy full release verification.

The verification pipeline runs the upstream interface guard, two reproducibility builds,
binary contract checks, C++/integration/Python tests, and a release smoke package.
`RELEASE_SMOKE_TAG` names the smoke bundle. Build/bench results do not substitute for
real-device validation of video, overlays, and driving behavior.

## Targeted checks

For installer/exporter changes, the Python suite is a useful first check:

```bash
python3 -m pytest comma/tests -q
```

For workflow changes, run `actionlint` on the changed workflow plus the local verification
required by [AGENTS.md](../AGENTS.md). Targeted checks complement the full matrix.

## CI and upstream coverage

`commaviewd-ci` runs for relevant path changes on pushes to **main** and on PRs once ready
for review. It and both canaries use
[commaviewd-verify.yml](../.github/workflows/commaviewd-verify.yml). Successful checks with
unchanged inputs/upstream commits are reused through pass markers; the recommended required
status is `verify / result`.

| Target file | Coverage |
| --- | --- |
| [ci/targets.json](../ci/targets.json) | Openpilot/sunnypilot release and staging branches, applicable mici/tizi layouts, chestnut variants, and the release pin. |
| [ci/canary-openpilot.json](../ci/canary-openpilot.json) | Nightly variants on mici/tizi; interface and patch applicability checks. |
| [ci/canary-sunnypilot.json](../ci/canary-sunnypilot.json) | Dev variants on mici/tizi, including full verification. |
| [ci/upstream-refs.env](../ci/upstream-refs.env) | Deliberately pinned `sunnypilot/openpilot` staging commit used for release builds. |

The release pin was chosen for video msgq header recovery; move it deliberately rather than
following a branch. The comma 3 (`tici`) is excluded because its frozen Qt-based UI does not
match the supported exporter path.

Canaries run Mondays and Thursdays at 07:23 UTC (openpilot) and 07:53 UTC (sunnypilot).
A failing canary is evidence of upstream drift to investigate. Reproduce failures locally;
do not rerun jobs, push empty commits, or close/reopen PRs to try to obtain a passing run.
If it does not reproduce, record that in the PR. Manual workflows are for an explicitly
requested release/deployment or other authorized operation, not work-in-progress testing.

### Materialize a canary source tree

```bash
scripts/sync-canary-upstream.sh <openpilot|sunnypilot> <ref> [dest-root]
```

The script supports canary and staging refs listed by its `--help`; it resolves the ref SHA,
force-checks out that commit in its dedicated cache, initializes submodules, and records
`source.env`. The default checkout is
`~/.cache/commaviewd-canary/<upstream>-<ref>/openpilot-src`. Use a dedicated destination;
the helper is not intended for a tree with local development changes.

## Verification script reference

| Script | What it checks | Main inputs |
| --- | --- | --- |
| [ci-local.sh](../scripts/ci-local.sh) | The selected CI target matrix, upstream patch applicability, and required verification builds. | Target names, `--all`, `--list`, `--targets`; `CI_LOCAL_SRC_DIR`. |
| [run-verification.sh](../commaviewd/scripts/run-verification.sh) | The complete build/test/bundle pipeline for one upstream tree. | `OP_ROOT`, `DIST_DIR`, `RELEASE_SMOKE_TAG`. |
| [upstream-interface-guard.sh](../commaviewd/scripts/upstream-interface-guard.sh) | Required upstream services/fields and repository exporter assets. | `OP_ROOT`, `DIST_DIR`; `--telemetry-only`, `--manifest <path>`. |
| [reproducible-build.sh](../commaviewd/scripts/reproducible-build.sh) | Two builds with fixed `SOURCE_DATE_EPOCH` have matching digests. | `OP_ROOT`, `DIST_DIR`; `--manifest <path>`. |
| [binary-contract-check.sh](../commaviewd/scripts/binary-contract-check.sh) | Architecture, dependencies, runpath, size, and bundled libraries. | `DIST_DIR`; `--manifest <path>`. |
| [run-unit-tests.sh](../commaviewd/scripts/run-unit-tests.sh) | Runtime build, C++ tests, contract/integration scripts, and `comma/tests` pytest. | `OP_ROOT`, compiler settings; ARM64 toolchain or host-only override. |
| [verify-telemetry-hardening.sh](../scripts/verify-telemetry-hardening.sh) | Raw-only forwarding and removal of obsolete debug switches. | No flags. |

## Selected contract and integration checks

These are useful starting points for focused checks; the full test runner remains
the source of truth for the suite. Run them from the repository root.

| Script | Purpose |
| --- | --- |
| `comma/tests/onroad_ui_export_patch_contract_test.sh` | Static contract check for the socket UI export patch. |
| `comma/tests/openpilot_ui_reload_test.py` | The patch lifecycle never signals openpilot's UI; reload status, `--before-openpilot` boot step (staged updates, timeout) and continue.sh hook upgrade. |
| `comma/tests/onroad_ui_export_canary_applicability_test.sh` | Applies/verifies patch against real openpilot/sunnypilot canary refs. |
| `commaviewd/tests/control_mode_api_contract_test.sh` | Verifies control API routes/contract are present. |
| `commaviewd/tests/msgq_reader_slot_contract_test.sh` | commaviewd creates no msgq subscriber and maps queues read-only. |
| `commaviewd/tests/test_msgq_ring_reader.cpp` | The slot-free encoder queue reader against real msgq: restarts and reconnects never evict loggerd, a lapped reader resumes on a keyframe, nothing torn; `--cpu-bench IDLE_POLL_US SECONDS` measures its polling. |
| `commaviewd/tests/runtime_control_integration_test.py` | runtime-debug apply in place (SIGHUP), `roadPhase`, Safe Repair queued while onroad. |
| `commaviewd/tests/test_process_events.cpp` | The process watcher (`/data/commaview/logs/process-events.jsonl`): managerState transitions, event lines, rotation, `/dev/kmsg` filtering; prints what one tick costs. |
| `comma/tests/deferred_maintenance_test.py` | `run_when_offroad.sh`: queued maintenance runs only after a stable offroad or parked stretch, never while driving. |
| `comma/tests/runtime_supervisor_restart_test.py` | `start.sh` restarts a crashed process with a bounded backoff, never a deliberate stop. |
| `commaviewd/tests/local_discovery_contract_test.sh` | Verifies local discovery responder contract. |
| `commaviewd/tests/onroad_ui_export_ci_contract_test.sh` | Ensures workflows align to direct v2 validation. |
| `commaviewd/tests/raw_only_runtime_contract_test.sh` | Guards raw-only runtime behavior. |
| `commaviewd/tests/reproducible_build_test.sh` | Checks the reproducible-build script's `--help`. |
| `commaviewd/tests/runtime_debug_policy_contract_test.sh` | Guards runtime-debug config/policy behavior. |
| `commaviewd/tests/timestamped_video_runtime_contract_test.sh` | Guards timestamped video runtime behavior. |
| `commaviewd/tests/unit_tests_pipeline_test.sh` | Guards unit-test pipeline script presence/help behavior. |
