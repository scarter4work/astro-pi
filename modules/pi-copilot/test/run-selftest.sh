#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
# Shared isolation helpers: private XDG_DATA_HOME, the real-journey-library
# check, and the slot-module seeding that keeps the INSTALLED PICopilot out of
# the test process (see harness-lib.sh's header for the measured root cause).
. "$HERE/harness-lib.sh"
PI="$PICOPILOT_PI"
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
# Before PI starts, the wiped slot is re-seeded with a Modules list that
# excludes the installed PICopilot (picopilot_seed_slot_modules, harness-lib.sh):
# a settings-less slot would otherwise install every bin/*-pxm.so, the
# installed PICopilot included, next to the -m= dev build.
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

# Per-run private TMPDIR (thist-hang-investigation.md item 2). Measured: PI's
# own ~PI~*.swp image swap files, its qipc instance lock files, and
# xvfb-run's own auth/lock dir all follow $TMPDIR. Giving every run its own
# private (0700), throwaway directory means a killed/timed-out run's leaked
# files can never collide with another concurrent run's and never pile up in
# bare /tmp (16,940 leaked swap files / 5.5 GB were found there from before
# this existed -- see test/clean-stale-swap.sh). Every OTHER mktemp /
# mktemp -d call below (HANDOFF_DIR, the private XDG_DATA_HOME, STALL_PORT_FILE,
# ECHO_DIR, R, ...) already honours $TMPDIR with no further changes, and
# cleanup() removes the whole thing on every exit path.
# /tmp/picopilot-<uid>: the per-user private directory shared by the watchdog
# logs, the throwlog and the module's GraXpert self-test lock
# (GraXpertCoreSelfTestLock refuses it unless it is a real 0700 directory of
# this user). `mkdir -p -m 700 <dir>/sub` would create <dir> itself with the
# umask mode (0755) and fail every later GraXpert check, so it is created on
# its own, 0700, and verified here -- loudly, never chmod-ed behind the user.
PICOPILOT_PRIVATE_TMP="/tmp/picopilot-$(id -u)"
mkdir -m 700 "$PICOPILOT_PRIVATE_TMP" 2>/dev/null || true
if [ -L "$PICOPILOT_PRIVATE_TMP" ] || [ ! -d "$PICOPILOT_PRIVATE_TMP" ] \
   || [ "$(stat -c '%u %a' "$PICOPILOT_PRIVATE_TMP")" != "$(id -u) 700" ]; then
   echo "FAIL: $PICOPILOT_PRIVATE_TMP must be a real directory owned by $(id -u) with mode 700 (got: $(stat -c '%U %a %F' "$PICOPILOT_PRIVATE_TMP" 2>&1))"
   exit 1
fi
PICOPILOT_RUNTIME_BASE="${XDG_RUNTIME_DIR:-/tmp/picopilot-$(id -u)/runs}"
mkdir -p -m 700 "$PICOPILOT_RUNTIME_BASE"
TMPDIR="$(mktemp -d "$PICOPILOT_RUNTIME_BASE/run.XXXXXX")"
chmod 700 "$TMPDIR"
export TMPDIR

# Static guard (Task 10 fix round 5): no pcl::String / IsoString (or a type holding one) in a std sequence
# container that is erased / inserted / sorted, and no move out of a data member -- a moved-from PCL value is not
# a valid assignment target (proven SIGSEGVs). Checks itself against known-bad snippets first. Fails fast.
if ! python3 "$HERE/check-pcl-moves.py" "$TMPDIR/pcl-move-guard"; then
   echo "FAIL: the moved-from PCL value guard (test/check-pcl-moves.py) found a violation"
   exit 1
fi

