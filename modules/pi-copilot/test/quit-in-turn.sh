#!/usr/bin/env bash
# PI Copilot: File > Quit handled inside the panel's own work (NOT part of the headless self-test; it needs
# the real GUI). Same isolation as gui-smoke.sh / quit-mid-tick.sh: a private Xvfb, an isolated test slot
# whose settings are wiped before and after, a private XDG_DATA_HOME and TMPDIR. No request leaves the
# machine: the chat turn talks to a loopback stub with a dummy key, and the keyring is never read.
#
#   PICOPILOT_TEST_SLOT=93 bash test/quit-in-turn.sh tool|keep|keepmodal <out-dir>
#   PICOPILOT_SO=<other build>/PICopilot-pxm.so ... (e.g. build-portable/)
#
# Where a Quit can land: PixInsight blocks the menus and shortcuts while a process runs (measured: Ctrl+Q,
# Alt+F and File-menu clicks during a 10 s PixelMath started by apply_process did nothing), and the panel's
# confirm boxes are modal (keepmodal measures ★ Keep's). So a Quit inside the panel's work needs an event pump
# that takes user input. The keep confirm is replaced by such a pump (gui.keepArm: 6 s of
# ProcessEvents( false ), then Yes) -- the stand-in for "something in the turn pumps events".
# tool:      a chat turn (gui.turn) whose first stub reply asks for mark_journey_best: the keep flow runs
#            inside e_Poll_Timer's tool loop, and Ctrl+Q lands in its pump. OnUnload runs inside the tool
#            loop (its JourneyService::Hold keeps the journey objects); the keep must then not happen, and
#            the turn must not send the tool result back (no model request once unload has begun).
# keep:      ★ Keep with the same stand-in: OnUnload runs inside KeepJourneyOfView; nothing may be kept.
# keepmodal: ★ Keep with the REAL confirm box: Ctrl+Q while it is up must not reach PixInsight (no Stop);
#            then Esc (No) and Ctrl+Q quit normally.
# In tool and keep the journey objects are destroyed when the panel code returns: JourneyService::Teardown
# with the timer host destroyed, after module unload (the path quit-mid-tick.sh does not cover).
#
# PixInsight's own "Exit anyway?" box at Quit (unsaved images) is answered Yes by the driver (Tab, Space).
# PASS needs (tool/keep): Stop() ran inside the tool loop / the keep flow -- proven by gdb
# (quit-in-turn-gdb.py), else the run FAILS as inconclusive --, the journey teardown ran, no model request
# (tool) and no keep after Stop(), the interface was not destroyed under the running turn, PixInsight exits
# 0 within 60 s, and the real journey library is untouched. No Planetary module is loaded.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/harness-lib.sh"
PI="$PICOPILOT_PI"
KEYS=/home/scarter4work/projects/keys/scarter4work_keys.xssk
MODE="${1:?usage: quit-in-turn.sh tool|keep|keepmodal <out-dir>}"
case "$MODE" in tool|keep|keepmodal) ;; *) echo "FAIL: mode must be tool, keep or keepmodal"; exit 1 ;; esac
SO="$(realpath -e "${PICOPILOT_SO:-$ROOT/build/src/module/PICopilot-pxm.so}")"
picopilot_require_selftest_build "$SO" || exit 1
OUT="${2:?usage: quit-in-turn.sh tool|keep|keepmodal <out-dir>}"
mkdir -p "$OUT"
OUT="$(realpath -e "$OUT")"
SLOT="${PICOPILOT_TEST_SLOT:-93}"
case "$SLOT" in ''|*[!0-9]*) echo "FAIL: bad slot '$SLOT'"; exit 1 ;; esac
SLOT=$(( 10#$SLOT ))
if (( SLOT < 50 || SLOT > 256 )); then echo "FAIL: slot must be in [50,256] (reserved for tests)"; exit 1; fi
DISPLAY_NO=$(( 100 + SLOT ))
command -v gdb >/dev/null || { echo "FAIL: gdb is needed to prove where the Quit ran"; exit 1; }
if [ "$(cat /proc/sys/kernel/yama/ptrace_scope 2>/dev/null || echo 0)" != 0 ]; then
   echo "FAIL: kernel.yama.ptrace_scope must be 0 for gdb to attach"; exit 1
fi

PRIV="/tmp/picopilot-$(id -u)"
mkdir -m 700 "$PRIV" 2>/dev/null || true
[ "$(stat -c '%u %a' "$PRIV")" = "$(id -u) 700" ] || { echo "FAIL: $PRIV is not a private 0700 dir"; exit 1; }
TMPDIR="$(mktemp -d "$PRIV/turn.XXXXXX")"; export TMPDIR
SLOT_SETTINGS="$(printf '%s/core-%03d-pxi.settings' "$HOME/.PixInsight" "$SLOT")"
rm -f "$SLOT_SETTINGS"
picopilot_isolate_data "$TMPDIR/xdg" || exit 1
picopilot_isolate_display
JOURNEYS_BEFORE="$(picopilot_journeys_fingerprint)"
export PICOPILOT_GUI_DIR="$TMPDIR/gui"; mkdir -m 700 "$PICOPILOT_GUI_DIR"
export PICOPILOT_SELFTEST_PHASE="$TMPDIR/phase.json"
export PICOPILOT_SELFTEST_OUT="$TMPDIR/unused-verdict.json"   # enables the phase handlers only
export QUIT_IN_TURN_LOG="$OUT/events.log"
rm -f "$QUIT_IN_TURN_LOG" "$OUT/stub.log"
XVFB_PID= PI_PID= GDB_PID= STUB_PID=
cleanup()
{
   [ -n "$GDB_PID" ] && kill "$GDB_PID" 2>/dev/null || true
   [ -n "$PI_PID" ] && kill -9 "$PI_PID" 2>/dev/null || true
   [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null || true
   [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null || true
   rm -f "$SLOT_SETTINGS"
   rm -rf "$TMPDIR"
}
trap cleanup EXIT

if [ ! -f "${SO%.so}.xsgn" ] || [ "${SO%.so}.xsgn" -ot "$SO" ]; then
   case "$SO" in
      /opt/*) echo "FAIL: $SO has no current .xsgn and is never signed in place; copy it out first"; exit 1 ;;
   esac
   picopilot_require_isolation || exit 1
   pi_headless "$PI" --sign-module-file="$SO" --xssk-file="$KEYS" --xssk-password="$(cat /tmp/.pi_codesign_pass)" >/dev/null
fi
[ -f "${SO%.so}.xsgn" ] || { echo "FAIL: no .xsgn next to $SO"; exit 1; }

# The Messages API stub (loopback, streamed replies): a turn's first request is answered with one
# mark_journey_best tool_use (on the turn view); a request carrying a tool_result with end_turn
# text. Every request is logged with its time.
python3 - "$PICOPILOT_GUI_DIR/stub-url" "$OUT/stub.log" <<'PY' & STUB_PID=$!
import json, sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
url_file, log_file = sys.argv[1], sys.argv[2]
n = [0]
class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("content-length", "0"))) or b"{}")
        n[0] += 1
        last = (body.get("messages") or [{}])[-1].get("content")
        has_result = isinstance(last, list) and any(b.get("type") == "tool_result" for b in last)
        with open(log_file, "a") as f:
            f.write("%.3f request %d tool_result=%d\n" % (time.time(), n[0], has_result))
        self.send_response(200)
        self.send_header("content-type", "text/event-stream")
        self.send_header("connection", "close")
        self.end_headers()
        self.close_connection = True
        msg = {"type": "message_start", "message": {"id": "msg_stub_%d" % n[0], "type": "message", "role": "assistant",
               "model": body.get("model", "stub"), "content": [], "stop_reason": None,
               "usage": {"input_tokens": 1, "output_tokens": 1}}}
        if has_result:
            blocks = [("text", {"type": "text", "text": ""}, {"type": "text_delta", "text": "Done."})]
            stop = "end_turn"
        else:
            inp = json.dumps({})   # mark_journey_best on the turn view (pcTurnM): the keep flow, inside the tool loop
            blocks = [("tool_use", {"type": "tool_use", "id": "toolu_stub_1", "name": "mark_journey_best", "input": {}},
                       {"type": "input_json_delta", "partial_json": inp})]
            stop = "tool_use"
        ev = [("message_start", msg)]
        for i, (_, start, delta) in enumerate(blocks):
            ev += [("content_block_start", {"type": "content_block_start", "index": i, "content_block": start}),
                   ("content_block_delta", {"type": "content_block_delta", "index": i, "delta": delta}),
                   ("content_block_stop", {"type": "content_block_stop", "index": i})]
        ev += [("message_delta", {"type": "message_delta", "delta": {"stop_reason": stop, "stop_sequence": None},
                                  "usage": {"output_tokens": 10}}),
               ("message_stop", {"type": "message_stop"})]
        for name, data in ev:
            self.wfile.write(("event: %s\ndata: %s\n\n" % (name, json.dumps(data))).encode("utf-8"))
            self.wfile.flush()
s = ThreadingHTTPServer(("127.0.0.1", 0), H)
open(url_file, "w").write("http://127.0.0.1:%d/v1/messages" % s.server_address[1])
s.serve_forever()
PY
for _ in $(seq 50); do [ -s "$PICOPILOT_GUI_DIR/stub-url" ] && break; sleep 0.1; done
[ -s "$PICOPILOT_GUI_DIR/stub-url" ] || { echo "FAIL: the stub did not start"; exit 1; }

Xvfb ":$DISPLAY_NO" -screen 0 1920x1080x24 -nolisten tcp >"$OUT/xvfb.log" 2>&1 & XVFB_PID=$!
export DISPLAY=":$DISPLAY_NO"
for _ in $(seq 50); do xprop -root >/dev/null 2>&1 && break; sleep 0.1; done

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
pi_require_private_display || exit 1
timeout 600 "$PI" -n="$SLOT" --no-startup-scripts -m="$SO" -r="$HERE/quit-in-turn-setup.js" >"$OUT/pi.log" 2>&1 & PI_PID=$!
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

gdb -q -p "$BIN_PID" -batch -ex 'set debuginfod enabled off' -ex 'set pagination off' \
    -ex "source $HERE/quit-in-turn-gdb.py" -ex 'continue' >"$OUT/gdb.log" 2>&1 & GDB_PID=$!
for _ in $(seq 120); do grep -q armed "$QUIT_IN_TURN_LOG" 2>/dev/null && break; sleep 0.5; done
grep -q armed "$QUIT_IN_TURN_LOG" 2>/dev/null || { echo "FAIL: gdb did not attach (see $OUT/gdb.log)"; exit 1; }
sleep 1

echo "$MODE" > "$PICOPILOT_GUI_DIR/mode"
picopilot_require_isolation || exit 1
pi_require_private_display || exit 1
"$PI" -x="$SLOT:$HERE/quit-in-turn-start.js" >/dev/null 2>&1
for _ in $(seq 80); do [ -f "$PICOPILOT_GUI_DIR/start-end" ] && break; sleep 0.25; done
cp "$PICOPILOT_GUI_DIR/start-end" "$OUT/start-end.txt" 2>/dev/null || true
grep -q "^armed $MODE" "$OUT/start-end.txt" 2>/dev/null || { echo "FAIL: start script: $(cat "$OUT/start-end.txt" 2>/dev/null)"; exit 1; }
quit()   # File > Quit PixInsight by its shortcut (no screen coordinates)
{
   python3 "$HERE/gui_drive.py" --key ctrl+q "$OUT"
}
if [ "$MODE" = tool ]; then
   cp "$PICOPILOT_GUI_DIR/turn.json" "$OUT/turn.json" 2>/dev/null || true
   for _ in $(seq 120); do grep -q "request 1 " "$OUT/stub.log" 2>/dev/null && break; sleep 0.25; done
   grep -q "request 1 " "$OUT/stub.log" 2>/dev/null || { echo "FAIL: the turn never reached the stub"; exit 1; }
   sleep 2.0   # the reply is in; mark_journey_best waits in the keep confirm stand-in (6 s of event pumping)
else
   cp "$PICOPILOT_GUI_DIR/keep.json" "$OUT/keep.json"
   read -r KX KY < <(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d["x"], d["y"])' "$OUT/keep.json")
   sleep 1.5   # ★ is disabled while a script runs; the panel's strip timer enables it once the -x script ended
   python3 "$HERE/gui_drive.py" --click "$KX" "$KY" "$OUT"   # ★, located from the panel's own geometry
   sleep 2.0   # the keep flow waits in its confirm (stand-in, or the real modal box)
fi
if [ "$MODE" = keepmodal ]; then
   import -window root "$OUT/keep-confirm.png" 2>/dev/null || true
   quit
   sleep 3
   import -window root "$OUT/keep-confirm-after-quit.png" 2>/dev/null || true
   STOPS_UNDER_MODAL="$(grep -c '^stop ' "$QUIT_IN_TURN_LOG" || true)"
   echo "Stop() calls while the keep confirm box was up: ${STOPS_UNDER_MODAL:-0}" | tee "$OUT/modal.txt"
   python3 "$HERE/gui_drive.py" --key Escape "$OUT"   # No: nothing is kept
   sleep 1.5
   import -window root "$OUT/keep-declined.png" 2>/dev/null || true
   picopilot_require_isolation || exit 1
   pi_require_private_display || exit 1
   "$PI" -x="$SLOT:$HERE/quit-in-turn-front.js" >/dev/null 2>&1
   sleep 1.5
fi
quit
QUIT_AT=$(date +%s.%N)
sleep 1
import -window root "$OUT/after-quit.png" 2>/dev/null || true
# PixInsight asks "One or more images have been modified and are unsaved ... Exit anyway?" (No is the
# default and has the focus): Tab moves to Yes, Space presses it. Drawn inside the main window, so keys only.
sleep 2
import -window root "$OUT/exit-question.png" 2>/dev/null || true
if kill -0 "$PI_PID" 2>/dev/null; then
   python3 "$HERE/gui_drive.py" --key Tab "$OUT"
   sleep 0.3
   python3 "$HERE/gui_drive.py" --key space "$OUT"
fi
rc=0
for _ in $(seq 220); do kill -0 "$PI_PID" 2>/dev/null || break; sleep 0.25; done
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
grep -E '^(stop|teardown|start-request|keeper-keep|interface-destroyed)' "$QUIT_IN_TURN_LOG" | tee -a "$OUT/result.txt" || true
sed 's/^/stub: /' "$OUT/stub.log" 2>/dev/null | tee -a "$OUT/result.txt" || true
picopilot_journeys_check "$JOURNEYS_BEFORE" || exit 1
ok=1
case "$MODE" in
   tool)
      grep -q '^stop in-tool=1' "$QUIT_IN_TURN_LOG" \
         || { echo "FAIL (inconclusive): no JourneyService::Stop() ran inside the tool loop"; ok=0; }
      # The stub's count is the criterion (StartRequest itself refuses once unload began; the gdb line only
      # says whether it was reached).
      requests="$(grep -c ' request ' "$OUT/stub.log" 2>/dev/null || true)"
      [ "${requests:-0}" = 1 ] || { echo "FAIL: the stub received ${requests:-0} requests (expected 1: none after Quit)"; ok=0; }
      ;;
   keep)
      grep -q '^stop .*in-keep=1' "$QUIT_IN_TURN_LOG" \
         || { echo "FAIL (inconclusive): no JourneyService::Stop() ran inside the keep flow"; ok=0; }
      ;;
   keepmodal)
      [ "${STOPS_UNDER_MODAL:-0}" = 0 ] || { echo "FAIL: Ctrl+Q reached PixInsight while the modal keep confirm was up"; ok=0; }
      grep -q '^stop ' "$QUIT_IN_TURN_LOG" || { echo "FAIL: the final Quit never ran OnUnload"; ok=0; }
      ;;
esac
if grep -q '^keeper-keep after-stop=1' "$QUIT_IN_TURN_LOG"; then
   echo "FAIL: a journey was kept (and a write-up could start) after unload began"; ok=0
fi
grep -q '^teardown' "$QUIT_IN_TURN_LOG" || { echo "FAIL: the journey teardown never ran"; ok=0; }
if grep -q '^interface-destroyed in-turn=1' "$QUIT_IN_TURN_LOG"; then
   echo "FAIL: the panel was destroyed while its turn / keep flow was still running"; ok=0
fi
[ "$rc" = 0 ] || { echo "FAIL: PixInsight exit code $rc (backtrace in $OUT/pi.log)"; ok=0; }
[ "$ok" = 1 ] && echo "PASS: Quit and the panel's $MODE work: clean exit, nothing new started after unload"
[ "$ok" = 1 ]
