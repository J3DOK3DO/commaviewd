#!/usr/bin/env bash
# Checks out an upstream commit the way CI does, cached for the next run (scripts/ci-local.sh).
#
#   scripts/ci-local-checkout.sh <repo-url> <sha> <dir>
#
# Prints the checkout's path: <dir>, cleaned back to the commit. The tree is blobless and sparse
# without the driving models, submodules included. <dir>.complete marks it finished; it sits beside
# the checkout, not in it, so cleaning the tree (git clean) never drops it.
#
# The download goes into <dir>.partial-XXXXXX and moves to <dir> once complete, so an interrupted
# run never leaves a half tree where the next run would take it for a finished one. Runs sharing
# the cache take <dir>.lock while they check out; a partial whose run has ended (its .owner pid is
# gone, or, from before owners were recorded, older than two hours) is deleted.
set -euo pipefail

url="${1:?repo url}" sha="${2:?sha}" dir="${3:?checkout dir}"

clean_tree() {
  git -C "$1" reset --hard -q HEAD
  git -C "$1" clean -fdq
}

mkdir -p "$(dirname "$dir")"
exec {lock}>"$dir.lock"
flock "$lock"

# Checkouts from before the marker moved out of the tree kept it inside, where cleaning the tree
# deleted it: one at the commit with every submodule checked out is complete.
complete_without_marker() {
  [[ -f "$1/.ci-local-complete" ]] && return 0
  [[ "$(git -C "$1" rev-parse HEAD 2>/dev/null)" == "$sha" ]] || return 1
  ! git -C "$1" submodule status --recursive 2>/dev/null | grep -q '^-' \
    && git -C "$1" submodule status --recursive >/dev/null 2>&1
}
if [[ -d "$dir" && ! -f "$dir.complete" ]] && complete_without_marker "$dir"; then
  touch "$dir.complete"
fi
if [[ -d "$dir" && -f "$dir.complete" ]]; then
  clean_tree "$dir"
  echo "$dir"
  exit 0
fi

for stale in "$dir".partial-*; do
  [[ -d "$stale" ]] || continue
  if [[ -f "$stale.owner" ]]; then
    kill -0 "$(cat "$stale.owner" 2>/dev/null || echo 0)" 2>/dev/null && continue
  elif [[ -z "$(find "$stale" -maxdepth 0 -mmin +120)" ]]; then
    continue  # perhaps still being written by an older ci-local.sh
  fi
  echo "removing an interrupted checkout: $stale" >&2
  rm -rf "$stale" "$stale.owner"
done
# Moved in between the move and the marker (an interrupted run), or never finished.
if [[ -e "$dir" ]]; then
  echo "removing an incomplete checkout: $dir" >&2
  rm -rf "$dir"
fi

partial="$(mktemp -d "$dir.partial-XXXXXX")"
echo "$$" > "$partial.owner"
trap 'rm -rf "$partial" "$partial.owner"' EXIT
git -C "$partial" init -q
git -C "$partial" remote add origin "$url"
git -C "$partial" sparse-checkout set --no-cone '/*' '!/selfdrive/modeld/models/' '!/openpilot/selfdrive/modeld/models/'
git -C "$partial" fetch -q --depth 1 --filter=blob:none origin "$sha"
git -C "$partial" checkout -q FETCH_HEAD
git -C "$partial" submodule update -q --init --recursive --depth 1
mv "$partial" "$dir"
touch "$dir.complete"
rm -f "$partial.owner"
trap - EXIT
echo "$dir"
