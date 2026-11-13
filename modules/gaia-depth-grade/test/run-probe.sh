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
#   The process cases (Task 16) run the GaiaDepthGrade process as a process icon / script does
#   (new GaiaDepthGrade; executeOn) through probe-process.js on the HaO3 NAS master:
#   process_category         process id, category scarter4work, every parameter id, type, default
#                            and range (the plan's Task 16 table; F47, F12-F25 defaults).
#   params_reach_core        every engine parameter set to a distinct value reaches the engine
#                            configuration ExecuteOn runs with (F43).
#   execute_on_offline       Prepare + render + output, offline (F4, F44): an output window, QA in
#                            the read-only parameters, the match rate not LOW.
#   output_naming_and_tag    <id>_depthgraded, 32-bit float, the target's geometry, DEPTHTAG and
#                            HISTORY = the honesty text, the solution; no work window left (F8, F42).
#   master_never_modified    the target's pixels and keywords are unchanged (F3).
#   split_requires_sxt_message  StarXTerminator hidden (GDG_TEST_HIDE_SXT=1): the exact message,
#                            nothing made (Review Focus 5, F31).
#   supplied_views_mode      StarXTerminator hidden, its layers supplied as views: the same output
#                            as the StarXTerminator run; missing / wrong-size views fail loudly.
#   replace_target_mode      outputMode ReplaceTarget: the target graded in place (= the new-window
#                            output), tagged, saved to a file (the headless script's file in/out).
#   mono_execute             a one-channel target runs end to end, one-channel output (Review Focus 2).
#   offline_without_local_db_fails  no local database (GDG_TEST_NO_LOCAL_GAIA=1), offline: the
#                            discovery error (queryLocalGaiaDR3's), verbatim and unwrapped; no network.
#   online_fallback_live     no local database, catalog Online, the headless script's engine gains:
#                            plate-solved against the ESA archive (C21), Bailer-Jones distances;
#                            skipped unless GDG_LIVE_TAP=1.
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
            solve_fails_loudly_without_seed_or_index
            process_category params_reach_core execute_on_offline output_naming_and_tag master_never_modified
            split_requires_sxt_message supplied_views_mode replace_target_mode mono_execute
            offline_without_local_db_fails online_fallback_live )
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

# Large outputs (masters, graded images) go to a disk-backed run directory, not a RAM-backed /tmp.
export TMPDIR="${GDG_TMPDIR:-$ROOT/build/tmp}"
mkdir -p "$TMPDIR"
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

# ---- The GaiaDepthGrade process (Task 16) ----------------------------------------------------

PROC_MASTER="${GDG_PROC_MASTER:-$MASTER_DIR/masterLight_BIN-1_3840x2160_EXPOSURE-120.00s_FILTER-HaO3_RGB.xisf}"
HONESTY='GAIA depth-derived; physically motivated, not per-pixel correct for gas'
SXT_MISSING='StarXTerminator is required (a paid PixInsight module) but is not installed. Install it, then re-run Gaia Depth Grade.'
NO_LOCAL_DB="No local Gaia DR3 database is configured in PixInsight (the one plate-solving uses). Configure it, or uncheck 'Offline' to use the online catalogue."

# The Gaia files as a JSON array (the gaiaDatabaseFiles parameter).
gaia_json(){ tr ':' '\n' <<<"$GAIA_FILES" | sed '/^$/d' | jq -R . | jq -s -c .; }

proc_ready()
{
   local name="$1"
   if [ -z "$GAIA_FILES" ]; then echo "  skip  $name: no Gaia DR3 files (GDG_GAIA_FILES)"; skipped=$((skipped + 1)); return 1; fi
   if [ ! -f "$PROC_MASTER" ]; then echo "  skip  $name: $PROC_MASTER not found (NAS)"; skipped=$((skipped + 1)); return 1; fi
}

# run_proc <name> <request JSON> [VAR=value ...]: probe-process.js; the result is $OUT.
run_proc()
{
   local name="$1" req="$2"; shift 2
   printf '%s\n' "$req" > "$GDG_RUN_DIR/$name.req.json"
   run_pi "$name" "$HERE/probe-process.js" GDG_PROC_REQ="$GDG_RUN_DIR/$name.req.json" \
      GDG_TEST_LAST_ERROR="$GDG_RUN_DIR/$name.err" GDG_TEST_DUMP_SETTINGS="$GDG_RUN_DIR/$name.settings.json" "$@"
}

