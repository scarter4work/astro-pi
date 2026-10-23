// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.
//
// Compiled ONLY into a self-test build (CMake option PICOPILOT_SELFTEST_BUILD, src/module/CMakeLists.txt).
// The sentinel marks such a module: test/check-no-selftest-hooks.sh (run by release.sh) refuses to ship
// one, and the test harnesses refuse to run without one (a release build has no self-test to run).

#ifndef PICOPILOT_SELFTEST_BUILD
#error "PICopilotSelfTestBuild.cpp belongs to the self-test build only"
#endif

extern "C" __attribute__(( used, visibility( "hidden" ) ))
const char picopilot_selftest_build_sentinel[] = "PICOPILOT-SELFTEST-BUILD-SENTINEL: this module carries self-test hooks";
