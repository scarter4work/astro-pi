// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "AnthropicClient.h"

#include <pcl/Control.h>
#include <pcl/NetworkTransfer.h>

#include <nlohmann/json.hpp>

namespace pcl
{

namespace
{

// NetworkTransfer::OnDownloadDataAvailable() only accepts a receiver
// derived from Control (its download_event_handler is declared as
// bool (Control::*)(...) in NetworkTransfer.h). AnthropicClient itself
// must stay GUI-free since it runs on a worker Thread, so this tiny,
// never-shown, unparented Control exists purely to own the response
// buffer and receive that one callback. Casting &ResponseSink::OnData to
// NetworkTransfer::download_event_handler below is the same
// derived-member-to-Control-member cast used throughout PCL's own event
// handler registration (e.g. Button::OnClick with a Dialog subclass).
class ResponseSink : public Control
{
public:

   IsoString buffer;

   bool OnData( NetworkTransfer& /*sender*/, const void* data, fsize_type size )
   {
      buffer.Append( reinterpret_cast<const char*>( data ), size_type( size ) );
      return true;
   }
};

} // namespace

AnthropicClient::AnthropicClient( String apiKey, IsoString model )
   : m_apiKey( std::move( apiKey ) )
   , m_model( std::move( model ) )
{
}

AnthropicResult AnthropicClient::Send( const String& systemPrompt, const Array<AnthropicMessage>& history )
{
   AnthropicResult result;

   // --- Build the request body -----------------------------------------
   //
   // nlohmann::json stores/emits text as UTF-8. pcl::String is UTF-16
   // internally, so every String -> json::string conversion below goes
   // through ToUTF8(), and the resulting UTF-8 std::string is decoded
   // back into a pcl::String via String::UTF8ToUTF16() (NOT the
   // String(const char*) ISO-8859-1 constructor, which would mangle any
   // non-ASCII byte in the JSON as a Latin-1 code point).
   std::string requestBody;
   try
   {
      nlohmann::json messages = nlohmann::json::array();
      for ( const AnthropicMessage& m : history )
         messages.push_back( { { "role", m.role.c_str() }, { "content", m.content.ToUTF8().c_str() } } );

      nlohmann::json req = {
         { "model", m_model.c_str() },
         { "max_tokens", 4096 },
         { "system", systemPrompt.ToUTF8().c_str() },
         { "messages", messages }
      };
      requestBody = req.dump();
   }
   catch ( const std::exception& x )
   {
      result.ok = false;
      result.error = String( "failed to build request: " ) + String( x.what() );
      return result;
   }

   // --- Perform the request ---------------------------------------------
   ResponseSink sink;
   NetworkTransfer transfer;
   try
   {
      transfer.SetURL( "https://api.anthropic.com/v1/messages" );
      transfer.SetSSL( true/*useSSL*/, false/*forceSSL*/, true/*verifyPeer*/, true/*verifyHost*/ );
      transfer.SetConnectionTimeout( 120 );
      transfer.SetCustomHTTPHeaders( String( "x-api-key: " ) + m_apiKey
         + "\nanthropic-version: 2023-06-01\ncontent-type: application/json" );
      transfer.OnDownloadDataAvailable( (NetworkTransfer::download_event_handler)&ResponseSink::OnData, sink );

      bool okHttp = transfer.POST( String::UTF8ToUTF16( requestBody.c_str() ) );
      result.httpStatus = transfer.ResponseCode();

      if ( !okHttp && result.httpStatus == 0 )
      {
         // No HTTP response at all (DNS/connect/TLS failure, or the
         // transfer was aborted) -- nothing to parse as JSON.
         result.ok = false;
         result.error = String( "network request failed: " ) + transfer.ErrorInformation();
         return result;
      }
   }
   catch ( const std::exception& x )
   {
      result.ok = false;
      result.error = String( "network request failed: " ) + String( x.what() );
      return result;
   }
   catch ( ... )
   {
      result.ok = false;
      result.error = "network request failed: unknown error";
      return result;
   }

   // --- Parse the response ----------------------------------------------
   //
   // json::parse() failing means the body isn't JSON at all -- that's the
   // only case that gets the "unparseable response" error. A 2xx body
   // that parses fine but doesn't have the expected content/text shape is
   // a different failure (Anthropic changed the response shape, or this
   // isn't really a Messages API response) and gets its own message,
   // extracted separately so the two aren't conflated.
   nlohmann::json j;
   try
   {
      j = nlohmann::json::parse( sink.buffer.c_str() );
   }
   catch ( ... )
   {
      result.ok = false;
      IsoString snippet = sink.buffer.Left( 200 );
      result.error = String( "unparseable response: " ) + String::UTF8ToUTF16( snippet.c_str() );
      return result;
   }

   if ( result.httpStatus >= 200 && result.httpStatus < 300 )
   {
      try
      {
         result.text = String::UTF8ToUTF16( j["content"][0]["text"].get<std::string>().c_str() );
         result.ok = true;
      }
      catch ( ... )
      {
         result.ok = false;
         result.error = "response missing expected content/text field";
      }
   }
   else
   {
      std::string msg = ( j.contains( "error" ) && j["error"].contains( "message" ) )
         ? j["error"]["message"].get<std::string>()
         : std::string( transfer.ErrorInformation().ToUTF8().c_str() );
      result.error = String::UTF8ToUTF16( msg.c_str() );
      result.ok = false;
   }

   return result;
}

} // namespace pcl
