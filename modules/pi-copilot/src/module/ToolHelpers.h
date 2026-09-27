// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ToolHelpers_h
#define PICopilot_ToolHelpers_h

#include <pcl/View.h>

namespace pcl
{

// True when the view cannot be read or written right now (locked, or a
// process is running on it). Non-waiting probe; never throws. Root thread.
bool IsBusy( const View& v );

} // namespace pcl

#endif // PICopilot_ToolHelpers_h
