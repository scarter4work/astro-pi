// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_SystemPrompt_h
#define PICopilot_SystemPrompt_h

#include "AgentTools.h"   // AgentMode

#include <pcl/String.h>

namespace pcl
{

// How the model treats the user -- in EVERY mode's prompt, verbatim. UTF-8.
extern const char* const kPICopilotToneGuidance;

// Phrases the self-test requires in kPICopilotToneGuidance (and so in every
// prompt), so the tone rules cannot silently regress.
extern const char* const kPICopilotToneMarkers[7];

// The full system prompt for one mode (run_pjsr described only when
// options.runPjsr and the mode is not Advisor).
String BuildSystemPrompt( AgentMode mode, const ToolOptions& options = ToolOptions() );

} // namespace pcl

#endif // PICopilot_SystemPrompt_h
