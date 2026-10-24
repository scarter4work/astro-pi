#!/usr/bin/env bash
# The shipped PICopilot-pxm.so must carry no self-test code (release.sh runs this, through
# check-tarball-no-selftest-hooks.sh, on the package it ships). Self-test entry points and overrides are
# compiled only with -DPICOPILOT_SELFTEST_BUILD=ON (a test build); the release build
# (tools/build-env/build-modules.sh build-portable/) leaves it off. Through them a release module could be
# driven by PICOPILOT_SELFTEST_* environment variables plus executeGlobal: the model endpoint redirected,
# the user's key sent there, tool calls forced, ★ Keep auto-approved, the data consent granted.
#
#   check-no-selftest-hooks.sh <PICopilot-pxm.so>      exit 0: none found; 1: markers found, or the file
#                                                      cannot be checked (fails closed)
#
# The markers: a sentinel only a test build contains (PICopilotSelfTestBuild.cpp); the generic names every
# guarded symbol carries (ForSelfTest: all 36 Set*/...ForSelfTest hooks and s_*ForSelfTest overrides;
# SelfTest: the self-test sources' functions; JourneySpike: the probe); the environment variables; the
# phase names; the URL hook names. Symbol names come from .symtab (the module ships unstripped); the
# sentinel, PICOPILOT_SELFTEST and the phase / URL names are string literals and survive stripping.
set -uo pipefail
SO="${1:?usage: check-no-selftest-hooks.sh <PICopilot-pxm.so>}"
if [ ! -f "$SO" ] || [ ! -r "$SO" ] || [ ! -s "$SO" ]; then
   echo "FAIL: cannot check $SO: missing, unreadable or empty"; exit 1
fi
if [ "$(head -c 4 "$SO" 2>/dev/null | od -An -tx1 | tr -d ' \n')" != "7f454c46" ]; then
   echo "FAIL: cannot check $SO: not an ELF file"; exit 1
fi
MARKERS=(
   "PICOPILOT-SELFTEST-BUILD-SENTINEL"
   "ForSelfTest"
   "SelfTest"
   "JourneySpike"
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
   LC_ALL=C grep -a -q -F -- "$m" "$SO"
   rc=$?
   case "$rc" in
      0) echo "self-test marker in $SO: $m"; found=1 ;;
      1) ;;
      *) echo "FAIL: cannot check $SO: grep exit $rc on marker $m"; exit 1 ;;
   esac
done
if [ "$found" = 1 ]; then
   echo "FAIL: $SO is a self-test build or carries self-test hooks (build it without PICOPILOT_SELFTEST_BUILD)"
   exit 1
fi
echo "OK: no self-test markers in $SO (${#MARKERS[@]} checked)"
