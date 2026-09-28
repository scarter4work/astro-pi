// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_WorkspaceIcons_h
#define PICopilot_WorkspaceIcons_h

// The workspace's process icons, for the model (fix/replay-file-params):
// list_process_icons and get_process_icon, read-only, in every mode. Icons are
// read through PCL (ProcessInstance::Icons / FromIcon) at each call; no
// ProcessInstance is kept past it. Values of FILE parameters reach the model
// as {"file": name, "available": ...} (never a directory); apply_process takes
// the same {"file": name} back and PI Copilot sets the full path
// (FileReferences.h).

#include "AgentTools.h"
#include "FileReferences.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace pcl
{

extern const char* const kWorkspaceIconPrompt;   // every mode (UTF-8)

nlohmann::json WorkspaceIconToolDefinitions();    // list_process_icons, get_process_icon
bool IsWorkspaceIconTool( const std::string& name );
// Root thread. Never throws; every failure is isError with a precise message.
ToolOutcome ExecuteWorkspaceIconTool( const ToolCall& call, const ToolContext& ctx );

// Every FILE-parameter value in the workspace's process icons (containers
// included; INPUT files only: FileRoleOf), found in "workspace icon <id>".
// Icons of deny/confirmAlways processes are never opened. Root thread. Never throws.
void AddWorkspaceIconFileCandidates( std::vector<FileCandidate>& to );

// Self-test only: how many times the icon-reading script has run (review M3).
int WorkspaceIconScriptRunsForSelfTest();

} // namespace pcl

#endif // PICopilot_WorkspaceIcons_h
