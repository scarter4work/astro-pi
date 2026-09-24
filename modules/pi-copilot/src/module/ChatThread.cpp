// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ChatThread.h"

#include <pcl/AutoLock.h>
#include <pcl/Exception.h>

namespace pcl
{

ChatThread::ChatThread( const String& apiKey, const String& systemPrompt, const Array<AnthropicMessage>& history,
                        const IsoString& model )
   : m_request( apiKey, model, systemPrompt, history )
{
}

void ChatThread::Run()
{
   // No GUI, no console here. AnthropicRequest::Perform() never throws by
   // contract, but pcl::Thread silently swallows anything that escapes
   // Run(), so belt-and-braces: turn any escape into an error result so the
   // UI thread always gets an outcome instead of waiting forever.
   AnthropicResult r;
   try
   {
      r = m_request.Perform();
   }
   catch ( const pcl::Exception& x )
   {
      r = AnthropicResult();
      r.error = "worker thread exception: " + x.Message();
   }
   catch ( const std::exception& x )
   {
      r = AnthropicResult();
      r.error = String( "worker thread exception: " ) + String( x.what() );
   }
   catch ( ... )
   {
      r = AnthropicResult();
      r.error = "worker thread exception: unknown error";
   }

   volatile AutoLock lock( m_mutex );
   m_result = std::move( r );
   m_done = true;
}

bool ChatThread::TryTakeResult( AnthropicResult& out )
{
   volatile AutoLock lock( m_mutex );
   if ( !m_done || m_taken )
      return false;
   out = m_result;
   m_taken = true;
   return true;
}

} // namespace pcl
