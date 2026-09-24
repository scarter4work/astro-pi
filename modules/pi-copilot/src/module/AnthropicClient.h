// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_AnthropicClient_h
#define PICopilot_AnthropicClient_h

#include <pcl/Array.h>
#include <pcl/String.h>

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

namespace pcl
{

// Default model used when the caller doesn't override it. A plain macro
// (not a constexpr) because it has to work as a default argument value in
// the AnthropicClient constructor declaration below.
#define PICOPILOT_DEFAULT_MODEL "claude-opus-4-8"

// Messages API endpoint. Overridable per request only so the self-test can
// point a request at a local stalling server (deadline proof).
#define PICOPILOT_MESSAGES_URL "https://api.anthropic.com/v1/messages"

// Overall wall-clock limit for one request, measured from the start of
// AnthropicRequest::Perform(). SetConnectionTimeout() only bounds the
// connect phase; this bounds the whole transfer, including a connection that
// stalls after connecting. Enforced from NetworkTransfer's progress callback.
constexpr int PICopilotRequestTimeoutSeconds = 300;

// One turn of chat history sent to the Anthropic Messages API.
//  - blocks non-null: the message's EXACT content-block array, sent as is
//    (an assistant tool_use turn echoed back verbatim; a user tool_result
//    turn; a user turn merged into a trailing tool_result turn). content and
//    imageJpegBase64 are then ignored. Strings inside are UTF-8.
//  - otherwise, non-empty imageJpegBase64 -> [image(base64 JPEG), text];
//    else a plain string.
struct AnthropicMessage
{
   IsoString      role;               // "user" | "assistant"
   String         content;
   IsoString      imageJpegBase64;    // optional, standard Base64, no data: prefix
   nlohmann::json blocks = nlohmann::json();   // optional, see above (null = absent)
};

// {"type":"image","source":{"type":"base64","media_type":"image/jpeg","data":...}}
nlohmann::json JpegImageBlock( const IsoString& base64 );

// The Messages API request body (UTF-8 JSON, non-streamed: no "stream" key).
// tools non-null -> a "tools" array. Pure function, any thread. Throws
// std::exception if JSON building fails.
std::string BuildMessagesRequestBody( const IsoString& model, const String& systemPrompt,
                                      const Array<AnthropicMessage>& history,
                                      const nlohmann::json& tools = nlohmann::json() );

// Outcome of an AnthropicClient::Send() call. Send() never throws across
// its caller -- success or failure both ride back in here, since the
// caller may be a worker Thread that must not let an exception escape.
struct AnthropicResult
{
   bool           ok = false;
   String         text;          // all "text" content blocks, concatenated in order (may be empty for tool_use)
   String         error;
   int            httpStatus = 0;
   bool           truncated = false; // stop_reason == "max_tokens"
   std::string    stopReason;        // "end_turn" | "tool_use" | "max_tokens" | ...
   nlohmann::json contentBlocks;     // the reply's "content" array, verbatim (echoed back in history)
};

// Parses one Messages API HTTP response. ok=true for a 2xx body with a
// content array that has text, or no text but stop_reason "tool_use".
// Non-2xx: error = the API's error.message, else transportError. Any thread.
AnthropicResult ParseMessagesResponse( int httpStatus, const IsoString& body, const String& transportError );

/*!
 * One prepared, blocking, non-streamed Anthropic Messages API request
 * (https://api.anthropic.com/v1/messages), built on pcl::NetworkTransfer.
 *
 * THREADING (proven by PICopilotSelfTest path 5, "workerThreadOk"):
 *  - The constructor and destructor MUST run on the root (UI) thread. They
 *    create/destroy a Control-derived response sink (NetworkTransfer's
 *    download callback requires a Control receiver) and the NetworkTransfer
 *    itself. Constructing a Control off the root thread FAILS -- the core
 *    throws "CreateControl(): API function error" (observed 2026-09-23).
 *  - Perform() MAY run on a worker pcl::Thread: it only issues the
 *    already-configured POST and parses the reply. The self-test runs it
 *    inside ChatThread with an invalid key and requires the API's 401 back.
 *  - Perform() never throws and must be called at most once.
 *  - Cancel() is thread-safe and may be called from any thread, before or
 *    during Perform(). The in-flight transfer is aborted from its progress
 *    callback (NetworkTransfer::OnTransferProgress -- returning false aborts
 *    the operation), which also enforces the overall deadline
 *    (timeoutSeconds, default PICopilotRequestTimeoutSeconds). A cancelled
 *    or timed-out request returns ok=false with error "request cancelled" /
 *    "request timed out after N s".
 *
 * All inputs are copied/serialized at construction; the request holds no
 * references to caller state.
 */
class AnthropicRequest
{
public:

   AnthropicRequest( const String& apiKey, const IsoString& model,
                     const String& systemPrompt, const Array<AnthropicMessage>& history,
                     const String& url = PICOPILOT_MESSAGES_URL,
                     int timeoutSeconds = PICopilotRequestTimeoutSeconds,
                     const nlohmann::json& tools = nlohmann::json() );
   ~AnthropicRequest();

   AnthropicRequest( const AnthropicRequest& ) = delete;
   AnthropicRequest& operator =( const AnthropicRequest& ) = delete;

   AnthropicResult Perform();

   // Thread-safe; idempotent. See THREADING above.
   void Cancel();

private:

   struct Impl;
   std::unique_ptr<Impl> m;
};

/*!
 * Convenience wrapper: Send() builds an AnthropicRequest and performs it on
 * the CALLING thread, so Send() itself is root-thread only (see
 * AnthropicRequest). For an off-UI-thread turn use ChatThread, which builds
 * the request on the root thread and only Perform()s it on the worker.
 */
class AnthropicClient
{
public:

   AnthropicClient( String apiKey, IsoString model = PICOPILOT_DEFAULT_MODEL );

   AnthropicResult Send( const String& systemPrompt, const Array<AnthropicMessage>& history );

private:

   String    m_apiKey;
   IsoString m_model;
};

} // namespace pcl

#endif // PICopilot_AnthropicClient_h
