// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PICopilotInstance.h"
#include "ProcessActivity.h"

#include <pcl/Console.h>

#ifdef PICOPILOT_SELFTEST_BUILD
#include "DataConsent.h"
#include "PICopilotJourneySelfTest.h"
#include "PICopilotSelfTest.h"
#include "ProcessApply.h"
#include "Utf8.h"

#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#endif

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
   // PixInsight is still inside this process execution for a moment after we
   // return (measured: a timer tick there applies without a History step), so
   // its end is process activity for the tool loop's quiet period (ProcessActivity.h).
   struct ActivityOnReturn
   {
      ~ActivityOnReturn() { NoteProcessActivity(); }
   } activityOnReturn;

#ifndef PICOPILOT_SELFTEST_BUILD
   // A release build (no PICOPILOT_SELFTEST_BUILD): executing the process only points the user at the
   // panel. No self-test code is compiled in, so no environment variable can reach any (re-review 1: the
   // self-test phases could redirect the model endpoint, send the user's key there, force tool calls and
   // auto-approve keeps; CPD N6: the consent grant below).
   Console().WriteLn( "PI Copilot: open the chat panel via Process > &lt;Etc&gt; > PICopilot" );
   return true;
#else
   // A self-test build (test/run-selftest.sh and the GUI tests). Without
   // PICOPILOT_SELFTEST_OUT in the environment, executing the process
   // does nothing but point the user at the panel: no Settings writes, no
   // network request, no file written -- and so no predictable /tmp path for
   // a symlink attack to target (CWE-59). test/run-selftest.sh sets it.
   const char* outPath = std::getenv( "PICOPILOT_SELFTEST_OUT" );
   if ( outPath == nullptr || *outPath == '\0' )
   {
      Console().WriteLn( "PI Copilot: open the chat panel via Process > &lt;Etc&gt; > PICopilot" );
      return true;
   }

   // CPD rule 6.2 (DataConsent.h): requests are refused until the user agreed.
   // The harness runs in an isolated, wiped settings slot and agrees there, so
   // its loopback and gated live requests can run; section S7 revokes it to
   // prove the refusal, then agrees again.
   DataConsent::Grant();

   // Multi-phase harness: a top-level check phase, not the final run.
   if ( RunPendingSelfTestPhase() )
      return true;

   String json;
   // Inside this call no process is ever recorded in History (harness fact):
   // in-process ApplyProcess calls are counted, not failed (ProcessApply.h).
   SetInProcessAppliesExpectedForSelfTest( true );
   bool ok = false;
   try
   {
      ok = RunSelfTest( json );
   }
   catch ( ... )
   {
      SetInProcessAppliesExpectedForSelfTest( false );
      throw;
   }
   SetInProcessAppliesExpectedForSelfTest( false );

   // Write with O_EXCL|O_NOFOLLOW so a pre-existing file or a planted
   // symlink at that path makes open() fail closed rather than
   // following/truncating it.
   {
      int fd = ::open( outPath, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600 );
      if ( fd >= 0 )
      {
         std::string u = U8( json );
         ssize_t w = ::write( fd, u.data(), u.size() );
         (void)w;
         ::close( fd );
      }
   }

   return ok;
#endif // PICOPILOT_SELFTEST_BUILD
}

} // namespace pcl
