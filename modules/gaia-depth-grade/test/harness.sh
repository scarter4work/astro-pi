# shellcheck shell=bash
# GaiaDepthGrade test harness: isolation helpers for every script that launches
# PixInsight for a GaiaDepthGrade test. Adapted from modules/planetary/test/harness.sh
# (see its header for the measured reasons behind each rule):
# - never slot 1 (the user's real settings); tests use slot GDG_TEST_SLOT
#   (default 95; 94 is the gaia-depth-grade scripts', 96/97 Planetary's),
#   wiped before and after each run, refused while a PixInsight runs on it;
# - the slot is seeded with every installed module except GaiaDepthGrade-pxm.so,
#   so the -m= build is the only GaiaDepthGrade;
# - every launch goes through pi_headless (tools/pi-headless.sh);
# - the module a test signs must be a build product, never inside a PixInsight
#   install or repository/.

. "$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)/tools/pi-headless.sh"

PI="${GDG_PI:-/opt/PixInsight/bin/PixInsight.sh}"
GDG_PI_BIN_DIR="$(dirname "$PI")"
GDG_KEYS=/home/scarter4work/projects/keys/scarter4work_keys.xssk
GDG_PASS_FILE=/tmp/.pi_codesign_pass
GDG_ASTROPI_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"

gdg_require_signing_pass()
{
   if [ ! -s "$GDG_PASS_FILE" ]; then
      echo "BLOCKED: signing pass file missing ($GDG_PASS_FILE)"
      return 2
   fi
}

