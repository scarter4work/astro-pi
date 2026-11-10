#!/usr/bin/env bash
# EZTools licence consistency (run by ctest as check_licence). The licence is stated ONCE, in
# src/EZToolsLicense.h (EZTOOLS_LICENSE); every other place that names it must agree:
#   - doc/EZHazeKill/EZHazeKill.html and package-description.xml: <span id="licence">NAME</span>
#   - the repository LICENSE: a carve-out stanza for modules/ez-tools/ naming NAME
#     (none when NAME is the repository's own BSD 3-Clause licence)
#   - README.md: the modules/ez-tools/ exception naming NAME (none for BSD 3-Clause)
# To relicense: change EZTOOLS_LICENSE (+ SPDX id and URL) in the header, run this, and fix each
# place it names.
#
#   bash modules/ez-tools/test/check-licence.sh   -> PASS (exit 0) / FAIL (exit 1)
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$ROOT/../.." && pwd)"

name="$(sed -nE 's/^#define EZTOOLS_LICENSE +"([^"]+)".*/\1/p' "$ROOT/src/EZToolsLicense.h")"
[ -n "$name" ] || { echo "FAIL: no EZTOOLS_LICENSE in src/EZToolsLicense.h"; exit 1; }
fails=0
bad(){ echo "  FAIL  $*"; fails=$((fails + 1)); }
ok(){ echo "  ok    $*"; }

for f in doc/EZHazeKill/EZHazeKill.html package-description.xml; do
   got="$(sed -nE 's/.*<span id="licence">([^<]*)<\/span>.*/\1/p' "$ROOT/$f" | sort -u)"
   if [ "$got" = "$name" ]; then ok "$f names $name"; else bad "$f names '${got:-nothing}' (<span id=\"licence\">), header says $name"; fi
done

carve="$(sed -n '/^Exceptions:/,$p' "$REPO/LICENSE")"
case "$name" in
   BSD-3-Clause|"BSD 3-Clause"*)
      if grep -qF 'modules/ez-tools/' <<<"$carve"; then bad "LICENSE still carves modules/ez-tools/ out of BSD 3-Clause"; else ok "LICENSE: no carve-out (BSD 3-Clause)"; fi
      if grep -qF 'modules/ez-tools/' "$REPO/README.md"; then bad "README.md still lists modules/ez-tools/ as an exception"; else ok "README.md: no exception"; fi ;;
   *)
      if grep -A3 -F 'modules/ez-tools/' <<<"$carve" | grep -qF "licensed under $name"; then ok "LICENSE carves modules/ez-tools/ out under $name"
      else bad "LICENSE has no carve-out stanza for modules/ez-tools/ 'licensed under $name'"; fi
      if grep -F 'modules/ez-tools/' "$REPO/README.md" | grep -qF "$name"; then ok "README.md lists modules/ez-tools/ under $name"
      else bad "README.md does not list modules/ez-tools/ as an exception under $name"; fi ;;
esac

[ "$fails" = 0 ] && echo "PASS: EZTools licence is $name everywhere" || { echo "FAIL: $fails place(s) disagree with src/EZToolsLicense.h"; exit 1; }
