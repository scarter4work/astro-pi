#!/usr/bin/env bash
# Planetary GUI smoke: the SERViewer interface in a REAL PixInsight GUI on a
# private Xvfb display, isolated slot 97 (96 is the headless probe's), driven
# with XTest (gui_drive.py). Modelled on modules/pi-copilot/test/gui-smoke.sh.
#
#   bash modules/planetary/test/gui-smoke.sh <out-dir>   -> PASS (exit 0) / FAIL (exit 1)
#                                                         BLOCKED (exit 2): no signing pass file
#
# 1. gui-smoke-setup.js (-r): the interface opened empty     -> 01-empty.png
# 2. gui_drive.py --open: Open... -> the Qt file dialog (01b-dialog.png),
#    fixture A's path typed + Return                         -> 02-loaded.png
#    the slider handle dragged past its end (last frame)      -> 03-last-frame.png
#    Debayer off / on again                                  -> 03b-cfa.png, 03c-debayer-on.png
#    Open... again with fixture B                            -> 04-second-file.png
#    PixInsight now holds exactly B open (/proc/<pid>/fd).
# 3. gui-smoke-import.js (-x IPC): launchInterface() from an instance with
#    filePath = A and frameIndex = A's last frame (ImportProcess) -> 05-import.png
#    PixInsight now holds exactly A open.
# 4. gui_drive.py --open-avi: Open... the AVI fixture        -> 08-avi.png
#    the Bayer pattern combo clicked, Down, Return:
#    Auto -> None (mono)                                     -> 09-bayer.png
#    Save as SER... (09b-save-dialog.png), the new path typed WITHOUT .ser
#    + Return, the .ser file written (no .partial left)      -> 10-saved.png
#    Open... the AVI again (Bayer pattern back to Auto)      -> 11-reopened.png
#    PixInsight now holds exactly the AVI open; the saved SER reads back
#    (check_saved_ser).
# 5. gui_drive.py --close: the interface's close button      -> 06-closed.png
#    PixInsight holds no video file open (OnHide releases it).
# 6. gui_drive.py --quit: File > Quit PixInsight.
# PASS needs: PixInsight exits 0 within 60 s of Quit, every PNG present, the
# open-file checks above, and gui_drive.py --check on the preview region
# (found in 01-empty.png): lit in 02-05 and 08-11; 02 != 03 (frame 0 vs the last
# frame), 03 != 03b (debayered vs raw CFA), 03b grey, 03 == 03c, 03 != 04 (A vs B),
# 03 == 05 (the slider's last frame == the imported last frame), 05 != 06,
# 05 != 08 (A's last frame vs the AVI's frame 0), 08 != 09 and 09 grey (Bayer
# None shows the raw CFA), 09 == 10 (the viewer stays on the AVI after saving),
# 08 == 11 (Open... resets the Bayer pattern to Auto); with the default fixture
# A, 03 red-dominant (catches an R/B swap); with the default AVI, 08 too, and
# with both defaults 02 == 08 (the AVI decodes exactly as bayer-colour.ser);
# with real fixtures, PLANETARY_GUI_EXPECT_RB=gt|lt asserts 03 shows mean R
# above/below B (known from the raw samples, see avi_to_ser.py) -- the same swap catch.
#
# Fixtures: PLANETARY_GUI_FIXTURE_A / PLANETARY_GUI_FIXTURE_B (absolute .ser
# paths; A must be a Bayer file, since the smoke toggles Debayer, with a
# non-zero header FrameCount). Default: make-fixtures' bayer-colour.ser
# (strongly red Bayer RGGB, 3 frames) and mono8.ser (3 frames).
# PLANETARY_GUI_FIXTURE_AVI (absolute path of an 8-bit AVI whose Bayer pattern
# Auto resolves to a Bayer layout, so None (mono) changes the preview) with
# PLANETARY_GUI_AVI_FRAMES (its frame count). Default: make-fixtures'
# bayer-colour.avi (bayer-colour.ser's frames, sidecar Bayer RG, 3 frames).
# PLANETARY_GUI_SLOT overrides the slot (default 97).
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

