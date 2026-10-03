#!/usr/bin/env bash
# Run one corpus through spike_stack with both research dumps enabled.
#
#   run_mu_corpus.sh <tag> <lights-dir> [max-frames] [norm|nonorm]
#
# Deliberately ONE corpus per invocation: two 24 MP stacks at once will not fit
# in 30 GB of RAM, and a swapping box measures the swap, not the fitter.
set -euo pipefail

# StackingEngine resolves the QE database at the RELATIVE path
# share/qe_database.json, so every run must start from the repo root.  Running
# from tools/research fails instantly with "QE database missing or unreadable".
REPO=$(cd "$(dirname "$0")/../.." && pwd)
cd "$REPO"

TAG=${1:?tag}
DIR=${2:?lights-dir}
MAXF=${3:-0}
MODE=${4:-norm}

OUT=$HOME/.cache/nukex_huber_postnorm
CACHE=$HOME/.cache/nukex_huber_frames
mkdir -p "$OUT"
rm -rf "$CACHE"; mkdir -p "$CACHE"

export NUKEX_DUMP_MU="$OUT/mu_${TAG}_${MODE}.bin"
export NUKEX_DUMP_VOXELS="$OUT/vox_${TAG}_${MODE}.bin"
export NUKEX_DUMP_MAX=500000
[ "$MAXF" != "0" ] && export NUKEX_SPIKE_MAX_FRAMES="$MAXF"
[ "$MODE" = "nonorm" ] && export NUKEX_SPIKE_NO_NORM=1

echo "=== $TAG ($MODE, max_frames=$MAXF) ==="
date
/usr/bin/time -v "$REPO/build/tools/research/spike_stack" \
    "$DIR" "$CACHE" 2>&1
date

# The frame cache is the biggest thing on disk here and is worthless once the
# run is over; leaving four of them behind fills /home.
rm -rf "$CACHE"
ls -la "$OUT"
