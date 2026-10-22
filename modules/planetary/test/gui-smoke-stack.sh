#!/usr/bin/env bash
# Planetary GUI smoke for PlanetaryStack: its interface in a REAL PixInsight GUI
# on a private Xvfb display, isolated slot 97 (shared with gui-smoke.sh: the two
# never run at once, the slot guard refuses a second PixInsight on it), driven
# with XTest (gui_drive.py). The display, launch and quit are harness.sh's
# planetary_gui_*.
#
#   bash modules/planetary/test/gui-smoke-stack.sh <out-dir>   -> PASS (exit 0) / FAIL (exit 1)
#                                                               BLOCKED (exit 2): no signing pass file
#
# Captures: make-fixtures --stack (the probe's synthetic captures) and
# --stack-long gui-long.avi (the long Bayer capture, 1500 frames: ~5.5 s on 32
# threads, so the progress bar, Cancel, a close and a Quit land mid-run). What
# each must give comes from the engine itself (stack-probe on the same file),
# so a capture that stops giving it fails here, before PixInsight starts.
#
#  1. gui-smoke-setup.js (-r): the interface opened empty          -> 01-opened.png
#  2. gui_drive.py --stack-open: Open... gui-long.avi              -> 02-loaded.png
#  3. --stack-run: Stack! -> the progress bar mid-run              -> 03-progress.png
#     the stack's preview at Fit, 100 %, 200 %                     -> 04-fit.png, 05-zoom-100.png, 06-zoom-200.png
#     zoom in, a drag (pan), Fit again                             -> 06b-zoom-in.png, 06c-pan.png, 06d-fit-again.png
#     Execute                                                      -> 07-execute.png
#     gui-smoke-stack-ipc.js (-x): exactly one image window, <stem>_stack, 1024x768 RGB, NCOMBINE /
#     NFRAMES the engine's, its centre sample the engine's (within 1e-5): the stack Stack! made,
#     opened without stacking again (it exists 1.5 s after the click; a stack takes ~5.5 s).
#     The window is then closed.
#  4. --stack-cancel: Stack! again -> mid-run                      -> 08-cancel-progress.png
#     Cancel -> idle                                               -> 09-cancelled.png
#     no image window, no video file held open, PixInsight's thread count back to its value
#     before the run (every engine thread and the worker joined).
#  4b. --stack-shade: Stack! -> mid-run                            -> 09b-shade-progress.png
#     the title bar's shade button: collapsed to its title bar     -> 09c-shaded.png
#     expanded again, in place, still running                      -> 09c2-expanded.png
#     the run goes on to its stack (collapsing is not closing)     -> 09d-unshaded.png
#  5. --stack-file: diag-notarget.ser (NoTarget) -> no preview     -> 10-diagnosis.png
#     stack-mosaic.avi (Ruling P9 warning)                         -> 11-warning-mosaic.png
#     stack-soft-disc.ser (focus warning, Rulings P16/P20)         -> 12-warning-focus.png
#     no image window after any of them.
#  6. --stack-bayer: stack-bayer.avi (sidecar RG), Auto            -> 13-bayer-auto.png
#     the combo set to BGGR: the stack dropped, a note says why    -> 13b-bayer-changed.png
#     Stack! again (the explicit pattern beats the sidecar)        -> 14-bayer-bggr.png
#  7. --stack-pattern: stack-mosaic.avi (no sidecar), RGGB chosen, Stack!   -> 15-pattern-rggb.png
#     Save pattern... -> the "PlanetaryStack" confirmation         -> 15b-confirm.png
#     Return: the sidecar written, the stack kept                  -> 16-pattern-saved.png
#     the sidecar is byte for byte WriteAsiairSidecar's (Bayer = RG, this build's version).
#  8. --stack-close: Open... gui-long.avi, Stack!, mid-run         -> 17-close-progress.png
#     the interface's close button                                 -> 18-closed.png
#     no video file held open, the thread count back.
#  9. gui-smoke-stack-ipc.js launch (filePath = gui-long.avi: ImportProcess), --stack-quit:
#     Stack!, mid-run                                              -> 19-quit-progress.png
#     File > Quit while it runs: PixInsight exits 0 within 60 s (the module joins the run).
# PASS needs all of that, every PNG present, and gui_drive.py --check-stack on the
# info box (never focused: no focus ring, no caret),
# preview, progress bar and Save pattern... boxes (see its --check-stack).
#
# PLANETARY_GUI_SLOT overrides the slot (default 97). PLANETARY_SO /
# PLANETARY_MAKE_FIXTURES / PLANETARY_STACK_PROBE override the build products.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/harness.sh"

