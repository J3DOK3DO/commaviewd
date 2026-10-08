#!/usr/bin/env bash
set -euo pipefail

# Releases come from commaview.com/commaviewd/<tag>/ (published there by commaviewd-release), not
# from GitHub, so the repository can be private: install.sh's tarball and companions, and the
# shim's installer. A branch (while developing) and another release repository still use GitHub.
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

# install.sh up to its release resolution, without running the install.
sed -n '1,/^# --tag and --current pick the release/p' "$REPO_ROOT/comma/install.sh" > "$TMP/resolve.sh"
urls() {
  env -u COMMAVIEWD_BASE_URL -u COMMAVIEWD_INSTALLER_RAW_BASE -u COMMAVIEWD_RELEASES_ORIGIN "$@" bash -c '
    set -euo pipefail
    source "$0"
    RELEASE_TAG=v0.0.57-alpha.1
    resolve_release_inputs
    echo "$BASE_URL $INSTALLER_RAW_BASE"' "$TMP/resolve.sh"
}

got="$(urls)"
[ "$got" = "https://commaview.com/commaviewd/v0.0.57-alpha.1 https://commaview.com/commaviewd/v0.0.57-alpha.1/comma" ] \
  || fail "a release installs from commaview.com (got: $got)"
for repo in CommaView/commaviewd RhynoTech/commaviewd; do
  got="$(urls COMMAVIEWD_RELEASE_REPO="$repo")"
  [ "$got" = "https://commaview.com/commaviewd/v0.0.57-alpha.1 https://commaview.com/commaviewd/v0.0.57-alpha.1/comma" ] \
    || fail "both official repository names use the hosted release (got: $got)"
done
got="$(urls COMMAVIEWD_INSTALLER_REF=my-branch)"
[ "$got" = "https://commaview.com/commaviewd/v0.0.57-alpha.1 https://raw.githubusercontent.com/CommaView/commaviewd/my-branch/comma" ] \
  || fail "a branch's companions come from the repository (got: $got)"
got="$(urls COMMAVIEWD_RELEASE_REPO=someone/fork)"
[ "$got" = "https://github.com/someone/fork/releases/download/v0.0.57-alpha.1 https://raw.githubusercontent.com/someone/fork/v0.0.57-alpha.1/comma" ] \
  || fail "another repository's release comes from its GitHub releases (got: $got)"

# The shim runs the asked-for release's installer from commaview.com, else the current release's.
mkdir -p "$TMP/bin"
# The stub serves the current release (or fails, with CURRENT_RELEASE_DOWN=1), the GitHub release
# list from RELEASES_FILE (or fails without one), and for an installer a script that records the
# arguments it was given.
cat > "$TMP/bin/curl" <<'STUB'
#!/usr/bin/env bash
for a in "$@"; do last="$a"; done
echo "$last" >> "$CURL_LOG"
case "$last" in
  *current-release*)
    [ "${CURRENT_RELEASE_DOWN:-0}" != 1 ] || exit 22
    echo '{"appTag":"app-v1","runtimeTag":"v0.0.56-alpha.5"}' ;;
  *api.github.com*)
    [ -f "${RELEASES_FILE:-}" ] || exit 22
    cat "$RELEASES_FILE" ;;
  *) echo 'printf "%s\n" "$@" > "$INSTALLER_ARGS"' ;;
