# shellcheck shell=bash
# EZTools test harness: isolation helpers for every script that launches
# PixInsight for an EZTools test. Adapted from modules/gaia-depth-grade/test/harness.sh and modules/planetary/test/harness.sh
# (see its header for the measured reasons behind each rule):
# - never slot 1 (the user's real settings); tests use slot EZT_TEST_SLOT
#   (default 99 for the headless probes, 92 for the GUI smoke; 95 is GaiaDepthGrade's,
#   96/97 Planetary's, 98 the EZHazeKill.js parity harness's),
#   wiped before and after each run, refused while a PixInsight runs on it;
# - the slot is seeded with every installed module except EZTools-pxm.so,
#   so the -m= build is the only EZTools;
# - every launch goes through pi_headless (tools/pi-headless.sh);
# - the module a test signs must be a build product, never inside a PixInsight
#   install or repository/.

. "$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)/tools/pi-headless.sh"

PI="${EZT_PI:-/opt/PixInsight/bin/PixInsight.sh}"
EZT_PI_BIN_DIR="$(dirname "$PI")"
EZT_KEYS=/home/scarter4work/projects/keys/scarter4work_keys.xssk
EZT_PASS_FILE=/tmp/.pi_codesign_pass
EZT_ASTROPI_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"

ezt_require_signing_pass()
{
   if [ ! -s "$EZT_PASS_FILE" ]; then
      echo "BLOCKED: signing pass file missing ($EZT_PASS_FILE)"
      return 2
   fi
}

