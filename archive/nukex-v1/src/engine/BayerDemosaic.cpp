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

#include "BayerDemosaic.h"

#include <algorithm>

namespace pcl
{

// ----------------------------------------------------------------------------
// ParseBayerPattern
// ----------------------------------------------------------------------------

BayerPattern ParseBayerPattern( const IsoString& fitsValue )
{
   IsoString s = fitsValue;

   // Strip surrounding single quotes (FITS keyword values are often quoted)
   if ( s.Length() >= 2 && s[0] == '\'' && s[s.Length() - 1] == '\'' )
      s = s.Substring( 1, s.Length() - 2 );

   s.Trim();
   s.ToUppercase();

   if ( s == "RGGB" )
      return RGGB;
   if ( s == "BGGR" )
      return BGGR;
   if ( s == "GRBG" )
      return GRBG;
   if ( s == "GBRG" )
      return GBRG;

   return Unknown;
}

// ----------------------------------------------------------------------------
// GetBayerOffsets
// ----------------------------------------------------------------------------

BayerOffsets GetBayerOffsets( BayerPattern pattern )
{
   BayerOffsets o;

   switch ( pattern )
   {
   case RGGB:
      o.R  = { 0, 0 };
      o.G1 = { 1, 0 };
      o.G2 = { 0, 1 };
      o.B  = { 1, 1 };
      break;

   case BGGR:
      o.B  = { 0, 0 };
      o.G1 = { 1, 0 };
      o.G2 = { 0, 1 };
      o.R  = { 1, 1 };
      break;

   case GRBG:
      o.G1 = { 0, 0 };
      o.R  = { 1, 0 };
      o.B  = { 0, 1 };
      o.G2 = { 1, 1 };
      break;

   case GBRG:
      o.G1 = { 0, 0 };
      o.B  = { 1, 0 };
      o.R  = { 0, 1 };
      o.G2 = { 1, 1 };
      break;

   default:
      // Unknown - default to RGGB
      o.R  = { 0, 0 };
      o.G1 = { 1, 0 };
      o.G2 = { 0, 1 };
      o.B  = { 1, 1 };
      break;
   }

   return o;
}

// ----------------------------------------------------------------------------
// Helper: classify pixel type at (x, y) given Bayer offsets
// Returns 0=R, 1=G, 2=B
// ----------------------------------------------------------------------------

static inline int PixelType( int x, int y, const BayerOffsets& o )
{
   int cx = x & 1;  // position within 2x2 cell
   int cy = y & 1;

   if ( cx == o.R.dx && cy == o.R.dy )
      return 0; // Red
   if ( cx == o.B.dx && cy == o.B.dy )
      return 2; // Blue
   return 1; // Green (G1 or G2)
}

// ----------------------------------------------------------------------------
// Helper: determine if a green pixel is on a red row or blue row
// Returns true if this green pixel is on the same row as the red pixel
// ----------------------------------------------------------------------------

static inline bool IsGreenOnRedRow( int x, int y, const BayerOffsets& o )
{
   int cy = y & 1;
   return cy == o.R.dy;
}

// ----------------------------------------------------------------------------
// BayerDemosaic::Demosaic - Full image bilinear demosaic
// ----------------------------------------------------------------------------

Image BayerDemosaic::Demosaic( const Image& cfa, BayerPattern pattern )
{
   int width  = cfa.Width();
   int height = cfa.Height();

   Image rgb( width, height, ColorSpace::RGB );

   BayerOffsets offsets = GetBayerOffsets( pattern );

   const float* src = cfa[0]; // single-channel CFA data

   #pragma omp parallel for
   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         int ptype = PixelType( x, y, offsets );

         // Clamped neighbor coordinates
         int xm1 = std::max( x - 1, 0 );
         int xp1 = std::min( x + 1, width - 1 );
         int ym1 = std::max( y - 1, 0 );
         int yp1 = std::min( y + 1, height - 1 );

         float R, G, B;

         if ( ptype == 0 ) // Red pixel
         {
            // R: direct read
            R = src[y * width + x];

            // G: average of up to 4 cardinal neighbors
            G = ( src[ym1 * width + x] +
                  src[yp1 * width + x] +
                  src[y * width + xm1] +
                  src[y * width + xp1] ) * 0.25f;

            // B: average of up to 4 diagonal neighbors
            B = ( src[ym1 * width + xm1] +
                  src[ym1 * width + xp1] +
                  src[yp1 * width + xm1] +
                  src[yp1 * width + xp1] ) * 0.25f;
         }
         else if ( ptype == 2 ) // Blue pixel
         {
            // R: average of up to 4 diagonal neighbors
            R = ( src[ym1 * width + xm1] +
                  src[ym1 * width + xp1] +
                  src[yp1 * width + xm1] +
                  src[yp1 * width + xp1] ) * 0.25f;

            // G: average of up to 4 cardinal neighbors
            G = ( src[ym1 * width + x] +
                  src[yp1 * width + x] +
                  src[y * width + xm1] +
                  src[y * width + xp1] ) * 0.25f;

            // B: direct read
            B = src[y * width + x];
         }
         else // Green pixel
         {
            G = src[y * width + x]; // direct read

            if ( IsGreenOnRedRow( x, y, offsets ) )
            {
               // Green on red row: R neighbors are left/right, B neighbors are top/bottom
               R = ( src[y * width + xm1] +
                     src[y * width + xp1] ) * 0.5f;
               B = ( src[ym1 * width + x] +
                     src[yp1 * width + x] ) * 0.5f;
            }
            else
            {
               // Green on blue row: R neighbors are top/bottom, B neighbors are left/right
               R = ( src[ym1 * width + x] +
                     src[yp1 * width + x] ) * 0.5f;
               B = ( src[y * width + xm1] +
                     src[y * width + xp1] ) * 0.5f;
            }
         }

         rgb.Pixel( x, y, 0 ) = R;
         rgb.Pixel( x, y, 1 ) = G;
         rgb.Pixel( x, y, 2 ) = B;
      }
   }

   return rgb;
}

