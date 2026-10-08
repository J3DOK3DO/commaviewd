#!/usr/bin/env bash
set -euo pipefail

# The installer shim: commaview.com/commaviewd/install.sh serves it (the newest release
# publishes it), and app versions that still fetch /comma4/install.sh reach it too. It runs the
# asked-for release's own installer (--tag), else the current CommaView release's runtime's.
# Keep it small.
shim_ref="${COMMAVIEWD_INSTALLER_REF:-${COMMAVIEWD_REF:-}}"
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
