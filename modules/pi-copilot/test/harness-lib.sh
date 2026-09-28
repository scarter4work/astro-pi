# PI Copilot test harness: shared isolation helpers. SOURCE this (bash) from
# every script that launches PixInsight for a PI Copilot test or probe:
# run-selftest.sh, gui-smoke.sh, run-load.sh -- and any ad-hoc probe. It is the
# one place that makes a test PixInsight (a) load ONLY the -m= dev build and
# (b) never see the user's real journey library.
#
#   . "$HERE/harness-lib.sh"
#   picopilot_isolate_data "$TMPDIR/xdg"              # private XDG_DATA_HOME, exported
#   JOURNEYS_BEFORE="$(picopilot_journeys_fingerprint)"
#   picopilot_seed_slot_modules "$SLOT" "$TMPDIR"      # slot settings WITHOUT the installed PICopilot
#   picopilot_require_isolation || exit 1              # immediately before EVERY PixInsight launch
#   ... run PixInsight -n=$SLOT -m=<dev .so> ...
#   picopilot_journeys_check "$JOURNEYS_BEFORE" || exit 1
#
# WHY the module seeding exists (root cause, measured 2026-09-27 on slot 99):
# every harness deletes its slot's core-NNN-pxi.settings before a run so each
# run is hermetic. A slot with no settings file (or one whose LastVersion is
# not the running core's) makes PixInsight do its first-run module
# installation: it scans /opt/PixInsight/bin and installs EVERY *-pxm.so there
# -- which, since the user installed PICopilot 0.2.0.0 into bin/, includes the
# INSTALLED PICopilot-pxm.so -- and then also loads the -m= dev build. Both
# end up mapped in the test process (same module ID "PICopilot"; the -m= build
# is the one that answers). The installed one is mapped ~70 ms BEFORE the dev
# one, so watchdog.py's "first PICopilot-pxm.so in /proc/<pid>/maps" check
# reported a module-load mismatch whenever its 0.5 s poll landed in that
# window: the intermittent failure. A slot whose settings already carry a
# Modules list and the current LastVersion is NOT rescanned (measured), so we
# seed exactly that: PixInsight writes its own settings file with
# --no-modules (documented: install no modules), and we add a Modules list of
# every @pxi_bin_dir/*-pxm.so except PICopilot-pxm.so. -m= then adds the dev
# build, and nothing else named PICopilot-pxm.so is ever mapped.

PICOPILOT_PI="${PICOPILOT_PI:-/opt/PixInsight/bin/PixInsight.sh}"
PICOPILOT_PI_BIN_DIR="$(dirname "$PICOPILOT_PI")"
PICOPILOT_REAL_DATA_HOME="$HOME/.local/share"
PICOPILOT_REAL_LIB="$PICOPILOT_REAL_DATA_HOME/PICopilot"
PICOPILOT_REAL_JOURNEYS="$PICOPILOT_REAL_LIB/journeys"