SLOT_SETTINGS="$(printf '%s/core-%03d-pxi.settings' "$HOME/.PixInsight" "$PICOPILOT_TEST_SLOT")"
rm -f "$SLOT_SETTINGS"
# One cleanup for every EXIT path (0.2.0.0). Every variable is empty until the
# line that sets it has run (set -u: always ${VAR:-}), so an early exit cleans
# exactly what exists. The private XDG_DATA_HOME lives under TMPDIR (removed
# below); never rm the caller's XDG_DATA_HOME.
cleanup()
{
   if [ -n "${PICOPILOT_ECHO_KEEP:-}" ] && [ -n "${ECHO_DIR:-}" ]; then
      cp "$ECHO_DIR"/body-* "$PICOPILOT_ECHO_KEEP"/ 2>/dev/null || true
   fi
   rm -f "${R:-}" "${STALL_PORT_FILE:-}" "${SLOT_SETTINGS:-}"
   if [ -n "${HANDOFF_DIR:-}" ]; then rm -rf "$HANDOFF_DIR"; fi
   if [ -n "${ECHO_DIR:-}" ]; then rm -rf "$ECHO_DIR"; fi
   if [ -n "${STALL_PID:-}" ]; then kill "$STALL_PID" 2>/dev/null || true; fi
   if [ -n "${ECHO_PID:-}" ]; then kill "$ECHO_PID" 2>/dev/null || true; fi
   if [ -n "${WATCHDOG_PID:-}" ]; then kill "$WATCHDOG_PID" 2>/dev/null || true; fi
   # Belt and suspenders on top of the individual removals above: everything
   # this run created via a bare mktemp/mktemp -d lives under TMPDIR (see
   # where it's set, above), so this catches anything new added here later
   # without a matching explicit rm, and covers an abnormal exit (e.g. ^C)
   # that never reaches the lines below it.
   if [ -n "${TMPDIR:-}" ] && [ -d "${TMPDIR:-}" ]; then rm -rf "$TMPDIR"; fi
   return 0
}
trap cleanup EXIT

# Image journey (0.2.0.0): the production JourneyService records into
# $XDG_DATA_HOME/PICopilot/journeys. Every PixInsight this script launches gets
# a private data home (harness-lib.sh; PICopilot left out, everything else
# mirrored), is refused unless that isolation is in effect, and the run fails
# if the user's real library (~/.local/share/PICopilot/journeys) changed.
picopilot_isolate_data "$TMPDIR/xdg" || exit 1
picopilot_isolate_display
JOURNEYS_BEFORE="$(picopilot_journeys_fingerprint)"
# Every file handed between selftest.js and the module lives in one private
# (0700, mktemp -d) directory owned by this shell, so no writer ever opens a
# guessable or pre-planted path (CWE-59); cleanup() removes it.
HANDOFF_DIR="$(mktemp -d "${TMPDIR:-/tmp}/picopilot-handoff.XXXXXX")"
chmod 700 "$HANDOFF_DIR"
# The selftest.js pre-phase ("user actions, panel never opened") writes its result here.
export PICOPILOT_SELFTEST_PRE="$HANDOFF_DIR/pre.json"
# Multi-phase harness: selftest.js writes {"phase", "payload"} here before each
# top-level check phase (PICopilot.executeGlobal() then runs that phase's
# handler instead of the self-test). See the header of test/selftest.js.
export PICOPILOT_SELFTEST_PHASE="$HANDOFF_DIR/phase.json"
# Scratch directory for top-level fixtures that write files (e.g. J0 save+reopen).
export PICOPILOT_SELFTEST_SCRATCH="$HANDOFF_DIR/scratch"
mkdir -m 700 "$PICOPILOT_SELFTEST_SCRATCH"
# Workspace process icons (fix/replay-file-params): test/load-icons.js, the
# first -r= script, writes a fixture .xpsm + the file its icons name here and
# yields it to this instance, which loads the icons before selftest.js runs.
export PICOPILOT_SELFTEST_ICONS="$HANDOFF_DIR/icons"
mkdir -m 700 "$PICOPILOT_SELFTEST_ICONS"
# Per-section wall-clock timings, rewritten at every section mark by selftest.js
# (fixture blocks) and by the module (self-test sections), so any run -- even
# one that hangs until the timeout -- prints where its time went.
export PICOPILOT_SELFTEST_JS_TIMINGS="$HANDOFF_DIR/js-timings.json"
export PICOPILOT_SELFTEST_SECTION_TIMINGS="$HANDOFF_DIR/section-timings.json"
print_timings()
{
   python3 - "$PICOPILOT_SELFTEST_JS_TIMINGS" "$PICOPILOT_SELFTEST_SECTION_TIMINGS" <<'PY' || true
import json, sys
for tag, path in (("js", sys.argv[1]), ("module", sys.argv[2])):
    try:
        d = json.load(open(path))
    except (OSError, ValueError) as e:
        print("timing %-6s (no timings: %s)" % (tag, e.__class__.__name__)); continue
    for t in d.get("done", []):
        print("timing %-6s %-52s +%7.1fs %9.0f ms" % (tag, t["section"], t["startS"], t["ms"]))
    if d.get("open"):
        print("timing %-6s %-52s +%7.1fs  STILL RUNNING at exit" % (tag, d["open"]["section"], d["open"]["startS"]))
PY
}

