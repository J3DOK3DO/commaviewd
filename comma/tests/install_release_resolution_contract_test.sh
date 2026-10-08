#!/usr/bin/env bash
set -euo pipefail

# install.sh --help prints the version it resolved, which shows the release a plain install
# would pick without touching a device. Both lookups point at local fixtures.
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
INSTALL_SH="$REPO_ROOT/comma/install.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

unset COMMAVIEWD_RELEASE_TAG COMMAVIEWD_DEFAULT_TAG COMMAVIEWD_INSTALLER_REF COMMAVIEWD_VERSION \
  COMMAVIEWD_RELEASE_REPO COMMAVIEWD_CURRENT_RELEASE_URL COMMAVIEWD_RELEASES_API_URL

cat > "$TMP/current-release.json" <<'JSON'
{"appVersion":"0.0.158-alpha","appTag":"app-v0.0.158-alpha","runtimeVersion":"0.0.55-alpha","runtimeTag":"v0.0.55-alpha","minAppVersion":"0.0.158-alpha","maxAppVersion":"0.0.158-alpha"}
JSON
# GitHub lists releases newest-created first, alphas (prereleases) included; a rebuilt older
# beta can come before a newer one.
cat > "$TMP/releases.json" <<'JSON'
[
  {"tag_name": "v0.0.58-alpha.1", "prerelease": true, "draft": false},
  {"tag_name": "v0.0.55-beta", "prerelease": false, "draft": false},
  {"tag_name": "v0.0.57-beta", "prerelease": false, "draft": false},
  {"tag_name": "v0.0.57-alpha.2", "prerelease": true, "draft": false},
  {"tag_name": "v0.0.56-beta", "prerelease": false, "draft": false}
]
JSON
cat > "$TMP/alphas.json" <<'JSON'
[
  {"tag_name": "v0.0.58-alpha.1", "prerelease": true, "draft": false},
  {"tag_name": "v0.0.57-alpha.2", "prerelease": true, "draft": false}
]
JSON
printf '<html>Service unavailable</html>\n' > "$TMP/not-json.html"
export COMMAVIEWD_RELEASES_API_URL="file://$TMP/releases.json"

installer_version() {
  bash "$INSTALL_SH" "$@" --help | sed -nE 's/^CommaView installer (.*)$/\1/p'
}

expect_version() {
  local want="$1" got="$2" message="$3"
  [[ "$got" == "$want" ]] || { echo "FAIL: $message (expected $want, got ${got:-nothing})" >&2; exit 1; }
}

grep -Fq 'CURRENT_RELEASE_URL="https://commaview.com/api/current-release"' "$INSTALL_SH" || {
  echo "FAIL: install.sh should default to the CommaView current-release API" >&2
  exit 1
}

expect_version "0.0.55-alpha" \
  "$(COMMAVIEWD_CURRENT_RELEASE_URL="file://$TMP/current-release.json" installer_version)" \
  "a plain install should use the runtime paired with the current app release"

expect_version "0.0.57-beta" \
  "$(COMMAVIEWD_CURRENT_RELEASE_URL="file://$TMP/missing.json" installer_version)" \
  "an unreachable current-release lookup should fall back to the newest public (beta) release, not an alpha"

expect_version "0.0.57-beta" \
  "$(COMMAVIEWD_CURRENT_RELEASE_URL="file://$TMP/not-json.html" installer_version)" \
  "a current-release response without a runtimeTag should fall back to the newest public (beta) release"

expect_version "0.0.57-beta" \
  "$(COMMAVIEWD_RELEASE_REPO=RhynoTech/commaviewd COMMAVIEWD_CURRENT_RELEASE_URL="file://$TMP/missing.json" installer_version)" \
  "the previous organisation name should fall back the same way"

expect_version "0.0.50-alpha" \
  "$(COMMAVIEWD_CURRENT_RELEASE_URL="file://$TMP/current-release.json" installer_version --tag v0.0.50-alpha)" \
  "--tag should win over the current release"

expect_version "0.0.58-alpha.1" \
  "$(COMMAVIEWD_RELEASE_REPO=someone/fork installer_version)" \
  "another release repo should skip the CommaView current-release lookup and take its newest release"

# With no public release to fall back to (only alphas, or a private repository's list that can't be
# read), a plain install stops with a clear error and never makes a release up.
sed -n '1,/^# --tag and --current pick the release/p' "$INSTALL_SH" > "$TMP/resolve.sh"
resolution() {
  env COMMAVIEWD_CURRENT_RELEASE_URL="file://$TMP/missing.json" "$@" bash -c '
    set -euo pipefail
    source "$0"
    resolve_release_inputs
    echo "$RELEASE_TAG"' "$TMP/resolve.sh"
}
for releases in "file://$TMP/alphas.json" "file://$TMP/missing.json"; do
  if got="$(resolution COMMAVIEWD_RELEASES_API_URL="$releases" 2>"$TMP/error.txt")"; then
    echo "FAIL: no public release at $releases should stop the install (got: $got)" >&2
    exit 1
  fi
  grep -Fq "couldn't find the release to install" "$TMP/error.txt" || {
    echo "FAIL: a failed lookup should say so (got: $(cat "$TMP/error.txt"))" >&2
    exit 1
  }
  if grep -Fq "v0.0.1-alpha" "$TMP/error.txt" || [[ "$got" == *alpha* ]]; then
    echo "FAIL: a failed lookup must not pick an alpha" >&2
    exit 1
  fi
done
if grep -Fq 'RELEASE_TAG="v0.0.1-alpha"' "$INSTALL_SH"; then
  echo "FAIL: install.sh must not fall back to a made-up release" >&2
  exit 1
fi
# --help still works without a release.
usage="$(COMMAVIEWD_CURRENT_RELEASE_URL="file://$TMP/missing.json" COMMAVIEWD_RELEASES_API_URL="file://$TMP/missing.json" \
  bash "$INSTALL_SH" --help 2>/dev/null)" || { echo "FAIL: --help should succeed without a release" >&2; exit 1; }
[[ "$usage" == *"Usage:"* ]] || { echo "FAIL: --help should print its usage without a release" >&2; exit 1; }

echo "PASS: install.sh installs the current app release's runtime when no tag is given, else the newest beta, else nothing"
