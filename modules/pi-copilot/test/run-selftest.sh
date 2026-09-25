#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
PI=/opt/PixInsight/bin/PixInsight.sh
KEYS=/home/scarter4work/projects/keys/scarter4work_keys.xssk
PASS="$(cat /tmp/.pi_codesign_pass)"
SO="$ROOT/build/src/module/PICopilot-pxm.so"

# -n (no slot number) claims the first free instance slot -- slot 1 when the
# GUI isn't running -- whose settings file IS the user's real
# ~/.PixInsight/core-001-pxi.settings. Loading a dev-build .so there rewrites
# that file's persisted Modules list (dev path inserted, the repo-installed
# @pxi_bin_dir/PICopilot-pxm.so entry dropped), which then breaks the user's
# real install next time he launches PI normally ("Duplicate MetaProcess
# identifier"). Pin every headless test run to one fixed, otherwise-unused
# slot instead, and wipe that slot's settings before AND after each run so
# every run starts hermetic and never accumulates dev-only Modules state.
PICOPILOT_TEST_SLOT="${PICOPILOT_TEST_SLOT:-90}"

# Guard: PICOPILOT_TEST_SLOT is env-controlled (typo/override risk), and this
# script deletes whatever settings file its value resolves to. Slots 1-49 are
# where a normal PI instance (or another tool) lives -- slot 1 in particular
# IS the user's real ~/.PixInsight/core-001-pxi.settings -- so a bad value
# must never resolve there. Reject anything that isn't a plain unsigned
# decimal integer BEFORE any arithmetic touches it: bash's own $(( )) and
# printf %d both reinterpret a leading "0x" as hex and a leading "0" as
# octal, so "1", "01", and "0x1" would all otherwise collide with slot 1.
case "$PICOPILOT_TEST_SLOT" in
   ''|*[!0-9]*)
      echo "FAIL: PICOPILOT_TEST_SLOT must be a plain decimal integer, got '$PICOPILOT_TEST_SLOT'"; exit 1
      ;;