[ -f "$SO" ] || { echo "FAIL: module not built at $SO"; exit 1; }
# Resolve before use (thist-hang-investigation.md item 3): a -m= path reached
# through a symlinked component (e.g. a harness copy living under a /tmp that
# is itself a symlink) can make PI's own module search silently map a
# DIFFERENT file than the one this argument names, while looking identical as
# text. Measured live: a copy under /tmp ran the INSTALLED 0.1.2.0 module
# instead of this dev build's -m= .so, and "T-hist takes minutes" turned out
# to be the live self-test running there, not a hang. Always pass PI the
# canonical, symlink-free path; watchdog.py's /proc/<pid>/maps check then
# compares like for like instead of maybe comparing two spellings of the same
# file, or missing a real substitution.
SO="$(realpath -e "$SO")"
picopilot_require_isolation || exit 1
"$PI" --sign-module-file="$SO" --xssk-file="$KEYS" --xssk-password="$PASS"
[ -f "${SO%.so}.xsgn" ] || { echo "FAIL: signing produced no .xsgn"; exit 1; }

# Vendored SQLite must stay private to the module (a clash with any other
# libsqlite3 in the PixInsight process would be undefined behaviour).
if nm -D --defined-only "$SO" | grep -q ' sqlite3_'; then
   echo "FAIL: PICopilot-pxm.so exports sqlite3_* symbols"; exit 1
fi

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

# GraXpert live check (self-test B10): the app path is the user's OWN GraXpert
# setting, read (read-only) from the real PixInsight settings file -- the
# slot-90 settings are empty. An explicit PICOPILOT_TEST_GRAXPERT_APP wins.
# Concurrent slots: PixInsight's GraXpert core bridges to the program through
# shared exchange files named /PixInsight.xisf and /PixInsight_GraXpert.xisf
# UNDER $TMPDIR -- MEASURED 2026-09-26 (thist-hang-investigation.md item 2):
# with this run's own private TMPDIR (set above) exported to PI, those two
# files showed up under it, actively rewritten, exactly during this run's own
# B10 live section -- not in bare /tmp. So two runs that BOTH use this
# harness (each with its own private TMPDIR) no longer clobber each other
# through these files. The flock below stays anyway: it still protects
# against anything that ISN'T isolated this way -- an older harness copy that
# predates the TMPDIR fix, or the user's own live, interactive PixInsight
# session, both of which still fall back to plain /tmp/PixInsight*.xisf.
# The self-test therefore still runs every GraXpert-core call (B10 live, B10b
# stand-in) under an exclusive flock on /tmp/picopilot-<uid>/graxpert-selftest.lock
# (dir 0700) (GraXpertCoreSelfTestLock); only those sections are serialized,
# the rest of the run stays parallel. The wait is printed below (lockWaitMs).
if [ -z "${PICOPILOT_TEST_GRAXPERT_APP:-}" ] && [ -f "$HOME/.PixInsight/core-001-pxi.settings" ]; then
   PICOPILOT_TEST_GRAXPERT_APP="$(python3 - "$HOME/.PixInsight/core-001-pxi.settings" <<'PY' 2>/dev/null || true
