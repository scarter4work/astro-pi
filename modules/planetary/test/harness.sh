# shellcheck shell=bash
# Planetary test harness: shared isolation helpers. SOURCE this (bash) from
# every script that launches PixInsight for a Planetary test (run-probe.sh,
# the GUI smoke). Adapted from modules/pi-copilot/test/harness-lib.sh and
# run-load.sh; see their headers for the measured root causes.
#
#   . "$HERE/harness.sh"
#   planetary_require_signing_pass || exit 2   # BEFORE anything touches a slot
#   planetary_slot_setup                       # PLANETARY_SLOT/_SETTINGS, wiped now and on exit
#   planetary_on_exit my_cleanup               # never `trap ... EXIT`: it would drop the helpers' cleanup
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

# EXIT cleanup registry. Helpers and callers register a function with
# planetary_on_exit instead of `trap ... EXIT`, which would replace every
# earlier handler. Each function runs once, newest first (a caller's own
# cleanup, e.g. killing PixInsight and its Xvfb, runs before the slot and run
# dir are wiped). An EXIT trap the caller set before the first registration
# is kept and runs last.
PLANETARY_EXIT_FNS=()
PLANETARY_EXIT_PREV=""
planetary_on_exit()
{
   local f
   for f in "${PLANETARY_EXIT_FNS[@]}"; do
      [ "$f" = "$1" ] && return 0
   done
   [ ${#PLANETARY_EXIT_FNS[@]} -eq 0 ] && PLANETARY_EXIT_PREV="$(trap -p EXIT)"
   PLANETARY_EXIT_FNS+=( "$1" )
   trap planetary_run_exit EXIT
}

planetary_run_exit()
{
   local i
   for (( i = ${#PLANETARY_EXIT_FNS[@]} - 1; i >= 0; --i )); do
      "${PLANETARY_EXIT_FNS[i]}" || true
   done
   if [ -n "$PLANETARY_EXIT_PREV" ]; then
      # `trap -p` prints: trap -- '<command>' EXIT
      eval "set -- $PLANETARY_EXIT_PREV"
      eval "$3"
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
   # A PixInsight already running on the slot (another test run, or a hung
   # one) owns its settings file: wiping it under that process corrupts both
   # runs. Checked BEFORE the cleanup is registered, so a refusal touches nothing.
   local running
   running="$(planetary_slot_processes "$PLANETARY_SLOT")"
   if [ -n "$running" ]; then
      echo "FAIL: a PixInsight is already running on slot $PLANETARY_SLOT; not touching its settings:"
      sed 's/^/  /' <<<"$running"
      return 1
   fi
   PLANETARY_SLOT_SETTINGS="$(printf '%s/core-%03d-pxi.settings' "$HOME/.PixInsight" "$PLANETARY_SLOT")"
   planetary_on_exit planetary_cleanup
   rm -f "$PLANETARY_SLOT_SETTINGS"
}

# "<pid> <command line>" of every process launching or running PixInsight on
# slot $1, one per line; empty when there is none. A process counts when one
# argument is PixInsight or PixInsight.sh (any directory: the binary, the
# launcher script under bash, or a wrapper such as timeout) and another is
# exactly -n=<slot>. Matched per argument, not on the joined command line, so a
# shell whose script text merely mentions "PixInsight.sh -n=96" is not a match.
planetary_slot_processes()
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

# Private scratch for this run (0700): fixtures, probe output, logs, and the
# TMPDIR PixInsight and pi_headless use. Removed on exit (planetary_cleanup).
planetary_private_tmp()
{
   PLANETARY_RUN_DIR="$(mktemp -d "${TMPDIR:-/tmp}/planetary-test.XXXXXX")" || return 1
   planetary_on_exit planetary_cleanup
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

# The running PixInsight of this slot maps exactly one Planetary-pxm.so: the
# dev build $1 (an installed release listed in the slot's settings would load
# next to the -m= build; planetary_seed_slot_modules prevents that).
planetary_assert_only_dev_mapped()
{
   local so="$1" pid maps
   pid="$(pgrep -f -- "^$PLANETARY_PI_BIN_DIR/PixInsight -n=$PLANETARY_SLOT " | head -1)"
   [ -n "$pid" ] || { echo "FAIL: no running PixInsight for slot $PLANETARY_SLOT to check"; return 1; }
   maps="$(grep -- 'Planetary-pxm\.so' "/proc/$pid/maps" | awk '{print $NF}' | sort -u | xargs -r -n1 realpath -m | sort -u)"
   if [ "$maps" != "$(realpath -e "$so")" ]; then
      echo "FAIL: PixInsight (slot $PLANETARY_SLOT, pid $pid) maps [${maps//$'\n'/ }], expected ONLY the dev build $so"; return 1
   fi
   echo "only the dev module is mapped (pid $pid): $maps"
}

# The video files (.ser, .avi, an AVI's .avi.txt sidecar, a Save as SER
# .partial) the slot's running PixInsight holds open (link targets of
# /proc/<pid>/fd), one per line, sorted. Fails if no such PixInsight runs.
planetary_open_video_files()
{
   local pid
   pid="$(pgrep -f -- "^$PLANETARY_PI_BIN_DIR/PixInsight -n=$PLANETARY_SLOT " | head -1)"
   [ -n "$pid" ] || { echo "FAIL: no running PixInsight for slot $PLANETARY_SLOT" >&2; return 1; }
   find "/proc/$pid/fd" -maxdepth 1 -type l -printf '%l\n' 2>/dev/null \
      | grep -E '\.(ser|avi|avi\.txt|partial)$' | sort -u || true
}

# ---- GUI smokes (gui-smoke.sh, gui-smoke-stack.sh) ------------------------
# A REAL PixInsight GUI on a private Xvfb display (:100 + slot), isolated slot
# PLANETARY_GUI_SLOT (default 97; 96 is the headless probes'), driven with
# XTest by gui_drive.py. Call order:
#   planetary_gui_setup                 # before fixtures: slot, run dir, display
#   ... fixtures, planetary_sign, planetary_seed_slot_modules, caller's exports ...
#   planetary_gui_launch OUT SETUP.js   # Xvfb + PixInsight -r=SETUP.js, ready, dev build mapped
#   ... gui_drive.py modes, -x IPC scripts (after pi_require_private_display) ...
#   planetary_gui_quit OUT              # File > Quit; PixInsight must exit 0 within 60 s
planetary_gui_setup()
{
   local tool
   for tool in Xvfb xprop import; do
      command -v "$tool" >/dev/null || { echo "FAIL: $tool not found"; return 1; }
   done
   PLANETARY_TEST_SLOT="${PLANETARY_GUI_SLOT:-97}" planetary_slot_setup || return 1
   planetary_private_tmp || return 1
   PLANETARY_DISPLAY_NO=$(( 100 + PLANETARY_SLOT ))
   if [ -e "/tmp/.X11-unix/X$PLANETARY_DISPLAY_NO" ]; then
      echo "FAIL: display :$PLANETARY_DISPLAY_NO is already in use (another GUI smoke on slot $PLANETARY_SLOT?)"; return 1
   fi
   PLANETARY_GUI_XVFB_PID="" PLANETARY_GUI_PI_PID=""
   planetary_on_exit planetary_gui_cleanup
}

planetary_gui_cleanup()
{
   [ -n "${PLANETARY_GUI_PI_PID:-}" ] && kill -9 "$PLANETARY_GUI_PI_PID" 2>/dev/null
   pkill -9 -f -- "^$PLANETARY_PI_BIN_DIR/PixInsight -n=$PLANETARY_SLOT " 2>/dev/null
   [ -n "${PLANETARY_GUI_XVFB_PID:-}" ] && kill "$PLANETARY_GUI_XVFB_PID" 2>/dev/null
   return 0
}

# Starts the Xvfb and PixInsight with the dev build $SO and the start-up script
# $2, which must write "$PLANETARY_GUI_DIR/ready" holding launchInterface=true.
# Display isolation beyond pi_headless_env: no desktop session variables and a
# session-bus address that leads nowhere, so Qt cannot hand a file dialog to
# the desktop portal (it would open on the user's real screen). A fresh slot
# shows start-up notices (e.g. "Risky Temporary Folders"): each is answered
# with Return until the script is ready.
planetary_gui_launch()
{
   local out="$1" setup="$2" n=0
   Xvfb ":$PLANETARY_DISPLAY_NO" -screen 0 1920x1080x24 -nolisten tcp >"$out/xvfb.log" 2>&1 & PLANETARY_GUI_XVFB_PID=$!
   export DISPLAY=":$PLANETARY_DISPLAY_NO"
   for _ in $(seq 50); do xprop -root >/dev/null 2>&1 && break; sleep 0.1; done

   pi_headless_env
   unset XDG_CURRENT_DESKTOP DESKTOP_SESSION KDE_FULL_SESSION KDE_SESSION_VERSION GNOME_DESKTOP_SESSION_ID \
         QT_QPA_PLATFORMTHEME XDG_SESSION_TYPE
   export DBUS_SESSION_BUS_ADDRESS="unix:path=$PLANETARY_RUN_DIR/no-session-bus"
   export XDG_DATA_HOME="$PLANETARY_RUN_DIR/xdg"; mkdir -m 700 "$XDG_DATA_HOME"
   export PLANETARY_GUI_DIR="$PLANETARY_RUN_DIR/gui"; mkdir -m 700 "$PLANETARY_GUI_DIR"

   pi_require_private_display || return 1   # this run's own Xvfb, never the desktop
   timeout 900 "$PI" -n="$PLANETARY_SLOT" --no-startup-scripts -m="$SO" -r="$setup" >"$out/pi.log" 2>&1 & PLANETARY_GUI_PI_PID=$!
   for _ in $(seq 360); do
      [ -f "$PLANETARY_GUI_DIR/ready" ] && break
      kill -0 "$PLANETARY_GUI_PI_PID" 2>/dev/null || break
      n=$(( n + 1 ))
      (( n % 10 == 0 )) && { python3 "$HERE/gui_drive.py" --dismiss "$out" || { echo "FAIL: driver (dismiss)"; return 1; }; }
      sleep 0.5
   done
   if [ ! -f "$PLANETARY_GUI_DIR/ready" ]; then
      import -window root "$out/not-ready.png" 2>/dev/null || true
      echo "FAIL: the GUI script never became ready (see $out/pi.log, $out/not-ready.png)"; return 1
   fi
   cp "$PLANETARY_GUI_DIR/ready" "$out/ready.txt"
   grep -q "launchInterface=true" "$out/ready.txt" || { echo "FAIL: setup: $(cat "$out/ready.txt")"; return 1; }
   planetary_assert_only_dev_mapped "$SO" || return 1
   sleep 1
}

# File > Quit PixInsight (gui_drive.py --quit); PixInsight must exit, with
# status 0, within 60 s.
planetary_gui_quit()
{
   local out="$1" rc=0
   python3 "$HERE/gui_drive.py" --quit "$out" || { echo "FAIL: driver (quit)"; return 1; }
   for _ in $(seq 240); do kill -0 "$PLANETARY_GUI_PI_PID" 2>/dev/null || break; sleep 0.25; done
   if kill -0 "$PLANETARY_GUI_PI_PID" 2>/dev/null; then
      import -window root "$out/hang.png" 2>/dev/null || true
      echo "FAIL: PixInsight still running 60 s after Quit (see $out/hang.png)"; return 1
   fi
   wait "$PLANETARY_GUI_PI_PID" || rc=$?
   PLANETARY_GUI_PI_PID=""
   [ "$rc" = 0 ] || { echo "FAIL: PixInsight exit code $rc (see $out/pi.log)"; return 1; }
}
