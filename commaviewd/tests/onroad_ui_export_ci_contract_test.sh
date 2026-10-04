#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$ROOT/.." && pwd)"
CI="$REPO_ROOT/.github/workflows/commaviewd-ci.yml"
CANARY_OPENPILOT="$REPO_ROOT/.github/workflows/commaviewd-canary-openpilot.yml"
CANARY_SUNNYPILOT="$REPO_ROOT/.github/workflows/commaviewd-canary-sunnypilot.yml"
# CI and the canaries run the same checks (commaviewd-verify.yml) on their own target lists.
VERIFY="$REPO_ROOT/.github/workflows/commaviewd-verify.yml"
OPENPILOT_TARGETS="$REPO_ROOT/ci/canary-openpilot.json"
SUNNYPILOT_TARGETS="$REPO_ROOT/ci/canary-sunnypilot.json"

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

assert_contains_fixed() {
  local needle="$1"
  local file="$2"
  local message="$3"
  grep -Fq -- "$needle" "$file" || fail "$message"
}

assert_not_contains_fixed() {
  local needle="$1"
  local file="$2"
  local message="$3"
  if grep -Fq -- "$needle" "$file"; then
    fail "$message"
  fi
}

for file in "$CI" "$CANARY_OPENPILOT" "$CANARY_SUNNYPILOT"; do
  [[ -f "$file" ]] || fail "missing workflow $file"
  assert_contains_fixed "uses: ./.github/workflows/commaviewd-verify.yml" "$file" "$file should run the shared verification workflow"
done
assert_contains_fixed "targets: ci/canary-openpilot.json" "$CANARY_OPENPILOT" "openpilot canary should verify its targets"
assert_contains_fixed "targets: ci/canary-sunnypilot.json" "$CANARY_SUNNYPILOT" "sunnypilot canary should verify its targets"

for file in "$VERIFY"; do
  [[ -f "$file" ]] || fail "missing workflow $file"
  assert_not_contains_fixed "android-schema" "$file" "$file should not reference android-schema after direct v2 cutover"
  assert_not_contains_fixed "check-android-schema-drift" "$file" "$file should not reference schema drift scripts after direct v2 cutover"
  assert_not_contains_fixed "dist/android-schema-drift.json" "$file" "$file should not upload schema drift artifacts after direct v2 cutover"
  assert_not_contains_fixed "apply_hud_lite_patch.sh" "$file" "$file should stop referencing stale HUD-lite helpers"
  assert_not_contains_fixed "verify_hud_lite_patch.sh" "$file" "$file should stop referencing stale HUD-lite helpers"
  assert_not_contains_fixed "hud-lite-status.json" "$file" "$file should stop surfacing stale HUD-lite artifacts"
  assert_contains_fixed "apply_onroad_ui_export_patch.sh" "$file" "$file should validate direct v2 patch applicability"
  assert_contains_fixed "verify_onroad_ui_export_patch.sh" "$file" "$file should verify direct v2 patch applicability"
  assert_contains_fixed "onroad-ui-export-status.json" "$file" "$file should surface direct v2 status artifacts"
  assert_contains_fixed "verify-telemetry-hardening.sh" "$file" "$file should run the telemetry hardening guard"
done

CI_TARGETS="$REPO_ROOT/ci/targets.json"
assert_contains_fixed '"name": "openpilot-release-mici"' "$CI_TARGETS" "commaviewd CI should run openpilot stable MICI release"
assert_not_contains_fixed 'release-tici' "$CI_TARGETS" "commaviewd CI should not check the unsupported comma 3 (release-tici)"
assert_contains_fixed '"upstream_ref": "release-mici-staging"' "$CI_TARGETS" "commaviewd CI should cover openpilot and sunnypilot MICI staging"
assert_contains_fixed '"upstream_ref": "release-tizi-staging"' "$CI_TARGETS" "commaviewd CI should cover openpilot and sunnypilot TIZI staging"
assert_contains_fixed '"upstream_ref": "release-chestnut-staging"' "$CI_TARGETS" "commaviewd CI should cover openpilot chestnut staging"
assert_contains_fixed '"upstream_ref": "staging-chestnut"' "$CI_TARGETS" "commaviewd CI should cover sunnypilot chestnut staging"
assert_contains_fixed '"name": "sunnypilot-release-mici"' "$CI_TARGETS" "commaviewd CI should run sunnypilot stable MICI release"
assert_contains_fixed '"name": "sunnypilot-release-tizi"' "$CI_TARGETS" "commaviewd CI should run sunnypilot stable TIZI release"
assert_contains_fixed '--platform "${{ matrix.target.ui_platform }}"' "$VERIFY" "commaviewd CI should pass explicit platform for release refs"
# The canaries run the bleeding edge (staging is in CI): openpilot's nightlies, sunnypilot's dev.
for ref in nightly nightly-dev nightly-chestnut nightly-chestnut-dev; do
  assert_contains_fixed "\"upstream_ref\": \"$ref\"" "$OPENPILOT_TARGETS" "openpilot canary should cover $ref drift"
done
assert_contains_fixed '"run_verification": false' "$OPENPILOT_TARGETS" "openpilot nightly canaries should be patch-applicability only"
assert_contains_fixed 't.get("run_verification", True) is not False' "$REPO_ROOT/ci/plan-targets.py" "canaries should run verification unless a target opts out"
assert_not_contains_fixed 'run_verification != false }}' "$VERIFY" "verification must not compare run_verification with != false (an unset key skips verification)"
assert_contains_fixed '"upstream_ref": "dev"' "$SUNNYPILOT_TARGETS" "sunnypilot canary should cover early dev drift"
assert_contains_fixed '"upstream_ref": "dev-chestnut"' "$SUNNYPILOT_TARGETS" "sunnypilot canary should cover chestnut dev drift"
for targets in "$OPENPILOT_TARGETS" "$SUNNYPILOT_TARGETS"; do
  assert_not_contains_fixed 'staging' "$targets" "$targets: staging belongs to CI (ci/targets.json), not the canaries"
  assert_not_contains_fixed 'tici' "$targets" "$targets: the comma 3 (tici) isn't supported"
done
assert_contains_fixed "./commaviewd/scripts/upstream-interface-guard.sh --telemetry-only" "$VERIFY" "every target should run the semantic interface guard, even when runtime verification is skipped"

for patch in "$REPO_ROOT/comma/patches/openpilot/0001-commaview-ui-export-v2.patch" "$REPO_ROOT/comma/patches/sunnypilot/0001-commaview-ui-export-v2.patch"; do
  [[ -f "$patch" ]] || fail "missing patch $patch"
  assert_contains_fixed '"faceOrientationStd"' "$patch" "$patch should export driverStateV2 faceOrientationStd for DMOji parity"
  assert_contains_fixed '"facePosition"' "$patch" "$patch should export driverStateV2 facePosition for upstream face_orientation_from_net parity"
  assert_contains_fixed '"facePositionStd"' "$patch" "$patch should export driverStateV2 facePositionStd for telemetry completeness"
  assert_contains_fixed '"faceProb"' "$patch" "$patch should export driverStateV2 faceProb for face detection parity"
  assert_contains_fixed '"wheelOnRightProb"' "$patch" "$patch should export driverStateV2 wheelOnRightProb"
done

echo "PASS: workflows are aligned to direct v2 validation"
