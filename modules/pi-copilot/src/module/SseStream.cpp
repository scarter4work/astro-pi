// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "SseStream.h"

#include <exception>
#include <utility>

namespace pcl
{

namespace
{

// In-place append to a JSON string field (amortized O(1) growth), instead of
// `block[field] = block.value(field, std::string()) + t`, which copies the
// whole accumulated string on every delta and makes a long streamed text or
// thinking block O(n^2) in its own length.
void AppendToStringField( nlohmann::json& block, const char* field, const std::string& t )
{
   nlohmann::json& v = block[field];
   if ( !v.is_string() )
      v = std::string();
   v.get_ref<std::string&>() += t;
}

} // namespace

void SseMessageAssembler::Fail( const std::string& why )
{
   if ( !m_failed )
   {
      m_failed = true;
      m_error = why;
   }
}

std::string SseMessageAssembler::Feed( const char* data, size_t size )
{
   std::string text;
   for ( size_t i = 0; i < size && !m_failed && !m_finished; ++i )
   {
      const char c = data[i];
      if ( c == '\n' || c == '\r' )
      {
         if ( c == '\n' && m_lastWasCR )   // the CR of this CRLF already ended the line
         {
            m_lastWasCR = false;
            continue;
         }
         m_lastWasCR = c == '\r';
         OnLine( text );
         m_line.clear();
      }
      else
      {
         m_lastWasCR = false;
         if ( m_line.size() >= kMaxSseBufferBytes )
         {
            Fail( "SSE line exceeded " + std::to_string( kMaxSseBufferBytes ) + " bytes without a line terminator" );
            break;
         }
         m_line.push_back( c );
      }
   }
   return text;
}

void SseMessageAssembler::OnLine( std::string& text )
{
   if ( m_line.empty() )
   {
      Dispatch( text );
      return;
   }
   if ( m_line[0] == ':' )   // comment
      return;
   const size_t colon = m_line.find( ':' );
   const std::string field = m_line.substr( 0, colon );
   std::string value = (colon == std::string::npos) ? std::string() : m_line.substr( colon + 1 );
   if ( !value.empty() && value[0] == ' ' )
      value.erase( 0, 1 );
   if ( field == "event" )
      m_event = value;
   else if ( field == "data" )
   {
      if ( m_data.size() + value.size() + 1 > kMaxSseBufferBytes )
      {
         Fail( "SSE event data exceeded " + std::to_string( kMaxSseBufferBytes ) + " bytes" );
         return;
      }
      if ( m_hasData )
         m_data += '\n';
      m_data += value;
      m_hasData = true;
   }
}

void SseMessageAssembler::Dispatch( std::string& text )
{
   if ( !m_hasData )
   {
      m_event.clear();
      return;
   }
   const std::string data = std::move( m_data );
   const std::string name = std::move( m_event );
   m_data.clear();
   m_event.clear();
   m_hasData = false;
   nlohmann::json ev;
   try
   {
      ev = nlohmann::json::parse( data );
   }
   catch ( const std::exception& )
   {
      Fail( "stream event '" + name + "' is not JSON: " + data.substr( 0, 120 ) );
      return;
   }
   m_sawEvent = true;
   try
   {
      OnEvent( ev, name, text );
   }
   catch ( const std::exception& x )
   {
      Fail( "malformed stream event '" + name + "': " + x.what() );
   }
}

void SseMessageAssembler::OnEvent( const nlohmann::json& ev, const std::string& name, std::string& text )
{
   const std::string type = (ev.is_object() && ev.contains( "type" ) && ev["type"].is_string())
                          ? ev["type"].get<std::string>() : name;
   if ( type == "ping" )
      return;
   if ( type == "error" )
   {
      const nlohmann::json& e = ev.at( "error" );
      m_errorType = e.value( "type", std::string( "error" ) );
      Fail( m_errorType + ": " + e.value( "message", std::string( "(no message)" ) ) );
      return;
   }
   if ( type == "message_start" )
   {
      if ( m_started )
      {
         Fail( "duplicate message_start (message_stop was not seen for the previous message)" );
         return;
      }
      m_message = ev.at( "message" );
      if ( !m_message.is_object() )
      {
         Fail( "message_start without a message object" );
         return;
      }
      m_message["content"] = nlohmann::json::array();
      m_partialJson.clear();
      m_badToolInput.clear();
      m_blockOpen = false;
      m_started = true;
      return;
   }
   if ( type != "content_block_start" && type != "content_block_delta" && type != "content_block_stop"
     && type != "message_delta" && type != "message_stop" )
      return;   // a future event type: ignored
   if ( !m_started )
   {
      Fail( type + " before message_start" );
      return;
   }

   nlohmann::json& content = m_message["content"];
   if ( type == "content_block_start" )
   {
      const size_t index = ev.at( "index" ).get<size_t>();
      if ( index != content.size() )
      {
         Fail( "content_block_start index " + std::to_string( index ) + " out of order (expected "
               + std::to_string( content.size() ) + ")" );
         return;
      }
      content.push_back( ev.at( "content_block" ) );
      m_partialJson.push_back( std::string() );
      m_blockOpen = true;
      return;
   }
   if ( type == "content_block_delta" || type == "content_block_stop" )
   {
      const size_t index = ev.at( "index" ).get<size_t>();
      if ( index >= content.size() )
      {
         Fail( type + " for unknown content index " + std::to_string( index ) );
         return;
      }
      nlohmann::json& block = content[index];
      const std::string at = " (content[" + std::to_string( index ) + "])";
      if ( type == "content_block_stop" )
      {
         const std::string btype = block.value( "type", std::string() );
         if ( btype == "tool_use" || btype == "server_tool_use" )
         {
            if ( !m_partialJson[index].empty() )
            {
               try
               {
                  block["input"] = nlohmann::json::parse( m_partialJson[index] );
               }
               catch ( const std::exception& )
               {
                  // Never fabricate a well-formed-looking {} for input that
                  // did not actually parse: a caller checking "is input an
                  // object" would see a plausible empty call and might
                  // re-send it as if the model asked for it. Keep the raw
                  // (possibly truncated) partial text instead -- visibly not
                  // an object -- and record the index. Under stop_reason
                  // "tool_use" the API guarantees complete, valid JSON here,
                  // so that combination fails the whole stream below; under
                  // any other stop_reason (e.g. max_tokens cut mid tool call)
                  // the caller's own truncation handling is what must drop it.
                  block["input"] = m_partialJson[index];
                  m_badToolInput.push_back( index );
               }
            }
            else if ( !block.contains( "input" ) || !block["input"].is_object() )
               block["input"] = nlohmann::json::object();
         }
         m_blockOpen = false;
         return;
      }
      const nlohmann::json& delta = ev.at( "delta" );
      const std::string dtype = delta.value( "type", std::string() );
      if ( dtype == "text_delta" )
      {
         const std::string t = delta.at( "text" ).get<std::string>();
         AppendToStringField( block, "text", t );
         text += t;
      }
      else if ( dtype == "input_json_delta" )
         m_partialJson[index] += delta.at( "partial_json" ).get<std::string>();
      else if ( dtype == "thinking_delta" )
         AppendToStringField( block, "thinking", delta.at( "thinking" ).get<std::string>() );
      else if ( dtype == "signature_delta" )
         block["signature"] = delta.at( "signature" ).get<std::string>();
      else if ( dtype == "citations_delta" )
      {
         if ( !block.contains( "citations" ) || !block["citations"].is_array() )
            block["citations"] = nlohmann::json::array();
         block["citations"].push_back( delta.at( "citation" ) );
      }
      else
         Fail( "unsupported stream delta type '" + dtype + "'" + at );
      return;
   }
   if ( type == "message_delta" )
   {
      if ( ev.contains( "delta" ) && ev["delta"].is_object() )
         for ( auto it = ev["delta"].begin(); it != ev["delta"].end(); ++it )
            m_message[it.key()] = it.value();
      if ( ev.contains( "usage" ) && ev["usage"].is_object() )
      {
         if ( !m_message.contains( "usage" ) || !m_message["usage"].is_object() )
            m_message["usage"] = nlohmann::json::object();
         for ( auto it = ev["usage"].begin(); it != ev["usage"].end(); ++it )
            m_message["usage"][it.key()] = it.value();
      }
      for ( auto it = ev.begin(); it != ev.end(); ++it )
         if ( it.key() != "type" && it.key() != "delta" && it.key() != "usage" )
            m_message[it.key()] = it.value();   // e.g. input_transformations after a fallback
      return;
   }
   // message_stop
   if ( m_blockOpen )
   {
      Fail( "message_stop while a content block is still open (no content_block_stop)" );
      return;
   }
   const bool toolStop = m_message.contains( "stop_reason" ) && m_message["stop_reason"].is_string()
                      && m_message["stop_reason"].get<std::string>() == "tool_use";
   if ( toolStop && !m_badToolInput.empty() )
   {
      Fail( "tool_use input (content[" + std::to_string( m_badToolInput.front() ) + "]) was not valid JSON" );
      return;
   }
   m_finished = true;
}

nlohmann::json SseMessageAssembler::FinalMessage() const
{
   nlohmann::json m = m_message;
   if ( m.is_object() && !m.contains( "type" ) )
      m["type"] = "message";
   return m;
}

} // namespace pcl
