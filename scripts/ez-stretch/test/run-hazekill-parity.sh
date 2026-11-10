#!/usr/bin/env bash
# EZHazeKill parity: runs the shipped EZHazeKill.js headless in PixInsight on the
# design study's test images and compares every output sample with the offline
# reference (modules/ez-tools/test/hk_lib.py), then recomputes the design's metrics on PixInsight's output.
#
#   bash scripts/ez-stretch/test/run-hazekill-parity.sh [case ...]   -> PASS (exit 0) / FAIL (exit 1)
#
# Cases (default: all whose source exists): L1_LDN1235_Full_linear L2_M33_Lqef_linear
# L3_NGC7000_HaO3_linear N1_M33_Scott_GHS_stretched N2_IC1396_HOO_stretched
# N3_NGC7000_HaO3_STFstretched M1_M33_Lqef_G_mono_linear. Sources are only read (NAS masters, see hazekill_parity.py).
# HK_RUN_DIR=<dir> keeps inputs, outputs and report.json there (default: a temporary
# directory, removed on exit). Needs python3 with numpy, scipy and xisf.
#
# PixInsight runs through pi_headless (private Xvfb) in the isolated settings slot
# HK_TEST_SLOT (default 98), whose settings file is removed before and after the run.
# No module is loaded with -m= and nothing is signed: the probe runs the script source.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../../../tools/pi-headless.sh"
PI="${HK_PI:-/opt/PixInsight/bin/PixInsight.sh}"

slot="${HK_TEST_SLOT:-98}"
case "$slot" in ''|*[!0-9]*) echo "FAIL: HK_TEST_SLOT must be a plain decimal integer"; exit 1 ;; esac
(( 10#$slot >= 50 && 10#$slot <= 256 )) || { echo "FAIL: HK_TEST_SLOT must be in [50,256]"; exit 1; }
slot=$(( 10#$slot ))
if pgrep -f -- "PixInsight.* -n=$slot( |$)" >/dev/null; then
   echo "FAIL: a PixInsight is already running on slot $slot"; exit 1
fi
SETTINGS="$(printf '%s/core-%03d-pxi.settings' "$HOME/.PixInsight" "$slot")"

if [ -n "${HK_RUN_DIR:-}" ]; then
   RUN="$HK_RUN_DIR"; mkdir -p "$RUN"; OWN_RUN=0
else
   RUN="$(mktemp -d "${TMPDIR:-/tmp}/hazekill-parity.XXXXXX")"; OWN_RUN=1
fi
cleanup() {
   rm -f -- "$SETTINGS"
   if [ "$OWN_RUN" = 1 ]; then rm -rf -- "${RUN:?}"; fi
}
trap cleanup EXIT
rm -f -- "$SETTINGS"

echo "== prepare inputs ($RUN) =="
python3 "$HERE/../../../modules/ez-tools/test/hazekill_parity.py" prep "$RUN" "$@"

echo "== EZHazeKill.js in PixInsight (slot $slot) =="
rm -f "$RUN/probe-out.json"
# --force-exit only exits after the -r= script ran; timeout fails a hang loudly.
if ! HK_PROBE_IN="$RUN/cases.json" HK_PROBE_OUT="$RUN/probe-out.json" pi_headless \
        timeout 900 "$PI" -n="$slot" --automation-mode --no-startup-scripts --no-startup-check-updates \
        --no-startup-gui-messages -r="$HERE/hazekill-probe.js" --force-exit >"$RUN/pi.log" 2>&1; then
   echo "FAIL: PixInsight timed out (900 s) or exited non-zero"; tail -20 "$RUN/pi.log"; exit 1
fi
[ -s "$RUN/probe-out.json" ] || { echo "FAIL: PixInsight never wrote the probe result (script parse failure?)"; tail -20 "$RUN/pi.log"; exit 1; }

echo "== compare with hk_lib.py =="
python3 "$HERE/../../../modules/ez-tools/test/hazekill_parity.py" check "$RUN"