esac
STUB
chmod +x "$TMP/bin/curl"
run_shim() {
  : > "$TMP/curl.log"
  : > "$TMP/installer-args"
  env -u COMMAVIEWD_COMMA_INSTALL_URL -u COMMAVIEWD_INSTALLER_RAW_BASE -u COMMAVIEWD_INSTALLER_REF -u COMMAVIEWD_REF \
    -u COMMAVIEWD_RELEASES_API_URL -u COMMAVIEWD_CURRENT_RELEASE_URL \
    PATH="$TMP/bin:$PATH" CURL_LOG="$TMP/curl.log" INSTALLER_ARGS="$TMP/installer-args" \
    bash "$REPO_ROOT/comma4/install.sh" "$@" >/dev/null
}
shim() {
  run_shim "$@"
  tail -1 "$TMP/curl.log"
}
got="$(shim --tag v0.0.57-alpha.1)"
[ "$got" = "https://commaview.com/commaviewd/v0.0.57-alpha.1/comma/install.sh" ] || fail "shim --tag (got: $got)"
got="$(shim --tag=v0.0.57-beta)"
[ "$got" = "https://commaview.com/commaviewd/v0.0.57-beta/comma/install.sh" ] || fail "shim --tag= (got: $got)"
got="$(shim)"
[ "$got" = "https://commaview.com/commaviewd/v0.0.56-alpha.5/comma/install.sh" ] || fail "shim without a tag uses the current release (got: $got)"
grep -q "commaview.com/api/current-release" "$TMP/curl.log" || fail "shim without a tag asks for the current release"
for repo in CommaView/commaviewd RhynoTech/commaviewd; do
  got="$(COMMAVIEWD_GITHUB_REPO="$repo" shim --tag v0.0.57-alpha.1)"
  [ "$got" = "https://commaview.com/commaviewd/v0.0.57-alpha.1/comma/install.sh" ] \
    || fail "both official repository names use the hosted installer (got: $got)"
  got="$(COMMAVIEWD_GITHUB_REPO="$repo" shim)"
  [ "$got" = "https://commaview.com/commaviewd/v0.0.56-alpha.5/comma/install.sh" ] \
    || fail "both official repository names use the current release (got: $got)"
done


# Without the current release, the newest public release: a beta by version, never an alpha (GitHub
# lists the newest-created first). Its installer is told which release, so it can't pick another.
cat > "$TMP/releases.json" <<'JSON'
[
  {"tag_name": "v0.0.58-alpha.1", "prerelease": true, "draft": false},
  {"tag_name": "v0.0.55-beta", "prerelease": false, "draft": false},
  {"tag_name": "v0.0.57-beta", "prerelease": false, "draft": false},
  {"tag_name": "v0.0.56-beta", "prerelease": false, "draft": false}
]
JSON
echo '[{"tag_name": "v0.0.58-alpha.1", "prerelease": true, "draft": false}]' > "$TMP/alphas.json"
args_given() { tr '\n' ' ' < "$TMP/installer-args"; }
for repo in CommaView/commaviewd RhynoTech/commaviewd; do
  got="$(COMMAVIEWD_GITHUB_REPO="$repo" CURRENT_RELEASE_DOWN=1 RELEASES_FILE="$TMP/releases.json" shim --force-offroad)"
  [ "$got" = "https://commaview.com/commaviewd/v0.0.57-beta/comma/install.sh" ] \
    || fail "without the current release, the newest beta's installer (got: $got)"
  grep -Fq "https://api.github.com/repos/$repo/releases" "$TMP/curl.log" || fail "$repo's release list is asked"
  [ "$(args_given)" = "--force-offroad --tag v0.0.57-beta " ] || fail "the fallback names its release (got: $(args_given))"
done
got="$(CURRENT_RELEASE_DOWN=1 RELEASES_FILE="$TMP/releases.json" shim --current)"
[ "$got" = "https://commaview.com/commaviewd/v0.0.57-beta/comma/install.sh" ] || fail "--current still runs a release's installer (got: $got)"
[ "$(args_given)" = "--current " ] || fail "--current reinstalls the installed release (got: $(args_given))"
# Nothing to fall back to: only alphas, or a list that can't be read (a private repository).
for releases in "$TMP/alphas.json" "$TMP/missing.json"; do
  if CURRENT_RELEASE_DOWN=1 RELEASES_FILE="$releases" run_shim 2>"$TMP/shim-error.txt"; then
    fail "with no public release the shim must fail ($releases)"
  fi
  grep -Fq "couldn't look up the current CommaView release" "$TMP/shim-error.txt" \
    || fail "the shim should say why it stopped (got: $(cat "$TMP/shim-error.txt"))"
  [ ! -s "$TMP/installer-args" ] || fail "the shim must not run an installer without a release"
  if grep -Eq '/comma/install\.sh$|raw\.githubusercontent' "$TMP/curl.log"; then
    fail "the shim must not fetch an installer without a release"
  fi
done

echo "PASS: releases install from commaview.com; branches and other repositories from GitHub; without the current release, the newest beta or nothing"
