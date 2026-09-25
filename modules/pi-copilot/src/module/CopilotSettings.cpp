// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "CopilotSettings.h"
#include "AnthropicClient.h"   // PICOPILOT_DEFAULT_MODEL
#include "ModelCatalog.h"
#include "Utf8.h"

#include <pcl/Settings.h>

namespace pcl { namespace CopilotSettings {

namespace
{
const char* const kModelKey   = "PICopilot/Model";
const char* const kRunPjsrKey = "PICopilot/RunPjsrEnabled";
const char* const kSideKey    = "PICopilot/PanelSide";
}

IsoString LoadModel( String* note )
{
   if ( note != nullptr )
      note->Clear();
   String s;
   Settings::Read( kModelKey, s );
   const IsoString id( U8( s ).c_str() );
   if ( FindModel( id ) != nullptr )
      return id;
   if ( !s.IsEmpty() && note != nullptr )
   {
      const ModelInfo* def = FindModel( PICOPILOT_DEFAULT_MODEL );
      *note = "The saved model \"" + s + "\" is not offered by this version of PI Copilot; using "
            + String( def != nullptr ? def->label : PICOPILOT_DEFAULT_MODEL )
            + " instead. Choose a model in PI Copilot's settings to keep this choice.";
   }
   return IsoString( PICOPILOT_DEFAULT_MODEL );
}

void SaveModel( const IsoString& id )
{
   Settings::Write( kModelKey, String( id ) );
}

bool LoadRunPjsrEnabled()
{
   bool enabled = false;
   Settings::Read( kRunPjsrKey, enabled );
   return enabled;
}

void SaveRunPjsrEnabled( bool enabled )
{
   Settings::Write( kRunPjsrKey, enabled );
}

PanelSide LoadPanelSide()
{
   int side = 0;
   Settings::Read( kSideKey, side );
   return side == 1 ? PanelSide::Left : PanelSide::Right;
}

void SavePanelSide( PanelSide side )
{
   Settings::Write( kSideKey, int( side ) );
}

} } // namespace pcl::CopilotSettings
