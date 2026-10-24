#!/usr/bin/env bash
# Does release.sh refuse to SHIP a PICopilot package with self-test hooks -- including an already-published
# tarball it would reuse byte for byte? (Re-review 2, Important: the published 0.2.2.0 carries the old
# PICOPILOT_SELFTEST_* URL hooks, and a release of the unchanged version re-ships that tarball.)
# No build, no signing, no manifest; repository/ is only read.
#
#   release-ship-gate-test.sh [<release PICopilot-pxm.so>]   (default: build-portable/, the shipped build)
#
# PASS needs all of:
#  1. the gate on the published tarball of the CURRENT version (if published) gives the verdict release.sh
#     must act on (with the published 0.2.2.0: markers found);
#  2. release.sh runs the tarball gate on "$REPO/$PICOPILOT_TGZ" at step 0 (before the build: a published
#     tarball with hooks stops the release there) and again right after package_module PICopilot (what is
#     shipped, built or reused);
#  3. release.sh --package-test reports that verdict ("release.sh would STOP ... self-test hooks") for a
#     hooked published version;
#  4. a tarball built from the clean release .so passes the gate.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SO="$(realpath -e "${1:-$ROOT/modules/pi-copilot/build-portable/src/module/PICopilot-pxm.so}")"
REL="$ROOT/release.sh"
GATE='modules/pi-copilot/test/check-tarball-no-selftest-hooks.sh" "$REPO/$PICOPILOT_TGZ"'
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
ok=1

PV="$(sed -nE 's/^#define PICOPILOT_MODULE_VERSION_(MAJOR|MINOR|REVISION|BUILD) +([0-9]+).*/\2/p' \
      "$ROOT/modules/pi-copilot/src/module/PICopilotVersion.h" | paste -sd. -)"
PUB="$(git -C "$ROOT" ls-files "repository/*-linux-x64-PICopilot-$PV.tar.gz")"
HOOKED=0
if [ -n "$PUB" ]; then
   if "$HERE/check-tarball-no-selftest-hooks.sh" "$ROOT/$PUB" > "$TMP/pub.txt" 2>&1; then
      echo "1. published $PUB: clean"
   else
      HOOKED=1
      echo "1. published $PUB: carries self-test hooks ($(grep -c '^self-test marker' "$TMP/pub.txt") markers) -- a release must stop"
   fi
else
   echo "1. PICopilot $PV is not published yet (a release builds a new tarball)"
fi

# 2. wiring: the gate on the shipped tarball, before the build and after packaging.
pkg_line="$(grep -n '^package_module "$PICOPILOT_TGZ" PICopilot' "$REL" | head -1 | cut -d: -f1)"
build_line="$(grep -n 'tools/build-env/build-modules.sh" || die' "$REL" | head -1 | cut -d: -f1)"
mapfile -t gate_lines < <(grep -nF -- "$GATE" "$REL" | cut -d: -f1)
before=0 after=0
for g in "${gate_lines[@]}"; do
   [ -n "$build_line" ] && [ "$g" -lt "$build_line" ] && before=1
   [ -n "$pkg_line" ] && [ "$g" -gt "$pkg_line" ] && [ "$g" -le $(( pkg_line + 3 )) ] && after=1
done
if [ "$before" = 1 ] && [ "$after" = 1 ]; then
   echo "2. release.sh gates \$REPO/\$PICOPILOT_TGZ at step 0 and right after package_module (lines ${gate_lines[*]})"
else
   echo "FAIL 2: release.sh does not gate the PICopilot tarball it ships (step 0: $before, after package_module: $after)"
   ok=0
fi

# 3 + 4. --package-test: its verdict for the published version, and the gate on the freshly built tarball.
for n in NukeX PICopilot Planetary; do echo standin > "$TMP/$n-pxm.xsgn"; done
(cd "$ROOT" && ./release.sh --package-test "$TMP/out" \
   modules/nukex/build-portable/src/module/NukeX-pxm.so "$TMP/NukeX-pxm.xsgn" \
   "$SO" "$TMP/PICopilot-pxm.xsgn" \
   modules/planetary/build-portable/src/module/Planetary-pxm.so "$TMP/Planetary-pxm.xsgn") > "$TMP/pt.log" 2>&1 \
   || { echo "FAIL: release.sh --package-test failed:"; tail -5 "$TMP/pt.log"; exit 1; }
if [ "$HOOKED" = 1 ]; then
   if grep -q "release.sh would STOP: PICopilot $PV .*self-test hooks" "$TMP/pt.log"; then
      echo "3. --package-test: $(grep "release.sh would STOP: PICopilot $PV .*self-test hooks" "$TMP/pt.log" | sed 's/^ *//')"
   else
      echo "FAIL 3: --package-test does not say a release would stop on the hooked published PICopilot $PV"
      ok=0
   fi
else
   echo "3. (no hooked published tarball for $PV: nothing to stop on)"
fi
NEW="$(ls "$TMP"/out/*-linux-x64-PICopilot-*.tar.gz)"
if "$HERE/check-tarball-no-selftest-hooks.sh" "$NEW" > "$TMP/new.txt" 2>&1; then
   echo "4. a tarball of the release build passes: $(tail -1 "$TMP/new.txt")"
else
   echo "FAIL 4: a tarball of $SO fails the gate:"; cat "$TMP/new.txt"; ok=0
fi
[ "$ok" = 1 ] && echo "PASS: release.sh refuses to ship a PICopilot package with self-test hooks"
[ "$ok" = 1 ]