esac
# Force base-10 interpretation (10#...) so a leading zero can't be read as
# octal, then require the reserved test range: PI's own -n slot range is
# [1,256] (PixInsight.sh --help), and 1-49 are left for real/other instances.
if (( 10#$PICOPILOT_TEST_SLOT < 50 || 10#$PICOPILOT_TEST_SLOT > 256 )); then
   echo "FAIL: PICOPILOT_TEST_SLOT must be in [50,256] (reserved for tests), got '$PICOPILOT_TEST_SLOT'"; exit 1
fi
PICOPILOT_TEST_SLOT=$(( 10#$PICOPILOT_TEST_SLOT ))

SLOT_SETTINGS="$(printf '%s/core-%03d-pxi.settings' "$HOME/.PixInsight" "$PICOPILOT_TEST_SLOT")"
rm -f "$SLOT_SETTINGS"
trap 'rm -f "$SLOT_SETTINGS"' EXIT

[ -f "$SO" ] || { echo "FAIL: module not built at $SO"; exit 1; }
"$PI" --sign-module-file="$SO" --xssk-file="$KEYS" --xssk-password="$PASS"
[ -f "${SO%.so}.xsgn" ] || { echo "FAIL: signing produced no .xsgn"; exit 1; }

# Load the module headlessly and run the self-test harness. --force-exit only
# exits AFTER running -r= scripts, so a bare -m= with no -r= sits idle
# forever; the harness + timeout close that hole.
#
# The result path is a private, unpredictable name (mktemp) passed via env
# var, not a fixed /tmp path — ExecuteGlobal() in the shipped module never
# writes to a guessable location (CWE-59 symlink attack); see
# PICopilotInstance.cpp.
R="$(mktemp -u "${TMPDIR:-/tmp}/picopilot-selftest.XXXXXX.json")"
rm -f "$R"
trap 'rm -f "$R" "$SLOT_SETTINGS"' EXIT

# Gated real-API checks (text + vision). Key source order: system keyring,
# then the gitignored local file, else skip. The key is never printed.
KEYFILE="$ROOT/test/.test_api_key"
if command -v secret-tool >/dev/null 2>&1 \
   && KR_KEY="$(secret-tool lookup service anthropic account default 2>/dev/null)" && [ -n "$KR_KEY" ]; then
   export PICOPILOT_TEST_API_KEY="$KR_KEY"
   echo "using API key from the system keyring (secret-tool); real-API checks will run"
elif [ -f "$KEYFILE" ]; then
   export PICOPILOT_TEST_API_KEY="$(cat "$KEYFILE")"
   echo "using local test API key file: $KEYFILE; real-API checks will run"
else
   echo "no API key (keyring or $KEYFILE); real-API checks will be skipped"
fi
unset KR_KEY

# Local "stalled server" for the cancel/deadline proof: accepts connections,
# reads the request, and never answers -- the case SetConnectionTimeout()
# cannot bound. Loopback only; killed on exit.
STALL_PORT_FILE="$(mktemp)"
python3 - "$STALL_PORT_FILE" <<'PY' &
import socket, sys, threading, time
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 0)); s.listen(8)
open(sys.argv[1], "w").write(str(s.getsockname()[1]))
held = []
def hold(c):
    try:
        while c.recv(65536): pass
    except OSError: pass
while True:
    c, _ = s.accept(); held.append(c)
    threading.Thread(target=hold, args=(c,), daemon=True).start()
PY
STALL_PID=$!
trap 'rm -f "$R" "$STALL_PORT_FILE" "$SLOT_SETTINGS"; kill "$STALL_PID" 2>/dev/null || true' EXIT
for _ in $(seq 50); do [ -s "$STALL_PORT_FILE" ] && break; sleep 0.1; done
[ -s "$STALL_PORT_FILE" ] || { echo "FAIL: stall server did not start"; exit 1; }
export PICOPILOT_SELFTEST_STALL_URL="http://127.0.0.1:$(cat "$STALL_PORT_FILE")/v1/messages"

# Local "echo" server for the wire-encoding proof (self-test Section 8): it
# strict-decodes the POSTed bytes as UTF-8 (Python's codec rejects encoded
# surrogates, like the real API) and JSON, then answers in Messages API shape
# with {"messages": <parsed messages>, "tools": <parsed tools or null>} as the
# reply text -- or a 400 naming the first bad byte, with a hex dump. Every raw body is kept in ECHO_DIR as
# evidence. Loopback only; killed on exit.
# A path ending in "/agent" is a scripted tool loop (self-test Section A5): it
# checks role alternation and tool_use/tool_result pairing (400 on a mismatch),
# answers a plain user turn with a non-BMP text block + one describe_process
# tool_use, and answers a tool_result turn with end_turn text summarizing the
# tool names it received and the tool_results it got.
ECHO_DIR="$(mktemp -d)"
ECHO_PORT_FILE="$ECHO_DIR/port"
python3 - "$ECHO_DIR" <<'PY' &
import json, os, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
out = sys.argv[1]
count = [0]; lock = threading.Lock()
class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def reply(self, code, obj):
        b = json.dumps(obj).encode("ascii")
        self.send_response(code)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(b)))
        self.end_headers(); self.wfile.write(b)
    def agent_reply(self, req, n):
        def err(msg):
            return 400, {"type": "error", "error": {"type": "invalid_request_error", "message": "body #%d: %s" % (n, msg)}}
        msgs = req.get("messages") or []
        names = [t.get("name") for t in (req.get("tools") or [])]
        for i, m in enumerate(msgs):
            if m.get("role") != ("user" if i % 2 == 0 else "assistant"):
                return err("message %d has role %r" % (i, m.get("role")))
        if not msgs or msgs[-1].get("role") != "user":
            return err("last message is not a user message")
        last = msgs[-1].get("content")
        results = [b for b in last if b.get("type") == "tool_result"] if isinstance(last, list) else []
        if results:
            prev = msgs[-2].get("content") if len(msgs) >= 2 else []
            uses = {b.get("id") for b in prev if isinstance(prev, list) and b.get("type") == "tool_use"}
            got = {b.get("tool_use_id") for b in results}
            if uses != got:
                return err("tool_result ids %s != tool_use ids %s" % (sorted(got), sorted(uses)))
            summary = []
            for b in results:
                c = b.get("content")
                text = c if isinstance(c, str) else "".join(x.get("text", "") for x in c if x.get("type") == "text")
                summary.append({"id": b.get("tool_use_id"), "is_error": b.get("is_error", False), "text": text[:4000]})
            return 200, {"content": [{"type": "text", "text": json.dumps({"tools": names, "tool_results": summary})}],
                         "stop_reason": "end_turn"}
        return 200, {"content": [{"type": "text", "text": "Checking PixelMath \u2014 one moment \U0001F4F7"},
                                 {"type": "tool_use", "id": "toolu_wire_%02d" % n, "name": "describe_process",
                                  "input": {"id": "PixelMath"}}],
                     "stop_reason": "tool_use"}
    def sse(self, events, delay=0.3, stall=False):
        self.send_response(200)
        self.send_header("content-type", "text/event-stream")
        self.send_header("connection", "close")
        self.end_headers()
        self.close_connection = True
        try:
            for name, data in events:
                self.wfile.write(("event: %s\ndata: %s\n\n" % (name, json.dumps(data, ensure_ascii=False))).encode("utf-8"))
                self.wfile.flush()
                time.sleep(delay)
            while stall:          # say nothing more: the client's idle deadline must end it
                time.sleep(0.5)
        except (BrokenPipeError, ConnectionResetError):
            pass
    def stream_events(self, req, n, kind):
        msgs = req.get("messages") or []
        last = msgs[-1].get("content") if msgs else None
        results = [b for b in last if b.get("type") == "tool_result"] if isinstance(last, list) else []
        ev = [("message_start", {"type": "message_start", "message": {"id": "msg_s%02d" % n, "type": "message",
               "role": "assistant", "model": req.get("model"), "content": [], "stop_reason": None,
               "stop_sequence": None, "usage": {"input_tokens": 10, "output_tokens": 1}}}),
              ("ping", {"type": "ping"})]
        def text_block(index, parts):
            out = [("content_block_start", {"type": "content_block_start", "index": index,
                                            "content_block": {"type": "text", "text": ""}})]
            out += [("content_block_delta", {"type": "content_block_delta", "index": index,
                                             "delta": {"type": "text_delta", "text": p}}) for p in parts]
            return out + [("content_block_stop", {"type": "content_block_stop", "index": index})]
        def end(stop, tokens):
            return [("message_delta", {"type": "message_delta", "delta": {"stop_reason": stop, "stop_sequence": None},
                                       "usage": {"output_tokens": tokens}}),
                    ("message_stop", {"type": "message_stop"})]
        if kind == "stall":
            return ev
        if kind == "truncated":   # the connection closes mid-reply: no message_stop
            return ev + text_block(0, ["Cut "])[:2]
        if kind == "failmore":    # an assembler failure, then ~9 s more bytes the client must not wait for
            return (ev + text_block(0, ["Early "])[:2]
                    + [("content_block_delta", {"type": "content_block_delta", "index": 0,
                                                "delta": {"type": "mystery_delta"}})]
                    + [("ping", {"type": "ping"})] * 30)
        if kind == "error":
            return ev + text_block(0, ["Partial"])[:2] + [("error", {"type": "error",
                    "error": {"type": "overloaded_error", "message": "Overloaded"}})]
        if results:
            return ev + text_block(0, ["Got %d tool_" % len(results), "result(s) — done."]) + end("end_turn", 12)
        return (ev + text_block(0, ["Hello, ", "streamed ", "world é"])
                + [("content_block_start", {"type": "content_block_start", "index": 1, "content_block":
                        {"type": "tool_use", "id": "toolu_s%02d" % n, "name": "describe_process", "input": {}}}),
                   ("content_block_delta", {"type": "content_block_delta", "index": 1,
                        "delta": {"type": "input_json_delta", "partial_json": "{\"id\": \"Pixel"}}),
                   ("content_block_delta", {"type": "content_block_delta", "index": 1,
                        "delta": {"type": "input_json_delta", "partial_json": "Math\"}"}}),
                   ("content_block_stop", {"type": "content_block_stop", "index": 1})]
                + end("tool_use", 30))
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("content-length", "0")))
        with lock:
            count[0] += 1; n = count[0]
        open(os.path.join(out, "body-%02d.bin" % n), "wb").write(body)
        try:
            text = body.decode("utf-8")          # strict: surrogates rejected
        except UnicodeDecodeError as e:
            ctx = body[max(0, e.start - 8):e.start + 16].hex(" ")
            return self.reply(400, {"type": "error", "error": {"type": "invalid_request_error",
                "message": "body #%d not valid UTF-8 at byte %d (%s): %s" % (n, e.start, e.reason, ctx)}})
        try:
            req = json.loads(text)
        except ValueError as e:
            return self.reply(400, {"type": "error", "error": {"type": "invalid_request_error",
                "message": "body #%d not JSON: %s" % (n, e)}})
        for suffix, code, etype, msg in (("/stream-http-error", 529, "overloaded_error", "Overloaded"),
                                         ("/stream-401", 401, "authentication_error", "invalid x-api-key")):
            if self.path.endswith(suffix):
                if req.get("stream") is not True:
                    return self.reply(400, {"type": "error", "error": {"type": "invalid_request_error",
                                            "message": "body #%d: \"stream\" is not true" % n}})
                return self.reply(code, {"type": "error", "error": {"type": etype, "message": msg}})
        for suffix, kind in (("/stream-error", "error"), ("/stream-fail-then-more", "failmore"), ("/stream-stall", "stall"), ("/stream-truncated", "truncated"), ("/stream", "ok")):
            if self.path.endswith(suffix):
                if req.get("stream") is not True:
                    return self.reply(400, {"type": "error", "error": {"type": "invalid_request_error",
                                            "message": "body #%d: \"stream\" is not true" % n}})
                return self.sse(self.stream_events(req, n, kind), stall=(kind == "stall"))
        if self.path.endswith("/agent"):
            return self.reply(*self.agent_reply(req, n))
        self.reply(200, {"content": [{"type": "text", "text": json.dumps({"messages": req.get("messages"),
                                                                          "tools": req.get("tools")})}],
                         "stop_reason": "end_turn"})
srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
open(os.path.join(out, "port"), "w").write(str(srv.server_address[1]))
srv.daemon_threads = True
srv.serve_forever()
PY
ECHO_PID=$!
# PICOPILOT_ECHO_KEEP=<dir> keeps the captured request bodies for inspection.
trap 'if [ -n "${PICOPILOT_ECHO_KEEP:-}" ]; then cp "$ECHO_DIR"/body-* "$PICOPILOT_ECHO_KEEP"/ 2>/dev/null || true; fi; rm -f "$R" "$STALL_PORT_FILE" "$SLOT_SETTINGS"; rm -rf "$ECHO_DIR"; kill "$STALL_PID" "$ECHO_PID" 2>/dev/null || true' EXIT
for _ in $(seq 50); do [ -s "$ECHO_PORT_FILE" ] && break; sleep 0.1; done
[ -s "$ECHO_PORT_FILE" ] || { echo "FAIL: echo server did not start"; exit 1; }
export PICOPILOT_SELFTEST_ECHO_URL="http://127.0.0.1:$(cat "$ECHO_PORT_FILE")/v1/messages"
export PICOPILOT_SELFTEST_AGENT_URL="http://127.0.0.1:$(cat "$ECHO_PORT_FILE")/v1/agent"
export PICOPILOT_SELFTEST_STREAM_BASE="http://127.0.0.1:$(cat "$ECHO_PORT_FILE")/v1"
export PICOPILOT_SELFTEST_FIXTURES="$HERE/fixtures"

