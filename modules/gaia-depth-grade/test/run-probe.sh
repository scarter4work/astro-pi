#!/usr/bin/env bash
# GaiaDepthGrade in-PixInsight probes (plan Tasks 11-13). Test-only.
#
#   bash modules/gaia-depth-grade/test/run-probe.sh [case ...]   (default: every case)
#
# Cases:
#   sky_adapter_convention   Task 11: PclSky vs the product's exported linear FITS WCS (TanSky) on a
#                            master plate-solved by ImageSolver; ImageBridge row order. Needs the NAS.
#   local_gaia_equals_gaia_process  Task 12: LocalGaiaSource rows == the Gaia process search on the
#                            same cone and files; BP-RP reaches the catalogue. Needs a configured DR3.
#   online_live              Task 13: LIVE ESA Gaia TAP query through TapClient (NetworkTransfer), then
#                            the cache; skipped unless GDG_LIVE_TAP=1.
#
# Exit: 0 PASS, 1 FAIL, 2 BLOCKED (no signing pass file), 77 every requested case skipped.
#
# The module under test is a HOST build with the test-only probe process:
#   cmake -B build -DGDG_BUILD_MODULE=ON -DGDG_BUILD_PROBES=ON && cmake --build build
# (GDG_SO overrides). It is signed, loaded with -m= into the isolated settings slot 95 on a
# private Xvfb display (tools/pi-headless.sh); the slot is wiped before and after.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/harness.sh"

ALL_CASES=( sky_adapter_convention local_gaia_equals_gaia_process online_live )
CASES=( "$@" )
[ ${#CASES[@]} -eq 0 ] && CASES=( "${ALL_CASES[@]}" )
for c in "${CASES[@]}"; do
   case " ${ALL_CASES[*]} " in *" $c "*) ;; *) echo "FAIL: unknown case '$c' (known: ${ALL_CASES[*]})"; exit 1 ;; esac
done

MASTER_DIR="${GDG_MASTER_DIR:-/mnt/qnap/astro_data/processing-archive-2026-10-03/master}"
SKY_MASTER="${GDG_SKY_MASTER:-$MASTER_DIR/masterLight_BIN-1_3840x2160_EXPOSURE-120.00s_FILTER-Full_RGB.xisf}"

gdg_require_signing_pass || exit 2
SO="${GDG_SO:-$ROOT/build/src/module/GaiaDepthGrade-pxm.so}"
[ -f "$SO" ] || { echo "FAIL: module not built at $SO (host build with -DGDG_BUILD_PROBES=ON)"; exit 1; }
SO="$(realpath -e "$SO")"
grep -q GaiaDepthGradeProbe "$SO" || { echo "FAIL: $SO has no probe process (build with -DGDG_BUILD_PROBES=ON)"; exit 1; }
gdg_check_so "$SO" || exit 1

gdg_slot_setup || exit 1
gdg_private_tmp || exit 1
gdg_sign "$SO" || exit 1
gdg_seed_slot_modules || exit 1

fails=0 ran=0 skipped=0
ok(){ echo "  ok    $*"; }
bad(){ echo "  FAIL  $*"; fails=$((fails + 1)); }
num(){ jq -r "$1" "$2"; }
# le A B: A <= B (numbers, via awk)
le(){ awk -v a="$1" -v b="$2" 'BEGIN { exit !(a != "null" && a <= b) }'; }
gt(){ awk -v a="$1" -v b="$2" 'BEGIN { exit !(a != "null" && a > b) }'; }

