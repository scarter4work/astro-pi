#!/usr/bin/env bash
# Planet processing real-data acceptance (planet Task 5; spec docs/superpowers/specs/2026-10-29-planetary-planet-
# processing-design.md, "Acceptance (the bar)" and rulings P1-P6). Host-only, test-only: needs /mnt/qnap, which is
# only read; never launches PixInsight.
#
#   bash modules/planetary/test/run-planet-real.sh <out-dir> [capture-id ...]
#
# For each capture below: stack-probe stacks it (at the capture's Bayer pattern: the sidecar's, or RGGB set
# explicitly where there is none -- mineral Ruling 3) and runs planet processing at the defaults (Planet.h), writing
# the stack, the output and its stages (stack-probe --planet-stages) to <out-dir>/<id>/full/, with the slider series
# re-run on the same cache as the window's sliders do: Sharpen 0, 1, 1.5, 2, 3, 4 (planet_s<A>.fits), Chroma blur
# 0, 0.5, 1, 1.5, 2, 2.5, 3, 4, 6 (planet_c<S>.fits) and Saturation 0 (planet_k0.fits). The even and the odd frames alone go
# the same way to <id>/even/ and <id>/odd/ (the split-half noise measures). planet_check.py then prints acceptance 1-7
# per capture with PASS / FAIL / NOT JUDGED, the series measurements, writes <id>/check.json, summary.json and the
# PNGs. A capture the table expects the stacker to refuse (Venus saturated, the 2023 Neptune with no target) must be
# refused with that diagnosis. The gating run (2026-10-05 Saturn, HaO3 only: spec P6) must be refused, exit 4, with the
# narrowband text.
# capture-id ... restricts the run to those captures (the gating run always runs); a part whose probe JSON is already
# in <out-dir> is not re-stacked unless PLANETARY_PLANET_RESTACK=1.
#
# Environment: PLANETARY_STACK_PROBE (default build/test/stack-probe), PLANETARY_PLANET_THREADS (default 32),
# PLANETARY_PLANET_PYTHON (numpy and Pillow; default python3), PLANETARY_PLANET_PNG (planet_check.py --png-dir).
# Exit 0 when every requirement judged passed on every capture, 1 otherwise (or on a harness error).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
PROBE="${PLANETARY_STACK_PROBE:-$ROOT/build/test/stack-probe}"
THREADS="${PLANETARY_PLANET_THREADS:-32}"
PY="${PLANETARY_PLANET_PYTHON:-python3}"
NAS=/mnt/qnap/astro_data

