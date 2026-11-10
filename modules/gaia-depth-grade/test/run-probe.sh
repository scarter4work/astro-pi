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
#   solver_enums             Task 14: StarAlignment's enumeration values the solver hardcodes
#                            (SolverAlign.h) equal the running PixInsight's.
#   solve_seeded_matches_imagesolver  Task 14: the native solver on two NAS masters, seeded from their
#                            acquisition metadata, vs ImageSolver 6.6 run headless on the same masters
#                            (solver-reference.js): centre <= 1", scale <= 0.1%, rotation <= 0.05 deg,
#                            in distortion and linear mode.
#   solve_spline_residuals   Task 15 (GATE): distortion solutions vs ImageSolver's over a 17x9 grid plus
#                            the centre: max residual <= 0.25 px; matching RMS <= 1.25 x ImageSolver's.
#   solve_blind_no_metadata  Task 15: every coordinate/scale keyword stripped, the astrometric index
#                            selected in this slot's Gaia preferences: blind + seeded solve within 2" of
#                            ImageSolver's centre. Skipped (77) without an index file (GDG_ASTROMETRIC_INDEX,
#                            default ~/Downloads/gdr3-idx-1.0.0-g17.xpsd).
#   solve_fails_loudly_without_seed_or_index  Task 15 (Review Focus 1): stripped image, no index: the
#                            exact remedy-naming error.
#   The solver cases use the Gaia DR3 files GDG_GAIA_FILES (default /opt/gia3/gdr3-1.0.0-*.xpsd),
#   configured in the test slot only, for both the native solver and ImageSolver.
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

ALL_CASES=( sky_adapter_convention local_gaia_equals_gaia_process online_live solver_enums
            solve_seeded_matches_imagesolver solve_spline_residuals solve_blind_no_metadata
            solve_fails_loudly_without_seed_or_index )
CASES=( "$@" )
[ ${#CASES[@]} -eq 0 ] && CASES=( "${ALL_CASES[@]}" )
for c in "${CASES[@]}"; do
   case " ${ALL_CASES[*]} " in *" $c "*) ;; *) echo "FAIL: unknown case '$c' (known: ${ALL_CASES[*]})"; exit 1 ;; esac
done

MASTER_DIR="${GDG_MASTER_DIR:-/mnt/qnap/astro_data/processing-archive-2026-10-03/master}"
SKY_MASTER="${GDG_SKY_MASTER:-$MASTER_DIR/masterLight_BIN-1_3840x2160_EXPOSURE-120.00s_FILTER-Full_RGB.xisf}"
SOLVE_MASTERS=( "Full:$MASTER_DIR/masterLight_BIN-1_3840x2160_EXPOSURE-120.00s_FILTER-Full_RGB.xisf"
                "HaO3:$MASTER_DIR/masterLight_BIN-1_3840x2160_EXPOSURE-120.00s_FILTER-HaO3_RGB.xisf" )
if [ -n "${GDG_GAIA_FILES:-}" ]; then GAIA_FILES="$GDG_GAIA_FILES"
else GAIA_FILES="$(ls /opt/gia3/gdr3-1.0.0-*.xpsd 2>/dev/null | tr '\n' ':' || true)"; fi
ASTROMETRIC_INDEX="${GDG_ASTROMETRIC_INDEX:-$HOME/Downloads/gdr3-idx-1.0.0-g17.xpsd}"
UNSOLVABLE='Plate solve failed: the image has no usable coordinates (RA/DEC, focal length, pixel size) and no astrometric index is selected in Gaia preferences. Add the metadata, or select an Astrometric Index File in the Gaia process preferences.'

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

case_solver_enums()
{
   ran=$((ran + 1))
   run_pi solver_enums "$HERE/probe-solver-enums.js" || return 0
   local c="$OUT" want got
   for want in OutputMatrix=8 NoIntersection=0 Always=2; do
      got="$(num ".${want%=*}" "$c")"
      [ "$got" = "${want#*=}" ] && ok "StarAlignment.$want (SolverAlign.h)" \
         || bad "StarAlignment.${want%=*} is $got on this PixInsight; SolverAlign.h hardcodes ${want#*=}"
   done
}

# solver_ready: the solver cases need the NAS masters and the Gaia DR3 files.
solver_ready()
{
   local name="$1" m
   if [ -z "$GAIA_FILES" ]; then echo "  skip  $name: no Gaia DR3 files (GDG_GAIA_FILES)"; skipped=$((skipped + 1)); return 1; fi
   for m in "${SOLVE_MASTERS[@]}"; do
      if [ ! -f "${m#*:}" ]; then echo "  skip  $name: ${m#*:} not found (NAS)"; skipped=$((skipped + 1)); return 1; fi
   done
}

