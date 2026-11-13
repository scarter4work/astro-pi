#!/usr/bin/env bash
# GaiaDepthGrade GUI smoke (plan Task 17): the GaiaDepthGrade interface in a REAL PixInsight GUI
# on a private Xvfb display, isolated slot 97 (GDG_GUI_SLOT), driven with XTest (gui_drive.py),
# on the HaO3 NAS master. Test-only, never packaged. The display, launch and quit are
# harness.sh's gdg_gui_* (adapted from Planetary's planetary_gui_*).
#
#   bash modules/gaia-depth-grade/test/gui-smoke.sh <out-dir>   -> PASS (exit 0) / FAIL (exit 1)
#                                                                BLOCKED (exit 2): no signing pass file
#
# 0. gui-smoke-setup.js (-r): a 64x64 window gdg_gui_dummy (the active window), then the interface
#    opened from a default instance.
# Steps (gui_drive.py; screenshots in <out-dir>):
#  1. controls_and_defaults        every control's value / label / range, the group labels and the
#                                  Offline tooltip verbatim, buttons, sizes (760x420, 256); the default
#                                  target = the active window; the empty preview's orange border on
#                                  screen; Advanced expanded and collapsed; Browse... opens the master
#                                  -> 01-opened, 01b-advanced, 01c-browse-dialog, 02-browsed
#  2. prepare_enables_preview      Prepare: the QA line, preview + inset painted, Preview/Execute
#                                  enabled, the inset at the image centre       -> 03-prepared
#                                  IPC: no <target>_gdgwork window survives Prepare (Review Focus 4)
#  3. slider_marks_preview_stale   Brightness slider: orange border; Preview / Update clears it
#                                  -> 04-preview-stale, 05-preview-updated
#  4. detect_edit_marks_prepare_stale  detect_threshold_sigma 7: Preview/Execute disabled (and a click
#                                  on Execute ignored); back to 6, Prepare again -> 06, 07
#  5. inset_click_moves_center     three clicks on the preview: centre, top-left, bottom-right clamps
#                                  -> 08, 09, 10
#  6. execute_creates_depthgraded  Execute -> "Done: created <id>_depthgraded"  -> 11-executed
#                                  IPC: <id>_depthgraded is a window: 32-bit float, the target's
#                                  geometry, DEPTHTAG = the honesty text, a solution; no _gdgwork
#  (6b. online_prepare_live        only with GDG_LIVE_TAP=1: Offline unchecked, Prepare against the
#                                  ESA archive, from the interface (plan C14) -> 14-online-prepared)
#  7. close_target_then_execute    IPC: the target force-closed; Execute: "ERROR: The target image
#                                  '<id>' was closed after Prepare. Select a target and Prepare again.",
#                                  no crash; Prepare: the same    -> 12-target-closed, 13-prepare-closed
#  8. close_button                 the Close button hides the interface (F9)    -> 15-closed
#  Then every window force-closed (IPC) and File > Quit: PixInsight must exit 0 within 60 s.
#
# The module under test is a HOST build with the test hooks (GDG_BUILD_PROBES: the interface's
# state file), signed and loaded with -m= (GDG_SO overrides). Needs the NAS master
# (GDG_PROC_MASTER, run-probe.sh's), a configured local Gaia DR3 (discovered as the module does)
# and StarXTerminator.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/harness.sh"

OUT="${1:?usage: gui-smoke.sh <out-dir>}"
gdg_require_signing_pass || exit 2

