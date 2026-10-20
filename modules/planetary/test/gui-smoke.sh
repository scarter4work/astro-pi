#!/usr/bin/env bash
# Planetary GUI smoke: the SERViewer interface in a REAL PixInsight GUI on a
# private Xvfb display, isolated slot 97 (96 is the headless probe's), driven
# with XTest (gui_drive.py). Modelled on modules/pi-copilot/test/gui-smoke.sh.
#
#   bash modules/planetary/test/gui-smoke.sh <out-dir>   -> PASS (exit 0) / FAIL (exit 1)
#                                                         BLOCKED (exit 2): no signing pass file
#
# 1. gui-smoke-setup.js (-r): the interface opened empty     -> 01-empty.png
# 2. gui_drive.py --open: Open SER... -> the Qt file dialog (01b-dialog.png),
#    fixture A's path typed + Return                         -> 02-loaded.png
#    the frame slider clicked, End                           -> 03-last-frame.png
#    Open SER... again with fixture B                        -> 04-second-file.png
# 3. gui-smoke-import.js (-x IPC): launchInterface() from an instance with
#    filePath = A and frameIndex = A's last frame (ImportProcess) -> 05-import.png
# 4. gui_drive.py --quit: File > Quit PixInsight.
# PASS needs: PixInsight exits 0 within 60 s of Quit, every PNG present, and
# the preview region (found in 01-empty.png) lit in 02-05, different between
# 02 and 03 (frame 0 vs the last frame) and between 03 and 04 (A vs B), and the
# same in 03 and 05 (the slider's last frame == the imported last frame).
#
# Fixtures: PLANETARY_GUI_FIXTURE_A / PLANETARY_GUI_FIXTURE_B (absolute .ser
# paths; A needs a non-zero header FrameCount). Default: make-fixtures'
# "2026-09-07 Saturn-rggb16.ser" (Bayer RGGB shown debayered, 3 frames) and
# mono8.ser (3 frames). PLANETARY_GUI_SLOT overrides the slot (default 97).
#
# Display isolation beyond pi_headless_env: no desktop session variables and a
# session-bus address that leads nowhere, so Qt cannot hand the file dialog to
# the desktop portal (it would open on the user's real screen).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/harness.sh"

OUT="${1:?usage: gui-smoke.sh <out-dir>}"
planetary_require_signing_pass || exit 2

SO="${PLANETARY_SO:-$ROOT/build/src/module/Planetary-pxm.so}"
MAKE_FIXTURES="${PLANETARY_MAKE_FIXTURES:-$ROOT/build/test/make-fixtures}"
[ -f "$SO" ] || { echo "FAIL: module not built at $SO (cmake -DPLANETARY_BUILD_MODULE=ON)"; exit 1; }
SO="$(realpath -e "$SO")"
for tool in Xvfb xprop import; do
   command -v "$tool" >/dev/null || { echo "FAIL: $tool not found"; exit 1; }
