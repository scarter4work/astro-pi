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

// Models an earlier version offered (ruling 2026-09-25 removed them): named
// by their label in the migration note. Any other unknown id is quoted as is.
struct RemovedModel { const char* id; const char* label; };
const RemovedModel kRemovedModels[] =
{
   { "claude-opus-4-8",  "Claude Opus 4.8"  },
   { "claude-fable-5-1", "Claude Fable 5.1" },
   { "claude-haiku-4-5", "Claude Haiku 4.5" }
};
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
      String old = s;
      for ( const RemovedModel& r : kRemovedModels )
         if ( id == r.id )
            old = r.label;
      const ModelInfo* def = FindModel( PICOPILOT_DEFAULT_MODEL );
      *note = old + " is no longer offered; using "
            + String( def != nullptr ? def->label : PICOPILOT_DEFAULT_MODEL );
      // Handed out once: the caller shows it, so the setting now names the
      // model actually in use.
      SaveModel( PICOPILOT_DEFAULT_MODEL );
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