OUT="${1:?usage: gui-smoke-stack.sh <out-dir>}"
planetary_require_signing_pass || exit 2

SO="${PLANETARY_SO:-$ROOT/build/src/module/Planetary-pxm.so}"
MAKE_FIXTURES="${PLANETARY_MAKE_FIXTURES:-$ROOT/build/test/make-fixtures}"
STACK_PROBE="${PLANETARY_STACK_PROBE:-$ROOT/build/test/stack-probe}"
[ -f "$SO" ] || { echo "FAIL: module not built at $SO (cmake -DPLANETARY_BUILD_MODULE=ON)"; exit 1; }
[ -x "$MAKE_FIXTURES" ] || { echo "FAIL: make-fixtures not built at $MAKE_FIXTURES"; exit 1; }
[ -x "$STACK_PROBE" ] || { echo "FAIL: stack-probe not built at $STACK_PROBE"; exit 1; }
SO="$(realpath -e "$SO")"
mkdir -p "$OUT"
OUT="$(realpath -e "$OUT")"
rm -f "$OUT"/*.png "$OUT"/*.json "$OUT"/windows.txt "$OUT"/checks.txt

planetary_gui_setup || exit 1

FIX="$PLANETARY_RUN_DIR/fixtures"
"$MAKE_FIXTURES" --stack "$FIX" >"$PLANETARY_RUN_DIR/make-fixtures.log" 2>&1 \
   || { echo "FAIL: make-fixtures --stack failed: $(cat "$PLANETARY_RUN_DIR/make-fixtures.log")"; exit 1; }
LONG="$FIX/gui-long.avi"
"$MAKE_FIXTURES" --stack-long "$LONG" 1500 >>"$PLANETARY_RUN_DIR/make-fixtures.log" 2>&1 \
   || { echo "FAIL: make-fixtures --stack-long failed: $(cat "$PLANETARY_RUN_DIR/make-fixtures.log")"; exit 1; }
DIAG="$FIX/diag-notarget.ser" MOSAIC="$FIX/stack-mosaic.avi" SOFT="$FIX/stack-soft-disc.ser" BAYER="$FIX/stack-bayer.avi"
[ ! -e "$MOSAIC.txt" ] || { echo "FAIL: $MOSAIC must have no sidecar (Save pattern... writes one)"; exit 1; }
[ -f "$BAYER.txt" ] || { echo "FAIL: $BAYER must have its RG sidecar"; exit 1; }
VERSION_H="$ROOT/src/module/PlanetaryVersion.h"
PLANETARY_VERSION="$(for f in MAJOR MINOR REVISION BUILD; do
   sed -n "s/^#define PLANETARY_MODULE_VERSION_$f  *\([0-9][0-9]*\)\$/\1/p" "$VERSION_H"; done | paste -sd.)"
[[ "$PLANETARY_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] \
   || { echo "FAIL: cannot read the module version from $VERSION_H (got '$PLANETARY_VERSION')"; exit 1; }

# The engine's outcome for $1 (stack-probe --json, plus --bayer $2 and the centre sample):
# $3 = image | diagnosis; $4 = a warning text the engine must give (or ""); $5 = --sample X,Y or empty.
engine()
{
   local file="$1" bayer="$2" want="$3" warn="$4" json rc=0
   local -a sample=()
   [ -n "${5:-}" ] && sample=( --sample "$5" )
   json="$OUT/engine-$(basename "$file")-$bayer.json"
   "$STACK_PROBE" "$file" --json --bayer "$bayer" "${sample[@]}" >"$json" 2>"$PLANETARY_RUN_DIR/stack-probe.err" || rc=$?
   case "$want" in
      image) [ "$rc" = 0 ] || { echo "FAIL: $(basename "$file") ($bayer) gives no image from the engine (stack-probe exit $rc)"; return 1; } ;;
      diagnosis) [ "$rc" = 2 ] || { echo "FAIL: $(basename "$file") is not diagnosed by the engine (stack-probe exit $rc)"; return 1; } ;;
   esac
   if [ -n "$warn" ]; then
      jq -e --arg w "$warn" '.warnings | any(contains($w))' "$json" >/dev/null \
         || { echo "FAIL: the engine gives $(basename "$file") no warning containing '$warn'"; return 1; }
   fi
   echo "engine: $(basename "$file") ($bayer): $(jq -c '{used, measured, seconds, kind: .diagnosis.kind}' "$json")"
}
engine "$LONG" auto image "" 512,384 || exit 1
engine "$DIAG" auto diagnosis "" || exit 1
engine "$MOSAIC" auto image "cannot be debayered afterwards" || exit 1
engine "$SOFT" auto image "may be out of focus" || exit 1
engine "$BAYER" auto image "" || exit 1
engine "$BAYER" BGGR image "" || exit 1
LONG_JSON="$OUT/engine-gui-long.avi-auto.json"
jq -e '.seconds >= 3' "$LONG_JSON" >/dev/null \
   || { echo "FAIL: gui-long.avi stacks in $(jq .seconds "$LONG_JSON") s: too fast for Cancel, a close and a Quit to land mid-run"; exit 1; }

planetary_sign "$SO" || exit 1
planetary_seed_slot_modules || exit 1

PI_PROC=""
# PixInsight must hold no video file open (the interface never keeps one).
expect_no_open_video()
{
   local got
   got="$(planetary_open_video_files)" || return 1
   [ -z "$got" ] || { echo "FAIL: $1: PixInsight holds [${got//$'\n'/, }] open, expected none"; return 1; }
   echo "open video files $1: none"
}

threads()
{
   sed -n 's/^Threads:[[:space:]]*//p' "/proc/$PI_PROC/status"
}

