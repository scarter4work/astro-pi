// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ModelCatalog_h
#define PICopilot_ModelCatalog_h

#include <pcl/String.h>

namespace pcl
{

// Header value of the thinking-binding controls beta (preserved thinking).
#define PICOPILOT_THINKING_BINDING_BETA "thinking-binding-controls-2026-08-01"

// The models offered in the ⚙ dialog. Ids exactly as the Anthropic API names
// them (no date suffixes). thinkingBinding: the model always thinks and binds
// thinking blocks to the conversation prefix (Opus 5.5, Fable 5.1): requests
// carry the drop_block binding control so the harness's history edits (image
// stripping, trimming, a mode switch) drop stale blocks instead of a 400.
struct ModelInfo
{
   const char* id;
   const char* label;
   bool        thinkingBinding;
};

constexpr size_type PICopilotModelCount = 5;

// [0] is PICOPILOT_DEFAULT_MODEL.
extern const ModelInfo kPICopilotModels[PICopilotModelCount];

const ModelInfo* FindModel( const IsoString& id );   // nullptr if unknown
int ModelIndex( const IsoString& id );               // -1 if unknown

} // namespace pcl

#endif // PICopilot_ModelCatalog_h