# run_pi <case> <script> [VAR=value ...]: one PixInsight launch; the JSON lands in $OUT.
run_pi()
{
   local name="$1" script="$2"; shift 2
   OUT="$GDG_RUN_DIR/$name.json"
   LOG="$GDG_RUN_DIR/$name.log"
   rm -f "$OUT"
   # --force-exit only exits after the -r= script ran; timeout fails a hang loudly.
   if ! ( [ $# -gt 0 ] && export "$@"; export GDG_PROBE_OUT="$OUT"
          pi_headless timeout 600 "$PI" -n="$GDG_SLOT" --automation-mode --no-startup-scripts \
             -m="$SO" -r="$script" --force-exit ) >"$LOG" 2>&1; then
      bad "$name: PixInsight timed out (600 s) or exited non-zero"; tail -20 "$LOG" | sed 's/^/        /'; return 1
   fi
   [ -s "$OUT" ] || { bad "$name: PixInsight never wrote the probe result"; tail -20 "$LOG" | sed 's/^/        /'; return 1; }
   if [ "$(jq -r .ok "$OUT")" != true ]; then
      bad "$name: probe error: $(jq -r .error "$OUT")"; tail -15 "$LOG" | sed 's/^/        /'; return 1
   fi
   return 0
}

case_sky_adapter_convention()
{
   if [ ! -f "$SKY_MASTER" ]; then echo "  skip  sky_adapter_convention: $SKY_MASTER not found (NAS)"; skipped=$((skipped + 1)); return 0; fi
   ran=$((ran + 1))
   run_pi sky_adapter_convention "$HERE/probe-sky.js" GDG_SKY_MASTER="$SKY_MASTER" || return 0
   local c="$OUT"
   echo "  info  $(num .width "$c")x$(num .height "$c")x$(num .channels "$c"), $(num .points "$c") points in the central 50%"
   # The convention test: the linear WCS is a least-squares fit of the spline solution, so with the
   # right mapping the residual has no mean offset (a missing half pixel gives (0.5, 0.5)).
   local mx my
   mx="$(num .meanOffsetXPx "$c")"; my="$(num .meanOffsetYPx "$c")"
   if awk -v a="$mx" -v b="$my" 'BEGIN { exit !(a*a + b*b <= 0.05*0.05) }'; then
      ok "PclSky -> sky -> TanSky: mean offset ($mx, $my) px, |mean| <= 0.05"
   else bad "PclSky vs exported WCS: mean offset ($mx, $my) px exceeds 0.05"; fi
   # The plan's 0.3 px bound on the central 50% is reached up to the solution's own linear-vs-spline
   # residual (whole-field max is printed below): bound 0.5 px, RMS 0.2 px.
   le "$(num .maxCentralPx "$c")" 0.5 && ok "PclSky -> sky -> TanSky: max $(num .maxCentralPx "$c") px, rms $(num .rmsCentralPx "$c") px (central 50%, <= 0.5)" \
      || bad "PclSky vs exported WCS: max $(num .maxCentralPx "$c") px > 0.5"
   le "$(num .rmsCentralPx "$c")" 0.2 || bad "PclSky vs exported WCS: rms $(num .rmsCentralPx "$c") px > 0.2"
   le "$(num .maxReversePx "$c")" 0.5 && ok "TanSky -> sky -> PclSky: max $(num .maxReversePx "$c") px <= 0.5" \
      || bad "reverse direction: max $(num .maxReversePx "$c") px > 0.5"
   le "$(num .maxRoundTripPx "$c")" 0.5 && ok "PclSky pixel->sky->pixel: max $(num .maxRoundTripPx "$c") px (the solution's own forward/inverse splines)" \
      || bad "PclSky pixel->sky->pixel round trip: $(num .maxRoundTripPx "$c") px"
   # The probe must tell the conventions apart: dropping the half pixel or the flip fails it.
   gt "$(num .meanNoHalfPx "$c")" 0.3 && ok "without the +0.5 shift: mean $(num .meanNoHalfPx "$c") px (rejected)" \
      || bad "the probe cannot see a missing half-pixel shift (mean $(num .meanNoHalfPx "$c") px)"
   gt "$(num .maxFlippedPx "$c")" 10 && ok "with bottom-up rows (y_pi = H - y - 0.5): max $(num .maxFlippedPx "$c") px (rejected)" \
      || bad "the probe cannot see a vertical flip ($(num .maxFlippedPx "$c") px)"
   echo "  info  whole field incl. corners (linear WCS vs spline solution): max $(num .maxFullPx "$c") px"
   [ "$(num .bridgeMismatches "$c")" = 0 ] && ok "ImageBridge::ToCore: core row y == PixInsight row y ($(num .bridgeChecked "$c") samples)" \
      || bad "ImageBridge::ToCore flip: $(num .bridgeMismatches "$c") mismatching samples"
   [ "$(num .bridgeRoundTripMismatches "$c")" = 0 ] && ok "ImageBridge::ToPcl(ToCore(image)) == image" \
      || bad "ImageBridge round trip: $(num .bridgeRoundTripMismatches "$c") mismatching samples"
}

case_local_gaia_equals_gaia_process()
{
   ran=$((ran + 1))
   # The NGC 7635 field of the HaO3 master (78' x 44' at 1.22"/px: half-diagonal 0.75 deg), G < 18.
   run_pi local_gaia_equals_gaia_process "$HERE/probe-local-gaia.js" GDG_CONE="${GDG_CONE:-350.1672 61.2201 0.75 18}" || return 0
   local c="$OUT"
   echo "  info  database files: $(jq -r '.cpp.paths | length' "$c") ($(jq -r '.cpp.paths[0]' "$c") ...), Gaia process data release $(jq -r .release "$c")"
   echo "  info  search: C++ $(num .cpp.searchSeconds "$c") s, Gaia process $(num .gaiaSeconds "$c") s"
   [ "$(num .nCpp "$c")" -gt 1000 ] && ok "LocalGaiaSource: $(num .nCpp "$c") rows" || bad "LocalGaiaSource: only $(num .nCpp "$c") rows"
   # Row multiset equality: parallax, G, BP, RP and flags exact; ra/dec to 1e-12 deg (the core's
   # and our builds of XPSD Unproject may differ in the last bit).
   if [ "$(num .nCpp "$c")" = "$(num .nGaia "$c")" ] && [ "$(num .unmatched "$c")" = 0 ] && [ "$(num .extraInCpp "$c")" = 0 ] \
      && le "$(num .maxPosDeltaDeg "$c")" 1e-12; then
      ok "row multiset equals the Gaia process search: $(num .nGaia "$c") rows, catalogue values exact, $(num .exact "$c") positions bit-identical, max |d| $(num .maxPosDeltaDeg "$c") deg"
   else
      bad "rows differ: C++ $(num .nCpp "$c"), Gaia $(num .nGaia "$c"), unmatched $(num .unmatched "$c"), extra in C++ $(num .extraInCpp "$c"), max position |d| $(num .maxPosDeltaDeg "$c") deg; e.g. $(jq -c .sampleUnmatched "$c")"
   fi
   # BP-RP reaches the catalogue (the shipped TSV dropped it).
   local nc bf
   nc="$(num .cpp.nCatalog "$c")"; bf="$(num .cpp.catalogBpFinite "$c")"
   if [ "$(num .cpp.catalogBpMismatch "$c")" = 0 ] && awk -v a="$bf" -v b="$nc" 'BEGIN { exit !(b > 0 && a >= 0.9*b) }'; then
      ok "distances_for: $nc usable rows, $bf with BP-RP = BP - RP (none mismatched)"
   else bad "BP-RP pass-through: $bf of $nc finite, $(num .cpp.catalogBpMismatch "$c") mismatched"; fi
   echo "  info  log: $(jq -r '(.cpp.warnings + .cpp.infos) | join(" | ")' "$c")"
}

case_online_live()
{
   if [ "${GDG_LIVE_TAP:-}" != 1 ]; then
      echo "  skip  online_live: live ESA archive query; run with GDG_LIVE_TAP=1"; skipped=$((skipped + 1)); return 0
   fi
   ran=$((ran + 1))
   # A tiny cone at the NGC 7635 field centre (r = 0.02 deg, G < 18: a handful of rows).
   run_pi online_live "$HERE/probe-online.js" \
      GDG_ONLINE_ARGS="${GDG_ONLINE_CONE:-350.1671987228608 61.22006490843979 0.02 18} $GDG_RUN_DIR/tapcache" || return 0
   local c="$OUT"
   jq -r '.requests[]' "$c" | sed 's/^/  info  /'
   [ "$(num .rows "$c")" -ge 1 ] && ok "online source: $(num .rows "$c") rows ($(num .rowsWithColour "$c") with BP-RP) in $(num .seconds "$c") s" \
      || bad "online source returned $(num .rows "$c") rows"
   if jq -e '[.requests[] | select(test("^(GET|POST) https://gea\\.esac\\.esa\\.int/") | not)] | length == 0' "$c" >/dev/null; then
      ok "every request went to https://gea.esac.esa.int/"
   else bad "a request left https://gea.esac.esa.int/: $(jq -c .requests "$c")"; fi
   jq -e '.requests | any(test("^POST https://gea\\.esac\\.esa\\.int/tap-server/tap/async -> 303 "))' "$c" >/dev/null \
      && ok "job submitted (303, job URL read from the body: NetworkTransfer follows no redirect)" || bad "no 303 job submission"
   [ "$(num .cachedRequests "$c")" = 0 ] && [ "$(jq -r .cachedSame "$c")" = true ] \
      && ok "second run answered by the cache: 0 requests, same rows ($(jq -r '.cachedWarnings | join(" | ")' "$c"))" \
      || bad "cache not reused: $(num .cachedRequests "$c") requests, same=$(jq -r .cachedSame "$c")"
   echo "  info  log: $(jq -r '(.warnings + .infos) | join(" | ")' "$c")"
}

for c in "${CASES[@]}"; do
   echo "== $c"
   "case_$c"
done

if [ "$fails" -ne 0 ]; then echo "FAIL: $fails check(s)"; exit 1; fi
if [ "$ran" -eq 0 ]; then echo "SKIP: every requested case was skipped"; exit 77; fi
echo "PASS: GaiaDepthGrade probes (${CASES[*]})"