import sys, xml.etree.ElementTree as ET
node = ET.parse(sys.argv[1]).getroot()
for k in ("ModuleData", "GraXpert", "Interfaces", "GraXpert", "appPath"):
    node = next((c for c in node if c.get("k") == k), None)
    if node is None: sys.exit(0)
print(node.text or "")
PY
)"
fi
export PICOPILOT_TEST_GRAXPERT_APP="${PICOPILOT_TEST_GRAXPERT_APP:-}"
echo "GraXpert app for the live check: ${PICOPILOT_TEST_GRAXPERT_APP:-(none; the live GraXpert check will be SKIPPED)}"

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
        if self.path.split("?", 1)[0].endswith("/writeup"):   # Haiku journey write-up (plan Task 9)
            # ?reasons=<JSON array>: the reply's inferredReasons entries, verbatim (guard tests);
            # ?truncate=1: the reply stops inside an opened, never-closed json fence (stop_reason max_tokens).
            from urllib.parse import parse_qs, urlsplit
            q = parse_qs(urlsplit(self.path).query)
            msgs = req.get("messages") or []
            def blocks(m):
                c = m.get("content")
                return c if isinstance(c, list) else [{"type": "text", "text": c or ""}]
            has_image = any(b.get("type") == "image" for m in msgs for b in blocks(m))
            if (has_image or req.get("stream") or req.get("tools") or req.get("thinking")
                    or req.get("model") != "claude-haiku-4-5" or req.get("max_tokens") != 8000 or len(msgs) != 1):
                return self.reply(400, {"type": "error", "error": {"type": "invalid_request_error",
                                        "message": "body #%d: write-up request shape is wrong" % n}})
            text = "".join(b.get("text", "") for b in blocks(msgs[0]))
            rec = json.loads(text.split("CONDENSED_RECIPE_JSON:\n", 1)[1])
            sid = next((s["id"] for s in rec["steps"] if s["actor"] == "user" and not s.get("reason")), None)
            fence = "`" * 3   # never three literal backticks in this file's markdown source
            if "truncate" in q:
                md = ("# %s\n\n## Processing\nStep %s brightened the faint signal (inferred).\n\n" + fence
                      + "json\n{\"inferredReasons\": [{\"step\": %s, \"reas") % (rec["journey"]["name"], sid, sid)
                return self.reply(200, {"content": [{"type": "text", "text": md}], "stop_reason": "max_tokens"})
            if "reasons" in q:
                md = ("# %s\n\n## Processing\nGuard test.\n\n" + fence + "json\n%s\n" + fence + "\n") % (
                      rec["journey"]["name"], json.dumps({"inferredReasons": json.loads(q["reasons"][0])}))
                return self.reply(200, {"content": [{"type": "text", "text": md}], "stop_reason": "end_turn"})
            md =("# %s\n\n## Equipment\nLoopback.\n\n## Acquisition\nLoopback.\n\n## Processing\nStep %s brightened the "
                  "faint signal (inferred).\n\n" + fence + "json\n%s\n" + fence + "\n") % (rec["journey"]["name"], sid,
                  json.dumps({"inferredReasons": [{"step": sid, "reason": "brighten the faint signal"}]}))
            return self.reply(200, {"content": [{"type": "text", "text": md}], "stop_reason": "end_turn"})
        if self.path.endswith("/agent"):
            return self.reply(*self.agent_reply(req, n))
        self.reply(200, {"content": [{"type": "text", "text": json.dumps({"messages": req.get("messages"), "tools": req.get("tools"), "system": req.get("system"),
            "cache_control": req.get("cache_control"), "thinking": req.get("thinking"),
            "anthropic_beta": self.headers.get("anthropic-beta")})}],
                         "stop_reason": "end_turn"})
srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
open(os.path.join(out, "port"), "w").write(str(srv.server_address[1]))
srv.daemon_threads = True
srv.serve_forever()
PY
ECHO_PID=$!
# PICOPILOT_ECHO_KEEP=<dir> keeps the captured request bodies for inspection.
for _ in $(seq 50); do [ -s "$ECHO_PORT_FILE" ] && break; sleep 0.1; done
[ -s "$ECHO_PORT_FILE" ] || { echo "FAIL: echo server did not start"; exit 1; }
export PICOPILOT_SELFTEST_ECHO_URL="http://127.0.0.1:$(cat "$ECHO_PORT_FILE")/v1/messages"
export PICOPILOT_SELFTEST_AGENT_URL="http://127.0.0.1:$(cat "$ECHO_PORT_FILE")/v1/agent"
export PICOPILOT_SELFTEST_STREAM_BASE="http://127.0.0.1:$(cat "$ECHO_PORT_FILE")/v1"
export PICOPILOT_SELFTEST_WRITEUP_URL="http://127.0.0.1:$(cat "$ECHO_PORT_FILE")/v1/writeup"
export PICOPILOT_SELFTEST_FIXTURES="$HERE/fixtures"

# Optional exception-logging shim (thist-hang-investigation.md item 4):
# PICOPILOT_THROWLOG=1 builds test/throwlog.c fresh into this run's private
# TMPDIR (never a committed binary -- see test/throwlog.c) and LD_PRELOADs it
# into PI ONLY (a per-command env prefix below, not `export`, so nothing else
# this script runs picks it up). Every std::bad_alloc-family throw anywhere in
# the process then logs a backtrace. Kept available because the T-hist
# "Out of memory" modal's actual throw site was never identified (investigation
# doc, "Recommended fixes" #4); the next occurrence under this flag catches it.
PICOPILOT_THROWLOG_PRELOAD=()
if [ "${PICOPILOT_THROWLOG:-0}" = "1" ]; then
   command -v gcc >/dev/null 2>&1 || { echo "FAIL: PICOPILOT_THROWLOG=1 needs gcc"; exit 1; }
   THROWLOG_SO="$TMPDIR/throwlog.so"
   gcc -shared -fPIC -O2 -o "$THROWLOG_SO" "$HERE/throwlog.c" -ldl
   [ -f "$THROWLOG_SO" ] || { echo "FAIL: throwlog.so build failed"; exit 1; }
   mkdir -m 700 "$PICOPILOT_PRIVATE_TMP/watchdog-logs" 2>/dev/null || [ -d "$PICOPILOT_PRIVATE_TMP/watchdog-logs" ]
   THROWLOG_FILE="/tmp/picopilot-$(id -u)/watchdog-logs/throwlog-$(date +%Y%m%dT%H%M%S)-slot${PICOPILOT_TEST_SLOT}.txt"
   PICOPILOT_THROWLOG_PRELOAD=( "LD_PRELOAD=$THROWLOG_SO" "THROWLOG_FILE=$THROWLOG_FILE" )
   echo "PICOPILOT_THROWLOG=1: LD_PRELOAD=$THROWLOG_SO, log (created only if something throws) -> $THROWLOG_FILE"
fi

# Private virtual display (Xvfb). A core-side rejection can raise a MODAL
# dialog that no module API can suppress or catch (Task 1: "PixelMath: Invalid
# table row index"); on the user's real DISPLAY that dialog would block his
# desktop. Under Xvfb it is invisible, and it just blocks this run until the
# timeout fails it loudly. timeout sits INSIDE xvfb-run so that, on expiry,
# xvfb-run still tears down the Xvfb server (which also takes down any
# PixInsight process the PixInsight.sh wrapper left behind).
command -v xvfb-run >/dev/null 2>&1 || { echo "FAIL: xvfb-run not found (needed to keep dialogs off the real display)"; exit 1; }

