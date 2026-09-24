// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_AgentSelfTest_h
#define PICopilot_AgentSelfTest_h

#include <nlohmann/json.hpp>

namespace pcl
{

// Increment-4 headless self-test sections (native process execution,
// ApplyProcess, tool transport, tools + system prompt, agent loop, wire,
// gated live agent run). Root thread only (ImageWindow/View/ProcessInstance).
// Adds its keys to `out`; returns true iff every section passed.
bool RunAgentSelfTest( nlohmann::json& out );

} // namespace pcl

#endif // PICopilot_AgentSelfTest_h