CHECK_ARGS=()
if [ -z "${PLANETARY_GUI_FIXTURE_A:-}" ] || [ -z "${PLANETARY_GUI_FIXTURE_B:-}" ] \
   || [ -z "${PLANETARY_GUI_FIXTURE_AVI:-}" ]; then
   [ -x "$MAKE_FIXTURES" ] || { echo "FAIL: make-fixtures not built at $MAKE_FIXTURES"; exit 1; }
   "$MAKE_FIXTURES" "$PLANETARY_RUN_DIR/fixtures" >"$PLANETARY_RUN_DIR/make-fixtures.log" 2>&1 \
      || { echo "FAIL: make-fixtures failed: $(cat "$PLANETARY_RUN_DIR/make-fixtures.log")"; exit 1; }
fi
if [ -z "${PLANETARY_GUI_FIXTURE_A:-}" ]; then
   CHECK_ARGS+=( --expect-red )   # bayer-colour.ser is strongly red by construction
fi
case "${PLANETARY_GUI_EXPECT_RB:-}" in
   "") ;;
   gt|lt) CHECK_ARGS+=( --expect-rb "$PLANETARY_GUI_EXPECT_RB" ) ;;
   *) echo "FAIL: PLANETARY_GUI_EXPECT_RB must be gt or lt, got '$PLANETARY_GUI_EXPECT_RB'"; exit 1 ;;
esac
if [ -z "${PLANETARY_GUI_FIXTURE_AVI:-}" ]; then
   CHECK_ARGS+=( --expect-avi-red )   # bayer-colour.avi: the same red frames, sidecar Bayer RG
   AVI_FRAMES=3
else
   AVI_FRAMES="${PLANETARY_GUI_AVI_FRAMES:?PLANETARY_GUI_FIXTURE_AVI needs PLANETARY_GUI_AVI_FRAMES (its frame count)}"
fi
if [ -z "${PLANETARY_GUI_FIXTURE_A:-}" ] && [ -z "${PLANETARY_GUI_FIXTURE_AVI:-}" ]; then
   CHECK_ARGS+=( --avi-is-a )   # bayer-colour.avi holds bayer-colour.ser's frames
