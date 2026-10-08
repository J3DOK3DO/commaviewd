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
cat > "$TMP/bin/curl" <<'STUB'
#!/usr/bin/env bash
for a in "$@"; do last="$a"; done
echo "$last" >> "$CURL_LOG"
case "$last" in
  *current-release*) echo '{"appTag":"app-v1","runtimeTag":"v0.0.56-alpha.5"}' ;;
  *) echo 'true' ;;
esac
STUB
chmod +x "$TMP/bin/curl"
shim() {
  : > "$TMP/curl.log"
  env -u COMMAVIEWD_COMMA_INSTALL_URL -u COMMAVIEWD_INSTALLER_RAW_BASE -u COMMAVIEWD_INSTALLER_REF -u COMMAVIEWD_REF \
    PATH="$TMP/bin:$PATH" CURL_LOG="$TMP/curl.log" bash "$REPO_ROOT/comma4/install.sh" "$@" >/dev/null
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

echo "PASS: releases install from commaview.com; branches and other repositories from GitHub"