[ $# -ge 1 ] || { echo "usage: run-planet-real.sh <out-dir> [capture-id ...]"; exit 1; }
OUT="$1"; shift
[ -x "$PROBE" ] || { echo "FAIL: stack-probe not built at $PROBE"; exit 1; }
[ -d "$NAS" ] || { echo "FAIL: $NAS not mounted"; exit 1; }
"$PY" -I -c 'import numpy, PIL' 2>/dev/null || { echo "FAIL: $PY lacks numpy / Pillow"; exit 1; }
mkdir -p "$OUT"

SHARPENS="0 1 1.5 2 3 4"
BLURS="0 0.5 1 1.5 2 2.5 3 4 6"

# id | path under the NAS | --bayer | role (series: the Sharpen series goes to Scott too) | expected stacker outcome:
# "image", or the diagnosis kind the stacker must refuse the capture with (stack-probe exit 2; planet_check.py fails
# any other outcome)
CAPTURES=(
  "sat1023|2026-10-23/Saturn/2026-10-23-204422-Saturn-Bin1 Lqef 65.5F.avi|auto|series|image"
  "sat0907|2026-09-06/Saturn/2026-09-07-020421-Saturn-Bin1 Lqef 16.7F.avi|auto|acceptance|image"
  "jup0813|8_13_2023/2023-08-13-024340-Jupiter-Bin1 LPro 15.3F.avi|auto|acceptance|image"
  "jup1102|11_3_2023/2023-11-02-210201-Jupiter-Bin1 LPro 13.8F.avi|auto|series|image"
  "mars0211|2_12_2023/2023-02-11-192403-Mars-Bin1 16.5F.avi|RGGB|series|image"
  # Saturated in 6483 of 6485 frames.
  "venus0211|2_12_2023/2023-02-11-190452-Venus-Bin1 15.8F.avi|RGGB|acceptance|Clipped"
  # A target in 0 of 7406 frames.
  "nep0211|2_12_2023/2023-02-11-191000-Neptune-Bin1 14.9F.avi|RGGB|acceptance|NoTarget"
  "nep1023|2026-10-23/Neptune/2026-10-23-212210-Neptune-Bin1 Lqef 62.4F.avi|auto|acceptance|image"
)

want() {  # is capture id $1 selected?
  [ ${#SELECTED[@]} -eq 0 ] && return 0
  local s; for s in "${SELECTED[@]}"; do [ "$s" = "$1" ] && return 0; done; return 1
}
SELECTED=("$@")

run_probe() {  # dir video bayer part(full|even|odd)
  local dir="$1" video="$2" bayer="$3" part="$4" a s
  mkdir -p "$dir/stages"
  local args=("$video" --bayer "$bayer" --threads "$THREADS" --json --target auto --planet
              --planet-out "$dir/planet.fits" --planet-stages "$dir/stages" --saturation-also "0:$dir/planet_k0.fits")
  if [ "$part" = full ]; then args+=(--out "$dir/stack.fits"); else args+=(--parity "$part"); fi
  for a in $SHARPENS; do args+=(--sharpen-also "$a:$dir/planet_s$a.fits"); done
  for s in $BLURS; do args+=(--chroma-blur-also "$s:$dir/planet_c$s.fits"); done
  local t0; t0=$(date +%s.%N)
  "$PROBE" "${args[@]}" > "$dir/probe.json" 2> "$dir/probe.log"
  local rc=$?
  awk -v a="$t0" -v b="$(date +%s.%N)" 'BEGIN { printf "%.2f\n", b - a }' > "$dir/wall.txt"
  return $rc
}

fail=0
ids=()
for entry in "${CAPTURES[@]}"; do
  IFS='|' read -r id rel bayer role expect <<< "$entry"
  want "$id" || continue
  ids+=("$id:$role:$expect")
  video="$NAS/$rel"
  [ -f "$video" ] || { echo "FAIL $id: $video not found"; fail=1; continue; }
  for part in full even odd; do
    d="$OUT/$id/$part"
    if [ -s "$d/probe.json" ] && [ "${PLANETARY_PLANET_RESTACK:-0}" != 1 ]; then continue; fi
    echo "== $id $part ($role, --bayer $bayer)"
    run_probe "$d" "$video" "$bayer" "$part"
    rc=$?
    # Exit 2 is the stacker's diagnosis: an outcome where the table expects a refusal (the checker judges its kind).
    if [ $rc -ne 0 ] && ! { [ $rc -eq 2 ] && [ "$expect" != image ]; }; then
      echo "FAIL $id $part: stack-probe exit $rc (see $d/probe.log)"; tail -3 "$d/probe.log"; fail=1
    fi
  done
done

# The gating run (spec P6): 2026-10-05 Saturn holds only a HaO3 capture -> exit 4, the narrowband text, nothing
# stacked; with the pattern Auto resolves and with RGGB set (so the refusal is the filter's, not the mono one).
GATE="$NAS/2026-10-05/Saturn/2026-10-06-023544-Saturn-Bin1 HaO3 9.0F.avi"
mkdir -p "$OUT/gate"
for b in auto RGGB; do
  "$PROBE" "$GATE" --bayer "$b" --json --planet > "$OUT/gate/$b.json" 2> "$OUT/gate/$b.log"; echo $? > "$OUT/gate/$b.exit"
done

pngs=()
[ -n "${PLANETARY_PLANET_PNG:-}" ] && pngs=(--png-dir "$PLANETARY_PLANET_PNG")
"$PY" -I "$HERE/planet_check.py" "${pngs[@]}" "$OUT" "${ids[@]}" || fail=1
exit $fail