# Section watchdog (thist-hang-investigation.md item 1). Runs alongside PI and
# reads the very JS + module section-timing files (selftest.js's jsMark() /
# SelfTestTiming.h's SelfTestSectionMark()) this run is about to write, plus a
# live /proc/<pid>/maps check against the -m= module we just resolved above
# (item 3). On a section overrun -- or a module-load mismatch -- it captures
# an all-thread gdb backtrace (symbol names only; NEVER disassemble PI, EULA)
# and an Xvfb screenshot into $WATCHDOG_LOG_BASE/<run>/, kills PI, and writes
# $WATCHDOG_FAIL_MARKER for us to report below with the section name and
# artefact paths -- instead of sitting out the full 900s `timeout` with
# nothing to show for it.
WATCHDOG_LOG_BASE="$PICOPILOT_PRIVATE_TMP/watchdog-logs"
mkdir -m 700 "$WATCHDOG_LOG_BASE" 2>/dev/null || [ -d "$WATCHDOG_LOG_BASE" ] || { echo "FAIL: cannot create $WATCHDOG_LOG_BASE"; exit 1; }
WATCHDOG_FAIL_MARKER="$HANDOFF_DIR/watchdog-fail.json"
WATCHDOG_OK_MARKER="$HANDOFF_DIR/watchdog-module-ok.json"
# The slot's settings: every installed module EXCEPT the installed
# PICopilot-pxm.so, so -m= is the only PICopilot in the process.
picopilot_seed_slot_modules "$PICOPILOT_TEST_SLOT" "$TMPDIR" || exit 1
python3 "$HERE/watchdog.py" \
   --slot "$PICOPILOT_TEST_SLOT" \
   --module-so "$SO" \
   --js-timings "$PICOPILOT_SELFTEST_JS_TIMINGS" \
   --section-timings "$PICOPILOT_SELFTEST_SECTION_TIMINGS" \
   --budgets "$HERE/section-budgets.json" \
   --fail-marker "$WATCHDOG_FAIL_MARKER" \
   --ok-marker "$WATCHDOG_OK_MARKER" \
   --xdg-data-home "$XDG_DATA_HOME" \
   --log-dir-base "$WATCHDOG_LOG_BASE" \
   ${PICOPILOT_WATCHDOG_OVERRIDE:+--override "$PICOPILOT_WATCHDOG_OVERRIDE"} \
   ${PICOPILOT_WATCHDOG_DEFAULT_CAP_S:+--default-cap "$PICOPILOT_WATCHDOG_DEFAULT_CAP_S"} \
   ${PICOPILOT_WATCHDOG_FLOOR_S:+--floor "$PICOPILOT_WATCHDOG_FLOOR_S"} \
   ${PICOPILOT_WATCHDOG_MULTIPLIER:+--multiplier "$PICOPILOT_WATCHDOG_MULTIPLIER"} \
   >>"$WATCHDOG_LOG_BASE/driver.log" 2>&1 &
WATCHDOG_PID=$!

PI_RC=0
picopilot_require_isolation || exit 1
env "${PICOPILOT_THROWLOG_PRELOAD[@]}" PICOPILOT_SELFTEST_OUT="$R" xvfb-run -a -s "-screen 0 1920x1080x24" \
      timeout 900 "$PI" -n="$PICOPILOT_TEST_SLOT" --automation-mode --no-startup-scripts -m="$SO" -r="$HERE/load-icons.js" -r="$HERE/selftest.js" --force-exit \
   || PI_RC=$?

kill "$WATCHDOG_PID" 2>/dev/null || true
wait "$WATCHDOG_PID" 2>/dev/null || true

if [ -s "$WATCHDOG_FAIL_MARKER" ]; then
   print_timings
   python3 - "$WATCHDOG_FAIL_MARKER" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