# The offline StarXTerminator run on the master, shared by the cases that read it (once per run).
offline_run()
{
   PROC="$GDG_RUN_DIR/offline-result.json"
   [ -s "$PROC" ] && return 0
   run_proc offline "{\"master\":\"$PROC_MASTER\",\"params\":{\"gaiaDatabaseFiles\":$(gaia_json)},\"saveOutput\":\"$GDG_RUN_DIR/offline-out.xisf\"}" \
      || return 1
   cp "$OUT" "$PROC"
   echo "  info  offline run: $(jq -r '.seconds' "$PROC") s; QA $(jq -c .qa "$PROC")"
}

case_process_category()
{
   ran=$((ran + 1))
   run_pi process_info "$HERE/probe-introspect.js" GDG_INTROSPECT="GaiaDepthGrade" || return 0
   local c="$OUT"
   [ "$(jq -c '.processes["GaiaDepthGrade.categories"]' "$c")" = '["scarter4work"]' ] \
      && ok "process GaiaDepthGrade in category scarter4work (menu scarter4work > Gaia Depth Grade)" \
      || bad "categories: $(jq -c '.processes["GaiaDepthGrade.categories"]' "$c")"
   # id type default [min max]: the plan's Task 16 table (dialog defaults, config.py's engine ones).
   local want got
   want="$(cat <<'EOF'
brightness real 0.65 0 2
size real 0.85 0 2
contrast real 0.12 0 2
saturation real 0.4 0 2
haloGain real 1 0 4
haloSigma real 2 0.5 6
pLow real 5 0 50
pHigh real 95 50 100
nebulaEnabled bool false
nebulaStrength real 1 0 2
nebulaAtmos real 1 0 2
nebulaStructure real 1 0 2
detectSigma real 6 1 30
detectFWHM real 3 0.5 20
matchTolerance real 4 1 12
minMatchRate real 0.3 0 1
neutralStrength real 0 -1 1
catalog enum 0
magLimitEnabled bool true
magLimit real 18 -1.5 25
parallaxSNR real 5 0 1000
haloMinRefs int 8 1 10000
haloRadiusSigma real 8 0.5 100
haloMoffatBeta real 2.5 0.1 20
splitMode enum 0
starsViewId string
starlessViewId string
outputMode enum 0
solveIndexFile string
gaiaDatabaseFiles table
outNDetected int 0 0 2147483647
outNMatched int 0 0 2147483647
outMatchRate real 0 0 1
outMedianOffset real 0 -1 1000
outLowMatch bool false
EOF
)"
   # Numbers as the shortest text (the probe prints %.17g: 0.65 arrives as 0.65000000000000002).
   got="$(jq -r 'def t: if . == null then empty elif type == "number" then (. * 1e12 | round / 1e12 | tostring) else tostring end;
                 .processes.GaiaDepthGrade[] | [.id, .type, (.default | t), (.min | t), (.max | t)] | join(" ")' "$c")"
   if [ "$got" = "$want" ]; then ok "35 parameters: ids, types, defaults and ranges as the plan's table"
   else bad "parameter table differs:"; diff <(echo "$want") <(echo "$got") | sed 's/^/        /' || true; fi
   jq -e '[.processes.GaiaDepthGrade[] | select(.id | test("^out(N|Match|Median|Low)")) | .readOnly] | length == 5 and all' "$c" >/dev/null \
      && ok "QA outputs are read-only" || bad "a QA output is writable"
   jq -e '.processes.GaiaDepthGrade[] | select(.id == "gaiaDatabaseFiles") | .columns[0].id == "gaiaDatabaseFilePath"' "$c" >/dev/null \
      && ok "gaiaDatabaseFiles: one string column gaiaDatabaseFilePath" || bad "gaiaDatabaseFiles columns: $(jq -c '.processes.GaiaDepthGrade[] | select(.id == "gaiaDatabaseFiles")' "$c")"
}

