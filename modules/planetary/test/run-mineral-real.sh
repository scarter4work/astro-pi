#!/usr/bin/env bash
# Mineral moon real-data acceptance (Task B9; spec docs/superpowers/specs/2026-10-27-planetary-mineral-moon-design.md,
# Part B "Acceptance" and its amendment). Host-only, test-only: needs /mnt/qnap, which is only read; never launches
# PixInsight.
#
#   bash modules/planetary/test/run-mineral-real.sh <out-dir> [capture-id ...]
#
# For each capture below: stack-probe stacks it (at the capture's Bayer pattern) and runs the mineral moon at
# k = 8 (64.6F also at k = 5 and 12, re-run on the cached stack as the window's slider does), writing the stack,
# the mineral image and its stages (stack-probe --mineral-stages) to <out-dir>/<id>/full/; then the even and the
# odd frames alone, the same way, to <id>/even/ and <id>/odd/ (the split-half checks), and the halves once more cut to
# their common field before the mineral moon (stack-probe --mineral-crop, from mineral_check.py --equal-field) to
# <id>/even_eq/ and <id>/odd_eq/, where R12 is judged. mineral_check.py then
# prints every requirement's number with PASS / FAIL / NOT JUDGED, writes <id>/check.json and the side-by-side
# PNGs, and one <out-dir>/summary.json. The gating run (HaO3) must be refused, exit 4, with the narrowband text.
# capture-id ... restricts the run to those captures (the gating run always runs); an id whose three probe
# JSONs are already in <out-dir> is not re-stacked unless PLANETARY_MINERAL_RESTACK=1.
#
# Environment: PLANETARY_STACK_PROBE (default build/test/stack-probe), PLANETARY_MINERAL_THREADS (default 32,
# the spec's timing condition), PLANETARY_MINERAL_PYTHON (numpy scipy Pillow; default python3).
# Exit 0 when every requirement judged passed on every capture, 1 otherwise (or on a harness error).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
PROBE="${PLANETARY_STACK_PROBE:-$ROOT/build/test/stack-probe}"
THREADS="${PLANETARY_MINERAL_THREADS:-32}"
PY="${PLANETARY_MINERAL_PYTHON:-python3}"
NAS=/mnt/qnap/astro_data

