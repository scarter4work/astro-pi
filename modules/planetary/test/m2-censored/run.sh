#!/bin/bash
# The M2 (Ruling R6) measurements behind Accumulate.h "Clipping", Limits. Not shipped, not a ctest case; never launches
# PixInsight. Reads the AVI only; writes only under SCRATCH (about 15 GB: four SERs, five builds, the stacks).
#   run.sh AVI SCRATCH [REV]
# AVI: the 2023-03-05 Moon, /mnt/qnap/astro_data/3_6_2023/moon/2023-03-05-224808-Moon-Bin1 18.3F.avi (copy it off
# the NAS first). REV: the engine to measure (default 014c89b6, which the patches apply to); the truth is the
# unmodified capture stacked at TRUTH_REV (default 3a3092b3).
set -euo pipefail
AVI=$1 S=$2 REV=${3:-014c89b6} TRUTH_REV=${TRUTH_REV:-3a3092b3}
here=$(cd "$(dirname "$0")" && pwd)
repo=$(git -C "$here" rev-parse --show-toplevel)
mkdir -p "$S/data" "$S/out"

build() {  # name rev [patch]
    local t=$S/$1
    rm -rf "$t" && mkdir -p "$t"
    git -C "$repo" archive "$2" modules/planetary tools modules/nukex/third_party/catch2 | tar -x -C "$t"
    if [ -n "${3:-}" ]; then patch -s -d "$t" -p1 < "$here/$3"; fi
    cmake -S "$t/modules/planetary" -B "$t/build" -DCMAKE_BUILD_TYPE=Release > /dev/null
    cmake --build "$t/build" -j"$(nproc)" --target stack-probe > /dev/null
}
build truth "$TRUTH_REV"
build head "$REV"
build wip "$REV" wip.patch
build tobit "$REV" tobit.patch  # the WIP plus the Tobit estimate, on when TOBIT_SIGMA is set

for v in "base" "dead --dead 1469,215,5" "g125 --gain 1.25 --gain-frames 0:256" \
         "g125dead --gain 1.25 --gain-frames 0:256 --dead 1469,215,5"; do
    set -- $v
    name=$1; shift
    [ -f "$S/data/$name.ser" ] || python3 "$here/mkser.py" "$AVI" "$S/data/$name.ser" 1920 1080 "$@"
done

stack() {  # build out data [env]
    env ${4:-} "$S/$1/build/test/stack-probe" "$S/data/$3.ser" --bayer RGGB --threads 32 --json --out "$S/out/$2.fits" \
        > "$S/out/$2.json" 2> /dev/null
}
stack truth truth base
stack truth truth_dead dead
for d in base dead g125 g125dead; do stack head "head_$d" "$d"; done
for d in g125 g125dead; do stack wip "wip_$d" "$d"; done
for s in 1e-6 0.01 0.03; do
    for d in base g125 g125dead; do stack tobit "tobit${s}_$d" "$d" "TOBIT_SIGMA=$s"; done
done

cmp() { python3 "$here/cmp.py" "$@"; }
echo "== REV against TRUTH_REV on the unmodified capture"; cmp diff "$S/out/truth.fits" "$S/out/head_base.fits"
echo "== dead site, gain 1 (HEAD)"; cmp dim "$S/out/head_base.fits" "$S/out/head_dead.fits"
for x in head wip tobit1e-6 tobit0.01 tobit0.03; do
    echo "== $x: g125 against the truth"; cmp m2 "$S/out/truth.fits" "$S/out/${x}_g125.fits"
    echo "== $x: dead site with g125"; cmp dim "$S/out/${x}_g125.fits" "$S/out/${x}_g125dead.fits"
done
for s in 1e-6 0.01 0.03; do
    echo "== tobit$s: the unmodified capture against the truth"; cmp m2 "$S/out/truth.fits" "$S/out/tobit${s}_base.fits"
done
echo "== dead site, gain 1, at TRUTH_REV"; cmp dim "$S/out/truth.fits" "$S/out/truth_dead.fits"
for x in truth truth_dead head_base head_dead; do echo "$x rejected cells: $(jq .rejectedCells "$S/out/$x.json")"; done
