// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ToolHelpers_h
#define PICopilot_ToolHelpers_h

#include "AgentTools.h"   // ToolOutcome
#include "ProcessApply.h" // ApplyProcessResult

#include <pcl/String.h>
#include <pcl/View.h>

#include <nlohmann/json.hpp>

#include <string>

namespace pcl
{

// True when the view cannot be read or written right now (locked, or a
// process is running on it). Non-waiting probe; never throws. Root thread.
bool IsBusy( const View& v );

// {"type":"text","text":utf8} -- one content block of a tool result.
nlohmann::json TextBlock( const std::string& utf8 );

// in[key] when it is a string, else "".
std::string StringField( const nlohmann::json& in, const char* key );

// is_error outcome: the error text for the model, "✖ what → error: …" (cut to 200) for the log.
ToolOutcome Fail( const String& what, const String& error );

// What apply_process does with the journey step note it posted before the run
// (review M5), from the run's result. Keep: the run's History step is the
// Copilot's; Cancel: there is no such step (failed, or no History step on the
// target); FlagNoEffect: the step it left changed nothing (T-graxpert).
enum class JourneyNoteAction { Keep, Cancel, FlagNoEffect };
JourneyNoteAction JourneyNoteActionFor( const ApplyProcessResult& r );

} // namespace pcl

#endif // PICopilot_ToolHelpers_h
