// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "TurnEndNotes.h"

namespace pcl
{

namespace
{

// One plain sentence per failure cause, with the next step. httpStatus 0 (no
// HTTP reply) is never shown as a status: never "Error 0".
String FailureNote( const AgentStep& s, int httpStatus )
{
   switch ( s.errorKind )
   {
   case RequestErrorKind::TimedOut:
      return "The request took too long and was stopped (" + s.error + "). Send the message again, or ask for something smaller.";
   case RequestErrorKind::Stalled:
      return "The reply stalled and was stopped (" + s.error + "). Send the message again.";
   case RequestErrorKind::Network:
      return "Could not reach the Anthropic API (" + s.error + "). Check the internet connection, then send the message again.";
   case RequestErrorKind::Stream:
      return "The reply was cut off by the Anthropic API (" + s.error + "). Send the message again.";
   case RequestErrorKind::BadReply:
      return "Unexpected reply from the Anthropic API: " + s.error;
   case RequestErrorKind::Build:
      return "PI Copilot could not build the request (" + s.error + "). Nothing was sent; send the message again, "
             "or press New chat if this keeps happening.";
   case RequestErrorKind::Internal:
      return "Internal error in PI Copilot (" + s.error + "). Send the message again.";
   case RequestErrorKind::Cancelled:   // a cancel is normally a Stopped step; worded anyway
      return "The request was cancelled (" + s.error + "). Send the message again.";
   case RequestErrorKind::Http:
      {
         String n = String( "Anthropic API error" ) + (httpStatus > 0 ? " " + String( httpStatus ) : String()) + ": " + s.error;
         if ( httpStatus == 401 )
            n += " (check your API key in PI Copilot's settings)";
         else if ( httpStatus == 429 || httpStatus == 529 )
            n += " (the service is busy; wait a moment, then send again)";
         return n;
      }
   case RequestErrorKind::None:   // no cause recorded: the increment-4 form
      break;
   }
   // No default: -Wswitch flags a new RequestErrorKind until it is worded here.
   return (httpStatus > 0 ? "Error " + String( httpStatus ) + ": " : String( "Error: " )) + s.error;
}

} // namespace

TurnEndView DescribeTurnEnd( const AgentStep& step, int httpStatus, bool partialReplyCut )
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
      v.notes.Add( partialReplyCut ? String( "(stopped -- the partial reply above is not kept in the conversation)" )
                                   : String( "(stopped)" ) );
      v.restoreInput = step.restoreInput;
      break;

   case AgentStep::Failed:
      if ( partialReplyCut )
         v.notes.Add( "(the partial reply above was interrupted; it is not kept in the conversation)" );
      v.notes.Add( FailureNote( step, httpStatus ) );
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
      v.notes.Add( "(this conversation can't be sent to the API any more -- press New chat to start fresh)" );
   }
   return v;
}

} // namespace pcl
