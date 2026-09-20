// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PICopilotInstance.h"
#include "PICopilotSelfTest.h"

#include <pcl/File.h>

namespace pcl
{

PICopilotInstance::PICopilotInstance( const MetaProcess* m )
   : ProcessImplementation( m )
{
}

PICopilotInstance::PICopilotInstance( const PICopilotInstance& x )
   : ProcessImplementation( x )
{
   Assign( x );
}

void PICopilotInstance::Assign( const ProcessImplementation& )
{
   // No parameters yet — nothing to assign. Later increments add the
   // process's persisted state here.
}

bool PICopilotInstance::IsHistoryUpdater( const View& ) const
{
   return false;  // Global process — doesn't modify existing views
}

UndoFlags PICopilotInstance::UndoMode( const View& ) const
{
   return UndoFlag::DefaultMode;
}

bool PICopilotInstance::CanExecuteOn( const View&, String& whyNot ) const
{
   whyNot = "PICopilot is a global process. It cannot be executed on views.";
   return false;
}

bool PICopilotInstance::CanExecuteGlobal( String& /*whyNot*/ ) const
{
   return true;
}

bool PICopilotInstance::ExecuteGlobal()
{
   String json;
   bool ok = RunSelfTest( json );
   File f;
   f.CreateForWriting( "/tmp/.picopilot_selftest.json" );
   f.OutTextLn( IsoString( json ) );
   f.Close();
   return ok;
}

} // namespace pcl
