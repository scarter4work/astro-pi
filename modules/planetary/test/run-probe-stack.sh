#!/usr/bin/env bash
# Headless proof of PlanetaryStack.executeGlobal(): writes synthetic captures
# (make-fixtures --stack), takes what each must give from the engine itself
# (stack-probe on the same file: pixel values, frames used, the summary lines,
# the diagnosis and its message), signs the dev build, loads it with -m= in an
# isolated settings slot on a private Xvfb display, and runs probe-stack.js
# over every case.
#
#   bash modules/planetary/test/run-probe-stack.sh   -> PASS (exit 0) / FAIL (exit 1)
#                                                     BLOCKED (exit 2): no signing pass file
#
# Slot 96, like run-probe.sh (the two never run at once: the slot guard refuses
# a second PixInsight on it). PLANETARY_SO / PLANETARY_MAKE_FIXTURES /
# PLANETARY_STACK_PROBE override the build products tested.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/harness.sh"
# A PLANETARY_SO inside the PixInsight install or repository/ is refused before anything runs (harness.sh).
[ -z "${PLANETARY_SO:-}" ] || planetary_check_so "$PLANETARY_SO" || exit 1

planetary_require_signing_pass || exit 2

SO="${PLANETARY_SO:-$ROOT/build/src/module/Planetary-pxm.so}"
MAKE_FIXTURES="${PLANETARY_MAKE_FIXTURES:-$ROOT/build/test/make-fixtures}"
STACK_PROBE="${PLANETARY_STACK_PROBE:-$ROOT/build/test/stack-probe}"
[ -f "$SO" ] || { echo "FAIL: module not built at $SO (cmake -DPLANETARY_BUILD_MODULE=ON)"; exit 1; }
[ -x "$MAKE_FIXTURES" ] || { echo "FAIL: make-fixtures not built at $MAKE_FIXTURES"; exit 1; }
[ -x "$STACK_PROBE" ] || { echo "FAIL: stack-probe not built at $STACK_PROBE"; exit 1; }
SO="$(realpath -e "$SO")"

planetary_slot_setup || exit 1
planetary_private_tmp || exit 1

FIX="$PLANETARY_RUN_DIR/fixtures"
"$MAKE_FIXTURES" --stack "$FIX" >"$PLANETARY_RUN_DIR/make-fixtures.log" 2>&1 \
   || { echo "FAIL: make-fixtures --stack failed: $(cat "$PLANETARY_RUN_DIR/make-fixtures.log")"; exit 1; }

