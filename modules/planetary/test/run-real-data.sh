#!/usr/bin/env bash
# Planetary real-data end-to-end from Scott's 2026-09-07 Saturn capture (ASIAIR,
# ASI585MC Air, RAW8 RGGB, 320x240 pal8 AVI). Test-only, host-only: needs the NAS.
#
#   bash modules/planetary/test/run-real-data.sh   -> PASS (exit 0) / FAIL (exit 1)
#                                                    BLOCKED (exit 2): no signing pass file
#
# 1. avi_to_ser.py converts the first 100 frames into a private temp dir (never
#    committed): five SER fixtures + expected.json derived from the AVI bytes.
# 2. check-real-fixtures validates the C++ reader against those expectations (no PixInsight).
# 3. BLOCKED here without /tmp/.pi_codesign_pass: nothing below launches PixInsight.
# 4. run-probe.sh (slot 96) runs SERViewer on every fixture case.
# 5. gui-smoke.sh (slot 97): fixture A = saturn_rggb16_12bit.ser (Bayer, 12-bit),
#    B = saturn_rgb8.ser; the R/B order expectation comes from the raw samples.
#    Its PNGs and checks.txt are copied to modules/planetary/test/output/real/ (gitignored).
#
# PLANETARY_SATURN_AVI overrides the source capture.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/harness.sh"

AVI="${PLANETARY_SATURN_AVI:-/mnt/qnap/astro_data/2026-09-06/Saturn/2026-09-07-020421-Saturn-Bin1 Lqef 16.7F.avi}"
CHECKER="${PLANETARY_CHECK_REAL:-$ROOT/build/test/check-real-fixtures}"
REAL_OUT="$HERE/output/real"

[ -f "$AVI" ] || { echo "FAIL: source AVI not found: $AVI"; exit 1; }
[ -x "$CHECKER" ] || { echo "FAIL: check-real-fixtures not built at $CHECKER (cmake --build modules/planetary/build)"; exit 1; }
for tool in python3 ffmpeg ffprobe jq; do
   command -v "$tool" >/dev/null || { echo "FAIL: $tool not found"; exit 1; }
done

WORK="$(mktemp -d "${TMPDIR:-/tmp}/planetary-real.XXXXXX")"
chmod 700 "$WORK"
trap 'rm -rf "$WORK"' EXIT

python3 -I "$HERE/avi_to_ser.py" "$AVI" "$WORK/ser" --frames 100 || { echo "FAIL: avi_to_ser.py"; exit 1; }
"$CHECKER" "$WORK/ser/expected-checks.tsv" || { echo "FAIL: the C++ reader disagrees with the AVI-derived expectations"; exit 1; }

# The GUI smoke's R/B-swap check needs a clear R/B balance in the raw samples. A neutral
# balance is only acceptable for a non-colour capture; a Bayer sidecar fails loudly here.
RB="$(jq -r '.gui.rbOrder' "$WORK/ser/expected.json")"
RB_ENV=()
if [ "$RB" = "none" ]; then
   if grep -qE '^(Bayer|Debayer Type) *=' "$AVI.txt" 2>/dev/null; then
      echo "FAIL: the capture sidecar says Bayer/colour but the raw R/B balance is neutral (ratio within 1.15): no R/B-swap check is possible, so the GUI run could not vouch for colour order"
      exit 1
   fi
   echo "NOTE: no R/B order check (neutral raw balance, sidecar shows no Bayer pattern)"
else
   RB_ENV=( PLANETARY_GUI_EXPECT_RB="$RB" )
   echo "R/B order check: raw samples say R $RB B"
fi

planetary_require_signing_pass || exit 2

PLANETARY_PROBE_CASES="$WORK/ser/expected.json" bash "$HERE/run-probe.sh" || exit 1

GUI_ENV=( PLANETARY_GUI_FIXTURE_A="$WORK/ser/saturn_rggb16_12bit.ser" PLANETARY_GUI_FIXTURE_B="$WORK/ser/saturn_rgb8.ser" )
GUI_ENV+=( "${RB_ENV[@]}" )
rc=0
env "${GUI_ENV[@]}" bash "$HERE/gui-smoke.sh" "$WORK/gui" || rc=$?
mkdir -p "$REAL_OUT"
rm -f "$REAL_OUT"/*.png "$REAL_OUT"/checks.txt
cp "$WORK"/gui/*.png "$REAL_OUT"/ 2>/dev/null || true
[ -f "$WORK/gui/checks.txt" ] && cp "$WORK/gui/checks.txt" "$REAL_OUT"/
[ "$rc" -eq 0 ] || { echo "FAIL: gui-smoke.sh exited $rc (PNGs, if any, in $REAL_OUT)"; exit "$rc"; }
echo "PASS: Saturn real-data e2e (probe + GUI); PNGs in $REAL_OUT"
