#!/usr/bin/env bash
# Planetary stack: the focus survey behind kGrossBlurLimit and kBlurLimit (Select.h step 7) on Scott's NAS
# captures (test-only, host-only: needs /mnt/qnap; no PixInsight). The NAS is only read.
#
#   bash modules/planetary/test/run-focus-survey.sh [survey|defocus|stack|limb] -> one line per run; exit 0
#                                                                                  when every run finished
#                                                                                  (and stack's checks held)
#
#   survey  (default) every planetary capture as captured, then over its first 8 / 30 / 60 / 120 frames: the
#           in-focus readings (class, reading over the judged radius, limit, S = reading / limit, verdict).
#   defocus the same captures blurred by PLANETARY_SURVEY_BLURS sensor px (default 1 2 3 4 6 8 12 16; the
#           capture's own noise put back by level, test/ProbeSource.h): the defocus series.
#   stack   RunStack (stack-probe --json) on the large in-focus discs blurred by 4 / 8 / 12 / 16 sensor px:
#           Select's verdict and the region stage's focus check (outcome, measure, warn level) -- the
#           defocus under kGrossBlurLimit that is stacked with a focus warning instead of refused (Ruling P15a).
#   limb    limb-probe on the large in-focus discs: the limb fitted on a stack of the sharpest half, and each
#           frame's quality with and without the capture's noise model (why the reading doubled with P26).
#
# PLANETARY_SURVEY_CAPTURES names a file of capture paths (one per line) in place of the list below;
# PLANETARY_FOCUS_SURVEY / PLANETARY_STACK_PROBE override the binaries. A full survey reads ~60 GB.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SURVEY="${PLANETARY_FOCUS_SURVEY:-$ROOT/build/test/focus-survey}"
PROBE="${PLANETARY_STACK_PROBE:-$ROOT/build/test/stack-probe}"
LIMB="${PLANETARY_LIMB_PROBE:-$ROOT/build/test/limb-probe}"
MODE="${1:-survey}"

A=/mnt/qnap/astro_data
V=$A/processing-archive-2026-10-03/videos
# Every planetary capture on the NAS (2026-10-22: each AVI / SER under astro_data named for a planet, one copy
# of each; the FOV / Moon / star captures hold no disc). Large discs (R_h >= kMinJudgedRadius) among them: the
# two 2022-09-17 1080p Jupiters (R_h 21.5); the 2022-08-03 015840 Jupiter is the largest small disc (R_h 14.5).
CAPTURES=(
    "$A/10_9/Video/2022-10-08-221043-Jupiter-Bin1 15.1F.avi"
    "$A/10_9/Video/2022-10-08-221545-Saturn-Bin1 16.9F.avi"
    "$A/11_10/Video/2022-11-09-190631-Jupiter-Bin1 14.4F.avi"
    "$A/11_3_2023/2023-11-02-205621-Jupiter-Bin1 LPro 17.8F.avi"
    "$A/11_3_2023/2023-11-02-210201-Jupiter-Bin1 LPro 13.8F.avi"
    "$A/11_3_2023/2023-11-02-210444-Jupiter-Bin1 LPro 13.8F.avi"
    "$A/2026-09-06/Saturn/2026-09-07-020421-Saturn-Bin1 Lqef 16.7F.avi"
    "$A/2026-09-06/Saturn/2026-09-07-020735-Saturn-Bin1 Lqef 17.6F.avi"
    "$A/2026-10-05/Saturn/2026-10-06-023544-Saturn-Bin1 HaO3 9.0F.avi"
    "$A/2_12_2023/2023-02-11-190452-Venus-Bin1 15.8F.avi"
    "$A/2_12_2023/2023-02-11-191000-Neptune-Bin1 14.9F.avi"
    "$A/2_12_2023/2023-02-11-191831-Jupiter-Bin1 13.3F.avi"
    "$A/2_12_2023/2023-02-11-192403-Mars-Bin1 16.5F.avi"
    "$A/8_13_2023/2023-08-13-023223-Saturn-Bin1 LPro 18.1F.avi"
    "$A/8_13_2023/2023-08-13-024340-Jupiter-Bin1 LPro 15.3F.avi"
    "$A/8_16/Video/2022-08-03-015840-Jupiter-Bin1 10.9F.avi"
    "$A/8_16/Video/2022-08-03-021039-Saturn-Bin1 12.0F.avi"
    "$A/8_16/Video/2022-08-17-010744-Jupiter-Bin1 10.9F.avi"
    "$A/8_16/Video/2022-08-17-010921-Jupiter-Bin1 10.9F.avi"
    "$A/8_16/Video/2022-08-17-011350-Saturn-Bin1 8.8F.avi"
    "$A/8_8_2023/video/2023-08-09-003829-Saturn-Bin1 4 18.3F.avi"
    "$A/8_8_2023/video/2023-08-09-004123-Saturn-Bin1 4 14.4F.avi"
    "$A/8_8_2023/video/2023-08-09-032151-Jupiter-Bin1 LPro 18.3F.avi"
    "$A/8_8_2023/video/2023-08-09-032515-Jupiter-Bin1 LPro 13.8F.avi"
    "$A/9_16_2023/2023-09-14-210659-Saturn-Bin1 4 16.5F.avi"
    "$A/9_16_2023/2023-09-16-041324-Jupiter-Bin1 LPro 17.8F.avi"
    "$A/9_18/Video/2022-09-08-223551-Jupiter-Bin1 6.6F.avi"
    "$A/9_18/Video/2022-09-17-223514-Jupiter-Bin1 82.4F.avi"
    "$A/9_18/Video/2022-09-17-225254-Jupiter-Bin1 76.3F.avi"
    "$A/9_18/Video/2022-09-17-225258-Jupiter-Bin1 77.0F.avi"
    "$A/9_18/Video/2022-09-17-225418-Jupiter-Bin1 80.6F.avi"
    "$A/9_18/Video/2022-09-17-230420-Jupiter-Bin1 80.6F.avi"
    "$A/9_18/Video/2022-09-18-030349-Mars-Bin1 4.3F.avi"
    "$A/9_22/Video/2022-09-23-022946-Jupiter-Bin1 -0.8F.avi"
    "$A/9_30_2023/2023-09-29-204604-Saturn-Bin1 LPro 14.9F.avi"
    "$A/9_7/Video/2022-09-08-001402-Jupiter-Bin1 40.1F.avi"
    "$A/9_7/Video/2022-09-08-002244-Saturn-Bin1 3.0F.avi"
    "$A/9_9/Video/2022-09-09-012958-Jupiter-Bin1 6.6F.avi"
    "$V/2026-01-30-195137-Jupiter-Bin1 Lqef -4.2F.avi"
    "$V/2026-01-30-195438-Jupiter-Bin1 Lqef -3.5F.avi"
    "$V/2026-01-30-195830-Jupiter-Bin1 Lqef -4.2F.avi"
    "$V/2026-01-30-195830-Jupiter-Bin1 Lqef -4.2F.ser"
    "$V/2026-01-30-201158-Jupiter-Bin1 Lqef -4.0F.avi"
    "$V/2026-01-30-202657-Jupiter-Bin1 Lqef -4.0F.avi"
    "$V/2026-01-30-203627-Saturn-Bin1 Lqef -3.8F.avi"
    "$V/2026-01-30-204515-Neptune-Bin1 Lqef -4.0F.avi"
)
LARGE=(
    "$A/9_18/Video/2022-09-17-225418-Jupiter-Bin1 80.6F.avi"
    "$A/9_18/Video/2022-09-17-230420-Jupiter-Bin1 80.6F.avi"
)
if [ -n "${PLANETARY_SURVEY_CAPTURES:-}" ]; then
    mapfile -t CAPTURES < "$PLANETARY_SURVEY_CAPTURES"
    LARGE=("${CAPTURES[@]}")