# The engine's own outcome for every case that stacks: stack-probe exits 0 with
# an image, 2 when diagnosed, 1 on error. A fixture that no longer gives the
# outcome make-fixtures names fails here, before PixInsight starts.
CASES="$PLANETARY_RUN_DIR/probe-stack-in.json"
ENGINE="$PLANETARY_RUN_DIR/engine.json"
: > "$PLANETARY_RUN_DIR/cases.jsonl"
FIRST_IMAGE=""
while IFS= read -r c; do
   name="$(jq -r .name <<<"$c")"; kind="$(jq -r .kind <<<"$c")"; path="$(jq -r .path <<<"$c")"
   if [ "$kind" = "error" ]; then
      printf '%s\n' "$c" >> "$PLANETARY_RUN_DIR/cases.jsonl"; continue
   fi
   args=( "$path" --json --bayer "$(jq -r .probeBayer <<<"$c")" )
   while IFS= read -r xy; do args+=( --sample "$xy" ); done < <(jq -r '.sample[] | "\(.[0]),\(.[1])"' <<<"$c")
   # Mineral moon (Task B8): the engine's mineral moon with the case's parameters (the process's defaults,
   # 8 and 2.5, when the case leaves them unset).
   if jq -e '.kind == "image" and .mineral != null' <<<"$c" >/dev/null; then
      args+=( --mineral "$(jq -r '"\(.mineral.strength // 8),\(.mineral.chromaBlur // 2.5)"' <<<"$c")" )
   fi
   rc=0
   "$STACK_PROBE" "${args[@]}" >"$ENGINE" 2>"$PLANETARY_RUN_DIR/stack-probe.err" || rc=$?
   case "$kind" in
      image|cancel)
         [ "$rc" -eq 0 ] || { echo "FAIL: fixture '$name' gives no image from the engine (stack-probe exit $rc): $(jq -r .diagnosis.message "$ENGINE" 2>/dev/null) $(tail -1 "$PLANETARY_RUN_DIR/stack-probe.err")"; exit 1; } ;;
      diagnosis)
         [ "$rc" -eq 2 ] || { echo "FAIL: fixture '$name' is not diagnosed by the engine (stack-probe exit $rc)"; exit 1; }
         got="$(jq -r .diagnosis.kind "$ENGINE")"
         [ "$got" = "$(jq -r .expectKind <<<"$c")" ] \
            || { echo "FAIL: fixture '$name' gives diagnosis $got from the engine, expected $(jq -r .expectKind <<<"$c")"; exit 1; } ;;
   esac
   if jq -e '.expectFocusWarning == true' <<<"$c" >/dev/null; then
      jq -e '.warnings | any(contains("may be out of focus"))' "$ENGINE" >/dev/null \
         || { echo "FAIL: fixture '$name' is not warned out of focus by the engine: $(jq -c .focus "$ENGINE")"; exit 1; }
   fi
   # A mineral moon case must give the engine a real mineral moon: seven steps, no notice, within [0, 1].
   if jq -e '.kind == "image" and .mineral != null' <<<"$c" >/dev/null; then
      jq -e '.mineral.history | length == 7' "$ENGINE" >/dev/null \
         || { echo "FAIL: fixture '$name' gives the engine's mineral moon $(jq '.mineral.history | length' "$ENGINE") HISTORY lines, expected 7"; exit 1; }
      jq -e '.mineral.notice == "" and .mineral.min >= 0 and .mineral.max <= 1' "$ENGINE" >/dev/null \
         || { echo "FAIL: fixture '$name': the engine's mineral moon has a notice or a sample outside [0, 1]: $(jq -c '.mineral | {notice, min, max}' "$ENGINE")"; exit 1; }
   fi
   if [ "$kind" = "cancel" ]; then
      secs="$(jq -r .seconds "$ENGINE")"
      jq -e --argjson s "$secs" '$s >= .minSeconds' <<<"$c" >/dev/null \
         || { echo "FAIL: fixture '$name' stacks in $secs s, too fast for an abort to land mid-run"; exit 1; }
   fi
   # image: the engine's size, pixel values, summary and warnings become the
   # expectations (a reader warning ending in the viewing tail is reworded for a
   # stack, and the engine's undeclared-mosaic warning (Ruling P9) is merged with
   # that reader warning into one line (stackcards::ShownWarnings), so for both only
   # the cases name the text); diagnosis: its message is the error text
   # and its numbers must be on the console.
   jq -c --slurpfile e "$ENGINE" '
      $e[0] as $r
      | ($r.warnings | map(select((endswith("; showing the raw CFA as mono -- choose the pattern under Bayer pattern")
                                   or startswith("No Bayer pattern is known for this AVI (no capture sidecar names one)")) | not))) as $w
      | if .kind == "image" then
           . + { width: $r.image.width, height: $r.image.height, channels: $r.image.channels,
                 samples: $r.samples,
                 keywords: (.keywords
                    + [ { name: "NCOMBINE", value: ($r.used | tostring) },
                        { name: "NFRAMES", value: ($r.measured | tostring) },
                        { name: "HISTORY", commentContains: ("PlanetaryStack: " + $r.selection) },
                        { name: "HISTORY", commentContains: ("PlanetaryStack: " + $r.alignment) } ]
                    + ($w | map({ name: "HISTORY", commentContains: ("PlanetaryStack warning: " + .) }))
                    + (if (.frameTimes | length) > 0
                       then [ { name: "DATE-OBS", value: .frameTimes[$r.stackedFirst] },
                              { name: "DATE-END", value: .frameTimes[$r.stackedLast] } ]
                       else [] end)),
                 consoleContains: (.consoleContains + [ $r.selection, $r.alignment ] + $w) }
             + (if .mineral != null
                then { mineralSamples: $r.mineral.samples, mineralHistory: $r.mineral.history }
                else {} end)
        elif .kind == "diagnosis" then
           . + { expectError: $r.diagnosis.message,
                 numbers: ($r.diagnosis.numbers | to_entries | map({ name: .key, value: .value })) }
        else . end' <<<"$c" >> "$PLANETARY_RUN_DIR/cases.jsonl"
   [ "$kind" = "image" ] && [ -z "$FIRST_IMAGE" ] && FIRST_IMAGE="$(tail -1 "$PLANETARY_RUN_DIR/cases.jsonl")"
done < <(jq -c '.cases[]' "$FIX/stack-cases.json")
[ -n "$FIRST_IMAGE" ] || { echo "FAIL: make-fixtures --stack wrote no image case"; exit 1; }
# After a cancel, the first image case must stack again (PixInsight still usable).
jq -s -c --argjson after "$FIRST_IMAGE" \
   '{ cases: map(if .kind == "cancel" then . + { after: $after } else . end) }' \
   "$PLANETARY_RUN_DIR/cases.jsonl" > "$CASES"
OUT="$PLANETARY_RUN_DIR/probe-stack-out.json"

planetary_sign "$SO" || exit 1
planetary_seed_slot_modules || exit 1

# --force-exit only exits after the -r= script ran; timeout fails a hang loudly.
if ! PLANETARY_PROBE_IN="$CASES" PLANETARY_PROBE_OUT="$OUT" pi_headless \
        timeout 600 "$PI" -n="$PLANETARY_SLOT" --automation-mode --no-startup-scripts \
        -m="$SO" -r="$HERE/probe-stack.js" --force-exit >"$PLANETARY_RUN_DIR/probe.log" 2>&1; then
   echo "FAIL: PixInsight timed out (600 s) or exited non-zero"; tail -20 "$PLANETARY_RUN_DIR/probe.log"; exit 1
fi
[ -s "$OUT" ] || { echo "FAIL: PixInsight never wrote the probe result (module load error or script parse failure)"; tail -20 "$PLANETARY_RUN_DIR/probe.log"; exit 1; }

jq -r '.cases[] | (if .ok then "  ok    " else "  FAIL  " end) + .name + (if .note then " (" + .note + ")" else "" end)
                 + (if .ok then "" else ": " + (.errors | join("; ")) end)' "$OUT"
if jq -e '.error' "$OUT" >/dev/null; then echo "FAIL: $(jq -r .error "$OUT")"; exit 1; fi
want="$(jq '.cases | length' "$CASES")"
got="$(jq '[.cases[] | select(.ok)] | length' "$OUT")"
if [ "$(jq -r .ok "$OUT")" = "true" ] && [ "$got" = "$want" ]; then
   echo "PASS: $got/$want PlanetaryStack probe cases"
else
   echo "FAIL: $got/$want PlanetaryStack probe cases ok"; exit 1
fi
