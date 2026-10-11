# CommaView C6-12 local Git origin compatibility repair — 2026-10-11

## User-visible regression
After deploying integration commit bd512800, the Android app reported
"Live View needs a repair — unsupported upstream remote for /data/openpilot".
The on-device apply script reproduced the same error.

## Root cause
The installed /data/openpilot checkout has a self-referential Git remote
(origin = /data/openpilot). The merged hostname-hardening rules allowed
GitHub remotes only and rejected this legitimate path before evaluating
the existing verified local provenance.

## Source correction
Only these CommaView scripts changed:
- comma/scripts/apply_onroad_ui_export_patch.sh
- comma/scripts/verify_onroad_ui_export_patch.sh

The scripts now recognize the exact local origin /data/openpilot only if:
- OP_ROOT is exactly /data/openpilot;
- The recorded flavor is openpilot;
- Recorded checkout root and recorded Git origin both match this path;
- The recorded upstream commit equals the current, nonempty Git HEAD.

All other local-path remotes remain rejected. The GitHub hostname allowlist
and lookalike-host rejection remain in place.

## Offline and on-device validation
- 22 standalone function-level checks passed across both scripts.
- 37 focused pytest checks passed (52 deselected), including the 24 new local-origin cases.
- Full Python regression suite: 386 passed, 1 skipped in 140.92s.
- Live parked/offroad device: read-only verify output returned
  state=patch-verified; patchVerified=true; repairNeeded=false;
  flavor=openpilot; uiPlatform=mici.
- Both CommaView bridge and control remained running; no restart.
- No openpilot UI, driving code, Panda firmware or bridge executable changes.

## Rollback and source provenance
Original scripts and patch provenance remain backed up at
/data/commaview/backups/c612-bd512800-local-origin-hotfix-20261011/.
Previous full CommaView snapshot remains at
/data/commaview/backups/c612-before-bd512800-20261011/.

The tested integrated ARM64 bridge stays at SHA-256:
9c96ef6df8864d0f22542a2f6509f020a062f3c5237093545a4b0cd85d97e6cd
Original working bridge rollback SHA-256:
6f86757274921d8a8ab57cd6bbeaac05961b3fb3bf79cd517252ae6c774244b0

The immutable published v0.0.58-beta tag and release artifacts remain unchanged.
A fresh parked Android Live View test is required after reopening the app:
a passing CommaView patch status is not proof of video-frame delivery.