EZT_EXIT_FNS=()
ezt_on_exit()
{
   local f
   for f in "${EZT_EXIT_FNS[@]}"; do [ "$f" = "$1" ] && return 0; done
   EZT_EXIT_FNS+=( "$1" )
   trap ezt_run_exit EXIT
}
ezt_run_exit()
{
   local i
   for (( i = ${#EZT_EXIT_FNS[@]} - 1; i >= 0; --i )); do "${EZT_EXIT_FNS[i]}" || true; done
}

ezt_cleanup()
{
   [ -n "${EZT_SLOT_SETTINGS:-}" ] && rm -f "$EZT_SLOT_SETTINGS"
   [ -n "${EZT_RUN_DIR:-}" ] && rm -rf "$EZT_RUN_DIR"
   return 0
}

# "<pid> <argv>" of every PixInsight process on slot $1.
ezt_slot_processes()
{
   local slot="$1" d a pi n
   local -a argv
   for d in /proc/[0-9]*; do
      argv=()
      mapfile -d '' -t argv < "$d/cmdline" 2>/dev/null || continue
      pi=0 n=0
      for a in "${argv[@]}"; do
         case "$a" in
            PixInsight|PixInsight.sh|*/PixInsight|*/PixInsight.sh) pi=1 ;;
            "-n=$slot") n=1 ;;
         esac
      done
      (( pi && n )) && echo "${d#/proc/} ${argv[*]}"
   done
   return 0
}

ezt_slot_setup()
{
   local slot="${EZT_TEST_SLOT:-99}" running
   case "$slot" in
      ''|*[!0-9]*) echo "FAIL: EZT_TEST_SLOT must be a plain decimal integer, got '$slot'"; return 1 ;;
   esac
   if (( 10#$slot < 50 || 10#$slot > 256 )); then
      echo "FAIL: EZT_TEST_SLOT must be in [50,256] (reserved for tests), got '$slot'"; return 1
   fi
   EZT_SLOT=$(( 10#$slot ))
   running="$(ezt_slot_processes "$EZT_SLOT")"
   if [ -n "$running" ]; then
      echo "FAIL: a PixInsight is already running on slot $EZT_SLOT; not touching its settings:"
      sed 's/^/  /' <<<"$running"
      return 1
   fi
   EZT_SLOT_SETTINGS="$(printf '%s/core-%03d-pxi.settings' "$HOME/.PixInsight" "$EZT_SLOT")"
   ezt_on_exit ezt_cleanup
   rm -f "$EZT_SLOT_SETTINGS"
}

ezt_private_tmp()
{
   EZT_RUN_DIR="$(mktemp -d "${TMPDIR:-/tmp}/eztools-test.XXXXXX")" || return 1
   ezt_on_exit ezt_cleanup
   chmod 700 "$EZT_RUN_DIR"
   mkdir -m 700 "$EZT_RUN_DIR/tmp"
   export TMPDIR="$EZT_RUN_DIR/tmp"
}

ezt_check_so()
{
   local real dir repo
   real="$(realpath -m -- "$1")"
   repo="$(realpath -m -- "$EZT_ASTROPI_ROOT/repository")"
   for dir in "$(dirname "$(dirname "$(realpath -m -- "$PI")")")" \
              "$(realpath -m -- "$(dirname "$EZT_PI_BIN_DIR")")" \
              "$(realpath -m -- /opt/PixInsight)"; do
      case "$real" in
         "$dir"/*) echo "FAIL: refusing $1: it is inside the PixInsight install ($dir)"; return 1 ;;
      esac
   done
   case "$real" in
      "$repo"/*) echo "FAIL: refusing $1: it is inside repository/ ($repo)"; return 1 ;;
   esac
}

ezt_sign()
{
   local so="$1" pass
   ezt_check_so "$so" || return 1
   pass="$(cat "$EZT_PASS_FILE")"
   rm -f "${so%.so}.xsgn"
   pi_headless "$PI" --sign-module-file="$so" --xssk-file="$EZT_KEYS" --xssk-password="$pass" \
      >"$EZT_RUN_DIR/sign.log" 2>&1 || { echo "FAIL: signing $so failed; log: $EZT_RUN_DIR/sign.log"; return 1; }
   [ -f "${so%.so}.xsgn" ] || { echo "FAIL: signing produced no ${so%.so}.xsgn"; return 1; }
}

ezt_seed_slot_modules()
{
   local noop="$EZT_RUN_DIR/seed-noop.js" rc=0
   rm -f "$EZT_SLOT_SETTINGS"
   printf '// EZTools harness: slot settings bootstrap (no-op)\n' > "$noop"
   PI_HEADLESS_SCREEN=1280x800x24 pi_headless \
      timeout 120 "$PI" -n="$EZT_SLOT" --automation-mode --no-startup-scripts --no-modules \
      -r="$noop" --force-exit >"$EZT_RUN_DIR/seed-bootstrap.log" 2>&1 || rc=$?
   if [ "$rc" -ne 0 ] || [ ! -s "$EZT_SLOT_SETTINGS" ]; then
      echo "FAIL: slot $EZT_SLOT settings bootstrap failed (rc=$rc); log: $EZT_RUN_DIR/seed-bootstrap.log"
      return 1
   fi
   python3 - "$EZT_SLOT_SETTINGS" "$EZT_PI_BIN_DIR" <<'PY' || { echo "FAIL: could not seed the Modules list of $EZT_SLOT_SETTINGS"; return 1; }
import glob, os, re, sys
from xml.sax.saxutils import escape
path, bindir = sys.argv[1], sys.argv[2]
s = open(path, encoding="utf-8").read()
if not re.search(r'<v k="LastVersion" t="s">[^<]+</v>', s):
    sys.exit("no LastVersion in the bootstrapped settings: PixInsight would rescan bin/")
if 'k="Modules"' in s:
    sys.exit("the --no-modules bootstrap unexpectedly wrote a Modules list")
mods = sorted(os.path.basename(p) for p in glob.glob(os.path.join(bindir, "*-pxm.so")))
if not mods:
    sys.exit("no *-pxm.so found in %s" % bindir)
mods = [m for m in mods if m != "EZTools-pxm.so"]
vals = "".join('<v k="%08d" t="s">%s</v>' % (i + 1, escape("@pxi_bin_dir/" + m)) for i, m in enumerate(mods))
end = s.rindex("</xsdt>")
s = s[:end] + '<i k="Modules">' + vals + "</i>" + s[end:]
tmp = path + ".seed-tmp"
open(tmp, "w", encoding="utf-8").write(s)
os.replace(tmp, path)
PY
   if grep -q 'EZTools-pxm\.so' "$EZT_SLOT_SETTINGS"; then
      echo "FAIL: seeded $EZT_SLOT_SETTINGS still names an EZTools-pxm.so"; return 1
   fi
}
