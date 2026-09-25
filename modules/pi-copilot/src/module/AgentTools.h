// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_AgentTools_h
#define PICopilot_AgentTools_h

#include <pcl/String.h>
#include <pcl/View.h>

#include <nlohmann/json.hpp>

#include <functional>
#include <set>
#include <string>

namespace pcl
{

// Assistant mode. Values == the panel's Mode_ComboBox item indices.
enum class AgentMode { Copilot = 0, Advisor = 1, Guided = 2 };

// Out-of-range indices (e.g. a corrupt Setting) fall back to Copilot.
AgentMode AgentModeFromIndex( int index );

constexpr size_type PICopilotToolLogParamChars   = 120;   // parameters shown in a chat-log tool line
constexpr size_type PICopilotConfirmChangesChars = 1500;  // parameter text in the Guided dialog

// Which optional tools a message offers. run_pjsr: the user allowed scripts
// in ⚙ (never offered in Advisor, whatever this says).
struct ToolOptions
{
   bool runPjsr = false;
};

// The Anthropic "tools" array for a mode: list_processes, describe_process,
// get_view_context, and -- except in Advisor -- apply_process, plus run_pjsr
// (last) when options.runPjsr.
nlohmann::json ToolDefinitions( AgentMode mode, const ToolOptions& options = ToolOptions() );

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

// Asked before a process run in Guided mode, and in EVERY mode when the
// process safety policy says confirm (ProcessSafety.h); true = the user approved.
using ConfirmApplyFn = std::function<bool( const String& processId, const String& viewId, const String& changes )>;

// run_pjsr: asked for EVERY script, in every mode; true = the user clicked Run script.
using ConfirmScriptFn = std::function<bool( const String& purpose, const String& code, const IsoString& targetViewId )>;

struct ToolContext
{
   AgentMode mode = AgentMode::Copilot;

   // FullId of the view captured when the user sent this message (the one
   // whose context/preview went with it); empty when no image was active.
   // The default target of get_view_context and apply_process, re-resolved
   // by id at every call -- NEVER "whatever window is active now": requests
   // take tens of seconds and the panel is non-modal.
   IsoString turnViewId;

   // FullIds that get_view_context inspected during this user message
   // (owned by the caller, emptied at each new user message). apply_process
   // accepts a view_id other than turnViewId only if it is in here. May be
   // null: then only the turn's own view can be targeted.
   std::set<std::string>* inspectedViews = nullptr;

   ConfirmApplyFn confirm;   // required in Guided mode and for safety-policy confirmations

   // The user allowed scripts (⚙ Allow scripts) for this message.
   bool runPjsr = false;

   // Required for run_pjsr: shows the whole script; nothing runs without a yes.
   ConfirmScriptFn confirmScript;
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
