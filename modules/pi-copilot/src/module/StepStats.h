// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_StepStats_h
#define PICopilot_StepStats_h

#include <pcl/Image.h>
#include <pcl/String.h>
#include <pcl/View.h>

#include <vector>

namespace pcl
{

constexpr int PICopilotJourneyThumbEdge        = 256;   // px, long edge (Ruling 8)
constexpr int PICopilotJourneyThumbJpegQuality = 85;

struct ChannelStats
{
   int    channel = 0;
   double median = 0;   // of the block-averaged copy (Ruling 7)
   double mad = 0;      // raw MAD of the block-averaged copy
   double mean = 0;
   double min = 0;
   double max = 0;
   double noise = 0;    // per ORIGINAL pixel (Ruling 6)
};

struct StepStatsResult
{
   bool                      ok = false;
   String                    error;
   std::vector<ChannelStats> channels;        // nominal channels only
   int                       blockFactor = 0;
   int                       rowStride = 1;
   int                       samplesPerBlock = 0;
   String                    thumbnailPath;   // set when requested AND written
   String                    thumbnailError;  // a failed thumbnail never fails the stats
   double                    elapsedMs = 0;
};

// Statistics of a view's current image for the journey, from
// BlockAveragedCopy( view, PICopilotPreviewBlockEdge, k, PICopilotJourneyStatsRowStride ).
// thumbnailPath non-empty: also writes a 256 px JPEG there (directories
// created). Root thread only. Never throws. A busy view is refused at once
// ("view <id> is busy (locked by a running process)").
StepStatsResult ComputeStepStats( const View& view, const String& thumbnailPath );

// Ruling 6 on one channel of a float image: 1.4826 * MAD of the 4-neighbour
// Laplacian residual / sqrt(1.25). 0 for images narrower than 3 px. Any thread.
double LaplacianNoiseSigma( const Image& img, int channel );

// StretchAndRender( copy, PICopilotJourneyThumbEdge ) -- the preview's own
// downscale + unlinked auto-STF + render -- then JPEG q85 at path through
// SafeRenderFile (private render, JPEG check, atomic rename; a symlink or
// non-regular target is refused, a failure leaves no file). "" when written,
// else why. Root thread only (Bitmap).
String WriteJourneyThumbnail( const Image& blockAveraged, const String& path );

} // namespace pcl

#endif // PICopilot_StepStats_h
