// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "AnthropicClient.h"
#include "Utf8.h"

#include <pcl/Control.h>
#include <pcl/Exception.h>
#include <pcl/NetworkTransfer.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace pcl
{

namespace
{

// NetworkTransfer::OnDownloadDataAvailable() only accepts a receiver
// derived from Control (its download_event_handler is declared as
// bool (Control::*)(...) in NetworkTransfer.h). This tiny, never-shown,
// unparented Control exists purely to own the response buffer and receive
// that one callback. Because it is a Control, it can only be constructed
// (and destroyed) on the root thread -- see AnthropicRequest in the header. Casting &ResponseSink::OnData to
// NetworkTransfer::download_event_handler below is the same
// derived-member-to-Control-member cast used throughout PCL's own event
// handler registration (e.g. Button::OnClick with a Dialog subclass).
class ResponseSink : public Control
{
public:

   using clock = std::chrono::steady_clock;

   IsoString buffer;

   // Set from any thread by AnthropicRequest::Cancel().
   std::atomic<bool> cancelRequested{ false };

   // Written on the performing thread only (inside the callbacks below),
   // read back on that same thread after POST() returns.
   bool             cancelled = false;
   bool             timedOut = false;
   clock::time_point deadline = clock::time_point::max();

   // Returns false (abort) when the request was cancelled or its overall
   // deadline has passed; records which one it was.
   bool ShouldContinue()
   {
      if ( cancelRequested.load() )
      {
         cancelled = true;
         return false;
      }
      if ( clock::now() >= deadline )
      {
         timedOut = true;
         return false;
      }
      return true;
   }

   bool OnData( NetworkTransfer& /*sender*/, const void* data, fsize_type size )
   {
      if ( !ShouldContinue() )
         return false;
      buffer.Append( reinterpret_cast<const char*>( data ), size_type( size ) );
      return true;
   }

   // NetworkTransfer.h: progress events are generated "at regular intervals
   // during an active data transfer operation", and returning false aborts
   // it. This is what bounds a connection that stalls after connecting --
   // SetConnectionTimeout() only covers the connect phase. The self-test's
   // stall path proves it fires while no bytes are moving.
   bool OnProgress( NetworkTransfer& /*sender*/, fsize_type /*downloadTotal*/, fsize_type /*downloadCurrent*/,
                    fsize_type /*uploadTotal*/, fsize_type /*uploadCurrent*/ )
   {
      return ShouldContinue();
   }
};

} // namespace

nlohmann::json JpegImageBlock( const IsoString& base64 )
{
   return { { "type", "image" },
            { "source", { { "type", "base64" },
                          { "media_type", "image/jpeg" },
                          { "data", std::string( base64.c_str() ) } } } };
}

namespace
{

nlohmann::json MessageContent( const AnthropicMessage& msg )
{
   if ( !msg.blocks.is_null() )
      return msg.blocks;
   const std::string text = U8( msg.content );
   if ( msg.imageJpegBase64.IsEmpty() )
      return text;
   nlohmann::json blocks = nlohmann::json::array();
   blocks.push_back( JpegImageBlock( msg.imageJpegBase64 ) );   // image first, then the question
   blocks.push_back( { { "type", "text" }, { "text", text } } );
   return blocks;
}

} // namespace

namespace
{

String PostBytes( const std::string& bytes )
{
   // NetworkTransfer::POST( const String& ) hands the UTF-16 string to the
   // core, which sends each 16-bit code unit as ONE byte (its low 8 bits) --
   // it does not UTF-8-encode. Proven by the self-test's loopback echo server
   // (Section 8): U+2014 went out as 0x14, U+2192 as 0x92, U+00B5 as 0xB5.
   // Handing it the UTF-16 decoding of the body (0.1.0.3 and earlier) thus
   // sent invalid UTF-8 for any non-ASCII character, and the API rejected
   // the whole body ("str is not valid UTF-8: surrogates not allowed") --
   // e.g. every turn after a reply containing an em dash or arrow.
   //
   // So widen each body BYTE to one code unit (0x00..0xFF): the core's
   // narrowing then reproduces the UTF-8 bytes exactly.
   String s;
   s.SetLength( bytes.size() );
   char16_type* out = s.Begin();
   for ( const char c : bytes )
      *out++ = char16_type( static_cast<unsigned char>( c ) );
   return s;
}

} // namespace

std::string BuildMessagesRequestBody( const IsoString& model, const String& systemPrompt,
                                      const Array<AnthropicMessage>& history,
                                      const nlohmann::json& tools )
{
   nlohmann::json messages = nlohmann::json::array();
   for ( const AnthropicMessage& msg : history )
      messages.push_back( { { "role", msg.role.c_str() }, { "content", MessageContent( msg ) } } );
   nlohmann::json req = {
      { "model", model.c_str() },
      { "max_tokens", 4096 },
      { "system", U8( systemPrompt ) },
      { "messages", messages }
   };
   if ( !tools.is_null() )
      req["tools"] = tools;
   return req.dump();
}

struct AnthropicRequest::Impl
{
   ResponseSink    sink;      // root-thread construction only (Control)
   NetworkTransfer transfer;
   String          body;      // request body as POST() wants it -- see PostBytes()
   String          buildError;
   int             timeoutSeconds = PICopilotRequestTimeoutSeconds;
};

AnthropicRequest::AnthropicRequest( const String& apiKey, const IsoString& model,
                                    const String& systemPrompt, const Array<AnthropicMessage>& history,
                                    const String& url, int timeoutSeconds,
                                    const nlohmann::json& tools )
   : m( new Impl )
{
   m->timeoutSeconds = timeoutSeconds;

   // --- Build the request body -----------------------------------------
   //
   // nlohmann::json stores/emits text as UTF-8 (every String goes in via
   // U8()); PostBytes() then carries those exact bytes through POST().
   try
   {
      m->body = PostBytes( BuildMessagesRequestBody( model, systemPrompt, history, tools ) );
   }
   catch ( const std::exception& x )
   {
      m->buildError = String( "failed to build request: " ) + String( x.what() );
      return;
   }

   // --- Configure the transfer (root thread) ------------------------------
   try
   {
      m->transfer.SetURL( url );
      m->transfer.SetSSL( true/*useSSL*/, false/*forceSSL*/, true/*verifyPeer*/, true/*verifyHost*/ );
      m->transfer.SetConnectionTimeout( 120 );
      m->transfer.SetCustomHTTPHeaders( String( "x-api-key: " ) + apiKey
         + "\nanthropic-version: 2023-06-01\ncontent-type: application/json" );
      m->transfer.OnDownloadDataAvailable( (NetworkTransfer::download_event_handler)&ResponseSink::OnData, m->sink );
      m->transfer.OnTransferProgress( (NetworkTransfer::progress_event_handler)&ResponseSink::OnProgress, m->sink );
   }
   catch ( const pcl::Exception& x )
   {
      m->buildError = "failed to configure request: " + x.Message();
   }
   catch ( ... )
   {
      m->buildError = "failed to configure request: unknown error";
   }
}

AnthropicRequest::~AnthropicRequest() = default;

void AnthropicRequest::Cancel()
{
   m->sink.cancelRequested.store( true );
}

AnthropicResult AnthropicRequest::Perform()
{
   AnthropicResult result;

   if ( !m->buildError.IsEmpty() )
   {
      result.error = m->buildError;
      return result;
   }

   // --- Perform the request ---------------------------------------------
   NetworkTransfer& transfer = m->transfer;
   ResponseSink& sink = m->sink;
   const String timedOutError = String().Format( "request timed out after %d s", m->timeoutSeconds );

   // A Cancel() that lands before we start never touches the network.
   if ( sink.cancelRequested.load() )
   {
      result.error = "request cancelled";
      return result;
   }

   sink.deadline = ResponseSink::clock::now() + std::chrono::seconds( m->timeoutSeconds );
   try
   {
      bool okHttp = transfer.POST( m->body );
      result.httpStatus = transfer.ResponseCode();

      // Aborted from our own callbacks: report why, never a generic
      // network error (and never try to parse a partial body).
      if ( transfer.WasAborted() || sink.cancelled || sink.timedOut )
      {
         result.ok = false;
         result.httpStatus = 0;
         if ( sink.cancelled )
            result.error = "request cancelled";
         else if ( sink.timedOut )
            result.error = timedOutError;
         else
            result.error = "request aborted: " + transfer.ErrorInformation();
         return result;
      }

      if ( !okHttp && result.httpStatus == 0 )
      {
         // No HTTP response at all (DNS/connect/TLS failure, or the
         // transfer was aborted) -- nothing to parse as JSON.
         result.ok = false;
         result.error = String( "network request failed: " ) + transfer.ErrorInformation();
         return result;
      }
   }
   catch ( const pcl::Exception& x )
   {
      result.ok = false;
      result.error = "network request failed: " + x.Message();
      return result;
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

   return ParseMessagesResponse( result.httpStatus, sink.buffer, transfer.ErrorInformation() );
}

AnthropicResult ParseMessagesResponse( int httpStatus, const IsoString& body, const String& transportError )
{
   AnthropicResult result;
   result.httpStatus = httpStatus;

   // json::parse() failing means the body isn't JSON at all -- the only case
   // that gets "unparseable response". A 2xx body that parses but lacks the
   // expected content shape gets its own message below, so the two are not
   // conflated.
   nlohmann::json j;
   try
   {
      j = nlohmann::json::parse( body.c_str() );
   }
   catch ( ... )
   {
      result.error = String( "unparseable response: " ) + String::UTF8ToUTF16( body.Left( 200 ).c_str() );
      return result;
   }

   if ( httpStatus >= 200 && httpStatus < 300 )
   {
      // Join every "text" content block in order (a reply is not guaranteed
      // to be a single block), keep the whole content array verbatim (an
      // assistant tool_use turn must be re-sent exactly), and flag a
      // max_tokens cut-off so the UI can say the reply is incomplete.
      try
      {
         const nlohmann::json& content = j.at( "content" );
         std::string joined;
         bool anyText = false;
         for ( const nlohmann::json& block : content )
            if ( block.value( "type", std::string() ) == "text" )
            {
               joined += block.at( "text" ).get<std::string>();
               anyText = true;
            }
         result.stopReason = ( j.contains( "stop_reason" ) && j["stop_reason"].is_string() )
                           ? j["stop_reason"].get<std::string>() : std::string();
         // A tool_use reply may carry no text at all; any other reply must.
         if ( !anyText && result.stopReason != "tool_use" )
            throw std::runtime_error( "no text block" );
         result.text = String::UTF8ToUTF16( joined.c_str() );
         result.truncated = result.stopReason == "max_tokens";
         result.contentBlocks = content;
         result.ok = true;
      }
      catch ( ... )
      {
         result.ok = false;
         result.text.Clear();
         result.stopReason.clear();
         result.contentBlocks = nlohmann::json();
         result.error = "response missing expected content/text field";
      }
   }
   else
   {
      // "error"/"message" present but not a string (a malformed or
      // intermediary error body) must not throw: fall back to the
      // transport's error information.
      std::string msg;
      try
      {
         msg = ( j.contains( "error" ) && j["error"].contains( "message" ) )
            ? j["error"]["message"].get<std::string>()
            : U8( transportError );
      }
      catch ( ... )
      {
         msg = U8( transportError );
      }
      result.error = String::UTF8ToUTF16( msg.c_str() );
      result.ok = false;
   }
   return result;
}

// ----------------------------------------------------------------------------

AnthropicClient::AnthropicClient( String apiKey, IsoString model )
   : m_apiKey( std::move( apiKey ) )
   , m_model( std::move( model ) )
{
}

AnthropicResult AnthropicClient::Send( const String& systemPrompt, const Array<AnthropicMessage>& history )
{
   // Never throws across this boundary (the request ctor and Perform()
   // both trap internally; this guards the Impl allocation too).
   try
   {
      AnthropicRequest request( m_apiKey, m_model, systemPrompt, history );
      return request.Perform();
   }
   catch ( const pcl::Exception& x )
   {
      AnthropicResult r;
      r.error = "request failed: " + x.Message();
      return r;
   }
   catch ( ... )
   {
      AnthropicResult r;
      r.error = "request failed: unknown error";
      return r;
   }
}

} // namespace pcl