case_params_reach_core()
{
   proc_ready params_reach_core || return 0
   ran=$((ran + 1))
   # Distinct non-default values; StarXTerminator hidden so the run stops right after the dump.
   run_proc params '{"master":"'"$PROC_MASTER"'","params":{"brightness":1.11,"size":1.22,"contrast":0.33,"saturation":0.44,
      "haloGain":2.5,"haloSigma":3.3,"pLow":7.5,"pHigh":88.5,"nebulaEnabled":true,"nebulaStrength":1.7,"nebulaAtmos":0.6,
      "nebulaStructure":1.4,"detectSigma":9.5,"detectFWHM":2.25,"matchTolerance":2.75,"minMatchRate":0.45,
      "neutralStrength":0.15,"catalog":1,"magLimitEnabled":true,"magLimit":16.5,"parallaxSNR":7,"haloMinRefs":5,
      "haloRadiusSigma":6.5,"haloMoffatBeta":3.25,"solveIndexFile":"/x/idx.xpsd","gaiaDatabaseFiles":["/a.xpsd","/b.xpsd"]}}' \
      GDG_TEST_HIDE_SXT=1 || return 0
   local c="$OUT"
   if jq -e '.settings | .brightness == 1.11 and .size == 1.22 and .contrast == 0.33 and .saturation == 0.44
         and .halo_gain == 2.5 and .base_sigma_px == 3.3 and .p_low == 7.5 and .p_high == 88.5 and .nebula_enabled
         and .nebula_strength == 1.7 and .nebula_atmos == 0.6 and .nebula_structure == 1.4
         and .detect_threshold_sigma == 9.5 and .detect_fwhm == 2.25 and .match_tolerance_px == 2.75
         and .min_match_rate == 0.45 and .neutral_strength == 0.15 and .online and .gaia_mag_limit == 16.5
         and .gaia_parallax_snr == 7 and .halo_min_refs == 5 and .halo_radius_sigma == 6.5 and .halo_moffat_beta == 3.25
         and .solveIndexFile == "/x/idx.xpsd" and .gaiaDatabaseFiles == ["/a.xpsd","/b.xpsd"]' "$c" >/dev/null; then
      ok "every engine parameter reaches the configuration ExecuteOn runs with"
   else bad "settings dump: $(jq -c .settings "$c")"; fi
   # The cut switched off = Python gaia_mag_limit None.
   run_proc params_nocut '{"master":"'"$PROC_MASTER"'","params":{"magLimitEnabled":false}}' GDG_TEST_HIDE_SXT=1 || return 0
   [ "$(jq -r .settings.gaia_mag_limit "$OUT")" = null ] && ok "magLimitEnabled false: no magnitude cut (None)" \
      || bad "magLimitEnabled false gave gaia_mag_limit $(jq -r .settings.gaia_mag_limit "$OUT")"
}

case_execute_on_offline()
{
   proc_ready execute_on_offline || return 0
   ran=$((ran + 1))
   offline_run || return 0
   local c="$PROC"
   [ "$(jq -r .executed "$c")" = true ] && ok "executeOn: true in $(jq -r .seconds "$c") s (error '$(jq -r .error "$c")')" \
      || { bad "executeOn failed: $(jq -r .error "$c")"; return 0; }
   jq -e '.newWindows | length == 1' "$c" >/dev/null && ok "one output window" || bad "windows made: $(jq -c '[.newWindows[].id]' "$c")"
   local nd nm rate
   nd="$(num .qa.nDetected "$c")"; nm="$(num .qa.nMatched "$c")"; rate="$(num .qa.matchRate "$c")"
   if [ "$nd" -gt 1000 ] && le 0.3 "$rate" && [ "$(jq -r .qa.lowMatch "$c")" = false ]; then
      ok "QA in the read-only parameters: $nm/$nd matched (rate $rate, offset $(num .qa.medianOffset "$c") px), not LOW"
   else bad "QA: $(jq -c .qa "$c")"; fi
}

case_output_naming_and_tag()
{
   proc_ready output_naming_and_tag || return 0
   ran=$((ran + 1))
   offline_run || return 0
   local c="$PROC" id
   id="$(jq -r .target.id "$c")"
   jq -e --arg id "${id}_depthgraded" '.newWindows[0].id == $id' "$c" >/dev/null && ok "output window ${id}_depthgraded" \
      || bad "output id $(jq -r '.newWindows[0].id' "$c"), expected ${id}_depthgraded"
   jq -e '.newWindows[0] as $o | .target as $t | $o.bitsPerSample == 32 and $o.isReal and $o.width == $t.width
          and $o.height == $t.height and $o.channels == $t.channels and $o.min >= 0 and $o.max <= 1' "$c" >/dev/null \
      && ok "32-bit float, the target's geometry, within [0, 1]" || bad "output format: $(jq -c '.newWindows[0] | del(.keywords)' "$c")"
   jq -e --arg h "$HONESTY" '.newWindows[0].keywords | .depthtag == $h and (.history | index($h) != null)' "$c" >/dev/null \
      && ok "DEPTHTAG and HISTORY: \"$HONESTY\"" || bad "tags: $(jq -c '.newWindows[0].keywords' "$c")"
   jq -e '.newWindows[0].keywords.count == .targetKeywordsBefore + 2' "$c" >/dev/null \
      && ok "the target's keywords carried, plus the two tags" || bad "keywords: $(jq -r '.newWindows[0].keywords.count' "$c") vs $(jq -r .targetKeywordsBefore "$c") + 2"
   [ "$(jq -r '.newWindows[0].hasSolution' "$c")" = true ] && ok "the plate solution is on the output" \
      || bad "no astrometric solution on the output"
   jq -e '[.windowsAfter[] | select(test("_gdgwork|stars"))] | length == 0' "$c" >/dev/null \
      && ok "no working or stars window left open" || bad "left open: $(jq -c .windowsAfter "$c")"
}

