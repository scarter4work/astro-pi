#!/usr/bin/env bash
# EZTools GUI smoke (plan Task 4): the EZHazeKill interface and its native Real-Time Preview in a
# REAL PixInsight GUI on a private Xvfb display, isolated slot 92 (EZT_GUI_SLOT), driven with XTest
# (gui_drive.py). Test-only.
#
#   bash modules/ez-tools/test/gui-smoke.sh <out-dir>   -> PASS (exit 0) / FAIL (exit 1)
#                                                        BLOCKED (exit 2): no signing pass file
#
# 1. gui-smoke-setup.js (-r): the L2 input (hazekill_parity.py prep) opened with a linked auto-STF,
#    the EZHazeKill interface opened                                        -> 01-launched.png
# 2. --rtp: the image window activated, the interface's Real-Time Preview button clicked; the button
#    shows its active mark                                                   -> 02-rtp.png (A)
# 3. --zoom: the RTP window maximized (rendition 1:2 instead of 1:6), then restored
#                                                                            -> 04-rtp-zoomed.png, 04b
# 4. --neutralize 0: typed into the Neutralize edit (+ Return)              -> 03-rtp-neutralize0.png (B)
# 5. --invert: Ctrl+I (Invert) and Ctrl+A (AutoStretch) on the image window -> 05-rtp-inverted.png (C)
# 6. --apply: the interface's Apply button on the (inverted) image          -> 06-applied.png
#    gui-smoke-save.js (-x IPC) saves it (executed.xisf) and closes the image without asking.
# 7. File > Quit: PixInsight exits 0 within 60 s.
# PASS needs (gui_drive.py --check): A's sky a lit neutral grey while the unprocessed view (01) is
# green-cast; the 1:2 rendition's sky the same neutral grey as A's, the restored 1:6 one equal to A;
# B green-cast again; C black -- the sky re-measured on the inverted image is far
# below the inverted image's own STF shadows clip, while the old cached sky would leave the view's look
# (and an empty, unrendered RTP fails too) -- with the inverted view itself lit; and the executed image
# equal to hk_v11(1 - input, Amount 0.5, Neutralize 0, Target 0.05) within the parity tolerances.
# EZTOOLS_SO overrides the module (default: the host build).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/harness.sh"
. "$HERE/gui-harness.sh"

OUT="${1:?usage: gui-smoke.sh <out-dir>}"
ezt_require_signing_pass || exit 2
SO="${EZTOOLS_SO:-$ROOT/build/src/module/EZTools-pxm.so}"
[ -f "$SO" ] || { echo "FAIL: module not built at $SO"; exit 1; }
SO="$(realpath -e "$SO")"
ezt_check_so "$SO" || exit 1
mkdir -p "$OUT"
OUT="$(realpath -e "$OUT")"
rm -f "$OUT"/*.png "$OUT"/*.txt "$OUT"/*.json "$OUT"/*.xisf "$OUT"/*.log

ezt_gui_setup || exit 1
echo "== input: L2 (hazekill_parity.py prep) =="
python3 "$HERE/hazekill_parity.py" prep "$EZT_RUN_DIR/in" L2_M33_Lqef_linear >"$OUT/prep.log" 2>&1 \
   || { cat "$OUT/prep.log"; echo "FAIL: input preparation"; exit 1; }
export EZT_GUI_INPUT="$EZT_RUN_DIR/in/L2_M33_Lqef_linear.in.xisf"
ezt_sign "$SO" || exit 1
ezt_seed_slot_modules || exit 1

drive(){ python3 "$HERE/gui_drive.py" "$@" || { echo "FAIL: driver $1 (see $OUT)"; exit 1; }; }

echo "== GUI (slot $EZT_SLOT, display :$EZT_DISPLAY_NO) =="
ezt_gui_launch "$OUT" "$HERE/gui-smoke-setup.js" || exit 1
drive --shot "$OUT" 01-launched.png
drive --rtp "$OUT"
drive --zoom "$OUT"
drive --neutralize "$OUT" 0 03-rtp-neutralize0.png
drive --invert "$OUT"
drive --apply "$OUT"
ezt_gui_ipc "$HERE/gui-smoke-save.js" save "$OUT" || exit 1
cp "$EZT_GUI_DIR/executed.xisf" "$OUT/executed.xisf"
ezt_gui_quit "$OUT" || exit 1

echo "== checks =="
fails=0
python3 "$HERE/gui_drive.py" --check "$OUT" || fails=1
python3 "$HERE/hazekill_parity.py" compare "$EZT_GUI_INPUT" "$OUT/executed.xisf" 0.5 0 0.05 --invert || fails=1
[ "$fails" = 0 ] && echo "PASS: EZHazeKill interface + Real-Time Preview (PNGs in $OUT)" || { echo "FAIL: see above"; exit 1; }