MASTER_DIR="${GDG_MASTER_DIR:-/mnt/qnap/astro_data/processing-archive-2026-10-03/master}"
MASTER="${GDG_PROC_MASTER:-$MASTER_DIR/masterLight_BIN-1_3840x2160_EXPOSURE-120.00s_FILTER-HaO3_RGB.xisf}"
HONESTY='GAIA depth-derived; physically motivated, not per-pixel correct for gas'
SO="${GDG_SO:-$ROOT/build/src/module/GaiaDepthGrade-pxm.so}"
[ -f "$SO" ] || { echo "FAIL: module not built at $SO (host build with -DGDG_BUILD_PROBES=ON)"; exit 1; }
SO="$(realpath -e "$SO")"
gdg_check_so "$SO" || exit 1
grep -q GaiaDepthGradeProbe "$SO" || { echo "FAIL: $SO has no test hooks (build with -DGDG_BUILD_PROBES=ON)"; exit 1; }
[ -f "$MASTER" ] || { echo "FAIL: master not found: $MASTER (NAS mounted?)"; exit 1; }
for tool in jq python3; do command -v "$tool" >/dev/null || { echo "FAIL: $tool not found"; exit 1; }; done
mkdir -p "$OUT"
OUT="$(realpath -e "$OUT")"
rm -f "$OUT"/*.png "$OUT"/*.json "$OUT"/driver.log "$OUT"/target.txt "$OUT"/steps.txt

# Large images go to a disk-backed run directory, never the RAM-backed /tmp.
export TMPDIR="${GDG_TMPDIR:-$ROOT/build/tmp}"
mkdir -p "$TMPDIR"
gdg_gui_setup || exit 1
gdg_sign "$SO" || exit 1
gdg_seed_slot_modules || exit 1

STEPS=( controls_and_defaults prepare_enables_preview slider_marks_preview_stale detect_edit_marks_prepare_stale
        inset_click_moves_center execute_creates_depthgraded close_target_then_execute )
result(){ printf '%-34s %s\n' "$1" "$2" | tee -a "$OUT/steps.txt"; }

# Every URL the module's TapClient opens (test builds only): the offline steps must open none.
export GDG_TEST_NET_LOG="$OUT/net.log"
rm -f "$GDG_TEST_NET_LOG"
if ! gdg_gui_launch "$OUT" "$HERE/gui-smoke-setup.js"; then
   for s in "${STEPS[@]}"; do result "$s" "FAIL (not run: the interface never opened; see $OUT/ready.txt)"; done
   echo "FAIL: GaiaDepthGrade GUI smoke (0/${#STEPS[@]} steps; screenshots in $OUT)"
   exit 1
fi

# ipc <name> <request JSON>: gui-smoke-ipc.js in the running PixInsight; its answer is $OUT/<name>.json.
ipc()
{
   local name="$1" req="$2" ans="$OUT/$1.json"
   rm -f "$ans"
   jq -c --arg out "$ans" '. + {out: $out}' <<<"$req" > "$GDG_GUI_DIR/ipc-request.json"
   pi_require_private_display || return 1
   timeout 60 "$PI" -x="$GDG_SLOT:$HERE/gui-smoke-ipc.js" >"$OUT/ipc-$name.log" 2>&1 \
      || { echo "        IPC $name: launch failed or timed out (rc $?; $OUT/ipc-$name.log)"; return 1; }
   for _ in $(seq 120); do
      [ -s "$ans" ] && break
      kill -0 "$GDG_GUI_PI_PID" 2>/dev/null || break
      sleep 0.25
   done
   [ -s "$ans" ] && [ "$(jq -r .ok "$ans")" = true ] \
      || { echo "        IPC $name: no answer or an error: $(jq -c . "$ans" 2>/dev/null)"; return 1; }
}

# step <name> [driver args...]: one driver step; a crash of PixInsight fails it too.
step()
{
   local name="$1"; shift
   if ! kill -0 "$GDG_GUI_PI_PID" 2>/dev/null; then result "$name" "FAIL (PixInsight is gone)"; return 1; fi
   echo "== $name"
   if python3 "$HERE/gui_drive.py" "$name" "$OUT" "$@" && kill -0 "$GDG_GUI_PI_PID" 2>/dev/null; then
      result "$name" PASS; return 0
   fi
   kill -0 "$GDG_GUI_PI_PID" 2>/dev/null || echo "        PixInsight exited (see $OUT/pi.log)"
   result "$name" FAIL; return 1
}

# Extra shell-side checks of a step (IPC); a failure turns its PASS into FAIL. The verdict counts
# the FAIL rows of steps.txt, so a step that fails twice (or failed already) counts once.
step_also_fails(){ echo "        FAIL $2"; sed -i "s/^\($1 *\)PASS\$/\1FAIL/" "$OUT/steps.txt"; }

step controls_and_defaults "$MASTER"
TARGET="$(cat "$OUT/target.txt" 2>/dev/null || true)"

if step prepare_enables_preview; then
   if ipc after-prepare '{"action":"windows"}'; then
      jq -e --arg t "$TARGET" '[.windows[].id] | (index($t) != null) and (map(select(test("_gdgwork"))) | length == 0)' \
         "$OUT/after-prepare.json" >/dev/null && echo "        ok no _gdgwork window after Prepare; windows $(jq -c '[.windows[].id]' "$OUT/after-prepare.json")" \
         || step_also_fails prepare_enables_preview "windows after Prepare: $(jq -c '[.windows[].id]' "$OUT/after-prepare.json")"
   else step_also_fails prepare_enables_preview "IPC windows failed"; fi
fi
step slider_marks_preview_stale
step detect_edit_marks_prepare_stale
step inset_click_moves_center
if step execute_creates_depthgraded; then
   if ipc after-execute '{"action":"windows"}'; then
      jq -e --arg t "$TARGET" --arg h "$HONESTY" '(.windows[] | select(.id == $t)) as $m
            | [.windows[] | select(.id == ($t + "_depthgraded"))] | length == 1 and
              (.[0] | .visible and .bitsPerSample == 32 and .isReal and .width == $m.width and .height == $m.height
                      and .channels == $m.channels and .depthtag == $h and (.history | index($h) != null) and .hasSolution)' \
         "$OUT/after-execute.json" >/dev/null \
         && jq -e '[.windows[].id | select(test("_gdgwork"))] | length == 0' "$OUT/after-execute.json" >/dev/null \
         && echo "        ok ${TARGET}_depthgraded: shown, 32-bit float, the target's geometry, DEPTHTAG + HISTORY, solution; no _gdgwork" \
         || step_also_fails execute_creates_depthgraded "windows after Execute: $(jq -c '[.windows[] | {id, bitsPerSample, width, height, depthtag}]' "$OUT/after-execute.json")"
   else step_also_fails execute_creates_depthgraded "IPC windows failed"; fi
fi
# The offline default makes no network connection (Global Constraints): steps 1-6 opened no URL.
if [ -s "$GDG_TEST_NET_LOG" ]; then
   step_also_fails execute_creates_depthgraded "offline steps opened: $(head -3 "$GDG_TEST_NET_LOG" | tr '\n' ' ')"
else
   echo "        ok steps 1-6 (offline) opened no network connection"
fi
if [ "${GDG_LIVE_TAP:-0}" = 1 ]; then
   STEPS+=( online_prepare_live )
   step online_prepare_live
fi
if [ -n "$TARGET" ] && ipc close-target "{\"action\":\"close\",\"id\":\"$TARGET\"}"; then
   echo "        closed $TARGET; windows $(jq -c '[.windows[].id]' "$OUT/close-target.json")"
   step close_target_then_execute
else
   result close_target_then_execute "FAIL (could not close the target '$TARGET' over IPC)"
fi

STEPS+=( close_button )
step close_button

# Every window force-closed (no save prompt at Quit), then File > Quit: a clean exit 0.
if ipc close-all '{"action":"windows"}'; then
   for id in $(jq -r '.windows[].id' "$OUT/close-all.json"); do ipc "close-$id" "{\"action\":\"close\",\"id\":\"$id\"}" || true; done
fi
if ! gdg_gui_quit "$OUT"; then result quit "FAIL"; else result quit PASS; fi

passed="$(grep -cE '^[a-z_]+ +PASS$' "$OUT/steps.txt" || true)"
fails="$(grep -cE '^[a-z_]+ +FAIL' "$OUT/steps.txt" || true)"
if [ "$fails" -ne 0 ]; then
   echo "FAIL: GaiaDepthGrade GUI smoke ($fails failed; $passed passed; screenshots in $OUT)"; exit 1
fi
echo "PASS: GaiaDepthGrade GUI smoke (${#STEPS[@]} steps + quit; screenshots in $OUT)"
