# Runtime releases and app compatibility

[Documentation](README.md) · [Development](development.md) · [Installation](installation.md)

This guide is for maintainers preparing a runtime release. Runtime publication, update
offers, and the public installer's default are separate operations. These workflows do not
build, register, or version an Android APK.

## Choose the operation

| Goal | Workflow/options | Result |
| --- | --- | --- |
| Publish a new test runtime without offering an update | `commaviewd-release`, `channel=alpha`, `offer_updates=false`, `promote_current=false` | New alpha tag and hosted assets only. |
| Publish and offer a runtime to compatible apps | `commaviewd-release`, chosen channel, `offer_updates=true`, explicit app bounds | Assets plus registration in production and staging account services. |
| Offer an already hosted runtime | `runtime-rollout`, existing tag, explicit app bounds/environment | Registration without rebuilding or releasing an APK. |
| Change the public installer's default runtime | A beta registration with `promote_current=true` | Update the default runtime while preserving APK release fields. |

Follow [AGENTS.md](../AGENTS.md): changes land through PRs, and release tags are created by
the release workflow. Do not push `main` or create/push a tag as the normal release procedure.
Run a release/rollout only when it has been requested.

## Prepare the source

1. Finish local verification and merge the approved runtime changes through the normal PR flow.
2. Check [comma/version.env](../comma/version.env) for the intended version floor and
   [ci/upstream-refs.env](../ci/upstream-refs.env) for the intended upstream source pin.
   Changing either is a deliberate source change, not a step needed for every alpha.
3. Add user-facing notes at `docs/release/<tag>-user-facing.md` when the planned tag is known.
   The workflow matches the **exact tag**, including numbered alpha suffixes. If that file
   is absent it generates a generic release body.
4. Select the app compatibility bounds from actual validated app versions. Confirm the
   account runtime registry and app discovery support are deployed in the target environments.

## Cut a new release

Run [commaviewd-release](../.github/workflows/commaviewd-release.yml) with `tag` left empty.
The workflow checks out the default branch to select the release:

- **Alpha:** the next `v<version>-alpha.<n>` from `main`.
- **Beta:** `v<version>-beta` from the selected alpha's commit. Set `from` to an explicit
  alpha tag, or leave it empty to use the version's newest alpha.

The workflow validates rollout inputs before creating a tag, then checks out that tag,
verifies exporter applicability, runs runtime verification, builds the bundle, and publishes
release assets and companion scripts.

| Input | Guidance |
| --- | --- |
| `channel` | `alpha` (default) or `beta`. |
| `from` | Beta-only alpha source tag; empty selects the newest eligible alpha. |
| `tag` | Empty for a new release. An explicit existing tag selects the rebuild/recovery path. |
| `offer_updates` | Defaults to `true`; requires both app bounds. Set `false` for assets only. |
| `min_app_version` | Oldest compatible APK version, inclusive. |
| `max_app_version` | Newest compatible APK version, inclusive. |
| `promote_current` | Defaults to `false`; beta-only, requires update registration and compatibility with the public app. |
| `allow_overwrite` | Existing GitHub release asset recovery only; leave `false` for normal releases. |

The tag format is `v<major>.<minor>.<patch>-alpha.<n>` or `-beta`; legacy unnumbered alpha
tags are also accepted. Supplying an existing tag does not create a new tag.

## Check publication

The release workflow publishes:

- `commaview-comma-<tag>.tar.gz` and its `.sha256`.
- Verification manifests for reproducibility, upstream interfaces, binary contracts,
  release smoke packaging, and UI export status on the GitHub release.
- The tarball, checksum, and matching `comma/` files to the hosted release bucket, served
  at `https://commaview.com/commaviewd/<tag>/`.
- The public installer shim at `https://commaview.com/commaviewd/install.sh`.

Confirm the workflow completed, the hosted checksum/bundle are available, and a compatible
test app can install the intended runtime. A hosted tag's files are immutable. An existing
tag's `allow_overwrite` option does not permit changing already published hosted content;
use a new tag for changed runtime bytes.

## Offer a runtime without an APK release

For a newly built runtime, set `offer_updates=true` and explicit inclusive app bounds in
`commaviewd-release`. Leave `promote_current=false` for a runtime-only rollout. Registration
uses the published tag and checksum in both production and staging account services.

For an existing hosted tag, run [runtime-rollout](../.github/workflows/runtime-rollout.yml)
with:

| Input | Value |
| --- | --- |
| `tag` | Existing immutable hosted runtime tag. |
| `min_app_version`, `max_app_version` | Explicit validated compatibility bounds. |
| `environment` | `staging`, `production`, or `all` (default). |
| `promote_current` | Normally `false`; `true` only for an intended beta default promotion. |

Use `environment=staging` to check the deployed account API before registering in production.
Registration is immutable and can be retried with the same values. If it fails after assets
were published, fix the control-plane issue and use `runtime-rollout` for that tag rather
than rebuilding unchanged files. Inspect which environments completed before retrying.

### Which apps see which runtimes

| App channel | Eligible runtime channel |
| --- | --- |
| Alpha | Alpha runtimes and final beta runtimes within its compatibility bounds. |
| Beta | Beta runtimes within its compatibility bounds. |

The newest compatible runtime wins. An app can remain on an older compatible runtime when
a newer runtime requires a newer APK. `offer_updates=false` publishes assets without
advertising an update. A tag-push-triggered release also publishes assets only; it is not
the normal way to cut a release.

## Promote the public installer default

`promote_current=true` is a separate beta-only operation. It requires update registration,
compatibility with the public APK in each target account environment, and no runtime
downgrade. It preserves the APK release fields. Alpha registration never changes this
default.

The installer reads the default at `https://commaview.com/api/current-release`. Publishing
assets or registering an update offer alone does not change what an unpinned manual install
requests.

## Build a local bundle for inspection

This produces local files; it does not publish or register a release:

```bash
OP_ROOT=/path/to/openpilot-src commaviewd/scripts/run-verification.sh
OP_ROOT=/path/to/openpilot-src tools/release/comma-build-bundle.sh <tag>
```

Output is under `release/<tag>/`. The bundle builder accepts `--skip-build` to use already
verified `DIST_DIR` artifacts, or uses `comma/version.env` when no tag is supplied. Keep a
local inspection tag separate from a production rollout decision.
