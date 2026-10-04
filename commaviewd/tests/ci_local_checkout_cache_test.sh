#!/usr/bin/env bash
set -euo pipefail

# scripts/ci-local-checkout.sh keeps a finished upstream checkout across runs (cleaning the tree
# doesn't drop its marker), adopts one an older ci-local.sh left without a marker, and clears the
# partial downloads interrupted runs left behind. The upstream is a local repository.
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CHECKOUT="$REPO_ROOT/scripts/ci-local-checkout.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail() { echo "FAIL: $1" >&2; exit 1; }

export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=protocol.file.allow GIT_CONFIG_VALUE_0=always
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@example.invalid GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@example.invalid
quiet() { "$@" >/dev/null 2>&1; }

# An upstream with a submodule and a driving model the sparse checkout leaves out.
make_repo() {
  quiet git init -q "$1"
  git -C "$1" config uploadpack.allowFilter true
  git -C "$1" config uploadpack.allowAnySHA1InWant true
}
make_repo "$TMP/sub"
echo sub > "$TMP/sub/file"
quiet git -C "$TMP/sub" add . && quiet git -C "$TMP/sub" commit -m sub
make_repo "$TMP/upstream"
mkdir -p "$TMP/upstream/selfdrive/modeld/models" "$TMP/upstream/selfdrive/ui"
echo model > "$TMP/upstream/selfdrive/modeld/models/big.onnx"
echo ui > "$TMP/upstream/selfdrive/ui/ui_state.py"
quiet git -C "$TMP/upstream" submodule add "file://$TMP/sub" sub
quiet git -C "$TMP/upstream" add . && quiet git -C "$TMP/upstream" commit -m upstream
sha="$(git -C "$TMP/upstream" rev-parse HEAD)"
url="file://$TMP/upstream"
cache="$TMP/cache/upstream"
dir="$cache/$sha"

out="$("$CHECKOUT" "$url" "$sha" "$dir")"
[[ "$out" == "$dir" ]] || fail "should print the checkout ($out)"
[[ -f "$dir.complete" ]] || fail "a finished checkout should be marked complete beside it"
[[ -f "$dir/selfdrive/ui/ui_state.py" && -f "$dir/sub/file" ]] || fail "the checkout should have the tree and its submodules"
[[ ! -e "$dir/selfdrive/modeld/models/big.onnx" ]] || fail "the checkout should leave the driving models out"
compgen -G "$dir.partial-*" >/dev/null && fail "a finished checkout should leave no partial behind"

# What ci-local.sh does after the patch check, which used to delete the marker.
touch "$dir/.git/reused"
echo patched >> "$dir/selfdrive/ui/ui_state.py"
echo helper > "$dir/selfdrive/ui/commaview_export.py"
git -C "$dir" reset --hard -q HEAD
git -C "$dir" clean -fdq
echo patched-again >> "$dir/selfdrive/ui/ui_state.py"
"$CHECKOUT" "$url" "$sha" "$dir" >/dev/null
[[ -f "$dir/.git/reused" ]] || fail "a cached checkout should be reused after its tree was cleaned"
[[ "$(cat "$dir/selfdrive/ui/ui_state.py")" == ui ]] || fail "a reused checkout should be reset to the commit"

# An older ci-local.sh kept the marker in the tree, and git clean deleted it.
rm "$dir.complete"
"$CHECKOUT" "$url" "$sha" "$dir" >/dev/null
[[ -f "$dir/.git/reused" && -f "$dir.complete" ]] || fail "a complete checkout without a marker should be adopted, not downloaded again"

# Partials interrupted runs left: a dead owner's and an unowned old one go; a fresh unowned one
# (an older ci-local.sh may still be writing it) and a live owner's stay.
dir2="$cache/second-$sha"
mkdir -p "$dir2.partial-dead" "$dir2.partial-old" "$dir2.partial-fresh" "$dir2.partial-live"
( exit 0 ) & dead=$!; wait "$dead"
echo "$dead" > "$dir2.partial-dead.owner"
echo "$$" > "$dir2.partial-live.owner"
touch -d '3 hours ago' "$dir2.partial-old"
mkdir -p "$dir2"  # moved in, but the run ended before marking it complete
"$CHECKOUT" "$url" "$sha" "$dir2" >/dev/null 2>"$TMP/err"
[[ ! -e "$dir2.partial-dead" && ! -e "$dir2.partial-dead.owner" ]] || fail "a partial whose run ended should be removed"
[[ ! -e "$dir2.partial-old" ]] || fail "an unowned partial older than two hours should be removed"
[[ -d "$dir2.partial-fresh" && -d "$dir2.partial-live" ]] || fail "a partial that may still be written should be kept"
[[ -f "$dir2.complete" && -f "$dir2/selfdrive/ui/ui_state.py" ]] || fail "an incomplete checkout should be replaced by a complete one"

# A failed download leaves nothing behind.
dir3="$cache/missing"
if "$CHECKOUT" "$url" 0123456789012345678901234567890123456789 "$dir3" >/dev/null 2>&1; then
  fail "a commit the upstream doesn't have should fail"
fi
compgen -G "$dir3.partial-*" >/dev/null && fail "a failed download should remove its partial"
[[ ! -e "$dir3" && ! -e "$dir3.complete" ]] || fail "a failed download should leave no checkout"

echo "PASS: ci-local.sh reuses finished upstream checkouts and clears interrupted ones"
