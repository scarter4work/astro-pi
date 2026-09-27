// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ToolHelpers_h
#define PICopilot_ToolHelpers_h

#include "AgentTools.h"   // ToolOutcome

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

} // namespace pcl

#endif // PICopilot_ToolHelpers_h
