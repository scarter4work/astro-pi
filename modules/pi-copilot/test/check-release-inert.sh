#!/usr/bin/env bash
# Proves a PI Copilot RELEASE build ignores every self-test environment variable: headless PixInsight
# (--automation-mode, private Xvfb, isolated slot and data) loads <so>, every PICOPILOT_SELFTEST_* variable
# is set -- PICOPILOT_SELFTEST_OUT, a PICOPILOT_SELFTEST_PHASE file asking for gui.turn against a loopback
# listener, and the URL hooks (_AGENT_URL, _ECHO_URL, _STALL_URL, _WRITEUP_URL, _STREAM_BASE) pointed at
# that listener -- and test/inert-probe.js runs `new PICopilot().executeGlobal()`.
#
#   check-release-inert.sh release  <PICopilot-pxm.so>   PASS: nothing happened (no verdict file, no
#                                                         connection, no consent saved)
#   check-release-inert.sh selftest <PICopilot-pxm.so>   the contrast: a self-test build DOES react
#                                                         (the consent is saved), so the variables are live
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/harness-lib.sh"
PI="$PICOPILOT_PI"
KEYS=/home/scarter4work/projects/keys/scarter4work_keys.xssk
EXPECT="${1:?usage: check-release-inert.sh release|selftest <PICopilot-pxm.so>}"
case "$EXPECT" in release|selftest) ;; *) echo "FAIL: expect release or selftest"; exit 1 ;; esac
SO="$(realpath -e "${2:?usage: check-release-inert.sh release|selftest <PICopilot-pxm.so>}")"
SLOT="${PICOPILOT_TEST_SLOT:-91}"
case "$SLOT" in ''|*[!0-9]*) echo "FAIL: bad slot '$SLOT'"; exit 1 ;; esac
SLOT=$(( 10#$SLOT ))
if (( SLOT < 50 || SLOT > 256 )); then echo "FAIL: slot must be in [50,256] (reserved for tests)"; exit 1; fi
SLOT_SETTINGS="$(printf '%s/core-%03d-pxi.settings' "$HOME/.PixInsight" "$SLOT")"
rm -f "$SLOT_SETTINGS"
PRIV="/tmp/picopilot-$(id -u)"
mkdir -m 700 "$PRIV" 2>/dev/null || true
RUN_DIR="$(mktemp -d "$PRIV/inert.XXXXXX")"
LISTEN_PID=
trap '[ -n "$LISTEN_PID" ] && kill "$LISTEN_PID" 2>/dev/null; rm -f "$SLOT_SETTINGS"; rm -rf "$RUN_DIR"' EXIT
picopilot_isolate_data "$RUN_DIR/xdg" || exit 1
picopilot_isolate_display
JOURNEYS_BEFORE="$(picopilot_journeys_fingerprint)"

if [ ! -f "${SO%.so}.xsgn" ] || [ "${SO%.so}.xsgn" -ot "$SO" ]; then
   case "$SO" in /opt/*) echo "FAIL: $SO is never signed in place"; exit 1 ;; esac
   picopilot_require_isolation || exit 1
   pi_headless "$PI" --sign-module-file="$SO" --xssk-file="$KEYS" --xssk-password="$(cat /tmp/.pi_codesign_pass)" >/dev/null
fi

# Loopback listener: counts every connection (any would mean a hook reached the network).
python3 - "$RUN_DIR/port" "$RUN_DIR/connections" <<'PY' & LISTEN_PID=$!
import socket, sys
s = socket.socket(); s.bind(("127.0.0.1", 0)); s.listen(8)
open(sys.argv[1], "w").write(str(s.getsockname()[1]))
while True:
    c, _ = s.accept()
    open(sys.argv[2], "a").write("connection\n")
    c.close()
PY
for _ in $(seq 50); do [ -s "$RUN_DIR/port" ] && break; sleep 0.1; done
URL="http://127.0.0.1:$(cat "$RUN_DIR/port")/v1/messages"

export PICOPILOT_SELFTEST_OUT="$RUN_DIR/verdict.json"
export PICOPILOT_SELFTEST_PHASE="$RUN_DIR/phase.json"
printf '{"phase":"gui.turn","payload":{"url":"%s","key":"","prompt":"hello"}}\n' "$URL" > "$PICOPILOT_SELFTEST_PHASE"
export PICOPILOT_SELFTEST_AGENT_URL="$URL" PICOPILOT_SELFTEST_ECHO_URL="$URL" PICOPILOT_SELFTEST_STALL_URL="$URL"
export PICOPILOT_SELFTEST_WRITEUP_URL="$URL" PICOPILOT_SELFTEST_STREAM_BASE="${URL%/v1/messages}"
export INERT_OUT="$RUN_DIR/probe.txt"

picopilot_seed_slot_modules "$SLOT" "$RUN_DIR" >/dev/null || exit 1
picopilot_require_isolation || exit 1
pi_headless timeout 180 "$PI" -n="$SLOT" --automation-mode --no-startup-scripts -m="$SO" \
   -r="$HERE/inert-probe.js" --force-exit >"$RUN_DIR/pi.log" 2>&1 || true
[ -f "$INERT_OUT" ] || { echo "FAIL: the probe script never ran (see $RUN_DIR/pi.log)"; cat "$RUN_DIR/pi.log" | tail -5; exit 1; }
sleep 0.5
connections="$(grep -c connection "$RUN_DIR/connections" 2>/dev/null || true)"
consent="$(grep -c 'AnthropicConsent' "$SLOT_SETTINGS" 2>/dev/null || true)"
echo "module: $SO"
sed 's/^/probe: /' "$INERT_OUT"
echo "loopback connections: ${connections:-0}"
echo "consent saved in the slot settings: $( [ "${consent:-0}" -gt 0 ] && echo yes || echo no )"
picopilot_journeys_check "$JOURNEYS_BEFORE" >/dev/null || exit 1
if [ "$EXPECT" = release ]; then
   ok=1
   grep -q '^selftestOutWritten=false' "$INERT_OUT" || { echo "FAIL: the self-test verdict file was written"; ok=0; }
   [ "${connections:-0}" = 0 ] || { echo "FAIL: a hook connected to the loopback"; ok=0; }
   [ "${consent:-0}" = 0 ] || { echo "FAIL: the data consent was saved"; ok=0; }
   [ "$ok" = 1 ] && echo "PASS: release build inert under every PICOPILOT_SELFTEST_* variable"
   [ "$ok" = 1 ]
else
   [ "${consent:-0}" -gt 0 ] || { echo "FAIL: the self-test build did not react (the contrast proves nothing)"; exit 1; }
   echo "PASS (contrast): the self-test build reacts to the same variables (consent saved)"
fi