// ----------------------------------------------------------------------------
// BayerDemosaic::DemosaicRow - Row-based bilinear demosaic for streaming
// ----------------------------------------------------------------------------

void BayerDemosaic::DemosaicRow( const float* prevRow, const float* curRow,
                                  const float* nextRow, int y, int width,
                                  BayerPattern pattern, int channel, float* output )
{
   BayerOffsets offsets = GetBayerOffsets( pattern );

   for ( int x = 0; x < width; ++x )
   {
      int ptype = PixelType( x, y, offsets );

      // Clamped neighbor x coordinates
      int xm1 = std::max( x - 1, 0 );
      int xp1 = std::min( x + 1, width - 1 );

      float value;

      if ( channel == 0 ) // Red channel
      {
         if ( ptype == 0 )
         {
            // At red pixel: direct read
            value = curRow[x];
         }
         else if ( ptype == 2 )
         {
            // At blue pixel: average of 4 diagonal neighbors
            value = ( prevRow[xm1] + prevRow[xp1] +
                      nextRow[xm1] + nextRow[xp1] ) * 0.25f;
         }
         else // Green pixel
         {
            if ( IsGreenOnRedRow( x, y, offsets ) )
            {
               // Green on red row: R is left/right
               value = ( curRow[xm1] + curRow[xp1] ) * 0.5f;
            }
            else
            {
               // Green on blue row: R is top/bottom
               value = ( prevRow[x] + nextRow[x] ) * 0.5f;
            }
         }
      }
      else if ( channel == 1 ) // Green channel
      {
         if ( ptype == 1 )
         {
            // At green pixel: direct read
            value = curRow[x];
         }
         else
         {
            // At red or blue pixel: average of 4 cardinal neighbors
            value = ( prevRow[x] + nextRow[x] +
                      curRow[xm1] + curRow[xp1] ) * 0.25f;
         }
      }
      else // channel == 2, Blue channel
      {
         if ( ptype == 2 )
         {
            // At blue pixel: direct read
            value = curRow[x];
         }
         else if ( ptype == 0 )
         {
            // At red pixel: average of 4 diagonal neighbors
            value = ( prevRow[xm1] + prevRow[xp1] +
                      nextRow[xm1] + nextRow[xp1] ) * 0.25f;
         }
         else // Green pixel
         {
            if ( IsGreenOnRedRow( x, y, offsets ) )
            {
               // Green on red row: B is top/bottom
               value = ( prevRow[x] + nextRow[x] ) * 0.5f;
            }
            else
            {
               // Green on blue row: B is left/right
               value = ( curRow[xm1] + curRow[xp1] ) * 0.5f;
            }
         }
      }

      output[x] = value;
   }
}

// ----------------------------------------------------------------------------

} // namespace pcl
