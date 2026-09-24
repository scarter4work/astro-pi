// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ViewPreview_h
#define PICopilot_ViewPreview_h

#include <pcl/String.h>
#include <pcl/View.h>

namespace pcl
{

constexpr int       PICopilotPreviewMaxEdge      = 1024;           // px, long edge
constexpr int       PICopilotPreviewJpegQuality  = 85;
constexpr size_type PICopilotMaxImageBase64Bytes = 5*1024*1024;    // Anthropic per-image limit
constexpr double    PICopilotMadToSigma          = 1.4826;
constexpr int       PICopilotPreviewBlockEdge    = 2048;           // px, long edge after block-average

struct ViewPreviewResult
{
   bool      ok = false;
   String    error;
   IsoString base64;       // standard Base64 of the JPEG; empty unless ok
   int       width = 0;
   int       height = 0;
   size_type jpegBytes = 0;
   int       blockFactor = 0; // k of the k x k block average applied to the source (1 = none)
   String    tempPath;     // staging path used (already deleted)
};

/*
 * Display-only JPEG preview of a view for the model:
 *   read the view's image READ-ONLY (under AutoViewWriteLock) and integer
 *   block-average it straight into a NEW small 32-bit float image
 *   (k = max(1, ceil(longEdge/2048)); output floor(W/k) x floor(H/k), nominal
 *   channels only, ragged right/bottom edge dropped; integer samples are
 *   normalized to [0,1] through PCL pixel traits) ->
 *   bicubic-spline Resample to a long edge <= PICopilotPreviewMaxEdge ->
 *   auto-STF (DisplayFunction::ComputeAutoStretch; center = per-channel
 *   median, sigma = MAD x 1.4826, both measured on the SMALL copy; unlinked;
 *   PCL defaults -2.8 / 0.25) ->
 *   Bitmap::Render -> temp .jpg (q85) -> File::ReadFile -> delete -> Base64.
 * The stretch is computed from the data, independent of the user's own
 * screen STF, so the preview is deterministic.
 *
 * Memory bound: the only image allocation is the small float copy, at most
 * 2048 px on its long edge (<= 2048 x 2048 x 3 x 4 B = 48 MiB, typically
 * ~2048 x 1366 x 3 x 4 B = 32 MiB for a 24 MP RGB frame). The user's
 * full-resolution image is never duplicated.
 *
 * Root thread only (View/Bitmap are UIObjects). Never throws; failures come
 * back as ok=false + error. Never modifies the view's image. The temp file is
 * removed on every path.
 */
ViewPreviewResult RenderViewPreview( const View& view );

} // namespace pcl

#endif // PICopilot_ViewPreview_h
