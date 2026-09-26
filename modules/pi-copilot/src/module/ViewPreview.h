// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ViewPreview_h
#define PICopilot_ViewPreview_h

#include <pcl/Bitmap.h>
#include <pcl/Image.h>
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
 * The read-only k x k block average that RenderViewPreview and StepStats use:
 * k = max(1, ceil(longEdge/maxEdge)); output floor(W/k) x floor(H/k), nominal
 * channels only, 32-bit float, integer samples normalized to [0,1] through
 * the pixel traits. rowStride > 1 sums only every rowStride-th row of each
 * block (fewer reads); *samplesPerBlock (optional) = k*ceil(k/rowStride), the
 * samples averaged per output pixel. The view's image is only read (under
 * AutoViewWriteLock) and never duplicated at full resolution. Root thread
 * only; the caller has checked the view is not busy. Throws pcl::Error with
 * exactly: "view has no image", "complex-sample images cannot be previewed",
 * "image is too thin to preview (WxH)".
 */
Image BlockAveragedCopy( const View& view, int maxEdge, int& blockFactor, int rowStride = 1,
                         int* samplesPerBlock = nullptr );

/*
 * RenderViewPreview's steps 2-4, shared with the journey thumbnail (one copy of
 * the display pipeline): bicubic-spline Resample of `work` IN PLACE to a long
 * edge <= maxEdge (no upscaling) -> unlinked auto-STF (center = per-channel
 * median, sigma = MAD x 1.4826, both measured on the small copy; PCL defaults)
 * applied to `work` -> Bitmap::Render (zoom 1, RGBK, no transparency). Root
 * thread only (Bitmap). Throws what PCL throws.
 */
Bitmap StretchAndRender( Image& work, int maxEdge );

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
