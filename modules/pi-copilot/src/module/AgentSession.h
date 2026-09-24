// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_AgentSession_h
#define PICopilot_AgentSession_h

#include "AgentTools.h"
#include "AnthropicClient.h"

#include <pcl/StringList.h>

#include <functional>

namespace pcl
{

// Tool rounds (one assistant tool_use turn + its tool_results) allowed per
// user message. The next tool_use response runs nothing and ends the loop.
constexpr int PICopilotMaxToolRounds = 12;

struct AgentStep
{
   enum Kind { SendAgain, Done, Failed, CapReached, Stopped };

   Kind       kind = Failed;
   String     assistantText;         // the model's text in this response (may be empty)
   bool       truncated = false;     // stop_reason == max_tokens
   StringList toolLog;               // one compact line per tool call (run, declined, failed or skipped)
   String     error;                 // Failed/Stopped-by-cancel: the request error
   bool       restoreInput = false;  // give the prompt back to the input line
   bool       toolsRan = false;      // some tool of this user message already ran (processes may have changed the image)
};

/*
 * The tool-calling loop over one conversation. UI thread only: OnResponse()
 * runs tools (views, processes, dialogs). The caller performs each HTTP
 * request (ChatThread) from History() and feeds the result back in.
 *
 * History invariants (checked by HistoryIsApiValid()): roles alternate
 * starting with user; every assistant tool_use is answered, in the NEXT
 * message, by a tool_result with its id; only the last message carries images.
 */
class AgentSession
{
public:

   using ToolRunner = std::function<ToolOutcome( const ToolCall& )>;

   const Array<AnthropicMessage>& History() const
   {
      return m_history;
   }

   // Appends the user's turn -- or, when the history ends with a user
   // tool_result turn (loop stopped, capped or failed mid-way), merges it
   // into that turn as trailing blocks so roles keep alternating -- then
   // strips older images. Resets the per-message round count.
   void BeginUserTurn( const AnthropicMessage& userTurn );

   // Feeds the result of the request built from History(). For stop_reason
   // tool_use, runs every tool_use in order through `run` (polling
   // stopRequested before each), then appends the assistant turn and the
   // tool_result turn together. onLog (optional) gets each tool line as
   // soon as it exists. Never throws.
   AgentStep OnResponse( const AnthropicResult& r, const ToolRunner& run,
                         const std::function<bool()>& stopRequested,
                         const std::function<void( const String& )>& onLog = nullptr );

   // Ends the current user message WITHOUT a response -- e.g. History() failed
   // HistoryIsApiValid() so nothing may be sent. Same rollback as a failed
   // request: before any round, the history returns to what it was before
   // BeginUserTurn(); completed rounds stay. Returns a Failed step with
   // restoreInput (and toolsRan when a tool of this message already ran).
   AgentStep AbortTurn( const String& error );

   void Clear();

private:

   Array<AnthropicMessage> m_history;
   Array<AnthropicMessage> m_snapshot;   // history before the current BeginUserTurn()
   int                     m_rounds = 0;
   bool                    m_anyToolRan = false;

   AgentStep Fail( AgentStep::Kind kind, const String& error );
};

// Structural Messages-API validity of a history about to be sent (see the
// invariants above, plus: no duplicate tool_use ids in a message, no duplicate
// tool_result for one id, no empty message or empty text block, and the last
// message must be a user message). why = the first violation, naming the
// message index / tool_use id.
bool HistoryIsApiValid( const Array<AnthropicMessage>& history, String& why );

} // namespace pcl

#endif // PICopilot_AgentSession_h