if d.get("reason") in ("module-mismatch", "module-not-loaded", "isolation-missing", "display-isolation"):
    print("FAIL: watchdog module/isolation check: %s" % d.get("reason"))
    print("  expected (-m=, resolved): %s" % d.get("expected"))
    print("  actual: %s" % d.get("actual"))
else:
    print("FAIL: watchdog killed PI -- section %r (%s) ran %.1fs > cap %.1fs (baseline=%s)" %
          (d.get("section"), d.get("source"), d.get("elapsed", 0.0), d.get("cap", 0.0), d.get("baseline")))
print("  pid: %s" % d.get("pid"))
print("  log dir: %s" % d.get("log_dir"))
if d.get("stacks"): print("  stacks: %s" % d.get("stacks"))
if d.get("screenshot"): print("  screenshot: %s" % d.get("screenshot"))
PY
   exit 1
fi

if [ "$PI_RC" -ne 0 ]; then
   print_timings
   echo "FAIL: PI load timed out (900s) or exited non-zero (rc=$PI_RC)"; exit 1
fi
print_timings
picopilot_journeys_check "$JOURNEYS_BEFORE" || exit 1
[ -s "$WATCHDOG_OK_MARKER" ] || { echo "FAIL: the watchdog never confirmed that only the dev module was mapped (see $WATCHDOG_LOG_BASE/driver.log)"; exit 1; }
echo "watchdog: $(tr -d '\n ' < "$WATCHDOG_OK_MARKER")"
[ -f "$R" ] || { echo "FAIL: no result file"; exit 1; }
cat "$R"
echo
python3 - "$R" "$SO" <<'PY' || { echo "FAIL: self-test verdict not all green"; exit 1; }
import json, os, sys
d = json.load(open(sys.argv[1]))
# The code that ran the self-test must be the -m= dev build (dladdr in RunSelfTest).
mp = d.get('modulePath') or ''
if not mp or os.path.realpath(mp) != sys.argv[2]:
    print('FAIL: the self-test ran in %r, not the dev build %r' % (mp, sys.argv[2]))
    sys.exit(1)
print('self-test ran in the dev build: %s' % mp)
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
    'stringRulesOk',
    'toolTransportOk',
    'agentToolsOk',
    'agentLoopOk', 'agentWireOk',
    'panelResizableOk', 'turnEndNotesOk',
    'turnTargetOk',
    'liveAgentOk',
    # increment 5
    'inc5SmokeOk',
    'sseParserOk',
    'byteAppendOk',
    'streamTransportOk',
    'conversationOk', 'liveConversationOk',
    'keyStoreKeyringOk',
    'configPolishOk',
    'processSafetyOk',
    'globalProcessOk',
    'runPjsrOk', 'runPjsrBreakoutOk',
    'finalFixOk',
    'pinnedOk',
    'bridgeOk',
    'rereviewFixOk',
    'reviewE4422c9Ok',
    'keyringRetryOk',
    # 0.2.0.0 image journey
    'journeySpikeOk',
    'sqliteVendorOk',
    'historyReaderOk',
    'stepStatsOk',
    'journeyStoreOk',
    'masterFactsOk',
    'journeyTrackerOk',
    'journeyExportOk',
    'journeyWriteupOk', 'liveWriteupOk',
    'journeyToolsOk', 'liveReplayOk', 'journeyUiOk',
    'journeyWiringOk',
    'histLandedOk',
    # history_step (undo / redo)
    'historyStepOk', 'liveHistoryStepOk',
    # fix/replay-file-params: file parameters in replays + workspace process icons
    'fileParamsOk',
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
    for k in ('anthropicSkipped', 'twoTurnSkipped', 'visionSkipped', 'liveAgentSkipped', 'liveConversationSkipped',
              'graxpertLiveSkipped', 'bridgeStandInSkipped', 'liveWriteupSkipped', 'liveReplaySkipped', 'liveHistoryStepSkipped',
              'mlDenoiseSkipped'):
        if d.get(k) is not False: missing.append(k + '==false (PICOPILOT_REQUIRE_LIVE=1)')
