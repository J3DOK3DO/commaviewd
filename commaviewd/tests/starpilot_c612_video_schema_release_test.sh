#!/usr/bin/env bash
# Dedicated CommaView StarPilot C6-12 video schema release guard.
set -euo pipefail
: "${C612_COMMA_BIN:?Set C612_COMMA_BIN to the staged ARM64 bridge executable}"
: "${C612_SERVICES_PY:?Set C612_SERVICES_PY to C6-12 cereal/services.py}"
[[ -f "$C612_COMMA_BIN" && -f "$C612_SERVICES_PY" ]] || exit 2
grep -Eq '["\x27]roadEncodeData["\x27]' "$C612_SERVICES_PY"
grep -Eq '["\x27]driverEncodeData["\x27]' "$C612_SERVICES_PY"
! grep -Eq '["\x27]narrowRoadEncodeData["\x27]' "$C612_SERVICES_PY"
dump="$(mktemp)"
trap 'rm -f "$dump"' EXIT
strings "$C612_COMMA_BIN" > "$dump"
grep -Fxq 'roadEncodeData' "$dump"
grep -Fxq 'driverEncodeData' "$dump"
grep -Fxq 'wideRoadEncodeData' "$dump"
! grep -Fxq 'narrowRoadEncodeData' "$dump"
readelf -h "$C612_COMMA_BIN" | grep -Fq 'Machine:                           AArch64'
readelf -d "$C612_COMMA_BIN" | grep -Fq '[libzmq.so.5]'
readelf -d "$C612_COMMA_BIN" | grep -Fq '$ORIGIN/lib'
echo 'PASS: verified C6-12 video schema, ARM64, ZMQ and bundled-library RPATH'
