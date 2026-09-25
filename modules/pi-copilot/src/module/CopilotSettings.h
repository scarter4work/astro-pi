// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_CopilotSettings_h
#define PICopilot_CopilotSettings_h

#include "PanelPlacement.h"

#include <pcl/String.h>

namespace pcl { namespace CopilotSettings {

// Persisted ⚙ settings (PixInsight local Settings, prefix PICopilot/).
// LoadModel: unknown or unset -> PICOPILOT_DEFAULT_MODEL (never a 400 on every
// request). When a saved id is no longer offered (or was never known) and the
// caller passes note, note gets "<old model> is no longer offered; using
// Claude Opus 5.5" and the setting is rewritten to the default, so the note is
// shown exactly once. Without note nothing is rewritten (the note is kept for
// the next caller that shows it). Otherwise note is cleared.
IsoString LoadModel( String* note = nullptr );
void      SaveModel( const IsoString& id );
bool      LoadRunPjsrEnabled();        // default false: run_pjsr is off until the user allows scripts
void      SaveRunPjsrEnabled( bool enabled );
PanelSide LoadPanelSide();             // default Right
void      SavePanelSide( PanelSide side );

} } // namespace pcl::CopilotSettings

#endif // PICopilot_CopilotSettings_h
