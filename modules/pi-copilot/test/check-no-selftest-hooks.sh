#!/usr/bin/env bash
# The shipped PICopilot-pxm.so must carry no self-test code (release.sh runs this on the build it ships).
# Self-test entry points and overrides are compiled only with -DPICOPILOT_SELFTEST_BUILD=ON (a test
# build); the release build (tools/build-env/build-modules.sh build-portable/) leaves it off. Through them a
# release module could be driven by PICOPILOT_SELFTEST_* environment variables plus executeGlobal: the
# model endpoint redirected, the user's key sent there, tool calls forced, ★ Keep auto-approved, the data
# consent granted.
#
#   check-no-selftest-hooks.sh <PICopilot-pxm.so>      exit 0: none found; 1: markers found (listed)
#
# The markers: entry-point names, the environment variables, the URL hook names, and a sentinel string that
# only a test build contains (PICopilotSelfTestBuild.cpp) -- so a test build always fails here, even if a
# future hook is named differently.
set -euo pipefail
SO="${1:?usage: check-no-selftest-hooks.sh <PICopilot-pxm.so>}"
[ -f "$SO" ] || { echo "FAIL: no such file: $SO"; exit 1; }
MARKERS=(
   "PICOPILOT-SELFTEST-BUILD-SENTINEL"
   "PICOPILOT_SELFTEST"
   "gui.turn"
   "gui.keepArm"
   "gui.panel"
   "gui.keepInFlight"
   "StartTurnForSelfTest"
   "ArmKeepForSelfTest"
   "s_messagesUrlForSelfTest"
   "s_chatKeyForSelfTest"
   "_AGENT_URL"
   "_ECHO_URL"
   "_STALL_URL"
   "_WRITEUP_URL"
   "_STREAM_BASE"
)
found=0
for m in "${MARKERS[@]}"; do
   if LC_ALL=C grep -a -q -F -- "$m" "$SO"; then
      echo "self-test marker in $SO: $m"
      found=1
   fi
done
if [ "$found" = 1 ]; then
   echo "FAIL: $SO is a self-test build or carries self-test hooks (build it without PICOPILOT_SELFTEST_BUILD)"
   exit 1
fi
echo "OK: no self-test markers in $SO (${#MARKERS[@]} checked)"
