#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
PI=/opt/PixInsight/bin/PixInsight.sh
KEYS=/home/scarter4work/projects/keys/scarter4work_keys.xssk
PASS="$(cat /tmp/.pi_codesign_pass)"
SO="$ROOT/build/src/module/PICopilot-pxm.so"

# -n (no slot number) claims the first free instance slot -- slot 1 when the
# GUI isn't running -- whose settings file IS the user's real
# ~/.PixInsight/core-001-pxi.settings. Loading a dev-build .so there rewrites
# that file's persisted Modules list (dev path inserted, the repo-installed
# @pxi_bin_dir/PICopilot-pxm.so entry dropped), which then breaks the user's
# real install next time he launches PI normally ("Duplicate MetaProcess
# identifier"). Pin every headless test run to one fixed, otherwise-unused
# slot instead, and wipe that slot's settings before AND after each run so
# every run starts hermetic and never accumulates dev-only Modules state.
PICOPILOT_TEST_SLOT="${PICOPILOT_TEST_SLOT:-90}"
SLOT_SETTINGS="$(printf '%s/core-%03d-pxi.settings' "$HOME/.PixInsight" "$PICOPILOT_TEST_SLOT")"
rm -f "$SLOT_SETTINGS"

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
trap 'rm -f "$OUT2" "$SLOT_SETTINGS"' EXIT
if ! PICOPILOT_LOAD_OUT="$OUT2" timeout 180 "$PI" -n="$PICOPILOT_TEST_SLOT" --automation-mode --no-startup-scripts -m="$SO" -r="$HERE/load-probe.js" --force-exit; then
   echo "FAIL: PI load timed out (180s) or exited non-zero"; exit 1
fi
[ -f "$OUT2" ] || { echo "FAIL: module loaded but PI never reached the probe script (load error)"; exit 1; }
echo "PASS: module built, signed, and loaded"
