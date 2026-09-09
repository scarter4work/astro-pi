#!/usr/bin/env bash
# Consolidated Stage 2 report: race vs Huber, on one code state, four corpora,
# normalisation ON and OFF.
#
# The archived 2026-09-07 dumps are re-analysed here too, with the SAME
# analyser as everything else.  They cannot serve as the OFF arm of the
# experiment -- they predate v5.0.4.0/1/2 -- but they do show whether this
# reconstructed analyser agrees with the published numbers.
set -uo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
cd "$REPO"
D=$HOME/.cache/nukex_huber_postnorm
A=$HOME/.cache/nukex_spike_corpora

echo "############################################################"
echo "# 1. Pixel-scale noise: race vs Huber, same voxels"
echo "############################################################"
echo
for tag in ngc7635 m27_2025 m16 m27_2023; do
    for mode in norm nonorm; do
        f="$D/mu_${tag}_${mode}.bin"
        [ -f "$f" ] && python3 tools/research/analyze_mu.py "$f" \
            "$tag -- normalisation $(echo "$mode" | tr 'a-z' 'A-Z' | sed 's/NONORM/OFF/;s/^NORM$/ON/')" \
            --estimator=std
    done
done

echo "############################################################"
echo "# 2. The 2026-09-07 archive, through the same analyser"
echo "############################################################"
echo
for f in "$A"/mu_*.bin; do
    [ -f "$f" ] && python3 tools/research/analyze_mu.py "$f" \
        "ARCHIVE $(basename "$f" .bin) (pre-norm, pre-v5.0.4.x)" --estimator=std
done

echo "############################################################"
echo "# 3. Winner-conditioned deviation from Huber, and the"
echo "#    unbiased scale / dAICc diagnostics Stage 1 left open"
echo "############################################################"
echo
for tag in ngc7635 m27_2025 m16 m27_2023; do
    for mode in norm nonorm; do
        f="$D/vox_${tag}_${mode}.bin"
        [ -f "$f" ] && python3 tools/research/analyze_winner_deviation.py "$f" \
            "$tag -- normalisation $(echo "$mode" | tr 'a-z' 'A-Z' | sed 's/NONORM/OFF/;s/^NORM$/ON/')"
    done
done