case_master_never_modified()
{
   proc_ready master_never_modified || return 0
   ran=$((ran + 1))
   offline_run || return 0
   local c="$PROC"
   if [ "$(num .targetMaxDiff "$c")" = 0 ] && [ "$(jq -r .targetModified "$c")" = false ] \
      && jq -e '.targetAfter.keywords.count == .targetKeywordsBefore and .targetAfter.keywords.depthtag == null' "$c" >/dev/null; then
      ok "the target: every sample identical (max |d| 0), keywords unchanged, not marked modified"
   else bad "target changed: max |d| $(num .targetMaxDiff "$c"), modified $(jq -r .targetModified "$c"), keywords $(jq -c .targetAfter.keywords "$c")"; fi
}

case_split_requires_sxt_message()
{
   proc_ready split_requires_sxt_message || return 0
   ran=$((ran + 1))
   run_proc nosxt "{\"master\":\"$PROC_MASTER\",\"params\":{\"gaiaDatabaseFiles\":$(gaia_json)}}" GDG_TEST_HIDE_SXT=1 || return 0
   local c="$OUT"
   if [ "$(jq -r .executed "$c")" = false ] && [ "$(jq -r .error "$c")" = "$SXT_MISSING" ]; then
      ok "executeOn false: \"$SXT_MISSING\""
   else bad "executed=$(jq -r .executed "$c"), error \"$(jq -r .error "$c")\""; fi
   jq -e '(.newWindows | length) == 0 and .targetMaxDiff == 0' "$c" >/dev/null && ok "nothing made, the target untouched" \
      || bad "windows $(jq -c '[.newWindows[].id]' "$c"), target max |d| $(num .targetMaxDiff "$c")"
}

