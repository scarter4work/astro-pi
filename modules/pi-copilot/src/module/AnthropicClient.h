// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_AnthropicClient_h
#define PICopilot_AnthropicClient_h

#include <pcl/Array.h>
#include <pcl/String.h>

namespace pcl
{

// Default model used when the caller doesn't override it. A plain macro
// (not a constexpr) because it has to work as a default argument value in
// the AnthropicClient constructor declaration below.
#define PICOPILOT_DEFAULT_MODEL "claude-opus-4-8"

// One turn of chat history sent to the Anthropic Messages API.
struct AnthropicMessage
{
   IsoString role;    // "user" | "assistant"
   String    content;
};

// Outcome of an AnthropicClient::Send() call. Send() never throws across
// its caller — success or failure both ride back in here, since the
// caller may be a worker Thread that must not let an exception escape.
struct AnthropicResult
{
   bool   ok = false;
   String text;
   String error;
   int    httpStatus = 0;
};

/*!
 * Blocking, non-streamed client for the Anthropic Messages API
 * (https://api.anthropic.com/v1/messages), built on pcl::NetworkTransfer.
 *
 * Send() does not touch the GUI or console, so it is safe to call from a
 * worker Thread.
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
