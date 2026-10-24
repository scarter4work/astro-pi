#!/usr/bin/env bash
# Planetary stack: global alignment on real captures from Scott's NAS (test-only, host-only:
# needs /mnt/qnap; no PixInsight). The NAS is only read.
#
#   bash modules/planetary/test/run-align-real.sh   -> the alignment table; exit 0 when every
#                                                      capture ran, 1 otherwise
#
# For each capture, build/test/align-probe measures and selects the frames, builds the reference and
# aligns every selected frame to it, printing: luma size and colour, disc/full-frame mode and window,
# frames selected, the shift range (luma px), confidence min / p1 / median, AlignmentCut and how many
# frames fall below it; for discs the agreement with the measured centroids; for full-frame targets
# (--split) the top/bottom-half check that shows which low-confidence frames are torn.
#
# PLANETARY_ALIGN_PROBE overrides the probe binary; PLANETARY_ALIGN_FRAMES limits each capture to its
# first N frames (default: all).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
PROBE="${PLANETARY_ALIGN_PROBE:-$ROOT/build/test/align-probe}"
[ -x "$PROBE" ] || { echo "FAIL: align-probe not built at $PROBE (cmake --build modules/planetary/build)"; exit 1; }

V=/mnt/qnap/astro_data/processing-archive-2026-10-03/videos
CAPTURES=(
    "$V/2026-01-30-195830-Jupiter-Bin1 Lqef -4.2F.avi"
    "$V/2026-01-30-202657-Jupiter-Bin1 Lqef -4.0F.avi"
    "$V/2026-01-30-195438-Jupiter-Bin1 Lqef -3.5F.avi"
    "$V/2026-01-30-204515-Neptune-Bin1 Lqef -4.0F.avi"
    "$V/2026-01-30-193142-Moon-Bin1 Lqef -4.0F.avi"
    "/mnt/qnap/astro_data/2_12_2023/2023-02-12-025133-Moon-Bin1 13.6F.avi"
    "/mnt/qnap/astro_data/2_2_23/Moon/2023-02-02-202030-Moon-Bin1 13.8F.avi"
)
ARGS=(--split)
[ -n "${PLANETARY_ALIGN_FRAMES:-}" ] && ARGS+=(--frames "$PLANETARY_ALIGN_FRAMES")

echo "capture | luma | mode window | selected | dx | dy | conf min / p1 / median | cut | below cut"
status=0
for f in "${CAPTURES[@]}"; do
    if [ ! -r "$f" ]; then
        echo "MISSING: $f"
        status=1
        continue
    fi
    "$PROBE" "$f" "${ARGS[@]}" || { echo "FAIL: $f"; status=1; }
done
exit $status
