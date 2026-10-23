#!/usr/bin/env bash
# PlanetaryStack benchmark on real captures (host-only, test-only; needs /mnt/qnap, which is only read;
# never launches PixInsight). Spec: docs/superpowers/specs/2026-10-21-planetary-stacker-design.md,
# Yardstick and Rulings 1-2; the set, metric and limits: test/bench/README.md.
#
#   bash modules/planetary/test/run-stack-bench.sh [--report-only] [run-id ...]
#
# For each run of `bench.py plan` (every benchmark capture, stacked RGGB) it stacks the video with
# stack-probe at the run's Bayer pattern into test/output/bench/<run>/ (ours.fits, probe.json,
# probe.log, wall.txt, source.txt), then -- the split-half check of END's noise model (Ruling P28,
# validation only) -- the capture's even and odd frames separately (halves.py split -> two SER files
# in PLANETARY_BENCH_STAGE, or the run's directory, deleted afterwards; halfA/halfB .fits/.json/.log
# kept). For ours and for each half, stack-region-probe --plan-only --keff-box computes our noise-power
# frame count over the box (keff.json, halfA/halfB.keff.json): END's budget. Then `bench.py report` measures ours, AS and ASI (END, the judged metric; DAN, secondary),
# writes <run>/compare.json and <run>/side-by-side.png, and one test/output/bench/summary.md.
# Diagnosis runs (empty Saturn, clipped Venus) stack with --bayer auto and are checked against the
# expected diagnosis. test/output/ is gitignored.
#
# --report-only    skip stacking; re-measure the runs already in the output directory
# --no-split       skip the split-half stacks (the summary then says so per run)
# run-id ...       only these runs (the summary keeps the others' earlier results)
#
# Environment:
#   PLANETARY_BENCH_PYTHON  Python with numpy scipy astropy tifffile Pillow opencv-python-headless
#                           (test/bench/README.md, Setup); default python3
#   PLANETARY_STACK_PROBE   the stack-probe binary; default build/test/stack-probe
#   PLANETARY_REGION_PROBE  the stack-region-probe binary; default build/test/stack-region-probe
#   PLANETARY_BENCH_OUT     output directory; default test/output/bench
#   PLANETARY_BENCH_STAGE   a LOCAL-disk directory: each video is copied there, flushed and evicted
#                           from the page cache before stacking (cold local-disk timing, the spec's
#                           timing condition), and deleted afterwards. Unset: stacked straight from
#                           the NAS (source.txt and the summary say which)
#   PLANETARY_BENCH_THREADS stack-probe --threads; default every hardware thread
#
# Exit 0 when every run stacked/diagnosed and was measured (whatever the pass/fail/review outcome:
# that verdict is in summary.md); 1 on a harness or engine error.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BENCH="$HERE/bench"
PY="${PLANETARY_BENCH_PYTHON:-python3}"
PROBE="${PLANETARY_STACK_PROBE:-$ROOT/build/test/stack-probe}"
RPROBE="${PLANETARY_REGION_PROBE:-$ROOT/build/test/stack-region-probe}"
OUT="${PLANETARY_BENCH_OUT:-$HERE/output/bench}"
STAGE="${PLANETARY_BENCH_STAGE:-}"

REPORT_ONLY=0
SPLIT=1
ONLY=()
for a in "$@"; do
    case "$a" in
        --report-only) REPORT_ONLY=1 ;;
        --no-split) SPLIT=0 ;;
        -h|--help) sed -n "2,/^set -uo/p" "$0" | sed "$ d"; exit 0 ;;
        *) ONLY+=("$a") ;;
    esac
done

[ -x "$PROBE" ] || { echo "FAIL: stack-probe not built at $PROBE (cmake --build modules/planetary/build --target stack-probe)"; exit 1; }
[ -x "$RPROBE" ] || { echo "FAIL: stack-region-probe not built at $RPROBE (cmake --build modules/planetary/build --target stack-region-probe)"; exit 1; }
[ -d /mnt/qnap/astro_data ] || { echo "FAIL: /mnt/qnap/astro_data is not mounted"; exit 1; }
"$PY" -I -c "import numpy, scipy, astropy, tifffile, PIL, cv2" 2>/dev/null \
    || { echo "FAIL: $PY lacks the bench dependencies (test/bench/README.md, Setup); set PLANETARY_BENCH_PYTHON"; exit 1; }
if [ -n "$STAGE" ]; then
    mkdir -p "$STAGE" || { echo "FAIL: cannot create PLANETARY_BENCH_STAGE=$STAGE"; exit 1; }
    case "$(realpath "$STAGE")" in /mnt/qnap*) echo "FAIL: PLANETARY_BENCH_STAGE must be local, not on the NAS"; exit 1 ;; esac
fi
mkdir -p "$OUT"

PLAN="$("$PY" -I "$BENCH/bench.py" plan)" || { echo "FAIL: bench.py plan"; exit 1; }
status=0

