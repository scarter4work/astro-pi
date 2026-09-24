// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ViewContext_h
#define PICopilot_ViewContext_h

#include <pcl/View.h>

#include <nlohmann/json.hpp>

namespace pcl
{

// FITS keyword caps for the context sent to the model (token cost).
constexpr int PICopilotMaxFitsKeywords   = 60;
constexpr int PICopilotMaxFitsValueChars = 80;

/*
 * JSON description of a view for the model: identity, geometry, robust
 * per-channel statistics of the REAL (usually linear) data, and the first
 * PICopilotMaxFitsKeywords FITS keywords. There is deliberately no process
 * history: PCL exposes no API for it.
 *
 * Root thread only. Reads the image under AutoViewWriteLock (blocks writers,
 * readers unaffected) and never modifies it: statistics use explicit
 * (rect, channel, channel) arguments -- no SelectChannel() calls.
 * Throws pcl::Error for a null view, a view with no image, or a complex image.
 */
nlohmann::json BuildViewContext( const View& view );

} // namespace pcl

#endif // PICopilot_ViewContext_h
