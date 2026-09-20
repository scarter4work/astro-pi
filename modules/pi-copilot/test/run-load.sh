#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
PI=/opt/PixInsight/bin/PixInsight.sh
KEYS=/home/scarter4work/projects/keys/scarter4work_keys.xssk
PASS="$(cat /tmp/.pi_codesign_pass)"
SO="$ROOT/build/src/module/PICopilot-pxm.so"

[ -f "$SO" ] || { echo "FAIL: module not built at $SO"; exit 1; }
"$PI" --sign-module-file="$SO" --xssk-file="$KEYS" --xssk-password="$PASS"
[ -f "${SO%.so}.xsgn" ] || { echo "FAIL: signing produced no .xsgn"; exit 1; }

# Load the module headlessly and run a trivial probe script that proves PI got
# past module load. --force-exit only exits AFTER running -r= scripts, so a
# bare -m= with no -r= sits idle forever; the probe + timeout close that hole.
#
# Private, unpredictable result path (mktemp) passed via env var — same
# defense-in-depth as run-selftest.sh, rather than a fixed /tmp name.
OUT2="$(mktemp -u "${TMPDIR:-/tmp}/picopilot-load.XXXXXX.txt")"
rm -f "$OUT2"
trap 'rm -f "$OUT2"' EXIT
if ! PICOPILOT_LOAD_OUT="$OUT2" timeout 180 "$PI" -n --automation-mode --no-startup-scripts -m="$SO" -r="$HERE/load-probe.js" --force-exit; then
   echo "FAIL: PI load timed out (180s) or exited non-zero"; exit 1
fi
[ -f "$OUT2" ] || { echo "FAIL: module loaded but PI never reached the probe script (load error)"; exit 1; }
echo "PASS: module built, signed, and loaded"