GDG_EXIT_FNS=()
gdg_on_exit()
{
   local f
   for f in "${GDG_EXIT_FNS[@]}"; do [ "$f" = "$1" ] && return 0; done
   GDG_EXIT_FNS+=( "$1" )
   trap gdg_run_exit EXIT
}
gdg_run_exit()
{
   local i
   for (( i = ${#GDG_EXIT_FNS[@]} - 1; i >= 0; --i )); do "${GDG_EXIT_FNS[i]}" || true; done
}

gdg_cleanup()
{
   [ -n "${GDG_SLOT_SETTINGS:-}" ] && rm -f "$GDG_SLOT_SETTINGS"
   [ -n "${GDG_RUN_DIR:-}" ] && rm -rf "$GDG_RUN_DIR"
   return 0
}

# "<pid> <argv>" of every PixInsight process on slot $1.
gdg_slot_processes()
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

gdg_slot_setup()
{
   local slot="${GDG_TEST_SLOT:-95}" running
   case "$slot" in
      ''|*[!0-9]*) echo "FAIL: GDG_TEST_SLOT must be a plain decimal integer, got '$slot'"; return 1 ;;
   esac
   if (( 10#$slot < 50 || 10#$slot > 256 )); then
      echo "FAIL: GDG_TEST_SLOT must be in [50,256] (reserved for tests), got '$slot'"; return 1
   fi
   GDG_SLOT=$(( 10#$slot ))
   running="$(gdg_slot_processes "$GDG_SLOT")"
   if [ -n "$running" ]; then
      echo "FAIL: a PixInsight is already running on slot $GDG_SLOT; not touching its settings:"
      sed 's/^/  /' <<<"$running"
      return 1
   fi
   GDG_SLOT_SETTINGS="$(printf '%s/core-%03d-pxi.settings' "$HOME/.PixInsight" "$GDG_SLOT")"
   gdg_on_exit gdg_cleanup
   rm -f "$GDG_SLOT_SETTINGS"
}

gdg_private_tmp()
{
   GDG_RUN_DIR="$(mktemp -d "${TMPDIR:-/tmp}/gdg-test.XXXXXX")" || return 1
   gdg_on_exit gdg_cleanup
   chmod 700 "$GDG_RUN_DIR"
   mkdir -m 700 "$GDG_RUN_DIR/tmp"
   export TMPDIR="$GDG_RUN_DIR/tmp"
}

gdg_check_so()
{
   local real dir repo
   real="$(realpath -m -- "$1")"
   repo="$(realpath -m -- "$GDG_ASTROPI_ROOT/repository")"
   for dir in "$(dirname "$(dirname "$(realpath -m -- "$PI")")")" \
              "$(realpath -m -- "$(dirname "$GDG_PI_BIN_DIR")")" \
              "$(realpath -m -- /opt/PixInsight)"; do
      case "$real" in
         "$dir"/*) echo "FAIL: refusing $1: it is inside the PixInsight install ($dir)"; return 1 ;;
      esac
   done
   case "$real" in
      "$repo"/*) echo "FAIL: refusing $1: it is inside repository/ ($repo)"; return 1 ;;
   esac
}

gdg_sign()
{
   local so="$1" pass
   gdg_check_so "$so" || return 1
   pass="$(cat "$GDG_PASS_FILE")"
   rm -f "${so%.so}.xsgn"
   pi_headless "$PI" --sign-module-file="$so" --xssk-file="$GDG_KEYS" --xssk-password="$pass" \
      >"$GDG_RUN_DIR/sign.log" 2>&1 || { echo "FAIL: signing $so failed; log: $GDG_RUN_DIR/sign.log"; return 1; }
   [ -f "${so%.so}.xsgn" ] || { echo "FAIL: signing produced no ${so%.so}.xsgn"; return 1; }
}

gdg_seed_slot_modules()
{
   local noop="$GDG_RUN_DIR/seed-noop.js" rc=0
   rm -f "$GDG_SLOT_SETTINGS"
   printf '// GaiaDepthGrade harness: slot settings bootstrap (no-op)\n' > "$noop"
   PI_HEADLESS_SCREEN=1280x800x24 pi_headless \
      timeout 120 "$PI" -n="$GDG_SLOT" --automation-mode --no-startup-scripts --no-modules \
      -r="$noop" --force-exit >"$GDG_RUN_DIR/seed-bootstrap.log" 2>&1 || rc=$?
   if [ "$rc" -ne 0 ] || [ ! -s "$GDG_SLOT_SETTINGS" ]; then
      echo "FAIL: slot $GDG_SLOT settings bootstrap failed (rc=$rc); log: $GDG_RUN_DIR/seed-bootstrap.log"
      return 1
   fi
   python3 - "$GDG_SLOT_SETTINGS" "$GDG_PI_BIN_DIR" <<'PY' || { echo "FAIL: could not seed the Modules list of $GDG_SLOT_SETTINGS"; return 1; }
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
mods = [m for m in mods if m != "GaiaDepthGrade-pxm.so"]
vals = "".join('<v k="%08d" t="s">%s</v>' % (i + 1, escape("@pxi_bin_dir/" + m)) for i, m in enumerate(mods))
end = s.rindex("</xsdt>")
s = s[:end] + '<i k="Modules">' + vals + "</i>" + s[end:]
# PixInsight 1.9.6 opens a modal "Risky Temporary Folders" warning at GUI start-up when image swap files go to a
# system temporary folder (the run's private TMPDIR, by design); the GUI smoke never got past it (2026-11-12,
# not-ready.png). Answered as its own "Don't show again" box does (PI Copilot's harness, a03f0efe). Headless
# --automation-mode runs never show it.
warn = '<v k="WarningOnSystemTempFolderBeingUsedForSwapFiles_Value" t="b" v="false"/>'
if "WarningOnSystemTempFolderBeingUsedForSwapFiles_Value" in s:
    sys.exit("the bootstrapped settings already carry the swap-folder warning answer; review this seeding")
if '<i k="DontShowAgain">' in s:
    s = s.replace('<i k="DontShowAgain">', '<i k="DontShowAgain">' + warn, 1)
else:
    end = s.rindex("</xsdt>")
    s = s[:end] + '<i k="DontShowAgain">' + warn + "</i>" + s[end:]
tmp = path + ".seed-tmp"
open(tmp, "w", encoding="utf-8").write(s)
os.replace(tmp, path)
PY
   if grep -q 'GaiaDepthGrade-pxm\.so' "$GDG_SLOT_SETTINGS"; then
      echo "FAIL: seeded $GDG_SLOT_SETTINGS still names a GaiaDepthGrade-pxm.so"; return 1
   fi
}

# ---- GUI smoke (gui-smoke.sh) -----------------------------------------------
# A REAL PixInsight GUI on a private Xvfb display (:100 + slot), isolated slot
# GDG_GUI_SLOT (default 97: Planetary's GUI slot; the slot guard refuses a second
# PixInsight on it), driven with XTest by gui_drive.py. Adapted from
# modules/planetary/test/harness.sh (planetary_gui_*), whose header explains each rule.
#   gdg_gui_setup                 # slot, run dir, display
#   ... gdg_sign, gdg_seed_slot_modules, caller's exports ...
#   gdg_gui_launch OUT SETUP.js   # Xvfb + PixInsight -r=SETUP.js; ready; only the dev build mapped
#   ... gui_drive.py steps, -x IPC scripts ...
#   gdg_gui_quit OUT              # File > Quit; PixInsight must exit 0 within 60 s
gdg_gui_setup()
{
   local tool
   for tool in Xvfb xprop import; do
      command -v "$tool" >/dev/null || { echo "FAIL: $tool not found"; return 1; }
   done
   GDG_TEST_SLOT="${GDG_GUI_SLOT:-97}" gdg_slot_setup || return 1
   gdg_private_tmp || return 1
   GDG_DISPLAY_NO=$(( 100 + GDG_SLOT ))
   if [ -e "/tmp/.X11-unix/X$GDG_DISPLAY_NO" ]; then
      echo "FAIL: display :$GDG_DISPLAY_NO is already in use (another GUI smoke on slot $GDG_SLOT?)"; return 1
   fi
   GDG_GUI_XVFB_PID="" GDG_GUI_PI_PID=""
   gdg_on_exit gdg_gui_cleanup
}

gdg_gui_cleanup()
{
   [ -n "${GDG_GUI_PI_PID:-}" ] && kill -9 "$GDG_GUI_PI_PID" 2>/dev/null
   pkill -9 -f -- "^$GDG_PI_BIN_DIR/PixInsight -n=$GDG_SLOT " 2>/dev/null
   [ -n "${GDG_GUI_XVFB_PID:-}" ] && kill "$GDG_GUI_XVFB_PID" 2>/dev/null
   return 0
}

gdg_assert_only_dev_mapped()
{
   local so="$1" pid maps
   pid="$(pgrep -f -- "^$GDG_PI_BIN_DIR/PixInsight -n=$GDG_SLOT " | head -1)"
   [ -n "$pid" ] || { echo "FAIL: no running PixInsight for slot $GDG_SLOT to check"; return 1; }
   maps="$(grep -- 'GaiaDepthGrade-pxm\.so' "/proc/$pid/maps" | awk '{print $NF}' | sort -u | xargs -r -n1 realpath -m | sort -u)"
   if [ "$maps" != "$(realpath -e "$so")" ]; then
      echo "FAIL: PixInsight (slot $GDG_SLOT, pid $pid) maps [${maps//$'\n'/ }], expected ONLY the dev build $so"; return 1
   fi
   echo "only the dev module is mapped (pid $pid): $maps"
}

# Starts the Xvfb and PixInsight with the dev build $SO and the start-up script $2, which must
# write "$GDG_GUI_DIR/ready" holding launchInterface=true (copied to OUT/ready.txt either way). No desktop session variables and a
# session bus that leads nowhere (a Qt file dialog must not reach the desktop portal).
gdg_gui_launch()
{
   local out="$1" setup="$2" n=0
   Xvfb ":$GDG_DISPLAY_NO" -screen 0 1920x1080x24 -nolisten tcp >"$out/xvfb.log" 2>&1 & GDG_GUI_XVFB_PID=$!
   export DISPLAY=":$GDG_DISPLAY_NO"
   for _ in $(seq 50); do xprop -root >/dev/null 2>&1 && break; sleep 0.1; done

   pi_headless_env
   unset XDG_CURRENT_DESKTOP DESKTOP_SESSION KDE_FULL_SESSION KDE_SESSION_VERSION GNOME_DESKTOP_SESSION_ID \
         QT_QPA_PLATFORMTHEME XDG_SESSION_TYPE
   export DBUS_SESSION_BUS_ADDRESS="unix:path=$GDG_RUN_DIR/no-session-bus"
   export XDG_DATA_HOME="$GDG_RUN_DIR/xdg"; mkdir -m 700 "$XDG_DATA_HOME"
   export GDG_GUI_DIR="$GDG_RUN_DIR/gui"; mkdir -m 700 "$GDG_GUI_DIR"
   # The interface's state file (test builds only; gui_drive.py reads it), and a private cache so an
   # online Prepare never writes the user's ~/.cache.
   export GDG_GUI_STATE="$GDG_GUI_DIR/state.json"
   export XDG_CACHE_HOME="$GDG_RUN_DIR/xdg-cache"; mkdir -m 700 "$XDG_CACHE_HOME"

   pi_require_private_display || return 1
   timeout 1800 "$PI" -n="$GDG_SLOT" --no-startup-scripts -m="$SO" -r="$setup" >"$out/pi.log" 2>&1 & GDG_GUI_PI_PID=$!
   for _ in $(seq 360); do
      [ -f "$GDG_GUI_DIR/ready" ] && break
      kill -0 "$GDG_GUI_PI_PID" 2>/dev/null || break
      n=$(( n + 1 ))
      (( n % 10 == 0 )) && { python3 "$HERE/gui_drive.py" dismiss "$out" || { echo "FAIL: driver (dismiss)"; return 1; }; }
      sleep 0.5
   done
   if [ ! -f "$GDG_GUI_DIR/ready" ]; then
      import -window root "$out/not-ready.png" 2>/dev/null || true
      echo "FAIL: the GUI script never became ready (see $out/pi.log, $out/not-ready.png)"; return 1
   fi
   cp "$GDG_GUI_DIR/ready" "$out/ready.txt"
   import -window root "$out/00-ready.png" 2>/dev/null || true
   grep -q "launchInterface=true" "$out/ready.txt" || { echo "FAIL: setup: $(cat "$out/ready.txt")"; return 1; }
   gdg_assert_only_dev_mapped "$SO" || return 1
   sleep 1
}

gdg_gui_quit()
{
   local out="$1" rc=0
   python3 "$HERE/gui_drive.py" quit "$out" || { echo "FAIL: driver (quit)"; return 1; }
   for _ in $(seq 240); do kill -0 "$GDG_GUI_PI_PID" 2>/dev/null || break; sleep 0.25; done
   if kill -0 "$GDG_GUI_PI_PID" 2>/dev/null; then
      import -window root "$out/hang.png" 2>/dev/null || true
      echo "FAIL: PixInsight still running 60 s after Quit (see $out/hang.png)"; return 1
   fi
   wait "$GDG_GUI_PI_PID" || rc=$?
   GDG_GUI_PI_PID=""
   [ "$rc" = 0 ] || { echo "FAIL: PixInsight exit code $rc (see $out/pi.log)"; return 1; }
}
