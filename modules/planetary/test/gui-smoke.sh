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
# 4b. gui_drive.py --save-pattern: Open... make-fixtures' avi-nosidecar.avi
#    (always this run's own copy, never a caller's fixture)  -> 12-nosidecar.png
#    Save pattern... shown, disabled under Auto; the combo two items down (RGGB)
#                                                            -> 13-rggb.png (enabled)
#    Debayer off (disabled again, raw CFA grey)              -> 13a-debayer-off.png
#    Debayer on, the slider dragged to the last frame        -> 13c-last-frame.png
#    Save pattern... clicked, the "SERViewer" confirmation   -> 13b-confirm.png
#    Return (Yes): the sidecar written, the AVI reopened with Auto on the same (last) frame
#                                                            -> 14-pattern-saved.png
#    Open... it again                                        -> 15-reopened-sidecar.png
#    PixInsight holds exactly that AVI open; its .avi.txt is byte for byte
#    WriteAsiairSidecar's text (Bayer = RG, this build's version; check_saved_sidecar).
# 5. gui_drive.py --close: the interface's close button      -> 06-closed.png
#    PixInsight holds no video file open (OnHide releases it).
# 6. gui_drive.py --quit: File > Quit PixInsight.
# PASS needs: PixInsight exits 0 within 60 s of Quit, every PNG present, the
# open-file checks above, and gui_drive.py --check on the preview region
# (found in 01-empty.png): lit in 02-05 and 08-15; 02 != 03 (frame 0 vs the last
# frame), 03 != 03b (debayered vs raw CFA), 03b grey, 03 == 03c, 03 != 04 (A vs B),
# 03 == 05 (the slider's last frame == the imported last frame), 05 != 06,
# 05 != 08 (A's last frame vs the AVI's frame 0), 08 != 09 and 09 grey (Bayer
# None shows the raw CFA), 09 == 10 (the viewer stays on the AVI after saving),
# 12 != 13 (raw CFA vs RGGB), 13a grey, 13 != 13c (frame 0 vs last), 13c == 14 (the
# written sidecar reads back as RGGB on the kept frame), 13 == 15 (Open... again: frame 0, as
# RGGB); in the Save pattern... button's box: 08 != 12 (shown only without a
# sidecar), 12 != 13 (enabled only for a chosen pattern), 12 == 13a (disabled with Debayer
# off), 13 == 13c, 08 == 14 == 15 (hidden
# once the sidecar exists);
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
# Auto resolves to a Bayer layout from its .avi.txt sidecar, which must exist, so None (mono)
# changes the preview and Save pattern... stays hidden) with
# PLANETARY_GUI_AVI_FRAMES (its frame count). Default: make-fixtures'
# bayer-colour.avi (bayer-colour.ser's frames, sidecar Bayer RG, 3 frames).
# PLANETARY_GUI_SLOT overrides the slot (default 97). The display, PixInsight
# launch and quit are harness.sh's planetary_gui_* (shared with gui-smoke-stack.sh).
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
mkdir -p "$OUT"
OUT="$(realpath -e "$OUT")"
rm -f "$OUT"/*.png "$OUT"/windows.txt "$OUT"/checks.txt "$OUT"/preview.json

planetary_gui_setup || exit 1

CHECK_ARGS=()
# Always: Save pattern... writes its sidecar next to make-fixtures' avi-nosidecar.avi in this
# run's private directory -- never next to a caller's fixture (real data lives on the NAS).
[ -x "$MAKE_FIXTURES" ] || { echo "FAIL: make-fixtures not built at $MAKE_FIXTURES"; exit 1; }
"$MAKE_FIXTURES" "$PLANETARY_RUN_DIR/fixtures" >"$PLANETARY_RUN_DIR/make-fixtures.log" 2>&1 \
   || { echo "FAIL: make-fixtures failed: $(cat "$PLANETARY_RUN_DIR/make-fixtures.log")"; exit 1; }
NOSIDECAR="$PLANETARY_RUN_DIR/fixtures/avi-nosidecar.avi"
[ -f "$NOSIDECAR" ] && [ ! -e "$NOSIDECAR.txt" ] \
   || { echo "FAIL: make-fixtures gave no sidecar-less $NOSIDECAR"; exit 1; }
VERSION_H="$ROOT/src/module/PlanetaryVersion.h"
PLANETARY_VERSION="$(for f in MAJOR MINOR REVISION BUILD; do
   sed -n "s/^#define PLANETARY_MODULE_VERSION_$f  *\([0-9][0-9]*\)\$/\1/p" "$VERSION_H"; done | paste -sd.)"
[[ "$PLANETARY_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] \
   || { echo "FAIL: cannot read the module version from $VERSION_H (got '$PLANETARY_VERSION')"; exit 1; }
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
# The --open-avi AVI resolves its pattern from its sidecar (Auto -> Bayer, so None changes the
# preview) and shows no Save pattern... button (08 is the hidden-button reference): it needs one.
[ -f "$AVI.txt" ] || { echo "FAIL: PLANETARY_GUI_FIXTURE_AVI must have its .avi.txt sidecar next to it (Auto resolves" \
                         "its Bayer pattern from it, and 08-avi.png is the hidden Save pattern... reference): $AVI.txt not found"; exit 1; }
# SER header (int32 LE): ColorID at byte 18, FrameCount at byte 38.
A_COLOR="$(od -An -t d4 -j 18 -N 4 --endian=little "$A" | tr -d ' ')"
A_FRAMES="$(od -An -t d4 -j 38 -N 4 --endian=little "$A" | tr -d ' ')"
case "$A_COLOR" in 8|9|10|11) ;; *) echo "FAIL: fixture A must be a Bayer file (ColorID 8-11, got '$A_COLOR'): $A"; exit 1 ;; esac
[ "${A_FRAMES:-0}" -gt 0 ] || { echo "FAIL: fixture A has no header FrameCount ($A)"; exit 1; }

planetary_sign "$SO" || exit 1
planetary_seed_slot_modules || exit 1

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

# The sidecar Save pattern... wrote next to $NOSIDECAR (the smoke chose RGGB): exactly
# WriteAsiairSidecar's three lines, LF, this build's version; no .partial left.
check_saved_sidecar()
{
   local want
   want="$(printf '[SERViewer]\nBayer = RG\nWritten By = SERViewer %s (Bayer pattern chosen by the user)\nx' \
           "$PLANETARY_VERSION")"
   want="${want%x}"
   [ -f "$NOSIDECAR.txt" ] || { echo "FAIL: Save pattern wrote no $NOSIDECAR.txt"; return 1; }
   [ ! -e "$NOSIDECAR.txt.partial" ] || { echo "FAIL: $NOSIDECAR.txt.partial left behind"; return 1; }
   if ! cmp -s "$NOSIDECAR.txt" <(printf '%s' "$want"); then
      echo "FAIL: $NOSIDECAR.txt is not the expected sidecar:"; od -c "$NOSIDECAR.txt" | head -20; return 1
   fi
   echo "sidecar written by Save pattern...: $(wc -c <"$NOSIDECAR.txt") bytes, Bayer = RG, SERViewer $PLANETARY_VERSION"
}

export PLANETARY_GUI_IMPORT_PATH="$A" PLANETARY_GUI_IMPORT_FRAME=$(( A_FRAMES - 1 ))
export PLANETARY_GUI_PROCESS=SERViewer   # gui-smoke-setup.js opens this process's interface
planetary_gui_launch "$OUT" "$HERE/gui-smoke-setup.js" || exit 1

python3 "$HERE/gui_drive.py" --open "$OUT" "$A" "$B" || { echo "FAIL: driver (see $OUT/windows.txt)"; exit 1; }
expect_open_video "after opening B" "$B" || exit 1

pi_require_private_display || exit 1
timeout 60 "$PI" -x="$PLANETARY_SLOT:$HERE/gui-smoke-import.js" >"$OUT/ipc.log" 2>&1 \
   || { echo "FAIL: IPC launch of gui-smoke-import.js failed or timed out (rc $?; see $OUT/ipc.log)"; exit 1; }
for _ in $(seq 120); do
   [ -f "$PLANETARY_GUI_DIR/imported" ] && break
   kill -0 "$PLANETARY_GUI_PI_PID" 2>/dev/null || break
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

python3 "$HERE/gui_drive.py" --save-pattern "$OUT" "$NOSIDECAR" \
   || { echo "FAIL: driver (Save pattern...; see $OUT/windows.txt)"; exit 1; }
expect_open_video "after Save pattern" "$NOSIDECAR" || exit 1
check_saved_sidecar || exit 1

python3 "$HERE/gui_drive.py" --close "$OUT" || { echo "FAIL: driver (close the interface; see $OUT/windows.txt)"; exit 1; }
expect_open_video "after closing the interface" "" || exit 1

ok=1
planetary_gui_quit "$OUT" || ok=0
for png in 01-empty 01b-dialog 02-loaded 03-last-frame 03b-cfa 03c-debayer-on 04-second-file 05-import 06-closed \
           08-avi 09-bayer 09b-save-dialog 10-saved 11-reopened 12-nosidecar 13-rggb 13a-debayer-off 13c-last-frame \
           13b-confirm 14-pattern-saved \
           15-reopened-sidecar; do
   [ -s "$OUT/$png.png" ] || { echo "FAIL: $png.png missing"; ok=0; }
done
python3 "$HERE/gui_drive.py" --check "$OUT" "${CHECK_ARGS[@]}" || ok=0
[ "$ok" = 1 ] || exit 1
echo "PASS: SERViewer GUI smoke (screenshots in $OUT)"
