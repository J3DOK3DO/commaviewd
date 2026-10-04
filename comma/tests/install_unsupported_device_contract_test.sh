#!/usr/bin/env bash
set -euo pipefail

# install.sh refuses a comma 3 (tici) before it looks anything up, downloads or changes anything,
# and says so; a comma 3X (tizi) goes on to install. The device is faked through the devicetree
# model file, and curl through a stub that records each call and fails.
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
INSTALL_SH="$REPO_ROOT/comma/install.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail() { echo "FAIL: $1" >&2; exit 1; }

mkdir -p "$TMP/bin"
cat > "$TMP/bin/curl" <<STUB
#!/usr/bin/env bash
echo "curl \$*" >> "$TMP/curl.log"
exit 22
STUB
chmod +x "$TMP/bin/curl"

unset COMMAVIEWD_RELEASE_TAG COMMAVIEWD_DEFAULT_TAG COMMAVIEWD_INSTALLER_REF COMMAVIEWD_VERSION \
  COMMAVIEWD_RELEASE_REPO COMMAVIEWD_CURRENT_RELEASE_URL COMMAVIEWD_RELEASES_API_URL

# Runs install.sh as a device with this devicetree model; sets rc, out (stdout and stderr).
run_install() {
  local model="$1"; shift
  printf '%s\0' "$model" > "$TMP/model"
  rm -f "$TMP/curl.log"
  rc=0
  out="$(PATH="$TMP/bin:$PATH" TMPDIR="$TMP" COMMAVIEWD_DEVICE_MODEL_FILE="$TMP/model" \
    COMMAVIEWD_DEFERRED_DIR="$TMP/deferred" COMMAVIEWD_BACKUP_ROOT="$TMP/backups" \
    bash "$INSTALL_SH" "$@" 2>"$TMP/stderr")" || rc=$?
  stdout="$out"
  out="$out
$(cat "$TMP/stderr")"
}

for args in "" "--tag v0.0.55-alpha" "--current" "--force-offroad"; do
  # shellcheck disable=SC2086
  run_install "comma tici" $args
  [[ $rc -eq 43 ]] || fail "install.sh $args on a comma 3 should exit 43, got $rc: $out"
  grep -Fq "CommaView doesn't support the comma 3 (tici)" <<<"$out" || fail "install.sh $args should say the comma 3 isn't supported: $out"
  grep -Fq "comma 3X and comma four are supported" <<<"$out" || fail "install.sh $args should name the supported devices: $out"
  # What the app reads from the SSH install's output (stdout): the marker line, and the sentence.
  grep -Fxq "COMMAVIEW_UNSUPPORTED_DEVICE=tici" <<<"$stdout" || fail "install.sh $args should print COMMAVIEW_UNSUPPORTED_DEVICE=tici on its own line to stdout: $stdout"
  grep -Fq "CommaView doesn't support the comma 3 (tici)" <<<"$stdout" || fail "install.sh $args should say so on stdout: $stdout"
  [[ ! -e "$TMP/curl.log" ]] || fail "install.sh $args on a comma 3 should refuse before any lookup or download: $(cat "$TMP/curl.log")"
done

run_install "comma tici" --help
[[ $rc -eq 0 ]] || fail "install.sh --help should still work on a comma 3, got $rc: $out"

run_install "comma tizi" --tag v0.0.55-alpha
[[ $rc -ne 43 ]] || fail "install.sh should not refuse a comma 3X: $out"
grep -Fq "COMMAVIEW_UNSUPPORTED_DEVICE" <<<"$out" && fail "install.sh should not refuse a comma 3X: $out"
[[ -s "$TMP/curl.log" ]] || fail "install.sh on a comma 3X should go on to fetch the release"

grep -Fq 'refuse_unsupported_device "$@"' "$INSTALL_SH" || fail "install.sh should check the device"
# The check runs before the default release lookup (the first thing that reaches the network).
check_line="$(grep -n '^refuse_unsupported_device "\$@"' "$INSTALL_SH" | cut -d: -f1)"
lookup_line="$(grep -n '^  resolve_release_inputs$' "$INSTALL_SH" | tail -n1 | cut -d: -f1)"
[[ "$check_line" -lt "$lookup_line" ]] || fail "install.sh should refuse a comma 3 before looking up a release"
# uninstall.sh has no device check: an earlier install on a comma 3 can still be removed.
! grep -Fq 'tici' "$REPO_ROOT/comma/uninstall.sh" || fail "uninstall.sh should work on every device"

echo "PASS: install.sh refuses the comma 3 (tici) up front and installs on the comma 3X"
