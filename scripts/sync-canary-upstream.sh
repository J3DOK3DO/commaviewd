#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/sync-canary-upstream.sh <openpilot|sunnypilot> <ref> [dest-root]

Materialize a local upstream checkout that matches commaviewd canary workflow shape:
- official upstream GitHub repo
- current resolved ref SHA
- checkout path named openpilot-src
- recursive submodules initialized

Defaults:
- dest-root: ~/.cache/commaviewd-canary/<upstream>-<ref>
- checkout path: <dest-root>/openpilot-src

Supported refs: the canaries' (ci/canary-*.json) and CI's staging refs (ci/targets.json):
- openpilot: nightly, nightly-dev, nightly-chestnut, nightly-chestnut-dev, release-mici-staging, release-tizi-staging, release-chestnut-staging
- sunnypilot: dev, dev-chestnut, release-mici-staging, release-tizi-staging, staging-chestnut
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" || $# -lt 2 || $# -gt 3 ]]; then
  usage
  [[ $# -ge 2 ]] && exit 1 || exit 0
fi

upstream="$1"
ref="$2"
dest_root="${3:-$HOME/.cache/commaviewd-canary/${upstream}-${ref}}"
checkout_path="$dest_root/openpilot-src"
meta_path="$dest_root/source.env"

case "$upstream" in
  openpilot)
    repo="https://github.com/commaai/openpilot.git"
    case "$ref" in
      nightly|nightly-dev|nightly-chestnut|nightly-chestnut-dev|release-mici-staging|release-tizi-staging|release-chestnut-staging) ;;
      *) echo "ERROR: unsupported openpilot ref '$ref' (expected nightly, nightly-dev, nightly-chestnut, nightly-chestnut-dev, release-mici-staging, release-tizi-staging, or release-chestnut-staging)" >&2; exit 2 ;;
    esac
    ;;
  sunnypilot)
    repo="https://github.com/sunnypilot/sunnypilot.git"
    case "$ref" in
      dev|dev-chestnut|release-mici-staging|release-tizi-staging|staging-chestnut) ;;
      *) echo "ERROR: unsupported sunnypilot ref '$ref' (expected dev, dev-chestnut, release-mici-staging, release-tizi-staging, or staging-chestnut)" >&2; exit 2 ;;
    esac
    ;;
  *)
    echo "ERROR: unsupported upstream '$upstream'" >&2
    exit 2
    ;;
esac

resolve_sha() {
  local ref="$1"
  local sha
  sha="$(git ls-remote "$repo" "refs/heads/${ref}" | awk '{print $1}')"
  if [[ -z "$sha" ]]; then
    sha="$(git ls-remote "$repo" "refs/tags/${ref}" | awk '{print $1}')"
  fi
  if [[ -z "$sha" ]]; then
    sha="$ref"
  fi
  printf '%s\n' "$sha"
}

sha="$(resolve_sha "$ref")"
mkdir -p "$dest_root"

if [[ ! -d "$checkout_path/.git" ]]; then
  git clone --no-checkout "$repo" "$checkout_path" >/dev/null 2>&1
else
  current_remote="$(git -C "$checkout_path" remote get-url origin 2>/dev/null || true)"
  if [[ "$current_remote" != "$repo" ]]; then
    echo "ERROR: existing checkout at $checkout_path points at $current_remote, expected $repo" >&2
    exit 2
  fi
fi

git -C "$checkout_path" remote set-url origin "$repo"
git -C "$checkout_path" fetch --depth 1 origin "$sha" >/dev/null 2>&1
git -C "$checkout_path" checkout --detach --force FETCH_HEAD >/dev/null 2>&1
git -C "$checkout_path" submodule sync --recursive >/dev/null 2>&1 || true
git -C "$checkout_path" submodule update --init --recursive --depth 1 >/dev/null 2>&1 || true

cat > "$meta_path" <<META
UPSTREAM=$upstream
REF=$ref
SHA=$sha
REPO=$repo
CHECKOUT_PATH=$checkout_path
META

printf 'UPSTREAM=%s\nREF=%s\nSHA=%s\nREPO=%s\nCHECKOUT_PATH=%s\n' \
  "$upstream" "$ref" "$sha" "$repo" "$checkout_path"
