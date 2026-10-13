// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ScriptSelfTest_h
#define PICopilot_ScriptSelfTest_h

#include <nlohmann/json.hpp>

namespace pcl
{

// 0.2.2.0 compliance self-test (CPD rules 5.x, 6.2): run_pjsr is gone (no
// model text reaches EvaluateScript), the module's own scripts embed data only
// as literals, the static script reader, run_script eligibility (the four
// proofs), argument plumbing into Parameters, describe+run acceptance on a
// real PixInsight script, the Anthropic consent gate, FITS path scrubbing and
// the retired-setting cleanup. Root thread only. Adds its keys to `out`;
// returns true iff every section passed.
bool RunScriptSelfTest( nlohmann::json& out );

} // namespace pcl

#endif // PICopilot_ScriptSelfTest_h
