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

# Guard: PICOPILOT_TEST_SLOT is env-controlled (typo/override risk), and this
# script deletes whatever settings file its value resolves to. Slots 1-49 are
# where a normal PI instance (or another tool) lives -- slot 1 in particular
# IS the user's real ~/.PixInsight/core-001-pxi.settings -- so a bad value
# must never resolve there. Reject anything that isn't a plain unsigned
# decimal integer BEFORE any arithmetic touches it: bash's own $(( )) and
# printf %d both reinterpret a leading "0x" as hex and a leading "0" as
# octal, so "1", "01", and "0x1" would all otherwise collide with slot 1.
case "$PICOPILOT_TEST_SLOT" in
   ''|*[!0-9]*)
      echo "FAIL: PICOPILOT_TEST_SLOT must be a plain decimal integer, got '$PICOPILOT_TEST_SLOT'"; exit 1
      ;;
esac
# Force base-10 interpretation (10#...) so a leading zero can't be read as
# octal, then require the reserved test range: PI's own -n slot range is
# [1,256] (PixInsight.sh --help), and 1-49 are left for real/other instances.
if (( 10#$PICOPILOT_TEST_SLOT < 50 || 10#$PICOPILOT_TEST_SLOT > 256 )); then
   echo "FAIL: PICOPILOT_TEST_SLOT must be in [50,256] (reserved for tests), got '$PICOPILOT_TEST_SLOT'"; exit 1
fi
PICOPILOT_TEST_SLOT=$(( 10#$PICOPILOT_TEST_SLOT ))

SLOT_SETTINGS="$(printf '%s/core-%03d-pxi.settings' "$HOME/.PixInsight" "$PICOPILOT_TEST_SLOT")"
rm -f "$SLOT_SETTINGS"
trap 'rm -f "$SLOT_SETTINGS"' EXIT

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
# Private virtual display (Xvfb). A core-side rejection can raise a MODAL
# dialog that no module API can suppress or catch (Task 1: "PixelMath: Invalid
# table row index"); on the user's real DISPLAY that dialog would block his
# desktop. Under Xvfb it is invisible, and it just blocks this run until the
# timeout fails it loudly. timeout sits INSIDE xvfb-run so that, on expiry,
# xvfb-run still tears down the Xvfb server (which also takes down any
# PixInsight process the PixInsight.sh wrapper left behind).
command -v xvfb-run >/dev/null 2>&1 || { echo "FAIL: xvfb-run not found (needed to keep dialogs off the real display)"; exit 1; }
if ! PICOPILOT_LOAD_OUT="$OUT2" xvfb-run -a -s "-screen 0 1920x1080x24" \
        timeout 180 "$PI" -n="$PICOPILOT_TEST_SLOT" --automation-mode --no-startup-scripts -m="$SO" -r="$HERE/load-probe.js" --force-exit; then
   echo "FAIL: PI load timed out (180s) or exited non-zero"; exit 1
fi
[ -f "$OUT2" ] || { echo "FAIL: module loaded but PI never reached the probe script (load error)"; exit 1; }
echo "PASS: module built, signed, and loaded"
