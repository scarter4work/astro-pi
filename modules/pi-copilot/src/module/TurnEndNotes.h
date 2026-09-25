// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_TurnEndNotes_h
#define PICopilot_TurnEndNotes_h

#include "AgentSession.h"

#include <pcl/StringList.h>

namespace pcl
{

// What the panel shows when one user message ends (pure logic, so the
// self-test can check it without a GUI).
struct TurnEndView
{
   StringList notes;                // plain-text chat-log notes, in order (the panel escapes them)
   bool       restoreInput = false; // give the typed prompt back to the input line
   bool       offerClear = false;   // the conversation cannot continue: point at Clear
};

// httpStatus: the last request's HTTP status (0 = no HTTP reply, e.g. a
// transport error, a cancel, or a turn aborted before sending).
// - Failed: one sentence per step.errorKind (cause + next step; the HTTP
//   status only when there was one -- never "Error 0"); errorKind None (not
//   from a request) keeps "Error <status>: <error>" / "Error: <error>".
//   Always restores the input.
// - Stopped: "(stopped)".
// - partialReplyCut: a streamed reply was already shown when the request
//   ended without success; the note says it is not kept (Stopped: in the
//   stop note; Failed: a note before the cause).
// - CapReached: explains the PICopilotMaxToolRounds limit and that the next
//   message lets it continue or summarize.
// - toolsRan on Failed/Stopped: processes already applied stay applied (undo via History).
// - needsClear: the history cannot be sent any more; press New chat.
TurnEndView DescribeTurnEnd( const AgentStep& step, int httpStatus, bool partialReplyCut = false );

} // namespace pcl

#endif // PICopilot_TurnEndNotes_h
