#!/usr/bin/env bash
# Headless proof of SERViewer.executeGlobal(): writes synthetic SER fixtures,
# signs the dev build, loads it with -m= in an isolated settings slot on a
# private Xvfb display, and runs probe-execute.js over every fixture case.
#
#   bash modules/planetary/test/run-probe.sh      -> PASS (exit 0) / FAIL (exit 1)
#                                                  BLOCKED (exit 2): no signing pass file
#
# PLANETARY_SO / PLANETARY_MAKE_FIXTURES override the build products tested.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/harness.sh"

planetary_require_signing_pass || exit 2

SO="${PLANETARY_SO:-$ROOT/build/src/module/Planetary-pxm.so}"
MAKE_FIXTURES="${PLANETARY_MAKE_FIXTURES:-$ROOT/build/test/make-fixtures}"
[ -f "$SO" ] || { echo "FAIL: module not built at $SO (cmake -DPLANETARY_BUILD_MODULE=ON)"; exit 1; }
[ -x "$MAKE_FIXTURES" ] || { echo "FAIL: make-fixtures not built at $MAKE_FIXTURES"; exit 1; }
SO="$(realpath -e "$SO")"

planetary_slot_setup || exit 1
planetary_private_tmp || exit 1

FIX="$PLANETARY_RUN_DIR/fixtures"
"$MAKE_FIXTURES" "$FIX" >"$PLANETARY_RUN_DIR/make-fixtures.log" 2>&1 \
   || { echo "FAIL: make-fixtures failed: $(cat "$PLANETARY_RUN_DIR/make-fixtures.log")"; exit 1; }
OUT="$PLANETARY_RUN_DIR/probe-out.json"

planetary_sign "$SO" || exit 1
planetary_seed_slot_modules || exit 1

# --force-exit only exits after the -r= script ran; timeout fails a hang loudly.
if ! PLANETARY_PROBE_IN="$FIX/probe-in.json" PLANETARY_PROBE_OUT="$OUT" pi_headless \
        timeout 300 "$PI" -n="$PLANETARY_SLOT" --automation-mode --no-startup-scripts \
        -m="$SO" -r="$HERE/probe-execute.js" --force-exit >"$PLANETARY_RUN_DIR/probe.log" 2>&1; then
   echo "FAIL: PixInsight timed out (300 s) or exited non-zero"; tail -20 "$PLANETARY_RUN_DIR/probe.log"; exit 1
fi
[ -s "$OUT" ] || { echo "FAIL: PixInsight never wrote the probe result (module load error or script parse failure)"; tail -20 "$PLANETARY_RUN_DIR/probe.log"; exit 1; }

jq -r '.cases[] | (if .ok then "  ok    " else "  FAIL  " end) + .name + (if .via then " (" + .via + ")" else "" end)
                 + (if .ok then "" else ": " + (.errors | join("; ")) end)' "$OUT"
# How each error case failed (probe-execute.js accepts both for now): an
# exception, or executeGlobal returning false with the reason on the console.
jq -r --slurpfile in "$FIX/probe-in.json" \
   '($in[0].cases | map(select(.expectError)) | map(.name)) as $errs
    | .cases[] | select(.name as $n | $errs | index($n))
    | "  via   " + .name + ": " + (.via // "none observed")' "$OUT"
if jq -e '.error' "$OUT" >/dev/null; then echo "FAIL: $(jq -r .error "$OUT")"; exit 1; fi
want="$(jq '.cases | length' "$FIX/probe-in.json")"
got="$(jq '[.cases[] | select(.ok)] | length' "$OUT")"
if [ "$(jq -r .ok "$OUT")" = "true" ] && [ "$got" = "$want" ]; then
   echo "PASS: $got/$want SERViewer probe cases"
else
   echo "FAIL: $got/$want SERViewer probe cases ok"; exit 1
fi
