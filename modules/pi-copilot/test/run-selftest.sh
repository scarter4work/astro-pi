#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
PI=/opt/PixInsight/bin/PixInsight.sh
KEYS=/home/scarter4work/projects/keys/scarter4work_keys.xssk
PASS="$(cat /tmp/.pi_codesign_pass)"
SO="$ROOT/build/src/module/PICopilot-pxm.so"

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
trap 'rm -f "$R"' EXIT

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
trap 'rm -f "$R" "$STALL_PORT_FILE"; kill "$STALL_PID" 2>/dev/null || true' EXIT
for _ in $(seq 50); do [ -s "$STALL_PORT_FILE" ] && break; sleep 0.1; done
[ -s "$STALL_PORT_FILE" ] || { echo "FAIL: stall server did not start"; exit 1; }
export PICOPILOT_SELFTEST_STALL_URL="http://127.0.0.1:$(cat "$STALL_PORT_FILE")/v1/messages"

if ! PICOPILOT_SELFTEST_OUT="$R" timeout 300 "$PI" -n --automation-mode --no-startup-scripts -m="$SO" -r="$HERE/selftest.js" --force-exit; then
   echo "FAIL: PI load timed out (300s) or exited non-zero"; exit 1
fi
[ -f "$R" ] || { echo "FAIL: no result file"; exit 1; }
cat "$R"
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
    'ok',
]
missing = [k for k in required_true if d.get(k) is not True]
if d.get('evalResult') != 3: missing.append('evalResult==3')
if d.get('stallSkipped') is not False: missing.append('stallSkipped==false')
print('anthropic check: %s' % ('SKIPPED (no key)' if d.get('anthropicSkipped') else 'RAN against real API'))
print('vision check: %s' % ('SKIPPED (no key)' if d.get('visionSkipped') else 'RAN against real API, answer=%r' % d.get('visionAnswer')))
if missing:
    print('FAILED keys: ' + ', '.join(missing))
    sys.exit(1)
PY
echo "PASS: self-test verdict all green"
