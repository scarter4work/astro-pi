// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_SseStream_h
#define PICopilot_SseStream_h

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace pcl
{

/*
 * Incremental parser for the Messages API's Server-Sent Events stream
 * ("stream": true) that rebuilds the final message in the NON-streamed shape,
 * so ParseMessagesResponse() -- and everything after it -- is unchanged.
 *
 * Bytes may arrive in any chunking (lines, UTF-8 sequences and CRLF pairs can
 * be split across Feed() calls). Events handled: message_start,
 * content_block_start/delta/stop, message_delta (every delta key + usage
 * merged into the message), message_stop, ping, error. Deltas: text_delta,
 * input_json_delta (tool input rebuilt and parsed at content_block_stop),
 * thinking_delta, signature_delta, citations_delta. An unknown DELTA type is a
 * failure (the block could not be echoed back faithfully); unknown EVENT types
 * are ignored (the API may add events). Pure: no PCL, no GUI; any thread.
 */
class SseMessageAssembler
{
public:

   // Returns the text of every text_delta completed by these bytes (UTF-8),
   // for live display. Ignores input after a failure or after message_stop.
   std::string Feed( const char* data, size_t size );

   bool Finished() const { return m_finished; }
   bool Failed() const { return m_failed; }
   const std::string& Error() const { return m_error; }
   const std::string& ErrorType() const { return m_errorType; }
   bool SawAnyEvent() const { return m_sawEvent; }

   // Valid when Finished() && !Failed().
   nlohmann::json FinalMessage() const;

private:

   std::string              m_line;
   bool                     m_lastWasCR = false;
   std::string              m_event;
   std::string              m_data;
   bool                     m_hasData = false;
   nlohmann::json           m_message;
   bool                     m_started = false;
   std::vector<std::string> m_partialJson;    // per content index (tool_use input)
   std::vector<size_t>      m_badToolInput;   // content indices whose input JSON did not parse
   bool                     m_finished = false;
   bool                     m_failed = false;
   bool                     m_sawEvent = false;
   std::string              m_error;
   std::string              m_errorType;

   void OnLine( std::string& text );
   void Dispatch( std::string& text );
   void OnEvent( const nlohmann::json& ev, const std::string& name, std::string& text );
   void Fail( const std::string& why );
};

} // namespace pcl

#endif // PICopilot_SseStream_h