case_supplied_views_mode()
{
   proc_ready supplied_views_mode || return 0
   ran=$((ran + 1))
   offline_run || return 0
   # StarXTerminator's layers made by the probe, supplied as views; the module may not run it.
   run_proc supplied "{\"master\":\"$PROC_MASTER\",\"split\":true,\"repeat\":true,\"sxtNoise\":true,\"params\":{\"gaiaDatabaseFiles\":$(gaia_json)},\"compareTo\":\"$GDG_RUN_DIR/offline-out.xisf\"}" \
      GDG_TEST_HIDE_SXT=1 || return 0
   local c="$OUT"
   [ "$(jq -r .executed "$c")" = true ] && ok "supplied views, StarXTerminator hidden: executeOn true ($(jq -c .supplied "$c"))" \
      || { bad "supplied views failed: $(jq -r .error "$c")"; return 0; }
   # The same layers twice: the same image and QA (the module adds no randomness).
   [ "$(jq -r .repeatExecuted "$c")" = true ] && [ "$(num .repeatMaxDiff "$c")" = 0 ] && jq -e '.qa == .repeatQA' "$c" >/dev/null \
      && ok "run again on the same layers: identical output (max |d| 0) and QA" \
      || bad "repeat: executed $(jq -r .repeatExecuted "$c"), max |d| $(num .repeatMaxDiff "$c"), QA $(jq -c .qa "$c") vs $(jq -c .repeatQA "$c")"
   # Against the module's own StarXTerminator run: equal up to StarXTerminator's run-to-run noise.
   local noise d
   noise="$(num .sxtNoise "$c")"; d="$(num .compareMaxDiff "$c")"
   if le "$d" "$(awk -v n="$noise" 'BEGIN { print 2*n }')" && gt "$noise" 0; then
      ok "= the StarXTerminator-mode output within twice StarXTerminator's own run-to-run noise (max |d| $d; its noise $noise)"
   else bad "supplied vs StarXTerminator mode: max |d| $d, StarXTerminator's own noise $noise"; fi
   jq -e '.qa.nMatched > 0 and (.qa.nDetected - $o[0].qa.nDetected | fabs) <= 5' --slurpfile o "$PROC" "$c" >/dev/null \
      && ok "QA $(jq -c .qa "$c") ~ StarXTerminator mode's $(jq -c .qa "$PROC")" \
      || bad "QA $(jq -c .qa "$c") vs StarXTerminator mode $(jq -c .qa "$PROC")"
   # Loud failures: a missing view, a view of another size.
   run_proc supplied_missing "{\"master\":\"$PROC_MASTER\",\"params\":{\"splitMode\":1,\"starsViewId\":\"nosuch\",\"starlessViewId\":\"nosuch2\"}}" \
      GDG_TEST_HIDE_SXT=1 || return 0
   [ "$(jq -r .error "$OUT")" = "The stars view 'nosuch' does not exist." ] && ok "missing view: \"$(jq -r .error "$OUT")\"" \
      || bad "missing view: \"$(jq -r .error "$OUT")\""
   run_proc supplied_empty "{\"master\":\"$PROC_MASTER\",\"params\":{\"splitMode\":1}}" GDG_TEST_HIDE_SXT=1 || return 0
   [ "$(jq -r .error "$OUT")" = "Split mode is SuppliedViews but no stars view is set (starsViewId)." ] \
      && ok "no view named: \"$(jq -r .error "$OUT")\"" || bad "no view named: \"$(jq -r .error "$OUT")\""
   # A one-channel layer for a colour target.
   run_proc supplied_size "{\"master\":\"$PROC_MASTER\",\"extraMono\":\"gdg_mono_layer\",\"params\":{\"splitMode\":1,\"starsViewId\":\"gdg_mono_layer\",\"starlessViewId\":\"gdg_mono_layer\"}}" \
      GDG_TEST_HIDE_SXT=1 || return 0
   jq -e '.error | test("^The stars view .gdg_mono_layer. is [0-9]+ x [0-9]+ x 1; the target is [0-9]+ x [0-9]+ x 3\\.")' "$OUT" >/dev/null \
      && ok "wrong geometry: \"$(jq -r .error "$OUT")\"" || bad "wrong geometry: \"$(jq -r .error "$OUT")\""
}

case_replace_target_mode()
{
   proc_ready replace_target_mode || return 0
   ran=$((ran + 1))
   # On StarXTerminator layers supplied as views (StarXTerminator itself is not repeatable: its two
   # runs on one image differ by ~1e-4), first a new-window run, then ReplaceTarget on the same target.
   run_proc replace "{\"master\":\"$PROC_MASTER\",\"split\":true,\"thenReplace\":true,\"params\":{\"gaiaDatabaseFiles\":$(gaia_json)},\"saveTo\":\"$GDG_RUN_DIR/replaced.xisf\"}" \
      GDG_TEST_HIDE_SXT=1 || return 0
   local c="$OUT"
   [ "$(jq -r .executed "$c")" = true ] || { bad "the new-window run failed: $(jq -r .error "$c")"; return 0; }
   [ "$(jq -r .replaceExecuted "$c")" = true ] && ok "outputMode ReplaceTarget: executeOn true" \
      || { bad "ReplaceTarget failed: $(jq -r .replaceError "$c")"; return 0; }
   [ "$(num .replaceNewWindows "$c")" = 0 ] && ok "no new window" || bad "$(num .replaceNewWindows "$c") window(s) made"
   [ "$(num .replaceMaxDiff "$c")" = 0 ] && ok "the target holds exactly the new-window output (max |d| 0)" \
      || bad "the replaced target differs from the new-window output: max |d| $(num .replaceMaxDiff "$c")"
   jq -e --arg h "$HONESTY" '.replaceTargetAfter.keywords.depthtag == $h and (.replaceTargetAfter.keywords.history | index($h) != null)' "$c" >/dev/null \
      && ok "DEPTHTAG and HISTORY on the target" || bad "target tags: $(jq -c .replaceTargetAfter.keywords "$c")"
   [ "$(jq -r .savedTo "$c")" = true ] && [ "$(num .savedMaxDiff "$c")" = 0 ] \
      && jq -e --arg h "$HONESTY" '.savedKeywords.depthtag == $h' "$c" >/dev/null \
      && ok "saved to a file and read back identical, tagged (the headless script's GAIA_DEPTH_OUTPUT)" \
      || bad "file out: saved $(jq -r .savedTo "$c"), max |d| $(num .savedMaxDiff "$c"), keywords $(jq -c .savedKeywords "$c")"
}

