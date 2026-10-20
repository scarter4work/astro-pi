# shellcheck shell=bash
# Planetary test harness: shared isolation helpers. SOURCE this (bash) from
# every script that launches PixInsight for a Planetary test (run-probe.sh,
# the GUI smoke). Adapted from modules/pi-copilot/test/harness-lib.sh and
# run-load.sh; see their headers for the measured root causes.
#
#   . "$HERE/harness.sh"
#   planetary_require_signing_pass || exit 2   # BEFORE anything touches a slot
#   planetary_slot_setup                       # PLANETARY_SLOT/_SETTINGS, wiped now and on exit
#   planetary_private_tmp                      # PLANETARY_RUN_DIR (0700), private TMPDIR
#   planetary_sign "$SO"                       # dev build signed with the developer key
#   planetary_seed_slot_modules                # slot settings WITHOUT any installed Planetary
#   ... pi_headless timeout N "$PI" -n="$PLANETARY_SLOT" ... -m="$SO" -r=<probe> --force-exit
#
# Isolation rules (all measured on PixInsight 1.9.5, see the pi-copilot harness):
# - Never run in slot 1: its settings file is the user's real
#   ~/.PixInsight/core-001-pxi.settings, and a -m= run persists the dev module
#   path into it ("Duplicate MetaProcess identifier" on the next real launch).
#   Tests use one fixed slot in [50,256], wiped before and after every run.
# - A slot with no settings file makes PixInsight install every *-pxm.so in
#   its bin/ directory; once Planetary is installed from the repository that
#   would load the installed Planetary next to the -m= dev build. The slot is
#   seeded with a Modules list of every installed module except Planetary.
# - Every launch goes through pi_headless (tools/pi-headless.sh): private Xvfb,
#   no Wayland, throwaway XDG_DATA_HOME (an installed PI Copilot would record
#   test images into the user's journey library otherwise).

. "$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)/tools/pi-headless.sh"

PI="${PLANETARY_PI:-/opt/PixInsight/bin/PixInsight.sh}"
PLANETARY_PI_BIN_DIR="$(dirname "$PI")"
PLANETARY_KEYS=/home/scarter4work/projects/keys/scarter4work_keys.xssk
PLANETARY_PASS_FILE=/tmp/.pi_codesign_pass

# Signing needs the pass file Scott provides; without it no -m= load can work.
# Checked first, so a blocked run never touches a settings slot.
planetary_require_signing_pass()
{
   if [ ! -s "$PLANETARY_PASS_FILE" ]; then
      echo "BLOCKED: signing pass file missing ($PLANETARY_PASS_FILE)"
      return 2
   fi
}

# Removes whatever this run created: the slot's settings and the run dir.
planetary_cleanup()
{
   [ -n "${PLANETARY_SLOT_SETTINGS:-}" ] && rm -f "$PLANETARY_SLOT_SETTINGS"
   [ -n "${PLANETARY_RUN_DIR:-}" ] && rm -rf "$PLANETARY_RUN_DIR"
   return 0
}

