#!/usr/bin/env bash
# AutoContrast probe cases: signs the container-built PROBE module, loads it with -m= in the isolated
# settings slot 93 on a private Xvfb display (tools/pi-headless.sh), and reaches the test-only
# AutoContrastProbe process through a one-line command script (<case>.scp) run with -r= under
# --automation-mode. No PJSR anywhere. Test-only.
#
#   bash modules/autocontrast/test/run-probe.sh [case...]    default: every case, in order
#     -> PASS (exit 0) / FAIL (exit 1) / BLOCKED (exit 2: no signing pass file)
#
# Cases:
#   scp_launch_reaches_probe  a .scp line reaches AutoContrastProbe::ProcessCommandLine with its arguments
#   ids                       module id, the three process ids in registration order, category
#
# AC_SO overrides the module tested (default: the container probe build,
# modules/autocontrast/build-portable-probes). Probe results are kept at
# <module>/build-portable-probes/probe-<case>.json (AC_PROBE_DEST_DIR overrides the directory).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/harness.sh"

ac_require_signing_pass || exit 2
AC_SO="${AC_SO:-$ROOT/build-portable-probes/src/module/AutoContrast-pxm.so}"
[ -f "$AC_SO" ] || { echo "FAIL: probe module not built at $AC_SO (tools/build-env/build-modules.sh --only autocontrast)"; exit 1; }
AC_SO="$(realpath -e "$AC_SO")"
ac_check_so "$AC_SO" || exit 1

CASES=("$@")
[ "${#CASES[@]}" -gt 0 ] || CASES=(scp_launch_reaches_probe ids)
for c in "${CASES[@]}"; do
   case "$c" in scp_launch_reaches_probe|ids) ;; *) echo "FAIL: unknown case '$c'"; exit 1 ;; esac
done

ac_slot_setup || exit 1
ac_private_tmp || exit 1
ac_sign "$AC_SO" || exit 1
ac_seed_slot_modules || exit 1

DEST_DIR="${AC_PROBE_DEST_DIR:-$ROOT/build-portable-probes}"
fails=0
ok(){ echo "  ok    $*"; }
bad(){ echo "  FAIL  $*"; fails=$((fails + 1)); }

for c in "${CASES[@]}"; do
   echo "== $c"
   OUT="$AC_RUN_DIR/$c.json"
   rc=0
   ac_run_scp "$c" "AutoContrastProbe --case=$c --out=$OUT" || rc=$?
   if [ "$rc" -ne 0 ]; then bad "PixInsight exited $rc (124 = timeout); log tail:"; tail -20 "$AC_RUN_DIR/$c.log" | sed 's/^/        /'; continue; fi
   if [ ! -s "$OUT" ]; then bad "the .scp never reached the probe (no $c.json); log tail:"; tail -20 "$AC_RUN_DIR/$c.log" | sed 's/^/        /'; continue; fi
   ok "force-exit returned 0 and the probe wrote $c.json"
   [ "$(jq -r .case "$OUT")" = "$c" ] && ok "case echoed" || bad "case is '$(jq -r .case "$OUT")'"
   case "$c" in
      scp_launch_reaches_probe)
         [ "$(jq -r .reached "$OUT")" = true ] && ok "ProcessCommandLine ran" || bad "reached != true"
         jq -e --arg o "--out=$OUT" '.argv | index($o) != null' "$OUT" >/dev/null && ok "argv carries the --out argument" || bad "argv lacks --out: $(jq -c .argv "$OUT")"
         ;;
      ids)
         [ "$(jq -r .module "$OUT")" = AutoContrast ] && ok "module id AutoContrast" || bad "module is '$(jq -r .module "$OUT")'"
         [ "$(jq -c .processes "$OUT")" = '["AutoContrast","AutoContrastReferences","AutoContrastProbe"]' ] \
            && ok "processes $(jq -c .processes "$OUT")" || bad "processes are $(jq -c .processes "$OUT")"
         [ "$(jq -r .category "$OUT")" = scarter4work ] && ok "category scarter4work" || bad "category is '$(jq -r .category "$OUT")'"
         [ "$(jq -c '[.categories[] | length == 1 and .[0] == "scarter4work"] | all' "$OUT")" = true ] \
            && ok "every process is in category scarter4work only" || bad "categories: $(jq -c .categories "$OUT")"
         [ "$(jq -r .version "$OUT")" = 0.1.0.0 ] && ok "version 0.1.0.0" || bad "version is '$(jq -r .version "$OUT")'"
         ;;
   esac
   mkdir -p "$DEST_DIR"
   cp "$OUT" "$DEST_DIR/probe-$c.json"
   echo "  probe result: $DEST_DIR/probe-$c.json"
done
[ "$fails" = 0 ] && echo "PASS: AutoContrast probe (${CASES[*]})" || { echo "FAIL: $fails check(s)"; exit 1; }
