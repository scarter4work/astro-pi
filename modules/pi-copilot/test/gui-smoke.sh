#!/usr/bin/env bash
# PI Copilot GUI smoke (NOT part of the headless self-test; the dialogs cannot
# run under --automation-mode). Starts a private Xvfb display and a REAL
# PixInsight GUI on an isolated test slot, with the same isolation as
# run-selftest.sh (private XDG_DATA_HOME for the journey library, the slot's
# settings wiped before and after, a private TMPDIR).
#
#   PICOPILOT_TEST_SLOT=95 bash test/gui-smoke.sh <out-dir>
#
# 1. gui-smoke-setup.js (-r): two masters with History steps, the panel shown.
# 2. gui_drive.py (XTest): clicks the journey strip (steps dialog), the gear
#    (settings with the journey fields) and Keep journey (confirm, answered
#    No), screenshotting each state into <out-dir>.
# 3. gui-smoke-keep.js (-x IPC): a keeper write-up against a loopback that
#    accepts and never answers; the images are closed.
# 4. gui_drive.py --quit: File > Quit PixInsight with that write-up in flight.
# PASS needs: PixInsight exits 0 within 60 s of Quit (a hang is a FAIL:
# JourneyService::Stop() must cancel the request), the loopback saw the
# request, and the real journey library is untouched. A crash shows as a
# non-zero exit code (the backtrace is in <out-dir>/pi.log).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/harness-lib.sh"   # data isolation + slot module seeding (see its header)
PI="$PICOPILOT_PI"
KEYS=/home/scarter4work/projects/keys/scarter4work_keys.xssk
SO="$(realpath -e "$ROOT/build/src/module/PICopilot-pxm.so")"
OUT="${1:?usage: gui-smoke.sh <out-dir>}"
mkdir -p "$OUT"
OUT="$(realpath -e "$OUT")"
SLOT="${PICOPILOT_TEST_SLOT:-95}"
case "$SLOT" in ''|*[!0-9]*) echo "FAIL: bad slot '$SLOT'"; exit 1 ;; esac
SLOT=$(( 10#$SLOT ))
if (( SLOT < 50 || SLOT > 256 )); then echo "FAIL: slot must be in [50,256] (reserved for tests)"; exit 1; fi
DISPLAY_NO=$(( 100 + SLOT ))

PRIV="/tmp/picopilot-$(id -u)"
mkdir -m 700 "$PRIV" 2>/dev/null || true
[ "$(stat -c '%u %a' "$PRIV")" = "$(id -u) 700" ] || { echo "FAIL: $PRIV is not a private 0700 dir"; exit 1; }
TMPDIR="$(mktemp -d "$PRIV/gui.XXXXXX")"; export TMPDIR
SLOT_SETTINGS="$(printf '%s/core-%03d-pxi.settings' "$HOME/.PixInsight" "$SLOT")"
rm -f "$SLOT_SETTINGS"
picopilot_isolate_data "$TMPDIR/xdg" || exit 1
picopilot_isolate_display
JOURNEYS_BEFORE="$(picopilot_journeys_fingerprint)"
export PICOPILOT_GUI_DIR="$TMPDIR/gui"; mkdir -m 700 "$PICOPILOT_GUI_DIR"
export PICOPILOT_SELFTEST_PHASE="$TMPDIR/phase.json"
export PICOPILOT_SELFTEST_OUT="$TMPDIR/unused-verdict.json"   # enables the phase handlers only
XVFB_PID= STALL_PID= PI_PID=
cleanup()
{
   [ -n "$PI_PID" ] && kill -9 "$PI_PID" 2>/dev/null || true
   [ -n "$STALL_PID" ] && kill "$STALL_PID" 2>/dev/null || true
   [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null || true
   rm -f "$SLOT_SETTINGS"
   rm -rf "$TMPDIR"
}
trap cleanup EXIT

picopilot_require_isolation || exit 1
pi_headless "$PI" --sign-module-file="$SO" --xssk-file="$KEYS" --xssk-password="$(cat /tmp/.pi_codesign_pass)" >/dev/null

Xvfb ":$DISPLAY_NO" -screen 0 1920x1080x24 -nolisten tcp >"$OUT/xvfb.log" 2>&1 & XVFB_PID=$!
export DISPLAY=":$DISPLAY_NO"
for _ in $(seq 50); do xprop -root >/dev/null 2>&1 && break; sleep 0.1; done

# The write-up target: accepts, reads the request, never answers; logs accept / close times.
python3 - "$PICOPILOT_GUI_DIR/stall-url" "$OUT/stall.log" <<'PY' & STALL_PID=$!
import socket, sys, threading, time
s = socket.socket(); s.bind(("127.0.0.1", 0)); s.listen(4)
open(sys.argv[1], "w").write("http://127.0.0.1:%d/v1/messages" % s.getsockname()[1])
log = open(sys.argv[2], "a", buffering=1)
def serve(c):
    log.write("%.3f accept\n" % time.time())
    n = 0
    while True:
        try: d = c.recv(65536)
        except OSError: break
        if not d: break
        n += len(d)
    log.write("%.3f closed-by-client after %d bytes\n" % (time.time(), n))
while True:
    c, _ = s.accept(); threading.Thread(target=serve, args=(c,), daemon=True).start()
PY

# The slot's settings: every installed module EXCEPT the installed PICopilot-pxm.so.
picopilot_seed_slot_modules "$SLOT" "$TMPDIR" || exit 1
START=$(date +%s.%N)
picopilot_require_isolation || exit 1
pi_require_private_display || exit 1   # this harness's own Xvfb, never the desktop
timeout 900 "$PI" -n="$SLOT" --no-startup-scripts -m="$SO" -r="$HERE/gui-smoke-setup.js" >"$OUT/pi.log" 2>&1 & PI_PID=$!
# A fresh slot shows start-up notices (e.g. "a system temporary folder is used
# for swap files"): each is answered with Return until the script is ready.
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
   echo "FAIL: the GUI script never became ready (see $OUT/pi.log, $OUT/not-ready.png)"; exit 1
fi

cp "$PICOPILOT_GUI_DIR/ready" "$OUT/ready.txt"
cp "$PICOPILOT_GUI_DIR/panel.json" "$OUT/panel.json" 2>/dev/null || true
# The DEV module must be the one running (review m7), and the only PICopilot-pxm.so mapped: a settings-less
# slot installs every bin/*-pxm.so, the installed PICopilot included (prevented by picopilot_seed_slot_modules).
LOADED="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("module",""))' "$OUT/panel.json" 2>/dev/null || true)"
if [ "$(realpath -e "$LOADED" 2>/dev/null || true)" != "$SO" ]; then
   echo "FAIL: the dev module did not load: running '${LOADED:-no PICopilot phase handler answered}', expected $SO."
   echo "      An installed PICopilot-pxm.so in the slot's module list collides with -m= (see $OUT/pi.log, $TMPDIR/seed-bootstrap.log)."
   exit 1
fi
echo "dev module loaded: $LOADED"
picopilot_assert_only_dev_mapped "$SLOT" "$SO" || exit 1
python3 "$HERE/gui_drive.py" ${PICOPILOT_GUI_DRIVE_ARGS:-} "$OUT" || { echo "FAIL: driver"; exit 1; }
picopilot_require_isolation || exit 1
pi_require_private_display || exit 1
"$PI" -x="$SLOT:$HERE/gui-smoke-keep.js" >/dev/null 2>&1
for _ in $(seq 240); do [ -f "$PICOPILOT_GUI_DIR/keep-end" ] && break; kill -0 "$PI_PID" 2>/dev/null || break; sleep 0.25; done
cp "$PICOPILOT_GUI_DIR/keep-end" "$OUT/keep-end.txt" 2>/dev/null || true
grep -q "keep started" "$OUT/keep-end.txt" 2>/dev/null || { echo "FAIL: the keep script failed: $(cat "$OUT/keep-end.txt" 2>/dev/null)"; exit 1; }
sleep 1
python3 "$HERE/gui_drive.py" --quit "$OUT" || { echo "FAIL: driver (quit)"; exit 1; }
END_SCRIPT=$(date +%s.%N)
rc=0
for _ in $(seq 240); do kill -0 "$PI_PID" 2>/dev/null || break; sleep 0.25; done
if kill -0 "$PI_PID" 2>/dev/null; then
   echo "FAIL: PixInsight still running 60 s after Quit (hang at shutdown)"
   import -display "$DISPLAY" -window root "$OUT/hang.png" 2>/dev/null || true
   exit 1
fi
wait "$PI_PID" || rc=$?
PI_PID=
EXIT_AT=$(date +%s.%N)
cp "$PICOPILOT_GUI_DIR/keep.json" "$OUT/keep.json" 2>/dev/null || true
printf 'pi exit code %s; shutdown took %.1f s after Quit (total %.1f s)\n' \
   "$rc" "$(echo "$EXIT_AT - $END_SCRIPT" | bc)" "$(echo "$EXIT_AT - $START" | bc)" | tee "$OUT/result.txt"
picopilot_journeys_check "$JOURNEYS_BEFORE" || exit 1
ok=1
[ "$rc" = 0 ] || { echo "FAIL: PixInsight exit code $rc"; ok=0; }
grep -q '"writeupStarted":true' "$OUT/keep.json" 2>/dev/null || { echo "FAIL: no write-up was in flight (keep.json)"; ok=0; }
grep -q accept "$OUT/stall.log" 2>/dev/null || { echo "FAIL: the loopback never saw the write-up request"; ok=0; }
[ "$ok" = 1 ] && echo "PASS: GUI smoke (shutdown with a keeper write-up in flight: clean exit)"
[ "$ok" = 1 ]