selected() {
    [ ${#ONLY[@]} -eq 0 ] && return 0
    local r
    for r in "${ONLY[@]}"; do [ "$r" = "$1" ] && return 0; done
    return 1
}

if [ ${#ONLY[@]} -gt 0 ]; then
    for r in "${ONLY[@]}"; do
        printf '%s\n' "$PLAN" | cut -f1 | grep -qxF -- "$r" || { echo "FAIL: unknown run $r (bench.py plan)"; exit 1; }
    done
fi

if [ "$REPORT_ONLY" -eq 0 ]; then
    THREADS=()
    [ -n "${PLANETARY_BENCH_THREADS:-}" ] && THREADS=(--threads "$PLANETARY_BENCH_THREADS")
    while IFS=$'\t' read -r run capture bayer expect video size; do
        selected "$run" || continue
        dir="$OUT/$run"
        rm -rf "$dir"
        mkdir -p "$dir"
        [ -r "$video" ] || { echo "FAIL $run: video missing: $video"; status=1; continue; }
        src="$video"
        if [ -n "$STAGE" ]; then
            src="$STAGE/$(basename "$video")"
            cp -- "$video" "$src" || { echo "FAIL $run: staging copy"; status=1; continue; }
            # The sidecar travels with the video: the reader honours its flip, so the frame layout
            # (and the meaning of --bayer) stays the NAS file's.
            [ -f "$video.txt" ] && cp -- "$video.txt" "$src.txt"
            sync -- "$src"
            dd if="$src" iflag=nocache count=0 status=none   # evict: a cold local-disk read
            echo "local disk (staged to $STAGE, page cache evicted)" >"$dir/source.txt"
        else
            echo "NAS (/mnt/qnap, CIFS)" >"$dir/source.txt"
        fi
        echo "== $run: $(basename "$video") --bayer $bayer"
        t0=$(date +%s.%N)
        "$PROBE" "$src" --bayer "$bayer" "${THREADS[@]}" --out "$dir/ours.fits" --json >"$dir/probe.json" 2>"$dir/probe.log"
        rc=$?
        t1=$(date +%s.%N)
        echo "$t0 $t1" | awk '{printf "%.3f\n", $2 - $1}' >"$dir/wall.txt"
        # Our noise-power k over the compare box (END's budget, Ruling P28): the region probe repeats the
        # engine's plan; bench.py report checks its kept counts against probe.json.
        keff_for() {   # <video> <stack.fits> <out.json> <box-command...>
            local box
            box="$("${@:4}")" || { echo "FAIL $run: box for $(basename "$3"): $box"; status=1; return; }
            "$RPROBE" "$1" --bayer "$bayer" "${THREADS[@]}" --plan-only --keff-box "$box" --json-out "$3" \
                >"${3%.json}.log" 2>&1 || { echo "FAIL $run: stack-region-probe for $(basename "$3"): $(tail -2 "${3%.json}.log")"; status=1; rm -f "$3"; }
        }
        if [ "$expect" = stack ] && [ "$rc" -eq 0 ]; then
            keff_for "$src" "$dir/ours.fits" "$dir/keff.json" "$PY" -I "$BENCH/bench.py" box "$run" "$dir/ours.fits"
        fi
        if [ "$SPLIT" -eq 1 ] && [ "$expect" = stack ] && [ "$rc" -eq 0 ]; then
            hdir="${STAGE:-$dir}"
            even="$hdir/$run.even.ser"
            odd="$hdir/$run.odd.ser"
            if "$PY" -I "$BENCH/halves.py" split "$src" "$size" "$even" "$odd" >"$dir/halves.log" 2>&1; then
                for h in A B; do
                    f="$even"; [ "$h" = B ] && f="$odd"
                    "$PROBE" "$f" --bayer "$bayer" "${THREADS[@]}" --out "$dir/half$h.fits" --json \
                        >"$dir/half$h.json" 2>"$dir/half$h.log" \
                        || { echo "FAIL $run: half $h stack-probe: $(tail -2 "$dir/half$h.log")"; rm -f "$dir/half$h.json"; status=1; }
                done
                # both halves' k over A's target box (in B's own grid for B), where halves.check measures D (N-m3, N2-m5)
                if [ -f "$dir/halfA.json" ] && [ -f "$dir/halfB.json" ]; then
                    keff_for "$even" "$dir/halfA.fits" "$dir/halfA.keff.json" "$PY" -I "$BENCH/bench.py" target-box "$dir/halfA.fits"
                    keff_for "$odd" "$dir/halfB.fits" "$dir/halfB.keff.json" "$PY" -I "$BENCH/bench.py" target-box "$dir/halfA.fits" "$dir/halfB.fits"
                fi
            else
                echo "FAIL $run: split: $(tail -2 "$dir/halves.log")"; status=1
            fi
            rm -f -- "$even" "$odd"
        fi
        if [ -n "$STAGE" ]; then rm -f -- "$src" "$src.txt"; fi
        case "$rc" in
            0) [ "$expect" = stack ] || { echo "FAIL $run: stacked an image, expected diagnosis $expect"; } ;;
            2) [ "$expect" != stack ] || { echo "FAIL $run: diagnosed, expected a stack"; } ;;
            *) echo "FAIL $run: stack-probe exit $rc: $(tail -3 "$dir/probe.log")"; status=1; rm -f "$dir/probe.json" ;;
        esac
        echo "   exit $rc, $(cat "$dir/wall.txt") s wall"
    done <<<"$PLAN"
fi

"$PY" -I "$BENCH/bench.py" report "$OUT" "${ONLY[@]}" || status=1
echo "summary: $OUT/summary.md"
exit $status
