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
# with the parsed "messages" array as the reply text -- or a 400 naming the
# first bad byte, with a hex dump. Every raw body is kept in ECHO_DIR as
# evidence. Loopback only; killed on exit.
ECHO_DIR="$(mktemp -d)"
ECHO_PORT_FILE="$ECHO_DIR/port"
python3 - "$ECHO_DIR" <<'PY' &
import json, os, sys, threading
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
        self.reply(200, {"content": [{"type": "text", "text": json.dumps(req.get("messages"))}],
                         "stop_reason": "end_turn"})
srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
open(os.path.join(out, "port"), "w").write(str(srv.server_address[1]))
srv.serve_forever()
PY
ECHO_PID=$!
# PICOPILOT_ECHO_KEEP=<dir> keeps the captured request bodies for inspection.
trap 'if [ -n "${PICOPILOT_ECHO_KEEP:-}" ]; then cp "$ECHO_DIR"/body-* "$PICOPILOT_ECHO_KEEP"/ 2>/dev/null || true; fi; rm -f "$R" "$STALL_PORT_FILE" "$SLOT_SETTINGS"; rm -rf "$ECHO_DIR"; kill "$STALL_PID" "$ECHO_PID" 2>/dev/null || true' EXIT
for _ in $(seq 50); do [ -s "$ECHO_PORT_FILE" ] && break; sleep 0.1; done
[ -s "$ECHO_PORT_FILE" ] || { echo "FAIL: echo server did not start"; exit 1; }
export PICOPILOT_SELFTEST_ECHO_URL="http://127.0.0.1:$(cat "$ECHO_PORT_FILE")/v1/messages"

if ! PICOPILOT_SELFTEST_OUT="$R" timeout 300 "$PI" -n="$PICOPILOT_TEST_SLOT" --automation-mode --no-startup-scripts -m="$SO" -r="$HERE/selftest.js" --force-exit; then
   echo "FAIL: PI load timed out (300s) or exited non-zero"; exit 1
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
    'ok',
]
missing = [k for k in required_true if d.get(k) is not True]
if d.get('evalResult') != 3: missing.append('evalResult==3')
if d.get('stallSkipped') is not False: missing.append('stallSkipped==false')
if d.get('utf8EchoSkipped') is not False: missing.append('utf8EchoSkipped==false')
print('anthropic check: %s' % ('SKIPPED (no key)' if d.get('anthropicSkipped') else 'RAN against real API'))
print('two-turn check: %s' % ('SKIPPED (no key)' if d.get('twoTurnSkipped') else 'RAN against real API'))
print('vision check: %s' % ('SKIPPED (no key)' if d.get('visionSkipped') else 'RAN against real API, answer=%r' % d.get('visionAnswer')))
if missing:
    print('FAILED keys: ' + ', '.join(missing))
    sys.exit(1)
PY
echo "PASS: self-test verdict all green"