# The thread count must come back to $2 (before the run) within 10 s.
expect_threads()
{
   local now
   for _ in $(seq 40); do
      now="$(threads)"
      [ "$now" -le "$2" ] && { echo "threads $1: $now (before the run: $2)"; return 0; }
      sleep 0.25
   done
   echo "FAIL: $1: PixInsight runs $now threads, $2 before the run (a thread was left behind)"; return 1
}

# gui-smoke-stack-ipc.js with the request $1 (JSON); its answer lands in $OUT/$2.
ipc()
{
   local req="$1" out="$2"
   printf '%s' "$req" > "$PLANETARY_GUI_DIR/ipc-request.json"
   rm -f "$PLANETARY_GUI_DIR/$out"
   pi_require_private_display || return 1
   timeout 60 "$PI" -x="$PLANETARY_SLOT:$HERE/gui-smoke-stack-ipc.js" >>"$OUT/ipc.log" 2>&1 \
      || { echo "FAIL: IPC gui-smoke-stack-ipc.js ($req) failed or timed out (see $OUT/ipc.log)"; return 1; }
   for _ in $(seq 120); do
      [ -f "$PLANETARY_GUI_DIR/$out" ] && break
      sleep 0.25
   done
   [ -f "$PLANETARY_GUI_DIR/$out" ] || { echo "FAIL: IPC ($req) wrote no answer"; return 1; }
   cp "$PLANETARY_GUI_DIR/$out" "$OUT/$out"
   if jq -e '.error' "$OUT/$out" >/dev/null; then echo "FAIL: IPC ($req): $(jq -r .error "$OUT/$out")"; return 1; fi
}

# No image window exists (every one was closed after the Execute check).
expect_no_window()
{
   ipc '{"mode":"windows","out":"'"$2"'"}' "$2" || return 1
   jq -e '.windows | length == 0' "$OUT/$2" >/dev/null \
      || { echo "FAIL: $1: image windows exist: $(jq -c '[.windows[].id]' "$OUT/$2")"; return 1; }
   echo "image windows $1: none"
}

drive()
{
   python3 "$HERE/gui_drive.py" "$@" || { echo "FAIL: driver ($1; see $OUT/windows.txt)"; return 1; }
}

export PLANETARY_GUI_PROCESS=PlanetaryStack   # gui-smoke-setup.js opens this process's interface
planetary_gui_launch "$OUT" "$HERE/gui-smoke-setup.js" || exit 1
PI_PROC="$(pgrep -f -- "^$PLANETARY_PI_BIN_DIR/PixInsight -n=$PLANETARY_SLOT " | head -1)"

drive --stack-open "$OUT" "$LONG" || exit 1
expect_no_open_video "after Open..." || exit 1
T0="$(threads)"
# PixInsight's thread count every 0.1 s while it stacks, for the record: the run's own threads
# (the worker and the engine's) are the peak less T0.
( while :; do threads; sleep 0.1; done ) > "$OUT/threads-during-run.txt" 2>/dev/null & SAMPLER=$!
drive --stack-run "$OUT" || { kill "$SAMPLER"; exit 1; }
kill "$SAMPLER"
echo "threads during the run: peak $(sort -n "$OUT/threads-during-run.txt" | tail -1) (before: $T0)"

