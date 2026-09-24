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

# Gated real-API check: if a local, gitignored key file is present, export it
# so RunSelfTest() makes one real (tiny) Anthropic call instead of skipping.
KEYFILE="$ROOT/test/.test_api_key"
if [ -f "$KEYFILE" ]; then
   export PICOPILOT_TEST_API_KEY="$(cat "$KEYFILE")"
   echo "using local test API key: $KEYFILE (anthropic check will run for real)"
else
   echo "no local test API key at $KEYFILE (anthropic check will be skipped)"
fi

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

if ! PICOPILOT_SELFTEST_OUT="$R" timeout 180 "$PI" -n --automation-mode --no-startup-scripts -m="$SO" -r="$HERE/selftest.js" --force-exit; then
   echo "FAIL: PI load timed out (180s) or exited non-zero"; exit 1
fi
[ -f "$R" ] || { echo "FAIL: no result file"; exit 1; }
cat "$R"
python3 -c "
import json, sys
d = json.load(open('$R'))
ok = (d.get('evalOk') and d.get('evalResult') == 3 and d.get('processInstanceValid')
      and d.get('keyStoreOk') and d.get('anthropicOk') and d.get('workerThreadOk')
      and d.get('stallSkipped') is False and d.get('cancelOk') and d.get('deadlineOk')
      and d.get('plainTextOk') and d.get('ok'))
skipped = d.get('anthropicSkipped')
print('anthropic check: %s' % ('SKIPPED (no key)' if skipped else 'RAN against real API'))
sys.exit(0 if ok else 1)
" || { echo "FAIL: self-test did not prove all execution paths"; exit 1; }
echo "PASS: EvaluateScript==3, ProcessInstance valid, Settings round-trip OK, Anthropic check OK, worker-thread 401 OK, cancel+deadline on stalled connection OK, PlainText </raw> OK"
