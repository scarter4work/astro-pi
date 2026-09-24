// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_VisionSelfTest_h
#define PICopilot_VisionSelfTest_h

#include <nlohmann/json.hpp>

namespace pcl
{

// Increment-3 headless self-test sections (vision smoke, ViewContext,
// ViewPreview, ProcessCatalog, request shape, gated real vision call).
// Root thread only (ImageWindow/View/Bitmap are UIObjects). Adds its keys
// to `out`; returns true iff every section passed.
bool RunVisionSelfTest( nlohmann::json& out );

} // namespace pcl

#endif // PICopilot_VisionSelfTest_h
