// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ScriptTools_h
#define PICopilot_ScriptTools_h

// list_scripts, describe_script (every mode, read-only) and run_script
// (Copilot and Guided): the model may start a script that PixInsight itself
// installed and whose signature PixInsight verifies (ScriptCatalog.h). It
// picks the script by catalog id -- it never supplies a path or any code --
// and may pass string key/value arguments, checked against what the script's
// source reads (ScriptSource.h). PixInsight's core loads, authorizes and runs
// the file (Console::ExecuteScriptOn / ExecuteScriptGlobal); PI Copilot
// executes nothing itself. Every run asks the user first, in every mode
// (default: don't run).

#include "AgentTools.h"

#include <nlohmann/json.hpp>

#include <string>

namespace pcl
{

extern const char* const kScriptToolsPromptRead;   // every mode (UTF-8)
extern const char* const kScriptToolsPromptAct;    // Copilot and Guided (UTF-8)

// list_scripts, describe_script in every mode; run_script except in Advisor.
nlohmann::json ScriptToolDefinitions( AgentMode mode );
bool IsScriptTool( const std::string& name );

// Root thread. Never throws; every failure is isError with a precise message.
ToolOutcome ExecuteScriptTool( const ToolCall& call, const ToolContext& ctx );

// The run_script confirmation text (HTML, every value escaped). Pure.
struct RunScriptConfirmation
{
   String menu, scriptId, path, developerName, developerId, signedAt;
   String target;            // view id, or empty for a global run
   nlohmann::json arguments = nlohmann::json::object();
   String reason;
   String interaction;       // what to expect (ScriptDescriptionJson interaction summary)
};
String RunScriptConfirmHtml( const RunScriptConfirmation& c );

} // namespace pcl

#endif // PICopilot_ScriptTools_h
