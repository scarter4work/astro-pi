// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ViewCapture_h
#define PICopilot_ViewCapture_h

#include "AnthropicClient.h"

#include <pcl/StringList.h>
#include <pcl/View.h>

namespace pcl
{

// Chat-log note when "Include view" is on but no image window is active. UTF-8.
extern const char* const kPICopilotNoActiveImageNote;

// Chat-log note when the view is locked by a running process/script. UTF-8.
extern const char* const kPICopilotViewBusyNote;

/*
 * Captures a view for one user turn: BuildViewContext() + RenderViewPreview(),
 * then ComposeUserTurn( prompt, context?, preview? ).
 *
 * view == nullptr means "no active image": the turn is text only and
 * kPICopilotNoActiveImageNote is added to notes. A view that is locked (read
 * or write) by a running process is never touched -- locking it from the GUI
 * thread would hang PixInsight -- so the turn is text only and
 * kPICopilotViewBusyNote is added to notes. A context or preview failure
 * is added to notes (plain text, one entry per event) and the turn is composed
 * without that part -- the text always sends. A successful preview also adds
 * an "(attached ...)" note, so the user sees what was sent.
 *
 * Root thread only (View/Bitmap are UIObjects). Never throws.
 */
AnthropicMessage CaptureViewTurn( const String& prompt, const View* view, StringList& notes );

} // namespace pcl

#endif // PICopilot_ViewCapture_h