[ $# -ge 1 ] || { echo "usage: run-mineral-real.sh <out-dir> [capture-id ...]"; exit 1; }
OUT="$1"; shift
[ -x "$PROBE" ] || { echo "FAIL: stack-probe not built at $PROBE"; exit 1; }
[ -d "$NAS" ] || { echo "FAIL: $NAS not mounted"; exit 1; }
"$PY" -I -c 'import numpy, scipy, PIL' 2>/dev/null || { echo "FAIL: $PY lacks numpy / scipy / Pillow"; exit 1; }
mkdir -p "$OUT"

# id | path under the NAS | --bayer | extra strengths (k:name,...) | role
CAPTURES=(
  "646F|2026-10-23/Moon/2026-10-23-203401-Moon-Bin1 Lqef 64.6F.avi|auto|5,12|reference"
  "700F|2026-10-23/Moon/2026-10-23-203825-Moon-Bin1 Lqef 70.0F.avi|auto||acceptance"
  "0528a|2026-05-28/Moon/2026-05-28-223104-Moon-Bin1 Lqef 18.9F.avi|auto||acceptance"
  "0528b|2026-05-28/Moon/2026-05-28-223435-Moon-Bin1 Lqef 19.2F.avi|auto||acceptance"
  "0201-149|2_1_2024/2024-02-01-013847-Moon-Bin1 Lqef 14.9F.avi|auto||acceptance"
  "0201-140|2_1_2024/2024-02-01-014259-Moon-Bin1 Lqef 14.0F.avi|RGGB||acceptance"
  "0219-160|2_20_2024/2024-02-19-191344-Moon-Bin1 Lqef 16.0F.avi|RGGB||acceptance"
  "0830-220451|8_31_2023/2023-08-30-220451-Moon-Bin1 LPro 14.0F.avi|RGGB||acceptance"
  # Hazy (B9 dispatch): the lowest-contrast Moon on the NAS by sky glow -- sky 0.62 of the disc median (first
  # frame, 2x2 sums: sky 435, disc median 705), top/median 1.43.
  "0830-220111|8_31_2023/2023-08-30-220111-Moon-Bin1 LPro 14.0F.avi|RGGB||hazy"
  # The lowest top/median of a lit Moon on the NAS (1.11; 2.5% saturated): the display stretch's extreme (ENG M4).
  "0130-193142|processing-archive-2026-10-03/videos/2026-01-30-193142-Moon-Bin1 Lqef -4.0F.avi|RGGB||low-contrast"
)

want() {  # is capture id $1 selected?
  [ ${#SELECTED[@]} -eq 0 ] && return 0
  local s; for s in "${SELECTED[@]}"; do [ "$s" = "$1" ] && return 0; done; return 1
}
SELECTED=("$@")

run_probe() {  # dir video bayer parity(full|even|odd) extras
  local dir="$1" video="$2" bayer="$3" part="$4" extras="$5"
  mkdir -p "$dir/stages"
  local args=("$video" --bayer "$bayer" --threads "$THREADS" --json --target auto --mineral
              --mineral-out "$dir/mineral.fits" --mineral-stages "$dir/stages")
  [ "$part" != full ] && args+=(--parity "$part")
  [ "$part" = full ] && args+=(--out "$dir/stack.fits")
  if [ -n "$extras" ]; then
    local k; IFS=',' read -ra ks <<< "$extras"
    for k in "${ks[@]}"; do args+=(--mineral-also "$k:$dir/mineral_k$k.fits"); done
  fi
  local t0; t0=$(date +%s.%N)
  "$PROBE" "${args[@]}" > "$dir/probe.json" 2> "$dir/probe.log"
  local rc=$?
  awk -v a="$t0" -v b="$(date +%s.%N)" 'BEGIN { printf "%.2f\n", b - a }' > "$dir/wall.txt"
  return $rc
}

fail=0
for entry in "${CAPTURES[@]}"; do
  IFS='|' read -r id rel bayer extras role <<< "$entry"
  want "$id" || continue
  video="$NAS/$rel"
  [ -f "$video" ] || { echo "FAIL $id: $video not found"; fail=1; continue; }
  for part in full even odd; do
    d="$OUT/$id/$part"
    if [ -s "$d/probe.json" ] && [ "${PLANETARY_MINERAL_RESTACK:-0}" != 1 ]; then continue; fi
    echo "== $id $part ($role, --bayer $bayer)"
    run_probe "$d" "$video" "$bayer" "$part" "$([ $part = full ] && echo "$extras")"
    rc=$?
    if [ $rc -ne 0 ]; then
      echo "FAIL $id $part: stack-probe exit $rc (see $d/probe.log)"; tail -3 "$d/probe.log"; fail=1
    fi
  done
  # The halves again, each cut to their common field before the mineral moon (R12's equal-field check).
  if [ ! -s "$OUT/$id/even_eq/probe.json" ] || [ "${PLANETARY_MINERAL_RESTACK:-0}" = 1 ]; then
    if crops=$("$PY" -I "$HERE/mineral_check.py" --equal-field "$OUT/$id"); then
      read -r ce co <<< "$crops"
      for part in even odd; do
        d="$OUT/$id/${part}_eq"; mkdir -p "$d/stages"
        crop=$([ $part = even ] && echo "$ce" || echo "$co")
        echo "== $id ${part}_eq (--mineral-crop $crop)"
        "$PROBE" "$video" --bayer "$bayer" --threads "$THREADS" --json --target auto --mineral --parity "$part" \
          --mineral-crop "$crop" --mineral-out "$d/mineral.fits" --mineral-stages "$d/stages" > "$d/probe.json" 2> "$d/probe.log" \
          || { echo "FAIL $id ${part}_eq: stack-probe exit $? (see $d/probe.log)"; fail=1; }
      done
    else
      echo "FAIL $id: no common field for the halves"; fail=1
    fi
  fi
done

# The gating run: HaO3 with the RGGB pattern set (Auto resolves it mono, refused first for that) -> exit 4,
# the narrowband text; nothing stacked.
GATE="$NAS/2_1_2024/2024-02-01-013058-Moon-Bin1 HaO3 16.2F.avi"
mkdir -p "$OUT/gate"
"$PROBE" "$GATE" --bayer RGGB --json --mineral > "$OUT/gate/rggb.json" 2> "$OUT/gate/rggb.log"; echo $? > "$OUT/gate/rggb.exit"
"$PROBE" "$GATE" --bayer auto --json --mineral > "$OUT/gate/auto.json" 2> "$OUT/gate/auto.log"; echo $? > "$OUT/gate/auto.exit"

ids=()
for entry in "${CAPTURES[@]}"; do
  IFS='|' read -r id rel bayer extras role <<< "$entry"
  want "$id" && ids+=("$id:$role")
done
"$PY" -I "$HERE/mineral_check.py" "$OUT" "${ids[@]}" || fail=1
exit $fail