# PLANETARY_TEST_SLOT (default 96; 94 is gaia-depth-grade's, 97 the GUI
# smoke's) -> PLANETARY_SLOT + PLANETARY_SLOT_SETTINGS, wiped now and on exit.
# The value is env-controlled and its settings file gets deleted, so reject
# anything but a plain decimal in the reserved test range BEFORE any arithmetic
# (bash reads "0x1" as hex and "01" as octal: both would otherwise be slot 1).
planetary_slot_setup()
{
   local slot="${PLANETARY_TEST_SLOT:-96}"
   case "$slot" in
      ''|*[!0-9]*) echo "FAIL: PLANETARY_TEST_SLOT must be a plain decimal integer, got '$slot'"; return 1 ;;
   esac
   if (( 10#$slot < 50 || 10#$slot > 256 )); then
      echo "FAIL: PLANETARY_TEST_SLOT must be in [50,256] (reserved for tests), got '$slot'"; return 1
   fi
   PLANETARY_SLOT=$(( 10#$slot ))
   PLANETARY_SLOT_SETTINGS="$(printf '%s/core-%03d-pxi.settings' "$HOME/.PixInsight" "$PLANETARY_SLOT")"
   trap planetary_cleanup EXIT
   rm -f "$PLANETARY_SLOT_SETTINGS"
}

# Private scratch for this run (0700): fixtures, probe output, logs, and the
# TMPDIR PixInsight and pi_headless use. Removed on exit (planetary_cleanup).
planetary_private_tmp()
{
   PLANETARY_RUN_DIR="$(mktemp -d "${TMPDIR:-/tmp}/planetary-test.XXXXXX")" || return 1
   trap planetary_cleanup EXIT
   chmod 700 "$PLANETARY_RUN_DIR"
   mkdir -m 700 "$PLANETARY_RUN_DIR/tmp"
   export TMPDIR="$PLANETARY_RUN_DIR/tmp"
}

planetary_sign()
{
   local so="$1" pass
   pass="$(cat "$PLANETARY_PASS_FILE")"
   rm -f "${so%.so}.xsgn"
   pi_headless "$PI" --sign-module-file="$so" --xssk-file="$PLANETARY_KEYS" --xssk-password="$pass" \
      >"$PLANETARY_RUN_DIR/sign.log" 2>&1 || { echo "FAIL: signing $so failed; log: $PLANETARY_RUN_DIR/sign.log"; return 1; }
   [ -f "${so%.so}.xsgn" ] || { echo "FAIL: signing produced no ${so%.so}.xsgn"; return 1; }
}

# Seed the slot's settings so PixInsight loads every installed module EXCEPT an
# installed Planetary-pxm.so (the -m= dev build is the only Planetary).
planetary_seed_slot_modules()
{
   local noop="$PLANETARY_RUN_DIR/seed-noop.js" rc=0
   rm -f "$PLANETARY_SLOT_SETTINGS"
   printf '// Planetary harness: slot settings bootstrap (no-op)\n' > "$noop"
   # PixInsight writes the slot's settings file (with its own LastVersion) at
   # start-up; --no-modules keeps every module out of this bootstrap process.
   PI_HEADLESS_SCREEN=1280x800x24 pi_headless \
      timeout 120 "$PI" -n="$PLANETARY_SLOT" --automation-mode --no-startup-scripts --no-modules \
      -r="$noop" --force-exit >"$PLANETARY_RUN_DIR/seed-bootstrap.log" 2>&1 || rc=$?
   if [ "$rc" -ne 0 ] || [ ! -s "$PLANETARY_SLOT_SETTINGS" ]; then
      echo "FAIL: slot $PLANETARY_SLOT settings bootstrap failed (rc=$rc); log: $PLANETARY_RUN_DIR/seed-bootstrap.log"
      return 1
   fi
   python3 - "$PLANETARY_SLOT_SETTINGS" "$PLANETARY_PI_BIN_DIR" <<'PY' || { echo "FAIL: could not seed the Modules list of $PLANETARY_SLOT_SETTINGS"; return 1; }
import glob, os, re, sys
from xml.sax.saxutils import escape
path, bindir = sys.argv[1], sys.argv[2]
s = open(path, encoding="utf-8").read()
if not re.search(r'<v k="LastVersion" t="s">[^<]+</v>', s):
    sys.exit("no LastVersion in the bootstrapped settings: PixInsight would rescan bin/ and load an installed Planetary")
if 'k="Modules"' in s:
    sys.exit("the --no-modules bootstrap unexpectedly wrote a Modules list")
mods = sorted(os.path.basename(p) for p in glob.glob(os.path.join(bindir, "*-pxm.so")))
if not mods:
    sys.exit("no *-pxm.so found in %s" % bindir)
mods = [m for m in mods if m != "Planetary-pxm.so"]
vals = "".join('<v k="%08d" t="s">%s</v>' % (i + 1, escape("@pxi_bin_dir/" + m)) for i, m in enumerate(mods))
end = s.rindex("</xsdt>")
s = s[:end] + '<i k="Modules">' + vals + "</i>" + s[end:]
tmp = path + ".seed-tmp"
open(tmp, "w", encoding="utf-8").write(s)
os.replace(tmp, path)
print("slot settings seeded: %d installed modules, any installed Planetary-pxm.so excluded" % len(mods))
PY
   if grep -q 'Planetary-pxm\.so' "$PLANETARY_SLOT_SETTINGS"; then
      echo "FAIL: seeded $PLANETARY_SLOT_SETTINGS still names a Planetary-pxm.so"; return 1
   fi
}
