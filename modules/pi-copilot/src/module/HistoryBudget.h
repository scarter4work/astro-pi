// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_HistoryBudget_h
#define PICopilot_HistoryBudget_h

#include "AnthropicClient.h"

namespace pcl
{

// History budget (estimated tokens) and where a trim brings it back to
// (hysteresis: a trim is a prompt-cache miss, so it should be rare).
constexpr size_type PICopilotHistoryTokenBudget = 100000;
constexpr size_type PICopilotHistoryTrimTarget  = 70000;
constexpr size_type PICopilotImageTokenEstimate = 1600;   // a <=1024 px preview

// Put in front of the new first message after a trim.
extern const char* const kPICopilotTrimNote;

// UTF-8 text bytes / 3 (+ PICopilotImageTokenEstimate per image, + 8 per
// message). A deliberate over-estimate: no network call, never under-counts
// Latin text by much.
size_type EstimateMessageTokens( const AnthropicMessage& m );
size_type EstimateHistoryTokens( const Array<AnthropicMessage>& h, size_type from = 0 );

// A user message that starts a new exchange: role user and not starting with
// a tool_result (a merged "tool_results + your next message" turn is not fresh).
bool IsFreshUserTurn( const AnthropicMessage& m );

// If h is over `budget`, removes its oldest messages up to the earliest fresh
// user turn from which the rest fits `target` -- or, if none does, up to the
// LAST fresh user turn (the current exchange is never cut). Cutting only in
// front of a fresh user turn keeps roles alternating and every
// tool_use/tool_result pair intact. The new first message gets
// kPICopilotTrimNote as its first text block. Returns the number removed.
size_type TrimHistoryToBudget( Array<AnthropicMessage>& h, size_type budget, size_type target );

} // namespace pcl

#endif // PICopilot_HistoryBudget_h
