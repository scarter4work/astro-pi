// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "AgentSession.h"
#include "Utf8.h"
#include "VisionTurn.h"

#include <pcl/Exception.h>

#include <exception>
#include <set>
#include <string>
#include <vector>

namespace pcl
{

namespace
{

String S16( const std::string& s )
{
   return String::UTF8ToUTF16( s.c_str() );
}

// "✖ <tool> → <suffix>"
String CallLine( const ToolCall& c, const String& suffix )
{
   return String::UTF8ToUTF16( "\xE2\x9C\x96 " ) + S16( c.name ) + String::UTF8ToUTF16( " \xE2\x86\x92 " ) + suffix;
}

ToolOutcome NotExecuted( const std::string& why )
{
   ToolOutcome o;
   o.isError = true;
   o.content.push_back( { { "type", "text" }, { "text", why } } );
   return o;
}

std::string BlockType( const nlohmann::json& b )
{
   if ( b.is_object() )
   {
      const auto t = b.find( "type" );
      if ( t != b.end() && t->is_string() )
         return t->get<std::string>();
   }
   return std::string();
}

bool IsEmptyTextBlock( const nlohmann::json& b )
{
   if ( BlockType( b ) != "text" )
      return false;
   const auto t = b.find( "text" );
   return t == b.end() || !t->is_string() || t->get_ref<const std::string&>().empty();
}

// A message as a content-block array (for merging a user turn into a
// trailing tool_result turn). Never produces an empty text block.
nlohmann::json ToBlocks( const AnthropicMessage& m )
{
   if ( m.blocks.is_array() )
      return m.blocks;
   nlohmann::json b = nlohmann::json::array();
   if ( !m.imageJpegBase64.IsEmpty() )
      b.push_back( JpegImageBlock( m.imageJpegBase64 ) );
   if ( !m.content.IsEmpty() || b.empty() )
      b.push_back( { { "type", "text" }, { "text", U8( m.content ) } } );
   return b;
}

// The reply's blocks as they may be stored in history: empty text blocks
// removed (the API rejects them when re-sent), and -- for a reply that did
// not stop for tool use (e.g. max_tokens cut a call) -- every tool_use block
// removed, so no unanswered tool_use ever enters the history. An emptied
// turn gets a placeholder that says what actually happened.
nlohmann::json StorableAssistantBlocks( const AnthropicResult& r )
{
   const bool keepToolUse = r.stopReason == "tool_use";
   bool droppedToolUse = false;
   bool hasContent = false;   // a kept block other than thinking / redacted_thinking
   nlohmann::json kept = nlohmann::json::array();
   for ( const nlohmann::json& b : r.contentBlocks )
   {
      if ( IsEmptyTextBlock( b ) )
         continue;
      if ( !keepToolUse && BlockType( b ) == "tool_use" )
      {
         droppedToolUse = true;
         continue;
      }
      const std::string type = BlockType( b );
      hasContent = hasContent || (type != "thinking" && type != "redacted_thinking");
      kept.push_back( b );
   }
   // Thinking blocks alone never make a turn: the API may drop them
   // (drop_block binding), which would leave an empty assistant turn. The
   // placeholder goes after them (thinking stays first, verbatim).
   if ( !hasContent )
   {
      const std::string note = (droppedToolUse && r.truncated)
         ? std::string( "[reply cut off (max_tokens) before a tool call completed]" )
         : droppedToolUse
         ? "[reply ended (stop_reason " + (r.stopReason.empty() ? std::string( "missing" ) : r.stopReason)
           + ") with an incomplete tool call]"
         : "[empty reply (stop_reason " + (r.stopReason.empty() ? std::string( "missing" ) : r.stopReason) + ")]";
      kept.push_back( { { "type", "text" }, { "text", note } } );
   }
   return kept;
}

} // namespace

void AgentSession::Clear()
{
   m_history.Clear();
   m_snapshot.Clear();
   m_rounds = 0;
   m_imageChanged = false;
   m_trimmed = 0;
}

void AgentSession::BeginUserTurn( const AnthropicMessage& userTurn )
{
   m_snapshot = m_history;
   m_rounds = 0;
   m_imageChanged = false;
   if ( !m_history.IsEmpty() && m_history[m_history.Length()-1].role == "user" )
   {
      AnthropicMessage& last = m_history[m_history.Length()-1];
      nlohmann::json merged = ToBlocks( last );          // tool_results first (API rule)
      for ( const nlohmann::json& b : ToBlocks( userTurn ) )
         merged.push_back( b );
      last.blocks = std::move( merged );
      last.content.Clear();
      last.imageJpegBase64.Clear();
   }
   else
      m_history.Add( userTurn );
   StripOlderImages( m_history );
   m_trimmed += TrimHistoryToBudget( m_history, PICopilotHistoryTokenBudget, PICopilotHistoryTrimTarget );
}

AgentStep AgentSession::Fail( AgentStep::Kind kind, const String& error )
{
   AgentStep s;
   s.kind = kind;
   s.error = error;
   s.toolsRan = m_imageChanged;
   if ( m_rounds == 0 )
   {
      m_history = m_snapshot;        // nothing ran: as if never sent
      m_trimmed = 0;                 // the snapshot is untrimmed: nothing to report
      s.restoreInput = true;
   }
   else
   {
      // Completed rounds stay (their processes really ran); the history ends
      // with a tool_result turn the next BeginUserTurn() merges into.
      s.restoreInput = kind == AgentStep::Failed;
   }
   return s;
}

AgentStep AgentSession::AbortTurn( const String& error )
{
   // The history was found invalid, and the invalid part is usually in this
   // message's own rounds (e.g. a repeated tool_use id from the model), so
   // keeping them would leave the session unsendable: always go back to the
   // snapshot. toolsRan still reports an image change those rounds made.
   AgentStep s;
   s.kind = AgentStep::Failed;
   s.error = error;
   s.toolsRan = m_imageChanged;
   s.restoreInput = true;
   m_history = m_snapshot;
   m_rounds = 0;
   m_trimmed = 0;   // the snapshot is untrimmed: nothing to report
   String why;
   s.needsClear = !HistoryPrefixIsApiValid( m_history, why );
   return s;
}

AgentStep AgentSession::OnResponse( const AnthropicResult& r, const ToolRunner& run,
                                    const std::function<bool()>& stopRequested,
                                    const std::function<void( const String& )>& onLog )
{
   try
   {
      if ( !r.ok )
         return Fail( r.cancelled ? AgentStep::Stopped : AgentStep::Failed, r.error );

      std::vector<ToolCall> calls;
      if ( r.contentBlocks.is_array() )
         for ( const nlohmann::json& b : r.contentBlocks )
            if ( BlockType( b ) == "tool_use" )
               calls.push_back( ToolCall{ b.value( "id", std::string() ), b.value( "name", std::string() ),
                                          b.contains( "input" ) ? b["input"] : nlohmann::json::object() } );

      AnthropicMessage assistant;
      assistant.role = "assistant";
      assistant.content = r.text;
      if ( r.contentBlocks.is_array() )
         assistant.blocks = StorableAssistantBlocks( r );

      if ( r.stopReason != "tool_use" )
      {
         if ( assistant.blocks.is_null() && assistant.content.IsEmpty() )
            return Fail( AgentStep::Failed, "the reply has neither text nor content blocks" );
         AgentStep s;
         s.assistantText = r.text;
         s.truncated = r.truncated;
         s.toolsRan = m_imageChanged;
         m_history.Add( assistant );
         // A long reply can push the history over the budget.
         m_trimmed += TrimHistoryToBudget( m_history, PICopilotHistoryTokenBudget, PICopilotHistoryTrimTarget );
         s.kind = AgentStep::Done;
         return s;
      }

      if ( calls.empty() )
         return Fail( AgentStep::Failed, "the model asked to use a tool (stop_reason tool_use) but sent no tool_use block" );

      AgentStep s;
      s.assistantText = r.text;
      s.truncated = r.truncated;
      auto log = [&]( const String& line )
      {
         s.toolLog.Add( line );
         if ( onLog )
            onLog( line );
      };
      auto stopNow = [&]() { return stopRequested && stopRequested(); };

      const bool capped = m_rounds >= PICopilotMaxToolRounds;
      bool stopped = false;
      int executed = 0;   // calls of this response run (or attempted) so far
      nlohmann::json results = nlohmann::json::array();
      for ( const ToolCall& call : calls )
      {
         if ( capped )
         {
            results.push_back( ToolResultBlock( call.id, NotExecuted(
               "not executed: the limit of " + std::to_string( PICopilotMaxToolRounds )
               + " tool rounds for one user message was reached; summarize your progress for the user" ) ) );
            log( CallLine( call, "skipped (tool-round limit reached)" ) );
            continue;
         }
         if ( executed >= PICopilotMaxToolCallsPerStep )
         {
            results.push_back( ToolResultBlock( call.id, NotExecuted(
               "not executed: at most " + std::to_string( PICopilotMaxToolCallsPerStep ) + " tool calls per step" ) ) );
            log( CallLine( call, "skipped (at most " + String( PICopilotMaxToolCallsPerStep ) + " tool calls per step)" ) );
            continue;
         }
         if ( stopped || stopNow() )
         {
            stopped = true;
            results.push_back( ToolResultBlock( call.id, NotExecuted( "not executed: the user pressed Stop" ) ) );
            log( CallLine( call, "skipped (stopped)" ) );
            continue;
         }
         ++executed;
         ToolOutcome o;
         try
         {
            o = run( call );
         }
         catch ( const pcl::Exception& x )
         {
            o = NotExecuted( "tool failed: " + U8( x.Message() ) );
            o.logLine = CallLine( call, "error: tool threw an exception" );
         }
         catch ( const std::exception& x )
         {
            o = NotExecuted( std::string( "tool failed: " ) + x.what() );
            o.logLine = CallLine( call, "error: tool threw an exception" );
         }
         catch ( ... )
         {
            o = NotExecuted( "tool failed: unknown error" );
            o.logLine = CallLine( call, "error: tool threw an exception" );
         }
         if ( o.mutated )
            m_imageChanged = true;
         results.push_back( ToolResultBlock( call.id, o ) );
         log( o.logLine );
      }

      // The round is appended as a unit: never half a tool_use/tool_result
      // pair. Both messages are built first; if appending or stripping
      // throws, the history is cut back to its pre-round length.
      AnthropicMessage user;
      user.role = "user";
      user.blocks = std::move( results );
      const size_type preRound = m_history.Length();
      try
      {
         m_history.Add( assistant );
         m_history.Add( user );
         StripOlderImages( m_history );
      }
      catch ( ... )
      {
         if ( m_history.Length() > preRound )
            m_history.Truncate( m_history.At( preRound ) );
         throw;
      }
      // After the round is committed (the rollback above counts on the
      // history's front being unchanged). The current exchange is never cut.
      m_trimmed += TrimHistoryToBudget( m_history, PICopilotHistoryTokenBudget, PICopilotHistoryTrimTarget );
      ++m_rounds;

      s.toolsRan = m_imageChanged;
      s.kind = capped ? AgentStep::CapReached
             : (stopped || stopNow()) ? AgentStep::Stopped
             : AgentStep::SendAgain;
      return s;
   }
   catch ( const pcl::Exception& x )
   {
      return Fail( AgentStep::Failed, "internal error handling the reply: " + x.Message() );
   }
   catch ( const std::exception& x )
   {
      return Fail( AgentStep::Failed, "internal error handling the reply: " + S16( x.what() ) );
   }
   catch ( ... )
   {
      return Fail( AgentStep::Failed, "internal error handling the reply" );
   }
}

namespace
{

// requireSendable: the whole history is about to be sent (non-empty, ends
// with a user message). Otherwise h is a prefix a user turn will complete
// (AbortTurn()'s snapshot): empty is fine, a trailing assistant turn is fine.
bool ValidateHistory( const Array<AnthropicMessage>& h, String& why, bool requireSendable )
{
   why.Clear();
   if ( h.IsEmpty() )
   {
      if ( !requireSendable )
         return true;
      why = "empty history";
      return false;
   }
   std::set<std::string> pending;   // tool_use ids the next (user) message must answer
   for ( size_type i = 0; i < h.Length(); ++i )
   {
      const AnthropicMessage& m = h[i];
      const String at = String().Format( "message %u", unsigned( i ) );
      const char* expected = (i % 2 == 0) ? "user" : "assistant";
      if ( m.role != expected )
      {
         why = at + " should be " + expected + " but is " + String( m.role );
         return false;
      }
      std::set<std::string> uses, answers;
      if ( m.blocks.is_null() )
      {
         if ( m.content.IsEmpty() && m.imageJpegBase64.IsEmpty() )
         {
            why = at + " is empty (no text, image or content blocks)";
            return false;
         }
      }
      else if ( !m.blocks.is_array() )
      {
         why = at + ": content blocks are not an array";
         return false;
      }
      else if ( m.blocks.empty() )
      {
         why = at + " has an empty content-block array";
         return false;
      }
      else
      {
         bool sawOther = false;   // a non-tool_result block: tool_results must come first
         for ( size_t j = 0; j < m.blocks.size(); ++j )
         {
            const nlohmann::json& b = m.blocks[j];
            const std::string type = BlockType( b );
            if ( IsEmptyTextBlock( b ) )
            {
               why = at + String().Format( ": content[%u] is an empty text block", unsigned( j ) );
               return false;
            }
            if ( type == "tool_use" )
            {
               const std::string id = b.value( "id", std::string() );
               if ( id.empty() )
               {
                  why = at + String().Format( ": content[%u] is a tool_use without an id", unsigned( j ) );
                  return false;
               }
               if ( m.role != "assistant" )
               {
                  why = at + ": tool_use " + S16( id ) + " in a user message";
                  return false;
               }
               if ( !uses.insert( id ).second )
               {
                  why = at + ": duplicate tool_use id " + S16( id );
                  return false;
               }
            }
            else if ( type == "tool_result" )
            {
               const std::string id = b.value( "tool_use_id", std::string() );
               if ( id.empty() )
               {
                  why = at + String().Format( ": content[%u] is a tool_result without a tool_use_id", unsigned( j ) );
                  return false;
               }
               if ( sawOther )
               {
                  why = at + String().Format( ": tool_result " ) + S16( id )
                      + String().Format( " at content[%u] comes after a non-tool_result block; tool_results must come first",
                                         unsigned( j ) );
                  return false;
               }
               if ( m.role != "user" || pending.count( id ) == 0 )
               {
                  why = "tool_result " + S16( id ) + " (" + at + ") has no matching tool_use in the previous message";
                  return false;
               }
               if ( !answers.insert( id ).second )
               {
                  why = at + ": duplicate tool_result for " + S16( id );
                  return false;
               }
            }
            if ( type != "tool_result" )
               sawOther = true;
         }
      }
      if ( m.role == "user" )
      {
         for ( const std::string& id : pending )
            if ( answers.count( id ) == 0 )
            {
               why = "tool_use " + S16( id ) + " is not answered by a tool_result in " + at;
               return false;
            }
         pending.clear();
      }
      else
         pending = uses;
   }
   if ( requireSendable && h[h.Length()-1].role != "user" )
   {
      why = "the last message must be a user message";
      return false;
   }
   if ( !pending.empty() )   // prefix ending with an assistant tool_use: a plain user turn can't answer it
   {
      why = "tool_use " + S16( *pending.begin() ) + " in the last message is never answered";
      return false;
   }
   return true;
}

} // namespace

bool HistoryIsApiValid( const Array<AnthropicMessage>& h, String& why )
{
   return ValidateHistory( h, why, true/*requireSendable*/ );
}

bool HistoryPrefixIsApiValid( const Array<AnthropicMessage>& h, String& why )
{
   return ValidateHistory( h, why, false/*requireSendable*/ );
}

} // namespace pcl
