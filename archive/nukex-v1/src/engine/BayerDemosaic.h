//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// BayerDemosaic - Bilinear Bayer demosaicing for CFA/RGGB astro camera data

#ifndef __BayerDemosaic_h
#define __BayerDemosaic_h

#include <pcl/Image.h>
#include <pcl/String.h>

namespace pcl
{

// ----------------------------------------------------------------------------
// Bayer pattern types
// ----------------------------------------------------------------------------

enum BayerPattern
{
   RGGB,
   BGGR,
   GRBG,
   GBRG,
   Unknown
};

// ----------------------------------------------------------------------------
// BayerOffsets - Positions of R, G1, G2, B within a 2x2 Bayer cell
// ----------------------------------------------------------------------------

struct BayerOffsets
{
   struct Pos { int dx, dy; };

   Pos R;
   Pos G1;
   Pos G2;
   Pos B;
};

// ----------------------------------------------------------------------------
// Free functions
// ----------------------------------------------------------------------------

/// Parse a FITS BAYERPAT keyword value to a BayerPattern enum.
/// Strips surrounding quotes, trims whitespace, and uppercases before matching.
/// Returns Unknown for unrecognized patterns.
BayerPattern ParseBayerPattern( const IsoString& fitsValue );

/// Get the 2x2 cell offsets for each color filter position.
BayerOffsets GetBayerOffsets( BayerPattern pattern );

// ----------------------------------------------------------------------------
// BayerDemosaic - Bilinear Bayer demosaicing
// ----------------------------------------------------------------------------
//
// Provides two modes of operation:
//
// 1. Full-image demosaic: Takes a single-channel CFA image and produces
//    a 3-channel RGB image of the same dimensions using bilinear interpolation.
//
// 2. Row-based demosaic: Stateless, operates on 3 raw CFA rows (prev, cur, next)
//    and produces one demosaiced row for a single channel. Designed for use with
//    FrameStreamer for streaming demosaic without loading the full image.
//
// Both modes use bilinear interpolation with clamped edge handling.
// ----------------------------------------------------------------------------

class BayerDemosaic
{
public:

   /// Full-image bilinear Bayer demosaic.
   /// @param cfa Single-channel CFA image (raw Bayer data)
   /// @param pattern The Bayer filter pattern
   /// @return 3-channel RGB image with same dimensions as input
   static Image Demosaic( const Image& cfa, BayerPattern pattern );

   /// Row-based bilinear Bayer demosaic for streaming.
   /// Stateless: operates on three raw CFA rows to produce one demosaiced output row
   /// for a single color channel.
   /// @param prevRow Raw CFA pixels for row y-1 (use curRow if y==0)
   /// @param curRow Raw CFA pixels for row y
   /// @param nextRow Raw CFA pixels for row y+1 (use curRow if y==height-1)
   /// @param y Row index in the full image (determines Bayer position)
   /// @param width Image width in pixels
   /// @param pattern The Bayer filter pattern
   /// @param channel Output channel: 0=R, 1=G, 2=B
   /// @param output Pre-allocated buffer of at least width floats
   static void DemosaicRow( const float* prevRow, const float* curRow,
                            const float* nextRow, int y, int width,
                            BayerPattern pattern, int channel, float* output );
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __BayerDemosaic_h