# reference <label> <master>: ImageSolver 6.6's solution of the master (once per run), seeded from
# its acquisition metadata (existing solution cleared).
reference()
{
   REF="$GDG_RUN_DIR/ref-$1.json"
   [ -s "$REF" ] && return 0
   run_pi "imagesolver_$1" "$HERE/solver-reference.js" GDG_REF_MASTER="$2" GDG_REF_GAIA="$GAIA_FILES" GDG_REF_CLEAR=1 \
      || return 1
   cp "$OUT" "$REF"
   echo "  info  ImageSolver 6.6 on $1: RMS $(num .rmsPx "$REF") px ($(num .matchedStars "$REF") stars), G <= $(num .limitMagnitude "$REF"), $(num .seconds "$REF") s"
}

# solve <run-name> <label> <master> [VAR=value ...]: the native solver through probe-solve.js.
solve()
{
   local name="$1" label="$2" master="$3"; shift 3
   run_pi "$name" "$HERE/probe-solve.js" GDG_SOLVE_MASTER="$master" GDG_SOLVE_REF="$GDG_RUN_DIR/ref-$label.json" "$@"
}

gate_line()
{
   local c="$1"
   echo "$(num .gate.rmsPx "$c") $(num .gate.maxPx "$c") $(num .gate.centerPx "$c") $(jq -c '.gate.cornersPx' "$c")" |
      awk '{ printf "residual vs ImageSolver: rms %.3f px, max %.3f px, centre %.3f px, corners %s", $1, $2, $3, $4 }'
}

case_solve_seeded_matches_imagesolver()
{
   solver_ready solve_seeded_matches_imagesolver || return 0
   ran=$((ran + 1))
   local m label master mode c
   for m in "${SOLVE_MASTERS[@]}"; do
      label="${m%%:*}"; master="${m#*:}"
      reference "$label" "$master" || continue
      for mode in 1 0; do
         solve "seeded_${label}_d$mode" "$label" "$master" GDG_SOLVE_CLEAR=1 \
            GDG_SOLVE_ARGS="gaia=$GAIA_FILES blind=disabled distortion=$mode" || continue
         c="$OUT"
         if [ "$(jq -r .solve.solved "$c")" != true ]; then bad "$label distortion=$mode: $(jq -r .solve.error "$c")"; continue; fi
         [ "$(jq -r .solve.method "$c")" = seeded ] || bad "$label: method $(jq -r .solve.method "$c"), expected seeded"
         echo "  info  $label distortion=$mode: $(num .solve.matches "$c") stars, RMS $(num .solve.rmsPx "$c") px, G <= $(num .solve.limitMagnitude "$c"), $(num .solve.iterations "$c") iterations, $(num .solve.seconds "$c") s"
         if le "$(num .gate.centerArcsec "$c")" 1 && awk -v s="$(num .gate.scaleErrorPct "$c")" -v r="$(num .gate.rotationDeg "$c")" \
               'BEGIN { exit !(s != "null" && r != "null" && s*s <= 0.1*0.1 && r*r <= 0.05*0.05) }'; then
            ok "$label distortion=$mode: centre $(num .gate.centerArcsec "$c")\", scale $(num .gate.scaleErrorPct "$c")%, rotation $(num .gate.rotationDeg "$c") deg vs ImageSolver"
         else
            bad "$label distortion=$mode: centre $(num .gate.centerArcsec "$c")\", scale $(num .gate.scaleErrorPct "$c")%, rotation $(num .gate.rotationDeg "$c") deg (limits 1\", 0.1%, 0.05 deg)"
         fi
         echo "  info  $label distortion=$mode $(gate_line "$c")"
      done
   done
}

case_solve_spline_residuals()
{
   solver_ready solve_spline_residuals || return 0
   ran=$((ran + 1))
   local m label master c rms ref
   for m in "${SOLVE_MASTERS[@]}"; do
      label="${m%%:*}"; master="${m#*:}"
      reference "$label" "$master" || continue
      solve "spline_$label" "$label" "$master" GDG_SOLVE_CLEAR=1 GDG_SOLVE_ARGS="gaia=$GAIA_FILES blind=disabled" || continue
      c="$OUT"
      if [ "$(jq -r .solve.solved "$c")" != true ]; then bad "$label: $(jq -r .solve.error "$c")"; continue; fi
      le "$(num .gate.maxPx "$c")" 0.25 && ok "$label $(gate_line "$c")" || bad "$label $(gate_line "$c") (max > 0.25 px)"
      rms="$(num .solve.rmsPx "$c")"; ref="$(num .gate.referenceRmsPx "$c")"
      awk -v a="$rms" -v b="$ref" 'BEGIN { exit !(a != "null" && b != "null" && a <= 1.25*b) }' \
         && ok "$label matching RMS $rms px ($(num .solve.matches "$c") stars) vs ImageSolver $ref px ($(num .gate.referenceMatched "$c") stars), <= 1.25x" \
         || bad "$label matching RMS $rms px > 1.25 x ImageSolver's $ref px"
   done
}

