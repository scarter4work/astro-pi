#!/usr/bin/env bash
# PI Copilot: File > Quit handled INSIDE a journey tick (NOT part of the headless
# self-test; it needs the real GUI). Same isolation as gui-smoke.sh: a private
# Xvfb, an isolated test slot whose settings are wiped before and after, a
# private XDG_DATA_HOME and TMPDIR.
#
#   PICOPILOT_TEST_SLOT=94 bash test/quit-mid-tick.sh <out-dir>
#   PICOPILOT_SO=<other build>/PICopilot-pxm.so ... (e.g. build-portable/)
#
# The bug (0.2.0.0-0.2.2.0): the journey tick's busy gate calls
# Console::AbortEnabled(), which pumps events. A Quit (click or Ctrl+Q) queued behind the
# journey timer is handled in that pump, and PixInsight calls every module's
# OnUnload from the main window's closeEvent -- inside JourneyTracker::Tick.
# JourneyService::Stop() freed the tracker under the running tick, which then
# wrote to freed memory: "free(): invalid pointer" / SIGSEGV, exit 134, about
# 5 s after Quit (first seen in Planetary's GUI smoke, 2026-10-23).
#
# 1. quit-mid-tick-setup.js (-r): four masters with History steps, recorded.
# 2. gdb attaches (quit-mid-tick-gdb.py): it records whether each Stop() ran
#    with the tick on the stack.
# 3. quit-mid-tick-busy.js (-x) closes the images and keeps the main thread
#    busy for 3 s without pumping; Ctrl+Q (File > Quit PixInsight) is pressed
#    meanwhile, so the key event waits behind the journey timer.
# PASS needs: Stop() ran with JourneyTracker::Tick (or OnTick) on the stack --
# without that the run did not exercise the bug and FAILS as inconclusive --
# the tracker was destroyed, but never with its Tick() on the stack (this fails
# 0.2.2.0 even in a run where the freed memory happens not to crash), PixInsight
# exits 0 within 60 s of Quit, and the real journey library is untouched. No
# Planetary module is loaded (it is dropped from the slot).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/harness-lib.sh"   # data isolation + slot module seeding (see its header)
PI="$PICOPILOT_PI"
KEYS=/home/scarter4work/projects/keys/scarter4work_keys.xssk
SO="$(realpath -e "${PICOPILOT_SO:-$ROOT/build/src/module/PICopilot-pxm.so}")"
OUT="${1:?usage: quit-mid-tick.sh <out-dir>}"
mkdir -p "$OUT"
OUT="$(realpath -e "$OUT")"
SLOT="${PICOPILOT_TEST_SLOT:-94}"
case "$SLOT" in ''|*[!0-9]*) echo "FAIL: bad slot '$SLOT'"; exit 1 ;; esac
SLOT=$(( 10#$SLOT ))
if (( SLOT < 50 || SLOT > 256 )); then echo "FAIL: slot must be in [50,256] (reserved for tests)"; exit 1; fi
DISPLAY_NO=$(( 100 + SLOT ))
command -v gdb >/dev/null || { echo "FAIL: gdb is needed to prove the Quit ran inside a tick"; exit 1; }
if [ "$(cat /proc/sys/kernel/yama/ptrace_scope 2>/dev/null || echo 0)" != 0 ]; then
   echo "FAIL: kernel.yama.ptrace_scope must be 0 for gdb to attach (it proves the Quit ran inside a tick)"; exit 1
fi

PRIV="/tmp/picopilot-$(id -u)"
mkdir -m 700 "$PRIV" 2>/dev/null || true
[ "$(stat -c '%u %a' "$PRIV")" = "$(id -u) 700" ] || { echo "FAIL: $PRIV is not a private 0700 dir"; exit 1; }
TMPDIR="$(mktemp -d "$PRIV/quit.XXXXXX")"; export TMPDIR
SLOT_SETTINGS="$(printf '%s/core-%03d-pxi.settings' "$HOME/.PixInsight" "$SLOT")"
rm -f "$SLOT_SETTINGS"
picopilot_isolate_data "$TMPDIR/xdg" || exit 1
picopilot_isolate_display
JOURNEYS_BEFORE="$(picopilot_journeys_fingerprint)"
export PICOPILOT_GUI_DIR="$TMPDIR/gui"; mkdir -m 700 "$PICOPILOT_GUI_DIR"
export QUIT_MID_TICK_STOP_LOG="$OUT/stop.log"
rm -f "$QUIT_MID_TICK_STOP_LOG"
XVFB_PID= PI_PID= GDB_PID=
cleanup()
{
   [ -n "$GDB_PID" ] && kill "$GDB_PID" 2>/dev/null || true
   [ -n "$PI_PID" ] && kill -9 "$PI_PID" 2>/dev/null || true
   [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null || true
   rm -f "$SLOT_SETTINGS"
   rm -rf "$TMPDIR"
}
trap cleanup EXIT

# Sign a copy that is not ours to re-sign (e.g. an installed module) only if its .xsgn is missing.
if [ ! -f "${SO%.so}.xsgn" ] || [ "${SO%.so}.xsgn" -ot "$SO" ]; then
   case "$SO" in
      /opt/*) echo "FAIL: $SO has no current .xsgn and is never signed in place; copy it out first"; exit 1 ;;
   esac
   picopilot_require_isolation || exit 1
   pi_headless "$PI" --sign-module-file="$SO" --xssk-file="$KEYS" --xssk-password="$(cat /tmp/.pi_codesign_pass)" >/dev/null
fi
[ -f "${SO%.so}.xsgn" ] || { echo "FAIL: no .xsgn next to $SO"; exit 1; }

Xvfb ":$DISPLAY_NO" -screen 0 1920x1080x24 -nolisten tcp >"$OUT/xvfb.log" 2>&1 & XVFB_PID=$!
export DISPLAY=":$DISPLAY_NO"
for _ in $(seq 50); do xprop -root >/dev/null 2>&1 && break; sleep 0.1; done

# The slot's settings: every installed module except the installed PICopilot-pxm.so (harness-lib.sh) and
# Planetary-pxm.so: this test proves the crash needs PI Copilot alone.
picopilot_seed_slot_modules "$SLOT" "$TMPDIR" || exit 1
python3 - "$SLOT_SETTINGS" <<'PY' || { echo "FAIL: could not drop Planetary from $SLOT_SETTINGS"; exit 1; }
import re, sys
p = sys.argv[1]
s = open(p, encoding="utf-8").read()
s = re.sub(r'<v k="\d+" t="s">@pxi_bin_dir/Planetary-pxm\.so</v>', "", s)
open(p, "w", encoding="utf-8").write(s)
PY
if grep -q 'Planetary-pxm\.so' "$SLOT_SETTINGS"; then echo "FAIL: the slot still loads Planetary-pxm.so"; exit 1; fi

picopilot_require_isolation || exit 1
pi_require_private_display || exit 1   # this harness's own Xvfb, never the desktop
timeout 600 "$PI" -n="$SLOT" --no-startup-scripts -m="$SO" -r="$HERE/quit-mid-tick-setup.js" >"$OUT/pi.log" 2>&1 & PI_PID=$!
n=0
for _ in $(seq "${PICOPILOT_GUI_READY_HALF_SECONDS:-360}"); do
   [ -f "$PICOPILOT_GUI_DIR/ready" ] && break
   kill -0 "$PI_PID" 2>/dev/null || break
   n=$(( n + 1 ))
   (( n % 10 == 0 )) && python3 "$HERE/gui_drive.py" --dismiss "$OUT"
   sleep 0.5
done
if [ ! -f "$PICOPILOT_GUI_DIR/ready" ]; then
   import -window root "$OUT/not-ready.png" 2>/dev/null || true
   echo "FAIL: the setup script never became ready (see $OUT/pi.log, $OUT/not-ready.png)"; exit 1
fi
cp "$PICOPILOT_GUI_DIR/ready" "$OUT/ready.txt"
grep -q ERROR "$OUT/ready.txt" && { echo "FAIL: setup: $(cat "$OUT/ready.txt")"; exit 1; }
picopilot_assert_only_dev_mapped "$SLOT" "$SO" || exit 1
BIN_PID="$(pgrep -f -- "^$PICOPILOT_PI_BIN_DIR/PixInsight -n=$SLOT " | head -1)"
if grep -q 'Planetary-pxm\.so' "/proc/$BIN_PID/maps"; then echo "FAIL: Planetary-pxm.so is mapped"; exit 1; fi
grep -- '-pxm\.so' "/proc/$BIN_PID/maps" | awk '{print $NF}' | sort -u > "$OUT/modules-mapped.txt"
echo "PixInsight pid $BIN_PID: $(wc -l < "$OUT/modules-mapped.txt") modules mapped, no Planetary"

gdb -q -p "$BIN_PID" -batch -ex 'set debuginfod enabled off' -ex 'set pagination off' \
    -ex "source $HERE/quit-mid-tick-gdb.py" -ex 'continue' >"$OUT/gdb.log" 2>&1 & GDB_PID=$!
for _ in $(seq 120); do grep -q armed "$QUIT_MID_TICK_STOP_LOG" 2>/dev/null && break; sleep 0.5; done
grep -q armed "$QUIT_MID_TICK_STOP_LOG" 2>/dev/null || { echo "FAIL: gdb did not attach (see $OUT/gdb.log)"; exit 1; }
sleep 2

picopilot_require_isolation || exit 1
pi_require_private_display || exit 1
"$PI" -x="$SLOT:$HERE/quit-mid-tick-busy.js" >/dev/null 2>&1
sleep 1.0
# File > Quit PixInsight by its shortcut (no screen coordinates), queued while the script runs.
python3 "$HERE/gui_drive.py" --key ctrl+q "$OUT"
QUIT_AT=$(date +%s.%N)
rc=0
for _ in $(seq 240); do kill -0 "$PI_PID" 2>/dev/null || break; sleep 0.25; done
if kill -0 "$PI_PID" 2>/dev/null; then
   echo "FAIL: PixInsight still running 60 s after Quit (hang at shutdown)"
   import -display "$DISPLAY" -window root "$OUT/hang.png" 2>/dev/null || true
   exit 1
fi
wait "$PI_PID" || rc=$?
PI_PID=
for _ in $(seq 20); do kill -0 "$GDB_PID" 2>/dev/null || break; sleep 0.25; done
printf 'pi exit code %s; %.1f s after Quit\n' "$rc" "$(echo "$(date +%s.%N) - $QUIT_AT" | bc)" | tee "$OUT/result.txt"
grep -m1 -E 'free\(\)|corrupted|double free|Received signal' "$OUT/pi.log" | tee -a "$OUT/result.txt" || true
grep -E '^(stop|tracker-destroyed) ' "$QUIT_MID_TICK_STOP_LOG" | tee -a "$OUT/result.txt" || true
picopilot_journeys_check "$JOURNEYS_BEFORE" || exit 1
ok=1
if ! grep -q '^stop tick-on-stack=1' "$QUIT_MID_TICK_STOP_LOG"; then
   echo "FAIL (inconclusive): no JourneyService::Stop() ran inside a journey tick, so this run did not exercise the bug"
   ok=0
fi
if grep -q '^tracker-destroyed tick-on-stack=1' "$QUIT_MID_TICK_STOP_LOG"; then
   echo "FAIL: the journey tracker was destroyed while its Tick() was running (use after free; see $QUIT_MID_TICK_STOP_LOG)"
   ok=0
elif ! grep -q '^tracker-destroyed ' "$QUIT_MID_TICK_STOP_LOG"; then
   echo "FAIL: the journey tracker was never destroyed (the deferred teardown did not run)"
   ok=0
fi
[ "$rc" = 0 ] || { echo "FAIL: PixInsight exit code $rc (backtrace in $OUT/pi.log)"; ok=0; }
[ "$ok" = 1 ] && echo "PASS: Quit handled inside a journey tick: clean exit"
[ "$ok" = 1 ]
