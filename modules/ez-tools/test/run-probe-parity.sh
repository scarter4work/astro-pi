#!/usr/bin/env bash
# EZTools EZHazeKill parity (plan Task 3): signs the module, loads it with -m= in the isolated
# settings slot 99 on a private Xvfb display (tools/pi-headless.sh), runs the EZHazeKill process on
# the design study's images plus three module-only cases, and compares every output sample with the
# offline reference (hk_lib.hk_v11) -- and, unless EZT_SCRIPT_PARITY=0, with the EZHazeKill.js 1.1.0
# script's own output from its harness (scripts/ez-stretch/test/run-hazekill-parity.sh, slot 98).
#
#   bash modules/ez-tools/test/run-probe-parity.sh [case ...]   -> PASS (exit 0) / FAIL (exit 1)
#                                                                 BLOCKED (exit 2): no signing pass file
#
# EZTOOLS_SO overrides the module (default: the host build, build/src/module/EZTools-pxm.so; the
# container build is modules/ez-tools/build-portable/src/module/EZTools-pxm.so).
# EZT_KEEP_DIR=<dir> keeps inputs, outputs, logs and report.json there (default: a private temporary
# directory, removed on exit). Sources are only read (see hazekill_parity.py). Needs python3 with
# numpy, scipy and xisf.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$ROOT/../.." && pwd)"
. "$HERE/harness.sh"

ezt_require_signing_pass || exit 2
SO="${EZTOOLS_SO:-$ROOT/build/src/module/EZTools-pxm.so}"
[ -f "$SO" ] || { echo "FAIL: module not built at $SO"; exit 1; }
SO="$(realpath -e "$SO")"
ezt_check_so "$SO" || exit 1

EZT_TEST_SLOT="${EZT_TEST_SLOT:-99}"
ezt_slot_setup || exit 1
ezt_private_tmp || exit 1
RUN="${EZT_KEEP_DIR:-$EZT_RUN_DIR/run}"
mkdir -p "$RUN"

echo "== prepare inputs ($RUN) =="
python3 "$HERE/hazekill_parity.py" prep "$RUN" --extra "$@"

echo "== sign $SO =="
ezt_sign "$SO" || exit 1
ezt_seed_slot_modules || exit 1

echo "== EZHazeKill process in PixInsight (slot $EZT_SLOT) =="
rm -f "$RUN/probe-out.json"
# --force-exit only exits after the -r= script ran; timeout fails a hang loudly.
if ! EZT_PROBE_IN="$RUN/cases.json" EZT_PROBE_OUT="$RUN/probe-out.json" pi_headless \
        timeout 900 "$PI" -n="$EZT_SLOT" --automation-mode --no-startup-scripts --no-startup-check-updates \
        --no-startup-gui-messages -m="$SO" -r="$HERE/probe-parity.js" --force-exit >"$RUN/pi.log" 2>&1; then
   echo "FAIL: PixInsight timed out (900 s) or exited non-zero"; tail -20 "$RUN/pi.log"; exit 1
fi
[ -s "$RUN/probe-out.json" ] || { echo "FAIL: PixInsight never wrote the probe result (module not loaded?)"; tail -20 "$RUN/pi.log"; exit 1; }

SCRIPT_ARGS=()
if [ "${EZT_SCRIPT_PARITY:-1}" = 1 ]; then
   echo "== EZHazeKill.js 1.1.0 harness (slot 98) on the same cases =="
   mkdir -p "$RUN/script"
   std=()
   for a in "$@"; do case "$a" in uint16_rgb|preview_target|nan_refused) ;; *) std+=("$a") ;; esac; done
   if ! HK_RUN_DIR="$RUN/script" bash "$REPO/scripts/ez-stretch/test/run-hazekill-parity.sh" ${std[@]+"${std[@]}"} \
           >"$RUN/script.log" 2>&1; then
      sed 's/^/  [script] /' "$RUN/script.log"; echo "FAIL: the script's own parity harness failed"; exit 1
   fi
   sed 's/^/  [script] /' "$RUN/script.log"
   SCRIPT_ARGS=(--script-run "$RUN/script")
fi

echo "== compare with hk_lib.py =="
python3 "$HERE/hazekill_parity.py" check "$RUN" ${SCRIPT_ARGS[@]+"${SCRIPT_ARGS[@]}"}
