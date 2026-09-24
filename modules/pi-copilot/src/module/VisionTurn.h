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

// Replaces an image inside an older content-block array (a tool_result preview,
// or a merged user turn) in re-sent history. UTF-8.
extern const char* const kPICopilotToolImageOmittedNote;

// One user turn. With a view context, the text is
//   "[PixInsight view context]\n<compact JSON>\n[/PixInsight view context]\n\n<userText>"
// otherwise just userText. jpegBase64 may be empty (no image block).
AnthropicMessage ComposeUserTurn( const String& userText, const nlohmann::json* viewContext,
                                  const IsoString& jpegBase64 );

// In place, for every USER message except the LAST (token cost of re-sent
// history):
//  - an image is dropped and kPICopilotImageOmittedNote is prepended;
//  - a leading view-context block is collapsed to
//    {"collapsed":true,"fullId":...,"geometry":...} (per-channel stats and
//    FITS keywords are not re-sent) -- also for a turn that never had an
//    image (preview failed). The user's own text is kept intact.
//  - a message with content blocks: every image block (top level or inside a
//    tool_result's content) becomes a text block kPICopilotToolImageOmittedNote,
//    and a top-level text block's leading view-context is collapsed.
// Idempotent: a second call changes nothing.
void StripOlderImages( Array<AnthropicMessage>& history );

} // namespace pcl

#endif // PICopilot_VisionTurn_h
