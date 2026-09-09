#!/usr/bin/env bash
# The full race-vs-Huber matrix: four corpora x {normalisation ON, OFF}, all on
# ONE code state, so every comparison in the write-up is single-variable.
#
# The 2026-09-07 dumps cannot serve as the OFF arm: they predate v5.0.4.0/1/2,
# so "pre-norm vs post-norm" across them would confound normalisation with
# three releases of other change.
set -euo pipefail
R=$(cd "$(dirname "$0")" && pwd)/run_mu_corpus.sh
L=$HOME/.cache

run() { echo "### $1 $4"; "$R" "$1" "$2" "$3" "$4" >> "$L/nukex_huber_$1_$4.log" 2>&1; }

run ngc7635  /mnt/qnap/astro_data/NGC7635/L/Lights          0  norm
run ngc7635  /mnt/qnap/astro_data/NGC7635/L/Lights          0  nonorm
run m27_2025 /mnt/qnap/astro_data/9_1_2025/M27              0  norm
run m27_2025 /mnt/qnap/astro_data/9_1_2025/M27              0  nonorm
run m16      /home/scarter4work/projects/processing/M16     30 norm
run m16      /home/scarter4work/projects/processing/M16     30 nonorm
run m27_2023 /mnt/qnap/astro_data/4_12_2023/M27             0  norm
run m27_2023 /mnt/qnap/astro_data/4_12_2023/M27             0  nonorm
echo "ALL RUNS COMPLETE"
