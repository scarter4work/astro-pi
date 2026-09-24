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
// removed, so no unanswered tool_use ever enters the history.
nlohmann::json StorableAssistantBlocks( const nlohmann::json& blocks, bool keepToolUse )
{
   nlohmann::json kept = nlohmann::json::array();
   for ( const nlohmann::json& b : blocks )
   {
      if ( IsEmptyTextBlock( b ) )
         continue;
      if ( !keepToolUse && BlockType( b ) == "tool_use" )
         continue;
      kept.push_back( b );
   }
   if ( kept.empty() )
      kept.push_back( { { "type", "text" }, { "text", "[reply cut off before a tool call completed]" } } );
   return kept;
}

} // namespace

void AgentSession::Clear()
{
   m_history.Clear();
   m_snapshot.Clear();
   m_rounds = 0;
   m_anyToolRan = false;
}

void AgentSession::BeginUserTurn( const AnthropicMessage& userTurn )
{
   m_snapshot = m_history;
   m_rounds = 0;
   m_anyToolRan = false;
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
}

AgentStep AgentSession::Fail( AgentStep::Kind kind, const String& error )
{
   AgentStep s;
   s.kind = kind;
   s.error = error;
   s.toolsRan = m_anyToolRan;
   if ( m_rounds == 0 )
   {
      m_history = m_snapshot;        // nothing ran: as if never sent
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
   return Fail( AgentStep::Failed, error );
}

AgentStep AgentSession::OnResponse( const AnthropicResult& r, const ToolRunner& run,
                                    const std::function<bool()>& stopRequested,
                                    const std::function<void( const String& )>& onLog )
{
   try
   {
      if ( !r.ok )
         return Fail( (r.error == "request cancelled") ? AgentStep::Stopped : AgentStep::Failed, r.error );

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
         assistant.blocks = StorableAssistantBlocks( r.contentBlocks, r.stopReason == "tool_use" );

      if ( r.stopReason != "tool_use" )
      {
         if ( assistant.blocks.is_null() && assistant.content.IsEmpty() )
            return Fail( AgentStep::Failed, "the reply has neither text nor content blocks" );
         AgentStep s;
         s.assistantText = r.text;
         s.truncated = r.truncated;
         s.toolsRan = m_anyToolRan;
         m_history.Add( assistant );
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
         if ( stopped || stopNow() )
         {
            stopped = true;
            results.push_back( ToolResultBlock( call.id, NotExecuted( "not executed: the user pressed Stop" ) ) );
            log( CallLine( call, "skipped (stopped)" ) );
            continue;
         }
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
         m_anyToolRan = true;
         results.push_back( ToolResultBlock( call.id, o ) );
         log( o.logLine );
      }

      // The round is appended as a unit: never half a tool_use/tool_result pair.
      m_history.Add( assistant );
      AnthropicMessage user;
      user.role = "user";
      user.blocks = std::move( results );
      m_history.Add( user );
      ++m_rounds;
      StripOlderImages( m_history );

      s.toolsRan = m_anyToolRan;
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

bool HistoryIsApiValid( const Array<AnthropicMessage>& h, String& why )
{
   why.Clear();
   if ( h.IsEmpty() )
   {
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
   if ( h[h.Length()-1].role != "user" )
   {
      why = "the last message must be a user message";
      return false;
   }
   return true;
}

} // namespace pcl
