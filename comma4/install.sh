#!/usr/bin/env bash
set -euo pipefail

# The installer shim: commaview.com/commaviewd/install.sh serves it (the newest release
# publishes it), and app versions that still fetch /comma4/install.sh reach it too. It runs the
# asked-for release's own installer (--tag), else the current CommaView release's runtime's.
# When the current release can't be looked up, the newest public release's (a beta, never an
# alpha), asked to install itself; when that can't be found either, nothing.
# Keep it small.
shim_ref="${COMMAVIEWD_INSTALLER_REF:-${COMMAVIEWD_REF:-}}"
current_asked=0
args=("$@")
for ((i = 0; i < ${#args[@]}; i++)); do
  case "${args[$i]}" in
    --tag)
      if (( i + 1 < ${#args[@]} )); then
        shim_ref="${args[$((i + 1))]}"
      fi
      ;;
    --tag=*)
      shim_ref="${args[$i]#--tag=}"
      ;;
    --current)
      current_asked=1
      ;;
  esac
done

SCRIPT_URL="${COMMAVIEWD_COMMA_INSTALL_URL:-}"
if [[ -z "$SCRIPT_URL" ]]; then
  if [[ -n "${COMMAVIEWD_INSTALLER_RAW_BASE:-}" ]]; then
    SCRIPT_URL="${COMMAVIEWD_INSTALLER_RAW_BASE%/}/install.sh"
  else
    GITHUB_REPO="${COMMAVIEWD_GITHUB_REPO:-CommaView/commaviewd}"
    OFFICIAL_REPO=0
    case "$GITHUB_REPO" in
      CommaView/commaviewd|RhynoTech/commaviewd) OFFICIAL_REPO=1 ;;
    esac
    RELEASES_ORIGIN="${COMMAVIEWD_RELEASES_ORIGIN:-https://commaview.com/commaviewd}"
    INSTALLER_REF="$shim_ref"
    # No release asked for: the runtime the current CommaView release uses.
    if [[ -z "$INSTALLER_REF" && "$OFFICIAL_REPO" == "1" ]]; then
      INSTALLER_REF="$(curl -fsL --max-time 10 --retry 2 "${COMMAVIEWD_CURRENT_RELEASE_URL:-https://commaview.com/api/current-release}" \
        | tr -d '\r\n' | sed -nE 's/.*"runtimeTag"[[:space:]]*:[[:space:]]*"(v[^"]+)".*/\1/p' || true)"
      if [[ -z "$INSTALLER_REF" ]]; then
        # GitHub lists releases newest-created first, alphas included: pick the newest beta.
        RELEASES_API_URL="${COMMAVIEWD_RELEASES_API_URL:-https://api.github.com/repos/${GITHUB_REPO}/releases?per_page=100}"
        INSTALLER_REF="$(curl -fsSL --max-time 20 --retry 2 "$RELEASES_API_URL" 2>/dev/null | python3 -c '
import json, re, sys
try:
    releases = json.load(sys.stdin)
except ValueError:
    releases = []
betas = [(tuple(map(int, m.groups())), r["tag_name"]) for r in (releases if isinstance(releases, list) else [])
         if isinstance(r, dict) and not r.get("draft") and not r.get("prerelease")
         for m in [re.fullmatch(r"v(\d+)\.(\d+)\.(\d+)-beta", str(r.get("tag_name")))] if m]
print(max(betas)[1] if betas else "")
' 2>/dev/null || true)"
        if [[ "$INSTALLER_REF" != v* ]]; then
          echo "ERROR: couldn't look up the current CommaView release, or the newest public release at $RELEASES_API_URL. Nothing was installed. Try again later, or name a release with --tag <tag>." >&2
          exit 1
        fi
        # That release's installer would look the default up again: name the release.
        [[ "$current_asked" == "1" ]] || set -- "$@" --tag "$INSTALLER_REF"
      fi
    fi
    if [[ "$OFFICIAL_REPO" == "1" && "$INSTALLER_REF" == v* ]]; then
      # A release's installer, published with it on commaview.com.
      SCRIPT_URL="${RELEASES_ORIGIN%/}/${INSTALLER_REF}/comma/install.sh"
    else
      # A branch while developing, or another repository's release.
      SCRIPT_URL="https://raw.githubusercontent.com/${GITHUB_REPO}/${INSTALLER_REF:-main}/comma/install.sh"
    fi
  fi
fi

curl -fsSL "$SCRIPT_URL" | bash -s -- "$@"
