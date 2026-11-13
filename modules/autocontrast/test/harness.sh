# shellcheck shell=bash
# AutoContrast test harness: isolation helpers for every script that launches
# PixInsight for an AutoContrast test. Adapted from modules/planetary/test/harness.sh
# (see its header for the measured reasons behind each rule):
# - never slot 1 (the user's real settings); tests use slot AC_TEST_SLOT
#   (default 93; 94 is the gaia-depth-grade scripts', 95 GDG, 96/97 Planetary, 92 AutoContrast GUI smoke),
#   wiped before and after each run, refused while a PixInsight runs on it;
# - the slot is seeded with every installed module except AutoContrast-pxm.so,
#   so the -m= build is the only AutoContrast;
# - every launch goes through pi_headless (tools/pi-headless.sh);
# - the module a test signs must be a build product, never inside a PixInsight
#   install or repository/.

. "$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)/tools/pi-headless.sh"

PI="${AC_PI:-/opt/PixInsight/bin/PixInsight.sh}"
AC_PI_BIN_DIR="$(dirname "$PI")"
AC_KEYS=/home/scarter4work/projects/keys/scarter4work_keys.xssk
AC_PASS_FILE=/tmp/.pi_codesign_pass
AC_MODULE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AC_ASTROPI_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"

ac_require_signing_pass()
{
   if [ ! -s "$AC_PASS_FILE" ]; then
      echo "BLOCKED: signing pass file missing ($AC_PASS_FILE)"
      return 2
   fi
}

