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
 * Send() touches no GUI or console state by design. NOTE: it constructs
 * an internal Control-based response sink (NetworkTransfer's download
 * callback requires a Control-derived receiver — see AnthropicClient.cpp).
 * Constructing a Control from a non-root Thread is NOT yet verified —
 * to date Send() has only run on the root thread (PICopilotSelfTest).
 * Smoke-test worker-thread use before relying on this (increment-2
 * Task 4) instead of assuming it's already proven.
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