case_mono_execute()
{
   proc_ready mono_execute || return 0
   ran=$((ran + 1))
   run_proc mono "{\"master\":\"$PROC_MASTER\",\"mono\":true,\"params\":{\"nebulaEnabled\":true,\"gaiaDatabaseFiles\":$(gaia_json)}}" || return 0
   local c="$OUT"
   [ "$(jq -r .executed "$c")" = true ] || { bad "mono run failed: $(jq -r .error "$c")"; return 0; }
   jq -e '.newWindows | length == 1 and .[0].channels == 1' "$c" >/dev/null \
      && ok "one-channel target, nebula on: one-channel output ($(jq -c .qa "$c"))" || bad "mono output: $(jq -c '.newWindows' "$c")"
}

case_offline_without_local_db_fails()
{
   proc_ready offline_without_local_db_fails || return 0
   ran=$((ran + 1))
   run_proc nolocal "{\"master\":\"$PROC_MASTER\",\"params\":{}}" GDG_TEST_NO_LOCAL_GAIA=1 || return 0
   local c="$OUT"
   # queryLocalGaiaDR3's text verbatim, not wrapped in the solver's "plate solve failed for ..." prefix.
   if [ "$(jq -r .executed "$c")" = false ] && [ "$(jq -r .error "$c")" = "$NO_LOCAL_DB" ]; then
      ok "offline, no local database: \"$(jq -r .error "$c")\""
   else bad "executed=$(jq -r .executed "$c"), error \"$(jq -r .error "$c")\""; fi
}

case_online_fallback_live()
{
   if [ "${GDG_LIVE_TAP:-}" != 1 ]; then
      echo "  skip  online_fallback_live: live ESA archive queries; run with GDG_LIVE_TAP=1"; skipped=$((skipped + 1)); return 0
   fi
   proc_ready online_fallback_live || return 0
   ran=$((ran + 1))
   # The headless script's configuration: engine gains, online. G < 15 keeps the Bailer-Jones join of
   # this dense field inside the archive's 60 s execution limit (G < 18 is ABORTED there, 2026-11-11).
   run_proc online "{\"master\":\"$PROC_MASTER\",\"params\":{\"brightness\":0.5,\"size\":0.4,\"contrast\":0.3,\"saturation\":0.3,\"catalog\":1,\"magLimit\":15}}" \
      GDG_TEST_NO_LOCAL_GAIA=1 XDG_CACHE_HOME="$GDG_RUN_DIR/xdg-cache" || return 0
   local c="$OUT"
   [ "$(jq -r .executed "$c")" = true ] && ok "no local database, online: graded in $(jq -r .seconds "$c") s, QA $(jq -c .qa "$c")" \
      || { bad "online run failed: $(jq -r .error "$c")"; return 0; }
   ls "$GDG_RUN_DIR"/xdg-cache/GaiaDepthGrade/gaia/gaia_pos*.csv >/dev/null 2>&1 \
      && ok "plate solved against the archive (positions cached: $(basename "$(ls "$GDG_RUN_DIR"/xdg-cache/GaiaDepthGrade/gaia/gaia_pos*.csv | head -1)"))" \
      || bad "no cached archive positions: the solve did not use the online catalogue"
   # G < 15 matches only the bright stars (the rate is low by design here); the match itself must be
   # as tight as offline's (the archive-solved WCS): many matches at a sub-pixel median offset.
   if [ "$(num .qa.nMatched "$c")" -ge 500 ] && le "$(num .qa.medianOffset "$c")" 0.3; then
      ok "Bailer-Jones matches: $(num .qa.nMatched "$c") at median offset $(num .qa.medianOffset "$c") px"
   else bad "online matches: $(jq -c .qa "$c")"; fi
}

for c in "${CASES[@]}"; do
   echo "== $c"
   "case_$c"
done

if [ "$fails" -ne 0 ]; then echo "FAIL: $fails check(s)"; exit 1; fi
if [ "$ran" -eq 0 ]; then echo "SKIP: every requested case was skipped"; exit 77; fi
echo "PASS: GaiaDepthGrade probes (${CASES[*]})"
