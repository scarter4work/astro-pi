// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ChatThread_h
#define PICopilot_ChatThread_h

#include "AnthropicClient.h"

#include <pcl/Mutex.h>
#include <pcl/Thread.h>

namespace pcl
{

/*!
 * Worker thread that performs one blocking Anthropic Messages API turn off
 * the UI thread.
 *
 * THREADING: construct and destroy a ChatThread ONLY on the root (UI)
 * thread. The constructor builds the AnthropicRequest -- whose Control-based
 * response sink cannot be created off the root thread -- and serializes the
 * key, system prompt and a snapshot of the history into it BY VALUE, so the
 * caller's state may change freely while the request is in flight. Run()
 * (worker thread) only calls AnthropicRequest::Perform(): the HTTPS POST and
 * reply parsing. Proven headlessly by PICopilotSelfTest path 5.
 *
 * Every request is bounded by an overall deadline (default
 * PICopilotRequestTimeoutSeconds) and can be cancelled with RequestCancel().
 *
 * Run() touches no GUI and no console. The UI thread polls TryTakeResult()
 * (from a Timer) to marshal the outcome back. Never destroy a ChatThread
 * while IsActive() -- Wait() for it first.
 */
class ChatThread : public Thread
{
public:

   // url/timeoutSeconds exist for the self-test (local stall server, short
   // deadline); production callers use the defaults.
   ChatThread( const String& apiKey, const String& systemPrompt, const Array<AnthropicMessage>& history,
               const IsoString& model = PICOPILOT_DEFAULT_MODEL,
               const String& url = PICOPILOT_MESSAGES_URL,
               int timeoutSeconds = PICopilotRequestTimeoutSeconds );

   void Run() override;

   // Thread-safe. Asks the in-flight (or not-yet-started) request to abort;
   // Run() then finishes promptly with error "request cancelled". Use this
   // -- not Thread::Abort(), which only raises a flag the blocking POST
   // never checks -- before Wait()ing on a thread that must stop.
   void RequestCancel();

   // Thread-safe. Returns true exactly once, after Run() has stored its
   // result, moving that result into `out`. Returns false while the request
   // is still in flight (or after the result has already been taken).
   bool TryTakeResult( AnthropicResult& out );

private:

   AnthropicRequest m_request;

   Mutex           m_mutex;
   AnthropicResult m_result;
   bool            m_done = false;
   bool            m_taken = false;
};

} // namespace pcl

#endif // PICopilot_ChatThread_h
