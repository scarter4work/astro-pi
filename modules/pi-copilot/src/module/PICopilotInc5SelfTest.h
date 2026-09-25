// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_Inc5SelfTest_h
#define PICopilot_Inc5SelfTest_h

#include <nlohmann/json.hpp>

namespace pcl
{

// Increment-5 headless self-test sections (platform smoke, SSE, streaming,
// conversation, keyring, config, safety policy, global processes, run_pjsr,
// gated live checks). Root thread only. Adds its keys to `out`; returns true
// iff every section passed.
bool RunInc5SelfTest( nlohmann::json& out );

} // namespace pcl

#endif // PICopilot_Inc5SelfTest_h