print('anthropic check: %s' % ('SKIPPED (no key)' if d.get('anthropicSkipped') else 'RAN against real API'))
print('two-turn check: %s' % ('SKIPPED (no key)' if d.get('twoTurnSkipped') else 'RAN against real API'))
print('vision check: %s' % ('SKIPPED (no key)' if d.get('visionSkipped') else 'RAN against real API, answer=%r' % d.get('visionAnswer')))
print('live agent check: %s' % ('SKIPPED (no key)' if d.get('liveAgentSkipped') else 'RAN against real API, ratio=%r log=%r' % (d.get('liveAgentRatio'), d.get('liveAgentLog'))))
print('live conversation check: %s' % ('SKIPPED (no key)' if d.get('liveConversationSkipped') else 'RAN against real API, cacheRead=%r trimThought=%r trimTransformations=%r%s' % (d.get('liveCacheRead'), d.get('liveTrimThought'), d.get('liveTrimTransformations'), ('' if d.get('liveConversationOk') else ' FAILED: %r' % d.get('liveConversationDetail', {}).get('trimLiveReason')))))
print('live write-up check: %s' % ('SKIPPED (no key)' if d.get('liveWriteupSkipped') else 'RAN against real API (claude-haiku-4-5), %r' % d.get('liveWriteupDetail')))
lr = d.get('liveReplayDetail') or {}
print('live replay check: %s' % ('SKIPPED (no key)' if d.get('liveReplaySkipped') else 'RAN against real API, recorded=%r replayed=%r final=%r requests=%r%s' % (lr.get('recorded'), lr.get('replayed'), lr.get('finalMedian'), lr.get('requests'), ('' if d.get('liveReplayOk') else ' FAILED: %r' % d.get('liveReplayError')))))
pd = d.get('pinnedDetail', {})
print('GraXpert live check: %s' % (('SKIPPED: %s' % pd.get('liveSkipReason')) if d.get('graxpertLiveSkipped') is not False else 'RAN, %r lockWaitMs=%r' % ({k: pd.get('live', {}).get(k) for k in ('seconds', 'gradientBefore', 'gradientAfter', 'log')}, pd.get('liveLock', {}).get('waitedMs'))))
bd = d.get('bridgeDetail', {})
print('GraXpert no-effect detection (stand-in): %s; digest 60 MP RGB float = %r ms; checks=%r' % (('SKIPPED: %s' % bd.get('standInSkipReason')) if d.get('bridgeStandInSkipped') is not False else 'RAN (lockWaitMs=%r)' % bd.get('lock', {}).get('waitedMs'), bd.get('digest60MP', {}).get('ms'), bd.get('checks')))
rd = d.get('rereviewFixDetail', {})
print('describe_process sizes (chars, cap %r): %r; list_processes chars=%r' % (rd.get('describeSizes', {}).get('cap'), rd.get('describeSizes', {}).get('top10'), rd.get('listProcesses', {}).get('chars')))
print('live history_step check: %s' % ('SKIPPED (no key)' if d.get('liveHistoryStepSkipped') else 'RAN against real API, positions=%r log=%r%s' % (d.get('liveHistoryStepPositions'), d.get('liveHistoryStepLog'), ('' if d.get('liveHistoryStepOk') else ' FAILED: %r' % d.get('liveHistoryStepError')))))
print('history_step checks: %r' % d.get('historyStepChecks'))
fp = d.get('fileParamsDetail', {})
print('file parameters: checks=%r; MLDenoise end-to-end: %s' % (d.get('fileParamsChecks'), ('SKIPPED: %s' % fp.get('mlSkipReason')) if d.get('mlDenoiseSkipped') is not False else 'RAN %r' % fp.get('ml')))
if d.get('liveModelSwitch') is not None:
    print('live model switch: %r' % d.get('liveModelSwitch'))
if missing:
    print('FAILED keys: ' + ', '.join(missing))
    sys.exit(1)
PY
echo "PASS: self-test verdict all green"
