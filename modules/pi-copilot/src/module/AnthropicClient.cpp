// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "AnthropicClient.h"

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

namespace
{

nlohmann::json MessageContent( const AnthropicMessage& msg )
{
   const std::string text( msg.content.ToUTF8().c_str() );
   if ( msg.imageJpegBase64.IsEmpty() )
      return text;
   nlohmann::json image = {
      { "type", "image" },
      { "source", { { "type", "base64" },
                    { "media_type", "image/jpeg" },
                    { "data", std::string( msg.imageJpegBase64.c_str() ) } } }
   };
   nlohmann::json textBlock = { { "type", "text" }, { "text", text } };
   nlohmann::json blocks = nlohmann::json::array();
   blocks.push_back( std::move( image ) );      // image first, then the question
   blocks.push_back( std::move( textBlock ) );
   return blocks;
}

} // namespace

std::string BuildMessagesRequestBody( const IsoString& model, const String& systemPrompt,
                                      const Array<AnthropicMessage>& history )
{
   nlohmann::json messages = nlohmann::json::array();
   for ( const AnthropicMessage& msg : history )
      messages.push_back( { { "role", msg.role.c_str() }, { "content", MessageContent( msg ) } } );
   nlohmann::json req = {
      { "model", model.c_str() },
      { "max_tokens", 4096 },
      { "system", systemPrompt.ToUTF8().c_str() },
      { "messages", messages }
   };
   return req.dump();
}

struct AnthropicRequest::Impl
{
   ResponseSink    sink;      // root-thread construction only (Control)
   NetworkTransfer transfer;
   String          body;      // UTF-16 request body, ready to POST
   String          buildError;
   int             timeoutSeconds = PICopilotRequestTimeoutSeconds;
};

AnthropicRequest::AnthropicRequest( const String& apiKey, const IsoString& model,
                                    const String& systemPrompt, const Array<AnthropicMessage>& history,
                                    const String& url, int timeoutSeconds )
   : m( new Impl )
{
   m->timeoutSeconds = timeoutSeconds;

   // --- Build the request body -----------------------------------------
   //
   // nlohmann::json stores/emits text as UTF-8. pcl::String is UTF-16
   // internally, so every String -> json::string conversion below goes
   // through ToUTF8(), and the resulting UTF-8 std::string is decoded
   // back into a pcl::String via String::UTF8ToUTF16() (NOT the
   // String(const char*) ISO-8859-1 constructor, which would mangle any
   // non-ASCII byte in the JSON as a Latin-1 code point).
   try
   {
      m->body = String::UTF8ToUTF16( BuildMessagesRequestBody( model, systemPrompt, history ).c_str() );
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
      // Join every "text" content block in order (a reply is not
      // guaranteed to be a single block), and flag a max_tokens cut-off so
      // the UI can say the reply is incomplete instead of presenting it as
      // whole.
      try
      {
         std::string joined;
         bool anyText = false;
         for ( const nlohmann::json& block : j.at( "content" ) )
            if ( block.value( "type", std::string() ) == "text" )
            {
               joined += block.at( "text" ).get<std::string>();
               anyText = true;
            }
         if ( !anyText )
            throw std::runtime_error( "no text block" );
         result.text = String::UTF8ToUTF16( joined.c_str() );
         result.truncated = j.contains( "stop_reason" ) && j["stop_reason"].is_string()
                         && j["stop_reason"].get<std::string>() == "max_tokens";
         result.ok = true;
      }
      catch ( ... )
      {
         result.ok = false;
         result.text.Clear();
         result.error = "response missing expected content/text field";
      }
   }
   else
   {
      // Guarded the same way as the 2xx content/text extraction above:
      // "error"/"message" being present but not string-convertible (a
      // number, object, or null in some malformed or intermediary error
      // body) must not throw out of Send() -- fall back to
      // ErrorInformation() instead of propagating.
      std::string msg;
      try
      {
         msg = ( j.contains( "error" ) && j["error"].contains( "message" ) )
            ? j["error"]["message"].get<std::string>()
            : std::string( transfer.ErrorInformation().ToUTF8().c_str() );
      }
      catch ( ... )
      {
         msg = std::string( transfer.ErrorInformation().ToUTF8().c_str() );
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