fi

fail=0
run() {
    "$@" || { echo "FAIL: exit $? -- $*"; fail=1; }
}
case "$MODE" in
    survey)
        [ -x "$SURVEY" ] || { echo "FAIL: focus-survey not built at $SURVEY (cmake --build modules/planetary/build)"; exit 1; }
        for f in "${CAPTURES[@]}"; do
            run "$SURVEY" "$f"
            for n in 8 30 60 120; do run "$SURVEY" "$f" --frames "$n"; done
        done
        ;;
    defocus)
        [ -x "$SURVEY" ] || { echo "FAIL: focus-survey not built at $SURVEY (cmake --build modules/planetary/build)"; exit 1; }
        for f in "${CAPTURES[@]}"; do
            for b in ${PLANETARY_SURVEY_BLURS:-1 2 3 4 6 8 12 16}; do run "$SURVEY" "$f" --blur "$b"; done
        done
        ;;
    stack)
        [ -x "$PROBE" ] || { echo "FAIL: stack-probe not built at $PROBE (cmake --build modules/planetary/build)"; exit 1; }
        command -v jq > /dev/null || { echo "FAIL: stack mode needs jq"; exit 1; }
        for f in "${LARGE[@]}"; do
            for b in 4 8 12 16; do
                json="$("$PROBE" "$f" --blur "$b" --json 2> /dev/null)"
                code=$?
                [ "$code" -le 2 ] || { echo "FAIL: exit $code -- $f --blur $b"; fail=1; continue; }
                printf '%s | blur %s | ' "$f" "$b"
                jq -r '"\(.diagnosis.kind) | focus \(.focus.outcome) | measure \(.focus.measure) (warn above \(.focus.warnAbove)) | limb \(.focus.limbBlur) px | radius \(.focus.radius) | focus warning \([.warnings[] | select(test("out of focus"))] | length)"' <<< "$json"
                # Ruling P15a's named cases: the 230420 Jupiter at 6 and 8 luma px (12 / 16 sensor px) reads under
                # kGrossBlurLimit (0.247 / 0.279) and must be stacked with the focus warning.
                if [[ "$f" == *230420* && ( "$b" == 12 || "$b" == 16 ) ]]; then
                    jq -e '.diagnosis.kind == "None" and ([.warnings[] | select(test("out of focus"))] | length) == 1' <<< "$json" > /dev/null \
                        || { echo "FAIL: 230420 blurred $b px is not stacked with the focus warning"; fail=1; }
                fi
            done
        done
        ;;
    limb)
        [ -x "$LIMB" ] || { echo "FAIL: limb-probe not built at $LIMB (cmake --build modules/planetary/build)"; exit 1; }
        for f in "${LARGE[@]}"; do run "$LIMB" "$f"; done
        ;;
    *)
        echo "usage: run-focus-survey.sh [survey|defocus|stack|limb]"
        exit 1
        ;;
esac
exit $fail
