// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PICopilotInstance.h"
#include "PICopilotSelfTest.h"

#include <pcl/Console.h>

#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

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
   // The self-test is test-only. In a shipped install there is no
   // PICOPILOT_SELFTEST_OUT in the environment, so executing the process
   // does nothing but point the user at the panel: no Settings writes, no
   // network request, no file written -- and so no predictable /tmp path for
   // a symlink attack to target (CWE-59). test/run-selftest.sh sets it.
   const char* outPath = std::getenv( "PICOPILOT_SELFTEST_OUT" );
   if ( outPath == nullptr || *outPath == '\0' )
   {
      Console().WriteLn( "PI Copilot: open the chat panel via Process > &lt;Etc&gt; > PICopilot" );
      return true;
   }

   String json;
   bool ok = RunSelfTest( json );

   // Write with O_EXCL|O_NOFOLLOW so a pre-existing file or a planted
   // symlink at that path makes open() fail closed rather than
   // following/truncating it.
   {
      int fd = ::open( outPath, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600 );
      if ( fd >= 0 )
      {
         IsoString u = json.ToUTF8();
         ssize_t w = ::write( fd, u.c_str(), u.Length() );
         (void)w;
         ::close( fd );
      }
   }

   return ok;
}

} // namespace pcl
