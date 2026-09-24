// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_AgentTools_h
#define PICopilot_AgentTools_h

#include <pcl/String.h>
#include <pcl/View.h>

#include <nlohmann/json.hpp>

#include <functional>
#include <string>

namespace pcl
{

// Assistant mode. Values == the panel's Mode_ComboBox item indices.
enum class AgentMode { Copilot = 0, Advisor = 1, Guided = 2 };

// Out-of-range indices (e.g. a corrupt Setting) fall back to Copilot.
AgentMode AgentModeFromIndex( int index );

constexpr size_type PICopilotToolLogParamChars   = 120;   // parameters shown in a chat-log tool line
constexpr size_type PICopilotConfirmChangesChars = 1500;  // parameter text in the Guided dialog

// The Anthropic "tools" array for a mode: list_processes, describe_process,
// get_view_context, and -- except in Advisor -- apply_process.
nlohmann::json ToolDefinitions( AgentMode mode );

struct ToolCall
{
   std::string    id;      // tool_use id
   std::string    name;
   nlohmann::json input;   // object
};

struct ToolOutcome
{
   bool           isError = false;
   nlohmann::json content = nlohmann::json::array();  // tool_result content blocks (text / image), never empty
   String         logLine;                            // compact plain-text chat-log line
   bool           mutated = false;                    // an image was changed (apply_process ran to completion)
};

// Guided mode: asked before each apply_process; true = the user approved.
using ConfirmApplyFn = std::function<bool( const String& processId, const String& viewId, const String& changes )>;

struct ToolContext
{
   AgentMode             mode = AgentMode::Copilot;
   std::function<View()> activeView;   // default target, resolved at call time (may return View::Null())
   ConfirmApplyFn        confirm;      // required in Guided mode
};

// Executes one tool call. Root thread only (views, processes, previews,
// and the Guided dialog). Never throws: every failure is isError=true with a
// precise, model-correctable message in content[0].
ToolOutcome ExecuteTool( const ToolCall& call, const ToolContext& ctx );

// {"type":"tool_result","tool_use_id":..,"content":[..],"is_error":..}
nlohmann::json ToolResultBlock( const std::string& toolUseId, const ToolOutcome& outcome );

// The small, re-sendable part of a BuildViewContext() result:
// viewId, fullId, geometry, channelStats (no FITS keywords).
nlohmann::json CollapsedViewContext( const nlohmann::json& fullContext );

} // namespace pcl

#endif // PICopilot_AgentTools_h