done
mkdir -p "$OUT"
OUT="$(realpath -e "$OUT")"
rm -f "$OUT"/*.png "$OUT"/windows.txt "$OUT"/checks.txt "$OUT"/preview.json

PLANETARY_TEST_SLOT="${PLANETARY_GUI_SLOT:-97}" planetary_slot_setup || exit 1
planetary_private_tmp || exit 1
DISPLAY_NO=$(( 100 + PLANETARY_SLOT ))
if [ -e "/tmp/.X11-unix/X$DISPLAY_NO" ]; then
   echo "FAIL: display :$DISPLAY_NO is already in use (another GUI smoke on slot $PLANETARY_SLOT?)"; exit 1
fi

if [ -z "${PLANETARY_GUI_FIXTURE_A:-}" ] || [ -z "${PLANETARY_GUI_FIXTURE_B:-}" ]; then
   [ -x "$MAKE_FIXTURES" ] || { echo "FAIL: make-fixtures not built at $MAKE_FIXTURES"; exit 1; }
   "$MAKE_FIXTURES" "$PLANETARY_RUN_DIR/fixtures" >/dev/null
fi
A="${PLANETARY_GUI_FIXTURE_A:-$PLANETARY_RUN_DIR/fixtures/2026-09-07 Saturn-rggb16.ser}"
B="${PLANETARY_GUI_FIXTURE_B:-$PLANETARY_RUN_DIR/fixtures/mono8.ser}"
for f in "$A" "$B"; do
   case "$f" in /*) ;; *) echo "FAIL: fixture path must be absolute: $f"; exit 1 ;; esac
   [ -f "$f" ] || { echo "FAIL: fixture not found: $f"; exit 1; }
done
# A's last frame from its header FrameCount (int32 LE at byte 38).
A_FRAMES="$(od -An -t d4 -j 38 -N 4 --endian=little "$A" | tr -d ' ')"
[ "${A_FRAMES:-0}" -gt 0 ] || { echo "FAIL: fixture A has no header FrameCount ($A)"; exit 1; }

planetary_sign "$SO" || exit 1
planetary_seed_slot_modules || exit 1

XVFB_PID="" PI_PID=""
gui_cleanup()
{
   [ -n "$PI_PID" ] && kill -9 "$PI_PID" 2>/dev/null
   pkill -9 -f -- "^$PLANETARY_PI_BIN_DIR/PixInsight -n=$PLANETARY_SLOT " 2>/dev/null
   [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null
   return 0
}
planetary_on_exit gui_cleanup

Xvfb ":$DISPLAY_NO" -screen 0 1920x1080x24 -nolisten tcp >"$OUT/xvfb.log" 2>&1 & XVFB_PID=$!
export DISPLAY=":$DISPLAY_NO"
for _ in $(seq 50); do xprop -root >/dev/null 2>&1 && break; sleep 0.1; done

pi_headless_env
unset XDG_CURRENT_DESKTOP DESKTOP_SESSION KDE_FULL_SESSION KDE_SESSION_VERSION GNOME_DESKTOP_SESSION_ID \
      QT_QPA_PLATFORMTHEME XDG_SESSION_TYPE
export DBUS_SESSION_BUS_ADDRESS="unix:path=$PLANETARY_RUN_DIR/no-session-bus"
export XDG_DATA_HOME="$PLANETARY_RUN_DIR/xdg"; mkdir -m 700 "$XDG_DATA_HOME"
export PLANETARY_GUI_DIR="$PLANETARY_RUN_DIR/gui"; mkdir -m 700 "$PLANETARY_GUI_DIR"
export PLANETARY_GUI_IMPORT_PATH="$A" PLANETARY_GUI_IMPORT_FRAME=$(( A_FRAMES - 1 ))

pi_require_private_display || exit 1   # this script's own Xvfb, never the desktop
timeout 900 "$PI" -n="$PLANETARY_SLOT" --no-startup-scripts -m="$SO" -r="$HERE/gui-smoke-setup.js" >"$OUT/pi.log" 2>&1 & PI_PID=$!
# A fresh slot shows start-up notices (e.g. "Risky Temporary Folders"): each
# is answered with Return until the script is ready.
n=0
for _ in $(seq 360); do
   [ -f "$PLANETARY_GUI_DIR/ready" ] && break
   kill -0 "$PI_PID" 2>/dev/null || break
   n=$(( n + 1 ))
   (( n % 10 == 0 )) && python3 "$HERE/gui_drive.py" --dismiss "$OUT"
   sleep 0.5
done
if [ ! -f "$PLANETARY_GUI_DIR/ready" ]; then
   import -window root "$OUT/not-ready.png" 2>/dev/null || true
   echo "FAIL: the GUI script never became ready (see $OUT/pi.log, $OUT/not-ready.png)"; exit 1
fi
cp "$PLANETARY_GUI_DIR/ready" "$OUT/ready.txt"
grep -q "launchInterface=true" "$OUT/ready.txt" || { echo "FAIL: setup: $(cat "$OUT/ready.txt")"; exit 1; }
planetary_assert_only_dev_mapped "$SO" || exit 1
sleep 1

python3 "$HERE/gui_drive.py" --open "$OUT" "$A" "$B" || { echo "FAIL: driver (see $OUT/windows.txt)"; exit 1; }

pi_require_private_display || exit 1
"$PI" -x="$PLANETARY_SLOT:$HERE/gui-smoke-import.js" >/dev/null 2>&1
for _ in $(seq 120); do
   [ -f "$PLANETARY_GUI_DIR/imported" ] && break
   kill -0 "$PI_PID" 2>/dev/null || break
   sleep 0.25
done
cp "$PLANETARY_GUI_DIR/imported" "$OUT/imported.txt" 2>/dev/null || true
grep -q "launchInterface=true" "$OUT/imported.txt" 2>/dev/null \
   || { echo "FAIL: import script: $(cat "$OUT/imported.txt" 2>/dev/null || echo 'never ran')"; exit 1; }
python3 "$HERE/gui_drive.py" --shot "$OUT" 05-import.png

python3 "$HERE/gui_drive.py" --quit "$OUT" || { echo "FAIL: driver (quit)"; exit 1; }
for _ in $(seq 240); do kill -0 "$PI_PID" 2>/dev/null || break; sleep 0.25; done
if kill -0 "$PI_PID" 2>/dev/null; then
   import -window root "$OUT/hang.png" 2>/dev/null || true
   echo "FAIL: PixInsight still running 60 s after Quit (see $OUT/hang.png)"; exit 1
fi
rc=0
wait "$PI_PID" || rc=$?
PI_PID=""

ok=1
[ "$rc" = 0 ] || { echo "FAIL: PixInsight exit code $rc (see $OUT/pi.log)"; ok=0; }
for png in 01-empty 01b-dialog 02-loaded 03-last-frame 04-second-file 05-import; do
   [ -s "$OUT/$png.png" ] || { echo "FAIL: $png.png missing"; ok=0; }
done
python3 "$HERE/gui_drive.py" --check "$OUT" || ok=0
[ "$ok" = 1 ] || exit 1
echo "PASS: SERViewer GUI smoke (screenshots in $OUT)"
