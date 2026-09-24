// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_VisionTurn_h
#define PICopilot_VisionTurn_h

#include "AnthropicClient.h"

#include <nlohmann/json.hpp>

namespace pcl
{

// Replaces an older turn's image in re-sent history (token cost): only the
// latest user turn carries pixels. UTF-8.
extern const char* const kPICopilotImageOmittedNote;

// One user turn. With a view context, the text is
//   "[PixInsight view context]\n<compact JSON>\n[/PixInsight view context]\n\n<userText>"
// otherwise just userText. jpegBase64 may be empty (no image block).
AnthropicMessage ComposeUserTurn( const String& userText, const nlohmann::json* viewContext,
                                  const IsoString& jpegBase64 );

// In place: every message except the LAST loses its image, and each message
// that loses one gets kPICopilotImageOmittedNote prepended to its text.
// Idempotent (a message without an image is never touched).
void StripOlderImages( Array<AnthropicMessage>& history );

} // namespace pcl

#endif // PICopilot_VisionTurn_h
