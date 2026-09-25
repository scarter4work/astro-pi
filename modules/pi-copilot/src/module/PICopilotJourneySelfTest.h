// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_JourneySelfTest_h
#define PICopilot_JourneySelfTest_h

#include <nlohmann/json.hpp>

namespace pcl
{

// Image journey (0.2.0.0) sections J0..J10. Called from RunSelfTest() after
// the increment-5 block. Root thread only.
bool RunJourneySelfTest( nlohmann::json& out );

// Multi-phase harness (test/selftest.js). When $PICOPILOT_SELFTEST_PHASE names
// an existing file {"phase": id, "payload": {...}}, runs that phase's handler,
// keeps its result for the main run's sections, and returns true (the caller
// then writes no self-test result). false: no phase pending, run the self-test.
// Only called when $PICOPILOT_SELFTEST_OUT is set. Root thread only.
bool RunPendingSelfTestPhase();

} // namespace pcl

#endif // PICopilot_JourneySelfTest_h