fi
A="${PLANETARY_GUI_FIXTURE_A:-$PLANETARY_RUN_DIR/fixtures/bayer-colour.ser}"
B="${PLANETARY_GUI_FIXTURE_B:-$PLANETARY_RUN_DIR/fixtures/mono8.ser}"
AVI="${PLANETARY_GUI_FIXTURE_AVI:-$PLANETARY_RUN_DIR/fixtures/bayer-colour.avi}"
mkdir -m 700 "$PLANETARY_RUN_DIR/saved"
SAVED="$PLANETARY_RUN_DIR/saved/gui-saved.ser"
for f in "$A" "$B" "$AVI"; do
   case "$f" in /*) ;; *) echo "FAIL: fixture path must be absolute: $f"; exit 1 ;; esac
   [ -f "$f" ] || { echo "FAIL: fixture not found: $f"; exit 1; }
done
# SER header (int32 LE): ColorID at byte 18, FrameCount at byte 38.
A_COLOR="$(od -An -t d4 -j 18 -N 4 --endian=little "$A" | tr -d ' ')"
A_FRAMES="$(od -An -t d4 -j 38 -N 4 --endian=little "$A" | tr -d ' ')"
case "$A_COLOR" in 8|9|10|11) ;; *) echo "FAIL: fixture A must be a Bayer file (ColorID 8-11, got '$A_COLOR'): $A"; exit 1 ;; esac
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

# PixInsight must hold exactly the video file $2 open (none when $2 is empty).
expect_open_video()
{
   local what="$1" want="" got
   [ -n "$2" ] && want="$(realpath -e "$2")"
   got="$(planetary_open_video_files)" || return 1
   if [ "$got" != "$want" ]; then
      echo "FAIL: $what: PixInsight holds [${got//$'\n'/, }] open, expected [${want}]"; return 1
   fi
   echo "open video files $what: [${got}]"
}

# The SER that Save as SER... wrote from the AVI ($SAVED), read back: SER magic,
# ColorID 0 (the smoke picked Bayer pattern None (mono) before saving), 8-bit,
# the AVI's frame count, and exactly 178 + frames x width x height bytes (no
# trailer). With the default AVI (bayer-colour.avi), its frame bytes must equal
# bayer-colour.ser's, which holds the same frames.
check_saved_ser()
{
   local magic color w h depth n size want
   [ -f "$SAVED" ] || { echo "FAIL: Save as SER wrote no $SAVED"; return 1; }
   [ ! -e "$SAVED.partial" ] || { echo "FAIL: $SAVED.partial left behind"; return 1; }
   magic="$(head -c 14 "$SAVED")"
   read -r color < <(od -An -t d4 -j 18 -N 4 --endian=little "$SAVED")
   read -r w h depth n < <(od -An -t d4 -j 26 -N 16 --endian=little "$SAVED")
   size="$(stat -c %s "$SAVED")"
   want=$(( 178 + n * w * h ))
   if [ "$magic" != "LUCAM-RECORDER" ] || [ "$color" != 0 ] || [ "$depth" != 8 ] || [ "$n" != "$AVI_FRAMES" ] \
      || [ "$size" != "$want" ]; then
      echo "FAIL: saved SER $SAVED: magic '$magic' ColorID $color depth $depth frames $n (want $AVI_FRAMES)" \
           "${w}x${h}, $size bytes (want $want)"; return 1
   fi
   if [ -z "${PLANETARY_GUI_FIXTURE_AVI:-}" ]; then
      cmp -i 178 -n $(( n * w * h )) "$SAVED" "$PLANETARY_RUN_DIR/fixtures/bayer-colour.ser" \
         || { echo "FAIL: saved SER frames differ from bayer-colour.ser's"; return 1; }
   fi
   echo "saved SER reads back: MONO, 8-bit, $n frames of ${w}x${h}, $size bytes"
}

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
   (( n % 10 == 0 )) && { python3 "$HERE/gui_drive.py" --dismiss "$OUT" || { echo "FAIL: driver (dismiss)"; exit 1; }; }
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
expect_open_video "after opening B" "$B" || exit 1

pi_require_private_display || exit 1
timeout 60 "$PI" -x="$PLANETARY_SLOT:$HERE/gui-smoke-import.js" >"$OUT/ipc.log" 2>&1 \
   || { echo "FAIL: IPC launch of gui-smoke-import.js failed or timed out (rc $?; see $OUT/ipc.log)"; exit 1; }
for _ in $(seq 120); do
   [ -f "$PLANETARY_GUI_DIR/imported" ] && break
   kill -0 "$PI_PID" 2>/dev/null || break
   sleep 0.25
done
cp "$PLANETARY_GUI_DIR/imported" "$OUT/imported.txt" 2>/dev/null || true
grep -q "launchInterface=true" "$OUT/imported.txt" 2>/dev/null \
   || { echo "FAIL: import script: $(cat "$OUT/imported.txt" 2>/dev/null || echo 'never ran')"; exit 1; }
python3 "$HERE/gui_drive.py" --shot "$OUT" 05-import.png || { echo "FAIL: screenshot 05-import.png"; exit 1; }
expect_open_video "after the import" "$A" || exit 1

python3 "$HERE/gui_drive.py" --open-avi "$OUT" "$AVI" "$SAVED" \
   || { echo "FAIL: driver (AVI, Bayer pattern, Save as SER; see $OUT/windows.txt)"; exit 1; }
expect_open_video "after Save as SER" "$AVI" || exit 1
check_saved_ser || exit 1

python3 "$HERE/gui_drive.py" --close "$OUT" || { echo "FAIL: driver (close the interface; see $OUT/windows.txt)"; exit 1; }
expect_open_video "after closing the interface" "" || exit 1

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
for png in 01-empty 01b-dialog 02-loaded 03-last-frame 03b-cfa 03c-debayer-on 04-second-file 05-import 06-closed \
           08-avi 09-bayer 09b-save-dialog 10-saved 11-reopened; do
   [ -s "$OUT/$png.png" ] || { echo "FAIL: $png.png missing"; ok=0; }
done
python3 "$HERE/gui_drive.py" --check "$OUT" "${CHECK_ARGS[@]}" || ok=0
[ "$ok" = 1 ] || exit 1
echo "PASS: SERViewer GUI smoke (screenshots in $OUT)"
