#!/usr/bin/env bash
# GaiaDepthGrade id probe (plan Task 2): signs the container-built module, loads
# it with -m= in the isolated settings slot 95 on a private Xvfb display
# (tools/pi-headless.sh), and runs probe-ids.js. Test-only.
#
#   bash modules/gaia-depth-grade/test/run-probe-ids.sh   -> PASS (exit 0) / FAIL (exit 1)
#                                                           BLOCKED (exit 2): no signing pass file
#
# Checks (plan Task 2 Step 1, test_module_loads):
#   - the core reports the module as loaded, under the module id GaiaDepthGrade;
#   - no process with id ImageSolver or BlindSolver exists;
#   - probe-ids.json records StarXTerminator / Gaia / StarAlignment ids.
# GDG_SO overrides the module tested (default: the container build). The probe
# result is kept at build-portable/probe-ids.json (GDG_PROBE_DEST overrides).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/harness.sh"

gdg_require_signing_pass || exit 2
SO="${GDG_SO:-$ROOT/build-portable/src/module/GaiaDepthGrade-pxm.so}"
[ -f "$SO" ] || { echo "FAIL: module not built at $SO (tools/build-env/build-modules.sh)"; exit 1; }
SO="$(realpath -e "$SO")"
gdg_check_so "$SO" || exit 1

gdg_slot_setup || exit 1
gdg_private_tmp || exit 1
gdg_sign "$SO" || exit 1
gdg_seed_slot_modules || exit 1

OUT="$GDG_RUN_DIR/probe-ids.json"
LOG="$GDG_RUN_DIR/probe.log"
# --force-exit only exits after the -r= script ran; timeout fails a hang loudly.
if ! GDG_PROBE_OUT="$OUT" pi_headless \
        timeout 300 "$PI" -n="$GDG_SLOT" --automation-mode --no-startup-scripts \
        -m="$SO" -r="$HERE/probe-ids.js" --force-exit >"$LOG" 2>&1; then
   echo "FAIL: PixInsight timed out (300 s) or exited non-zero"; tail -20 "$LOG"; exit 1
fi
[ -s "$OUT" ] || { echo "FAIL: PixInsight never wrote the probe result"; tail -20 "$LOG"; exit 1; }

fails=0
ok(){ echo "  ok    $*"; }
bad(){ echo "  FAIL  $*"; fails=$((fails + 1)); }

# test_module_loads, part 1: the module's identity as the core's module query
# reads it (module-ident.c: InstallPixInsightModule(QueryModuleInfo) +
# IdentifyPixInsightModule), so the id is the one PixInsight records.
cc -O2 -Wall -o "$GDG_RUN_DIR/module-ident" "$HERE/module-ident.c" -ldl \
   || { echo "FAIL: could not build module-ident"; exit 1; }
"$GDG_RUN_DIR/module-ident" "$SO" > "$GDG_RUN_DIR/ident.json" || { echo "FAIL: module-ident could not identify $SO"; exit 1; }
jq -s '.[0] + {ident: .[1]} + {module: .[1].module}' "$OUT" "$GDG_RUN_DIR/ident.json" > "$GDG_RUN_DIR/merged.json"
mv "$GDG_RUN_DIR/merged.json" "$OUT"

[ "$(jq -r .ok "$OUT")" = true ] && ok "probe script ran (core $(jq -r .core "$OUT"))" || bad "probe script error: $(jq -r .error "$OUT")"
[ "$(jq -r .module "$OUT")" = GaiaDepthGrade ] && ok "module id GaiaDepthGrade, version $(jq -r .ident.version "$OUT"), API $(jq -r .ident.apiVersion "$OUT")" \
   || bad "module id is '$(jq -r .module "$OUT")', expected GaiaDepthGrade"
# test_module_loads, part 2: the running core installed it (a rejected -m= module
# is unloaded before the -r= script runs), and it is exactly this build.
if [ "$(jq -r '.mappedModules | length' "$OUT")" = 1 ] && [ "$(realpath -m "$(jq -r '.mappedModules[0]' "$OUT")")" = "$SO" ]; then
   ok "loaded by PixInsight $(jq -r .core "$OUT"): $SO is mapped while the probe runs"
else bad "PixInsight did not keep $SO loaded (mapped: $(jq -c .mappedModules "$OUT"))"; tail -15 "$LOG" | sed 's/^/        /'; fi
[ "$(jq -r .imageSolverProcess "$OUT")" = false ] && ok "no ImageSolver process" || bad "a process with id ImageSolver exists"
[ "$(jq -r .blindSolverProcess "$OUT")" = false ] && ok "no BlindSolver process" || bad "a process with id BlindSolver exists"
for k in gaia starAlignment; do
   [ "$(jq -r ".$k" "$OUT")" = true ] && ok "$k process present ($(jq -r ".${k}Parameters | length" "$OUT") parameters)" || bad "$k process missing"
done
echo "  info  StarXTerminator installed: $(jq -r .starXTerminator "$OUT")"

DEST="${GDG_PROBE_DEST:-$ROOT/build-portable/probe-ids.json}"
cp "$OUT" "$DEST"
echo "  probe result: $DEST"
[ "$fails" = 0 ] && echo "PASS: GaiaDepthGrade id probe" || { echo "FAIL: $fails check(s)"; exit 1; }
