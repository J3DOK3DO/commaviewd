#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$ROOT/.." && pwd)"
WORKFLOW="$REPO_ROOT/.github/workflows/commaviewd-ci.yml"
VERIFY="$REPO_ROOT/.github/workflows/commaviewd-verify.yml"

assert_file() {
  [[ -f "$1" ]] || { echo "FAIL: missing $1" >&2; exit 1; }
}

assert_contains() {
  local needle="$1"
  local file="$2"
  local message="$3"
  grep -Fq -- "$needle" "$file" || { echo "FAIL: $message" >&2; exit 1; }
}

TARGETS="$REPO_ROOT/ci/targets.json"
PLAN="$REPO_ROOT/ci/plan-targets.py"

assert_file "$WORKFLOW"
assert_file "$VERIFY"
assert_file "$TARGETS"
assert_file "$PLAN"
python3 -m json.tool "$TARGETS" >/dev/null || { echo "FAIL: ci/targets.json is not valid JSON" >&2; exit 1; }

# The targets (ci/targets.json, planned by ci/plan-targets.py).
assert_contains '"name": "openpilot-release-mici"' "$TARGETS" "commaviewd CI should validate openpilot release-mici"
assert_contains '"upstream_ref": "release-mici"' "$TARGETS" "commaviewd CI should include MICI release refs"
# The comma 3 (tici) isn't supported: its frozen release-tici draws its screen with the Qt UI.
if grep -Fq 'tici' "$TARGETS"; then
  echo "FAIL: commaviewd CI should not check the unsupported comma 3 (release-tici)" >&2
  exit 1
fi
assert_contains '"upstream_repo": "commaai/openpilot"' "$TARGETS" "commaviewd CI should include commaai/openpilot"
assert_contains '"name": "sunnypilot-release-mici"' "$TARGETS" "commaviewd CI should validate sunnypilot release-mici"
assert_contains '"name": "sunnypilot-release-tizi"' "$TARGETS" "commaviewd CI should validate sunnypilot release-tizi"
assert_contains '"upstream_ref": "release-tizi"' "$TARGETS" "commaviewd CI should include sunnypilot TIZI release ref"
assert_contains '"upstream_repo": "sunnypilot/sunnypilot"' "$TARGETS" "commaviewd CI should include sunnypilot/sunnypilot"
assert_contains '"name": "openpilot-release-tizi"' "$TARGETS" "commaviewd CI should validate openpilot release-tizi"
assert_contains '"name": "openpilot-release-chestnut-mici"' "$TARGETS" "commaviewd CI should validate openpilot release-chestnut on MICI"
assert_contains '"name": "openpilot-release-chestnut-tizi"' "$TARGETS" "commaviewd CI should validate openpilot release-chestnut on TIZI"
assert_contains '"upstream_ref": "release-chestnut"' "$TARGETS" "commaviewd CI should include the openpilot chestnut release ref"
assert_contains '"name": "sunnypilot-release-pin"' "$TARGETS" "commaviewd CI should build the pinned sunnypilot release source"
# Staging, so drift between releases shows before it ships.
for name in openpilot-release-mici-staging openpilot-release-tizi-staging \
    openpilot-release-chestnut-staging-mici openpilot-release-chestnut-staging-tizi \
    sunnypilot-release-mici-staging sunnypilot-release-tizi-staging \
    sunnypilot-staging-chestnut-mici sunnypilot-staging-chestnut-tizi; do
  assert_contains "\"name\": \"$name\"" "$TARGETS" "commaviewd CI should validate $name"
done
assert_contains 'upstream-refs.env' "$PLAN" "commaviewd CI should resolve the release pin from ci/upstream-refs.env"
assert_contains 'COMMAVIEWD_RELEASE_SUNNYPILOT_REF' "$PLAN" "commaviewd CI should resolve the release pin from ci/upstream-refs.env"
# An unset run_verification means "verify": only an explicit false opts a target out.
assert_contains 't.get("run_verification", True) is not False' "$PLAN" "commaviewd CI should run verification unless a target opts out"

# The workflows: commaviewd-ci runs commaviewd-verify on ci/targets.json.
assert_contains "comma4/install.sh" "$WORKFLOW" "commaviewd CI should run when the legacy install shim changes"
assert_contains "uses: ./.github/workflows/commaviewd-verify.yml" "$WORKFLOW" "commaviewd CI should run the shared verification workflow"
assert_contains "targets: ci/targets.json" "$WORKFLOW" "commaviewd CI should verify ci/targets.json"
assert_contains "ci/plan-targets.py" "$VERIFY" "commaviewd CI should plan its targets with ci/plan-targets.py"
assert_contains 'target: ${{ fromJSON(needs.plan.outputs.targets) }}' "$VERIFY" "commaviewd CI matrix should come from the plan"
assert_contains '--platform "${{ matrix.target.ui_platform }}"' "$VERIFY" "commaviewd CI should pass explicit MICI/TIZI platform"
assert_contains 'ref: ${{ matrix.target.sha }}' "$VERIFY" "commaviewd CI upstream checkout should be pinned to the planned SHA"
assert_contains 'if: matrix.target.run_verification' "$VERIFY" "commaviewd CI should run verification for the targets the plan picks"
if grep -Fq 'run_verification != false }}' "$VERIFY"; then
  echo "FAIL: 'run_verification != false' treats an unset key as false and skips verification" >&2
  exit 1
fi

printf 'PASS: CI workflow contract validates pinned upstream checkout\n'
