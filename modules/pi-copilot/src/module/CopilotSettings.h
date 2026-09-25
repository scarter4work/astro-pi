// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_CopilotSettings_h
#define PICopilot_CopilotSettings_h

#include "PanelPlacement.h"

#include <pcl/String.h>

namespace pcl { namespace CopilotSettings {

// Persisted ⚙ settings (PixInsight local Settings, prefix PICopilot/).
// LoadModel: unknown or unset -> PICOPILOT_DEFAULT_MODEL. note (optional) gets
// a plain-text sentence when a saved id is not in the catalog (never a 400 on
// every request), else it is cleared.
IsoString LoadModel( String* note = nullptr );
void      SaveModel( const IsoString& id );
bool      LoadRunPjsrEnabled();        // default false: run_pjsr is off until the user allows scripts
void      SaveRunPjsrEnabled( bool enabled );
PanelSide LoadPanelSide();             // default Right
void      SavePanelSide( PanelSide side );

} } // namespace pcl::CopilotSettings

#endif // PICopilot_CopilotSettings_h
