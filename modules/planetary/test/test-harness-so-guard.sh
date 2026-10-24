#!/usr/bin/env bash
# The PixInsight harness never re-signs a shipped module (final review, module m6): planetary_check_so
# and planetary_sign refuse a PLANETARY_SO inside the PixInsight install (found through a symlinked launcher
# too, and /opt/PixInsight always: bench6) or inside repository/, and every
# script that takes PLANETARY_SO (run-probe.sh, run-probe-stack.sh, gui-smoke.sh, gui-smoke-stack.sh)
# stops on it before anything runs. Against a stand-in install in a scratch directory (PLANETARY_PI points
# there; its PixInsight.sh only records that it was called): the real install is never touched.
#
#   bash modules/planetary/test/test-harness-so-guard.sh   -> PASS (exit 0) / FAIL (exit 1)
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ASTROPI="$(cd "$HERE/../../.." && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
mkdir -p "$T/pi/bin" "$T/build" "$T/elsewhere"
printf '#!/bin/sh\necho called >> "%s/pi-called"\nexit 1\n' "$T" > "$T/pi/bin/PixInsight.sh"
chmod +x "$T/pi/bin/PixInsight.sh"
printf 'installed module\n' > "$T/pi/bin/Planetary-pxm.so"
printf 'installed signature\n' > "$T/pi/bin/Planetary-pxm.xsgn"
printf 'build product\n' > "$T/build/Planetary-pxm.so"
ln -s "$T/pi/bin/Planetary-pxm.so" "$T/elsewhere/Planetary-pxm.so"   # a link from outside into the install
XSGN_BEFORE="$(sha1sum < "$T/pi/bin/Planetary-pxm.xsgn")"
export PLANETARY_PI="$T/pi/bin/PixInsight.sh"

fails=0
ok(){ echo "  ok    $*"; }
bad(){ echo "  FAIL  $*"; fails=$((fails + 1)); }

# The functions themselves.
(
   . "$HERE/harness.sh"
   n=0
   for so in "$T/pi/bin/Planetary-pxm.so" "$T/elsewhere/Planetary-pxm.so" "$ASTROPI/repository/x/Planetary-pxm.so"; do
      if planetary_check_so "$so" >/dev/null; then echo "  FAIL  planetary_check_so accepted $so"; n=$((n + 1))
      else echo "  ok    planetary_check_so refuses $so"; fi
   done
   if planetary_check_so "$T/build/Planetary-pxm.so" >/dev/null; then echo "  ok    planetary_check_so accepts a build product"
   else echo "  FAIL  planetary_check_so refused a build product"; n=$((n + 1)); fi
   PLANETARY_RUN_DIR="$T"
   if planetary_sign "$T/pi/bin/Planetary-pxm.so" >/dev/null; then echo "  FAIL  planetary_sign signed the installed module"; n=$((n + 1))
   else echo "  ok    planetary_sign refuses the installed module"; fi
   exit "$n"
)
fails=$((fails + $?))

# A symlinked launcher (bench6; final-fix re-review Minor 1): PLANETARY_PI names a link, in another directory,
# to the stand-in install's launcher. The guard follows the link to the install it runs, and refuses
# /opt/PixInsight whatever PLANETARY_PI says (paths need not exist: realpath -m).
mkdir -p "$T/link/bin"
ln -s "$T/pi/bin/PixInsight.sh" "$T/link/bin/PixInsight.sh"
(
   export PLANETARY_PI="$T/link/bin/PixInsight.sh"
   . "$HERE/harness.sh"
   n=0
   for so in "$T/pi/bin/Planetary-pxm.so" "$T/link/../pi/bin/Planetary-pxm.so" /opt/PixInsight/bin/Planetary-pxm.so \
             /opt/PixInsight/../PixInsight/bin/Planetary-pxm.so; do
      if planetary_check_so "$so" >/dev/null; then echo "  FAIL  planetary_check_so accepted $so (launcher a symlink)"; n=$((n + 1))
      else echo "  ok    planetary_check_so refuses $so (launcher a symlink)"; fi
   done
   if planetary_check_so "$T/build/Planetary-pxm.so" >/dev/null; then echo "  ok    planetary_check_so accepts a build product (launcher a symlink)"
   else echo "  FAIL  planetary_check_so refused a build product (launcher a symlink)"; n=$((n + 1)); fi
   PLANETARY_RUN_DIR="$T"
   if planetary_sign "$T/pi/bin/Planetary-pxm.so" >/dev/null; then echo "  FAIL  planetary_sign signed the installed module (launcher a symlink)"; n=$((n + 1))
   else echo "  ok    planetary_sign refuses the installed module (launcher a symlink)"; fi
   exit "$n"
)
fails=$((fails + $?))
for s in run-probe.sh run-probe-stack.sh gui-smoke.sh gui-smoke-stack.sh; do
   out="$(PLANETARY_PI="$T/link/bin/PixInsight.sh" PLANETARY_SO="$T/pi/bin/Planetary-pxm.so" bash "$HERE/$s" "$T/out-link-$s" 2>&1)"; rc=$?
   if [ "$rc" -eq 1 ] && grep -q "refusing PLANETARY_SO" <<<"$out" && [ ! -e "$T/pi-called" ]; then
      ok "$s refuses the installed module with a symlinked launcher before anything runs"
   else bad "$s with a symlinked launcher: exit $rc, PixInsight $([ -e "$T/pi-called" ] && echo called || echo "not called"): $(tail -2 <<<"$out")"; fi
done

# Every script that takes PLANETARY_SO stops on it first: exit 1, the refusal said, PixInsight never started.
for s in run-probe.sh run-probe-stack.sh gui-smoke.sh gui-smoke-stack.sh; do
   for so in "$T/pi/bin/Planetary-pxm.so" "$T/elsewhere/Planetary-pxm.so"; do
      out="$(PLANETARY_SO="$so" bash "$HERE/$s" "$T/out-$s" 2>&1)"; rc=$?
      if [ "$rc" -eq 1 ] && grep -q "refusing PLANETARY_SO" <<<"$out" && [ ! -e "$T/pi-called" ]; then
         ok "$s refuses PLANETARY_SO=$so before anything runs"
      else bad "$s with PLANETARY_SO=$so: exit $rc, PixInsight $([ -e "$T/pi-called" ] && echo called || echo "not called"): $(tail -2 <<<"$out")"; fi
   done
done
[ "$(sha1sum < "$T/pi/bin/Planetary-pxm.xsgn")" = "$XSGN_BEFORE" ] && ok "the stand-in install's .xsgn is untouched" \
   || bad "the stand-in install's .xsgn was changed"

if [ "$fails" -eq 0 ]; then echo "PASS: the harness refuses a shipped PLANETARY_SO"; exit 0; fi
echo "FAIL: $fails check(s)"; exit 1