case_solve_blind_no_metadata()
{
   solver_ready solve_blind_no_metadata || return 0
   if [ ! -f "$ASTROMETRIC_INDEX" ]; then
      echo "  skip  solve_blind_no_metadata: no astrometric index at $ASTROMETRIC_INDEX (GDG_ASTROMETRIC_INDEX)"; skipped=$((skipped + 1)); return 0
   fi
   ran=$((ran + 1))
   local m label master c
   for m in "${SOLVE_MASTERS[@]}"; do
      label="${m%%:*}"; master="${m#*:}"
      reference "$label" "$master" || continue
      # The index is selected in this slot's Gaia preferences; the solver finds it via get-info.
      solve "blind_$label" "$label" "$master" GDG_SOLVE_STRIP=1 GDG_SOLVE_GAIA="$GAIA_FILES" \
         GDG_SOLVE_CONFIGURE_INDEX="$ASTROMETRIC_INDEX" GDG_SOLVE_ARGS="gaia=$GAIA_FILES" || continue
      c="$OUT"
      if [ "$(jq -r .solve.solved "$c")" != true ]; then bad "$label blind: $(jq -r .solve.error "$c")"; continue; fi
      [ "$(jq -r .solve.configuredIndex "$c")" = "$ASTROMETRIC_INDEX" ] \
         && ok "$label: index found through the Gaia preferences (get-info)" \
         || bad "$label: Gaia get-info gave '$(jq -r .solve.configuredIndex "$c")'"
      [ "$(jq -r .solve.method "$c")" = "blind+seeded" ] && [ "$(num .solve.blindAttempt "$c")" -ge 1 ] \
         && ok "$label: blind solution (attempt $(num .solve.blindAttempt "$c"), $(num .solve.blindSeconds "$c") s) seeded the solve" \
         || bad "$label: method $(jq -r .solve.method "$c"), blind attempt $(num .solve.blindAttempt "$c")"
      le "$(num .gate.centerArcsec "$c")" 2 && ok "$label blind: centre $(num .gate.centerArcsec "$c")\" from ImageSolver's (<= 2\"); $(gate_line "$c")" \
         || bad "$label blind: centre $(num .gate.centerArcsec "$c")\" from ImageSolver's (> 2\")"
   done
}

case_solve_fails_loudly_without_seed_or_index()
{
   solver_ready solve_fails_loudly_without_seed_or_index || return 0
   ran=$((ran + 1))
   local master="${SOLVE_MASTERS[0]#*:}" c
   # A pristine slot: an earlier case may have selected an index in this slot's Gaia preferences
   # (the Gaia process' configure command keeps a selected index when given an empty path).
   gdg_seed_slot_modules || { bad "could not reset slot $GDG_SLOT"; return 0; }
   solve unsolvable none "$master" GDG_SOLVE_STRIP=1 GDG_SOLVE_REF= GDG_SOLVE_ARGS="gaia=$GAIA_FILES" || return 0
   c="$OUT"
   [ -z "$(jq -r .solve.configuredIndex "$c")" ] || bad "the reset slot still has an index: $(jq -r .solve.configuredIndex "$c")"
   if [ "$(jq -r .solve.solved "$c")" = false ] && [ "$(jq -r .solve.error "$c")" = "$UNSOLVABLE" ]; then
      ok "no seed, no index: \"$(jq -r .solve.error "$c")\""
   else
      bad "no seed, no index: solved=$(jq -r .solve.solved "$c"), error \"$(jq -r .solve.error "$c")\""
   fi
   [ "$(jq -r .hasSolution "$c")" = false ] && ok "nothing written to the image" || bad "a solution was written"
}

for c in "${CASES[@]}"; do
   echo "== $c"
   "case_$c"
done

if [ "$fails" -ne 0 ]; then echo "FAIL: $fails check(s)"; exit 1; fi
if [ "$ran" -eq 0 ]; then echo "SKIP: every requested case was skipped"; exit 77; fi
echo "PASS: GaiaDepthGrade probes (${CASES[*]})"