# Execute opened the stack Stack! made: the engine's size, counts and pixels.
ipc '{"mode":"windows","out":"windows-execute.json","sample":[[512,384]],"close":true}' windows-execute.json || exit 1
jq -e --slurpfile e "$LONG_JSON" '
   .windows | length == 1 and .[0].id == "gui_long_stack" and .[0].width == 1024 and .[0].height == 768
   and .[0].channels == 3 and .[0].keywords.NCOMBINE == ($e[0].used | tostring)
   and .[0].keywords.NFRAMES == ($e[0].measured | tostring)' "$OUT/windows-execute.json" >/dev/null \
   || { echo "FAIL: Execute: expected one window gui_long_stack (1024x768 RGB, NCOMBINE $(jq .used "$LONG_JSON")), got $(cat "$OUT/windows-execute.json")"; exit 1; }
python3 - "$OUT/windows-execute.json" "$LONG_JSON" <<'PY' || exit 1
import json, sys
w = json.load(open(sys.argv[1]))["windows"][0]
e = json.load(open(sys.argv[2]))
want = [s["v"] for s in e["samples"]]
got = w["samples"][0]
if len(got) != 3 or any(abs(g - v) > 1e-5 for g, v in zip(got, want)):
    sys.exit("FAIL: Execute: the window's centre sample %r is not the engine's %r" % (got, want))
print("Execute: %s %dx%d RGB, NCOMBINE %s, centre sample %s == the engine's" % (w["id"], w["width"], w["height"],
      w["keywords"]["NCOMBINE"], ["%.6f" % g for g in got]))
PY
expect_no_open_video "after Execute" || exit 1

drive --stack-cancel "$OUT" || exit 1
expect_no_window "after Cancel" windows-cancel.json || exit 1
expect_no_open_video "after Cancel" || exit 1
expect_threads "after Cancel" "$T0" || exit 1

drive --stack-shade "$OUT" || exit 1
expect_no_open_video "after collapsing and expanding the window mid-run" || exit 1
drive --stack-file "$OUT" "$DIAG" 10-diagnosis.png || exit 1
expect_no_window "after the diagnosis" windows-diagnosis.json || exit 1
drive --stack-file "$OUT" "$MOSAIC" 11-warning-mosaic.png || exit 1
drive --stack-file "$OUT" "$SOFT" 12-warning-focus.png || exit 1
expect_no_window "after the warned stacks" windows-warned.json || exit 1

drive --stack-bayer "$OUT" "$BAYER" || exit 1
drive --stack-pattern "$OUT" "$MOSAIC" || exit 1
want="$(printf '[SERViewer]\nBayer = RG\nWritten By = SERViewer %s (Bayer pattern chosen by the user)\nx' "$PLANETARY_VERSION")"
want="${want%x}"
[ -f "$MOSAIC.txt" ] && [ ! -e "$MOSAIC.txt.partial" ] && cmp -s "$MOSAIC.txt" <(printf '%s' "$want") \
   || { echo "FAIL: Save pattern... wrote no sidecar, or not the expected one:"; od -c "$MOSAIC.txt" 2>&1 | head; exit 1; }
echo "sidecar written by Save pattern...: Bayer = RG, SERViewer $PLANETARY_VERSION"
expect_no_open_video "after Save pattern..." || exit 1

T1="$(threads)"
drive --stack-close "$OUT" "$LONG" || exit 1
expect_no_open_video "after closing the interface mid-run" || exit 1
expect_threads "after closing the interface mid-run" "$T1" || exit 1

ipc '{"mode":"launch","out":"launch.json","path":"'"$LONG"'"}' launch.json || exit 1
jq -e '.launchInterface == true' "$OUT/launch.json" >/dev/null || { echo "FAIL: relaunch: $(cat "$OUT/launch.json")"; exit 1; }
drive --stack-quit "$OUT" || exit 1
ok=1
planetary_gui_quit "$OUT" || ok=0
grep -n -E "Segmentation fault|SIGSEGV|SIGABRT|terminate called|core dumped" "$OUT/pi.log" \
   && { echo "FAIL: PixInsight's log reports a crash"; ok=0; }

for png in 01-opened 02-loaded 03-progress 04-fit 05-zoom-100 06-zoom-200 06b-zoom-in 06c-pan 06d-fit-again 07-execute \
           08-cancel-progress 09-cancelled 09b-shade-progress 09c-shaded 09c2-expanded 09d-unshaded 10-diagnosis 11-warning-mosaic 12-warning-focus 13-bayer-auto \
           13b-bayer-changed 14-bayer-bggr 15-pattern-rggb 15b-confirm 16-pattern-saved 17-close-progress 18-closed \
           19-quit-progress; do
   [ -s "$OUT/$png.png" ] || { echo "FAIL: $png.png missing"; ok=0; }
done
python3 "$HERE/gui_drive.py" --check-stack "$OUT" || ok=0
[ "$ok" = 1 ] || exit 1
echo "PASS: PlanetaryStack GUI smoke (screenshots in $OUT)"