AC_EXIT_FNS=()
ac_on_exit()
{
   local f
   for f in "${AC_EXIT_FNS[@]}"; do [ "$f" = "$1" ] && return 0; done
   AC_EXIT_FNS+=( "$1" )
   trap ac_run_exit EXIT
}
ac_run_exit()
{
   local i
   for (( i = ${#AC_EXIT_FNS[@]} - 1; i >= 0; --i )); do "${AC_EXIT_FNS[i]}" || true; done
}

ac_cleanup()
{
   [ -n "${AC_SLOT_SETTINGS:-}" ] && rm -f "$AC_SLOT_SETTINGS"
   [ -n "${AC_RUN_DIR:-}" ] && [ -z "${AC_KEEP_RUN:-}" ] && rm -rf -- "${AC_RUN_DIR:?}"
   return 0
}

# "<pid> <argv>" of every PixInsight process on slot $1.
ac_slot_processes()
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

ac_slot_setup()
{
   local slot="${AC_TEST_SLOT:-93}" running
   case "$slot" in
      ''|*[!0-9]*) echo "FAIL: AC_TEST_SLOT must be a plain decimal integer, got '$slot'"; return 1 ;;
   esac
   if (( 10#$slot < 50 || 10#$slot > 256 )); then
      echo "FAIL: AC_TEST_SLOT must be in [50,256] (reserved for tests), got '$slot'"; return 1
   fi
   AC_SLOT=$(( 10#$slot ))
   running="$(ac_slot_processes "$AC_SLOT")"
   if [ -n "$running" ]; then
      echo "FAIL: a PixInsight is already running on slot $AC_SLOT; not touching its settings:"
      sed 's/^/  /' <<<"$running"
      return 1
   fi
   AC_SLOT_SETTINGS="$(printf '%s/core-%03d-pxi.settings' "$HOME/.PixInsight" "$AC_SLOT")"
   ac_on_exit ac_cleanup
   rm -f "$AC_SLOT_SETTINGS"
}

ac_private_tmp()
{
   # Under the worktree (build-probe-run/, gitignored), never /tmp: /tmp is RAM-backed.
   mkdir -p "$AC_MODULE_ROOT/build-probe-run" || return 1
   AC_RUN_DIR="$(mktemp -d "$AC_MODULE_ROOT/build-probe-run/run.XXXXXX")" || return 1
   ac_on_exit ac_cleanup
   chmod 700 "$AC_RUN_DIR"
   mkdir -m 700 "$AC_RUN_DIR/tmp"
   export TMPDIR="$AC_RUN_DIR/tmp"
}

ac_check_so()
{
   local real dir repo
   real="$(realpath -m -- "$1")"
   repo="$(realpath -m -- "$AC_ASTROPI_ROOT/repository")"
   for dir in "$(dirname "$(dirname "$(realpath -m -- "$PI")")")" \
              "$(realpath -m -- "$(dirname "$AC_PI_BIN_DIR")")" \
              "$(realpath -m -- /opt/PixInsight)"; do
      case "$real" in
         "$dir"/*) echo "FAIL: refusing $1: it is inside the PixInsight install ($dir)"; return 1 ;;
      esac
   done
   case "$real" in
      "$repo"/*) echo "FAIL: refusing $1: it is inside repository/ ($repo)"; return 1 ;;
   esac
}

ac_sign()
{
   local so="$1" pass
   ac_check_so "$so" || return 1
   pass="$(cat "$AC_PASS_FILE")"
   rm -f "${so%.so}.xsgn"
   pi_headless "$PI" --sign-module-file="$so" --xssk-file="$AC_KEYS" --xssk-password="$pass" \
      >"$AC_RUN_DIR/sign.log" 2>&1 || { echo "FAIL: signing $so failed; log: $AC_RUN_DIR/sign.log"; return 1; }
   [ -f "${so%.so}.xsgn" ] || { echo "FAIL: signing produced no ${so%.so}.xsgn"; return 1; }
}

ac_seed_slot_modules()
{
   local noop="$AC_RUN_DIR/seed-noop.js" rc=0
   rm -f "$AC_SLOT_SETTINGS"
   printf '// AutoContrast harness: slot settings bootstrap (no-op)\n' > "$noop"
   PI_HEADLESS_SCREEN=1280x800x24 pi_headless \
      timeout 120 "$PI" -n="$AC_SLOT" --automation-mode --no-startup-scripts --no-modules \
      -r="$noop" --force-exit >"$AC_RUN_DIR/seed-bootstrap.log" 2>&1 || rc=$?
   if [ "$rc" -ne 0 ] || [ ! -s "$AC_SLOT_SETTINGS" ]; then
      echo "FAIL: slot $AC_SLOT settings bootstrap failed (rc=$rc); log: $AC_RUN_DIR/seed-bootstrap.log"
      return 1
   fi
   python3 - "$AC_SLOT_SETTINGS" "$AC_PI_BIN_DIR" <<'PY' || { echo "FAIL: could not seed the Modules list of $AC_SLOT_SETTINGS"; return 1; }
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
mods = [m for m in mods if m != "AutoContrast-pxm.so"]
vals = "".join('<v k="%08d" t="s">%s</v>' % (i + 1, escape("@pxi_bin_dir/" + m)) for i, m in enumerate(mods))
end = s.rindex("</xsdt>")
s = s[:end] + '<i k="Modules">' + vals + "</i>" + s[end:]
tmp = path + ".seed-tmp"
open(tmp, "w", encoding="utf-8").write(s)
os.replace(tmp, path)
PY
   if grep -q 'AutoContrast-pxm\.so' "$AC_SLOT_SETTINGS"; then
      echo "FAIL: seeded $AC_SLOT_SETTINGS still names a AutoContrast-pxm.so"; return 1
   fi
}

# ac_run_scp <case> <one-line>: writes <one-line> as <run-dir>/<case>.scp and runs it in PixInsight
# under --automation-mode with the signed module loaded (-m="$AC_SO", set by the caller), on slot
# $AC_SLOT. The command line is executed by the core's command-line processor, which dispatches to
# the process named first (here AutoContrastProbe). No PJSR. Returns the launch's exit status; the
# log is <run-dir>/<case>.log.
ac_run_scp()
{
   local case_id="$1" line="$2" scp rc=0
   scp="$AC_RUN_DIR/$case_id.scp"
   printf '%s\n' "$line" > "$scp"
   pi_headless timeout 600 "$PI" -n="$AC_SLOT" --automation-mode --no-startup-scripts \
      -m="$AC_SO" -r="$scp" --force-exit >"$AC_RUN_DIR/$case_id.log" 2>&1 || rc=$?
   return $rc
}
