// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "TurnEndNotes.h"

namespace pcl
{

TurnEndView DescribeTurnEnd( const AgentStep& step, int httpStatus )
{
   TurnEndView v;
   switch ( step.kind )
   {
   case AgentStep::Done:
   case AgentStep::SendAgain:
      return v;

   case AgentStep::CapReached:
      v.notes.Add( String().Format( "(paused: this message reached the limit of %d tool steps. "
                                    "Your next message lets it continue -- e.g. \"continue\" -- "
                                    "or ask it to summarize what it did.)", PICopilotMaxToolRounds ) );
      break;

   case AgentStep::Stopped:
      v.notes.Add( "(stopped)" );
      v.restoreInput = step.restoreInput;
      break;

   case AgentStep::Failed:
      v.notes.Add( (httpStatus > 0 ? "Error " + String( httpStatus ) + ": " : String( "Error: " )) + step.error );
      // Every failure gives the typed prompt back for a resend.
      v.restoreInput = true;
      break;
   }

   if ( step.toolsRan && (step.kind == AgentStep::Failed || step.kind == AgentStep::Stopped) )
      v.notes.Add( "(processes already applied to the image stay applied -- undo them from the view's "
                   "History if you don't want them; resending the prompt may apply them again)" );

   if ( step.needsClear )
   {
      v.offerClear = true;
      v.notes.Add( "(this conversation can't be sent to the API any more -- press Clear to start a new chat)" );
   }
   return v;
}

} // namespace pcl