# Private XDG_DATA_HOME: the journey library ($XDG_DATA_HOME/PICopilot/journeys,
# JourneyService::LibraryRoot) is the only user data PICopilot reads through the
# environment; its settings live in the (per-slot, wiped) PixInsight settings
# file and its API key is never touched by the self-test. An EMPTY data home
# breaks unrelated PixInsight checks (measured: GraXpert live run, process
# catalog scan), so every other top-level entry of the real data home is
# mirrored in as a symlink; only PICopilot is left out.
picopilot_isolate_data()
{
   local dir="$1" real entry
   if [ -z "$dir" ] || [ "${dir#/}" = "$dir" ]; then
      echo "FAIL: picopilot_isolate_data needs an absolute directory (got '$dir')"; return 1
   fi
   if [ -n "${PICOPILOT_ISOLATED_XDG:-}" ]; then
      echo "FAIL: data isolation already set up (PICOPILOT_ISOLATED_XDG=$PICOPILOT_ISOLATED_XDG)"; return 1
   fi
   mkdir -m 700 "$dir" || { echo "FAIL: cannot create private XDG_DATA_HOME $dir"; return 1; }
   real="${XDG_DATA_HOME:-$PICOPILOT_REAL_DATA_HOME}"
   if [ -d "$real" ]; then
      for entry in "$real"/* "$real"/.[!.]*; do
         [ -e "$entry" ] || [ -L "$entry" ] || continue
         [ "$(basename "$entry")" = "PICopilot" ] && continue
         ln -s "$entry" "$dir/$(basename "$entry")"
      done
   fi
   export XDG_DATA_HOME="$dir"
   export PICOPILOT_ISOLATED_XDG="$dir"
}

# Refuse to launch PixInsight unless data isolation is really in effect.
# Call it immediately before EVERY PixInsight launch.
picopilot_require_isolation()
{
   local xdg="${XDG_DATA_HOME:-}" real_home real_lib lib
   if [ -z "$xdg" ] || [ "${xdg#/}" = "$xdg" ]; then
      echo "FAIL: refusing to launch PixInsight: XDG_DATA_HOME is unset or relative ('$xdg'), so PICopilot would open the REAL journey library $PICOPILOT_REAL_JOURNEYS"; return 1
   fi
   if [ "$xdg" != "${PICOPILOT_ISOLATED_XDG:-}" ]; then
      echo "FAIL: refusing to launch PixInsight: XDG_DATA_HOME ($xdg) is not the private data home picopilot_isolate_data created ('${PICOPILOT_ISOLATED_XDG:-none}')"; return 1
   fi
   if [ -L "$xdg" ] || [ ! -d "$xdg" ]; then
      echo "FAIL: refusing to launch PixInsight: private XDG_DATA_HOME $xdg is not a real directory"; return 1
   fi
   real_home="$(realpath -m "$PICOPILOT_REAL_DATA_HOME")"
   if [ "$(realpath -m "$xdg")" = "$real_home" ]; then
      echo "FAIL: refusing to launch PixInsight: XDG_DATA_HOME resolves to the real data home $real_home"; return 1
   fi
   lib="$xdg/PICopilot"
   if [ -e "$lib" ] || [ -L "$lib" ]; then
      real_lib="$(realpath -m "$PICOPILOT_REAL_LIB")"
      case "$(realpath -m "$lib")/" in
         "$real_lib"/*)
            echo "FAIL: refusing to launch PixInsight: $lib resolves into the real library $real_lib"; return 1 ;;
      esac
      if [ -L "$lib" ]; then
         echo "FAIL: refusing to launch PixInsight: $lib is a symlink"; return 1
      fi
   fi
   return 0
}

# Fingerprint of the user's real PICopilot data (the journeys directory's own
# mtime first, then every entry under ~/.local/share/PICopilot with its size
# and mtime). Take it before the first launch; picopilot_journeys_check fails
# the run if it changed.
picopilot_journeys_fingerprint()
{
   if [ -e "$PICOPILOT_REAL_JOURNEYS" ]; then
      printf 'journeys-mtime %s\n' "$(stat -c '%.9Y' "$PICOPILOT_REAL_JOURNEYS")"
   else
      printf 'journeys-absent\n'
   fi
   if [ -e "$PICOPILOT_REAL_LIB" ]; then
      find "$PICOPILOT_REAL_LIB" -printf '%p %y %s %T@\n' 2>&1 | sort
   else
      printf 'lib-absent\n'
   fi
}

picopilot_journeys_check()
{
   local before="$1" after
   after="$(picopilot_journeys_fingerprint)"
   if [ "$before" != "$after" ]; then
      echo "FAIL: the user's real journey library changed during this run ($PICOPILOT_REAL_JOURNEYS)"
      diff <(printf '%s\n' "$before") <(printf '%s\n' "$after") | head -20
      return 1
   fi
   echo "real journey library untouched: $(printf '%s\n' "$after" | head -1)"
}

# Loud check on a RUNNING test PixInsight (-n=<slot>): the dev .so is mapped
# and no other file named PICopilot-pxm.so is. (run-selftest.sh gets the same
# check continuously from watchdog.py.)
picopilot_assert_only_dev_mapped()
{
   local slot="$1" so="$2" pid maps
   pid="$(pgrep -f -- "^$PICOPILOT_PI_BIN_DIR/PixInsight -n=$(( 10#$slot )) " | head -1)"
   [ -n "$pid" ] || { echo "FAIL: no running PixInsight for slot $slot to check"; return 1; }
   maps="$(grep -- 'PICopilot-pxm\.so' "/proc/$pid/maps" | awk '{print $NF}' | sort -u | xargs -r -n1 realpath -m | sort -u)"
   if [ "$maps" != "$(realpath -e "$so")" ]; then
      echo "FAIL: PixInsight (slot $slot, pid $pid) maps [$(echo $maps)], expected ONLY the dev build $so"; return 1
   fi
   echo "only the dev module is mapped (pid $pid): $maps"
}

# Seed <slot>'s settings file so PixInsight loads every installed module EXCEPT
# the installed PICopilot-pxm.so (the -m= dev build is the only PICopilot).
# See the header for the measured root cause. Needs a private TMPDIR-style
# scratch dir; must run after picopilot_isolate_data (it launches PixInsight).
picopilot_seed_slot_modules()
{
   local slot="$1" scratch="$2" settings noop rc=0
   case "$slot" in ''|*[!0-9]*) echo "FAIL: picopilot_seed_slot_modules: bad slot '$slot'"; return 1 ;; esac
   if (( 10#$slot < 50 || 10#$slot > 256 )); then
      echo "FAIL: picopilot_seed_slot_modules: slot $slot is outside the reserved test range [50,256]"; return 1
   fi
   settings="$(printf '%s/core-%03d-pxi.settings' "$HOME/.PixInsight" "$(( 10#$slot ))")"
   picopilot_require_isolation || return 1
   rm -f "$settings"
   noop="$scratch/seed-noop.js"
   printf '// PI Copilot harness: slot settings bootstrap (no-op)\n' > "$noop"
   # PixInsight writes the slot's settings file (with its own LastVersion) at
   # start-up; --no-modules keeps every module, including the installed
   # PICopilot, out of this bootstrap process.
   xvfb-run -a -s "-screen 0 1280x800x24" \
      timeout 120 "$PICOPILOT_PI" -n="$(( 10#$slot ))" --automation-mode --no-startup-scripts --no-modules \
      -r="$noop" --force-exit >"$scratch/seed-bootstrap.log" 2>&1 || rc=$?
   rm -f "$noop"
   if [ "$rc" -ne 0 ] || [ ! -s "$settings" ]; then
      echo "FAIL: slot $slot settings bootstrap failed (rc=$rc, settings file $( [ -s "$settings" ] && echo present || echo missing )); log: $scratch/seed-bootstrap.log"
      return 1
   fi
   python3 - "$settings" "$PICOPILOT_PI_BIN_DIR" <<'PY' || { echo "FAIL: could not seed the Modules list of $settings"; return 1; }
import glob, os, re, sys
from xml.sax.saxutils import escape
path, bindir = sys.argv[1], sys.argv[2]
s = open(path, encoding="utf-8").read()
if not re.search(r'<v k="LastVersion" t="s">[^<]+</v>', s):
    sys.exit("no LastVersion in the bootstrapped settings: PixInsight would rescan bin/ and load the installed PICopilot")
if 'k="Modules"' in s:
    sys.exit("the --no-modules bootstrap unexpectedly wrote a Modules list")
mods = sorted(os.path.basename(p) for p in glob.glob(os.path.join(bindir, "*-pxm.so")))
if not mods:
    sys.exit("no *-pxm.so found in %s" % bindir)
mods = [m for m in mods if m != "PICopilot-pxm.so"]
vals = "".join('<v k="%08d" t="s">%s</v>' % (i + 1, escape("@pxi_bin_dir/" + m)) for i, m in enumerate(mods))
end = s.rindex("</xsdt>")
s = s[:end] + '<i k="Modules">' + vals + "</i>" + s[end:]
tmp = path + ".seed-tmp"
open(tmp, "w", encoding="utf-8").write(s)
os.replace(tmp, path)
print("slot settings seeded: %d installed modules, installed PICopilot-pxm.so excluded" % len(mods))
PY
   if grep -q 'PICopilot-pxm\.so' "$settings"; then
      echo "FAIL: seeded $settings still names a PICopilot-pxm.so"; return 1
   fi
}