# Private virtual display (Xvfb). A core-side rejection can raise a MODAL
# dialog that no module API can suppress or catch (Task 1: "PixelMath: Invalid
# table row index"); on the user's real DISPLAY that dialog would block his
# desktop. Under Xvfb it is invisible, and it just blocks this run until the
# timeout fails it loudly. timeout sits INSIDE xvfb-run so that, on expiry,
# xvfb-run still tears down the Xvfb server (which also takes down any
# PixInsight process the PixInsight.sh wrapper left behind).
command -v xvfb-run >/dev/null 2>&1 || { echo "FAIL: xvfb-run not found (needed to keep dialogs off the real display)"; exit 1; }
if ! PICOPILOT_SELFTEST_OUT="$R" xvfb-run -a -s "-screen 0 1920x1080x24" \
        timeout 600 "$PI" -n="$PICOPILOT_TEST_SLOT" --automation-mode --no-startup-scripts -m="$SO" -r="$HERE/selftest.js" --force-exit; then
   echo "FAIL: PI load timed out (600s) or exited non-zero"; exit 1
fi
[ -f "$R" ] || { echo "FAIL: no result file"; exit 1; }
cat "$R"
echo
python3 - "$R" <<'PY' || { echo "FAIL: self-test verdict not all green"; exit 1; }
import json, sys
d = json.load(open(sys.argv[1]))
required_true = [
    'evalOk', 'processInstanceValid', 'keyStoreOk', 'anthropicOk', 'workerThreadOk',
    'cancelOk', 'deadlineOk', 'plainTextOk',
    # increment 3
    'visionSmokeOk',
    'viewContextOk',
    'previewOk',
    'previewU16Ok',
    'previewMonoOk',
    'catalogOk',
    'visionTurnOk', 'visionOk',
    'panelCaptureOk',
    # multi-turn body is strict UTF-8 on the wire (turn-2 400 regression)
    'utf8BodyOk', 'twoTurnOk',
    # increment 4
    'agentSmokeOk',
    'applyProcessOk',
    'toolTransportOk',
    'agentToolsOk',
    'agentLoopOk', 'agentWireOk',
    'panelResizableOk', 'turnEndNotesOk',
    'turnTargetOk',
    'liveAgentOk',
    # increment 5
    'inc5SmokeOk',
    'sseParserOk',
    'streamTransportOk',
    'ok',
]
missing = [k for k in required_true if d.get(k) is not True]
if d.get('evalResult') != 3: missing.append('evalResult==3')
if d.get('stallSkipped') is not False: missing.append('stallSkipped==false')
if d.get('utf8EchoSkipped') is not False: missing.append('utf8EchoSkipped==false')
if d.get('agentWireSkipped') is not False: missing.append('agentWireSkipped==false')
if d.get('streamLoopbackSkipped') is not False: missing.append('streamLoopbackSkipped==false')
import os
if os.environ.get('PICOPILOT_REQUIRE_LIVE') == '1':
    for k in ('anthropicSkipped', 'twoTurnSkipped', 'visionSkipped', 'liveAgentSkipped'):
        if d.get(k) is not False: missing.append(k + '==false (PICOPILOT_REQUIRE_LIVE=1)')
print('anthropic check: %s' % ('SKIPPED (no key)' if d.get('anthropicSkipped') else 'RAN against real API'))
print('two-turn check: %s' % ('SKIPPED (no key)' if d.get('twoTurnSkipped') else 'RAN against real API'))
print('vision check: %s' % ('SKIPPED (no key)' if d.get('visionSkipped') else 'RAN against real API, answer=%r' % d.get('visionAnswer')))
print('live agent check: %s' % ('SKIPPED (no key)' if d.get('liveAgentSkipped') else 'RAN against real API, ratio=%r log=%r' % (d.get('liveAgentRatio'), d.get('liveAgentLog'))))
if missing:
    print('FAILED keys: ' + ', '.join(missing))
    sys.exit(1)
PY
echo "PASS: self-test verdict all green"
