// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.
//
// SERViewer preview pixel math. PCL-free and header-only, so it is unit
// tested without PixInsight (test/unit/test_preview.cpp); FramePreview.h
// wraps it in a pcl::Bitmap.

#ifndef __PreviewPixels_h
#define __PreviewPixels_h

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "Debayer.h"

namespace planetary::preview
{

// The frame as opaque 0xAARRGGBB words, rows top-down: a linear stretch from 0
// to the frame's maximum onto 0..255 (planetary frames are a bright disc on
// black, so max-scaling shows them well). The maximum is the largest finite
// sample over every channel; a frame whose maximum is <= 0 renders black. One
// channel is replicated to grey; three are R, G, B. Negative and NaN samples
// render 0. Throws std::invalid_argument for an empty image, a channel count
// other than 1 or 3, or sample data that does not match the geometry.
inline std::vector<uint32_t> PreviewArgb( const ser::FloatImage& img )
{
   if ( img.width <= 0 || img.height <= 0 )
      throw std::invalid_argument( "preview: empty image" );
   if ( img.channels != 1 && img.channels != 3 )
      throw std::invalid_argument( "preview: " + std::to_string( img.channels ) + " channels (1 or 3 expected)" );
   const size_t n = size_t( img.width ) * size_t( img.height );
   if ( img.data.size() != n * size_t( img.channels ) )
      throw std::invalid_argument( "preview: sample count does not match the image geometry" );

   float max = 0;
   for ( float s : img.data )
      if ( std::isfinite( s ) && s > max )
         max = s;

   std::vector<uint32_t> out( n, 0xFF000000u );
   if ( !(max > 0) )
      return out;
   const float scale = 255.0f / max;
   auto level = [scale]( float s ) -> uint32_t
   {
      if ( !(s > 0) ) // negative or NaN
         return 0;
      const float v = s * scale;
      return v >= 255.0f ? 255u : uint32_t( v + 0.5f );
   };
   const float* s = img.data.data();
   for ( size_t i = 0; i < n; ++i )
      if ( img.channels == 1 )
      {
         const uint32_t g = level( *s++ );
         out[i] = 0xFF000000u | (g << 16) | (g << 8) | g;
      }
      else
      {
         const uint32_t r = level( s[0] ), g = level( s[1] ), b = level( s[2] );
         s += 3;
         out[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
      }
   return out;
}

// x1, y1 exclusive.
struct FitBox
{
   int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// The largest rectangle with the image's aspect ratio that fits a boxW x boxH
// box, centred (the rest of the box is letterbox). Empty when any size <= 0.
inline FitBox FitRect( int boxW, int boxH, int imgW, int imgH )
{
   if ( boxW <= 0 || boxH <= 0 || imgW <= 0 || imgH <= 0 )
      return {};
   // Compare boxW/boxH with imgW/imgH in 64-bit integers: no rounding decides the side.
   int w, h;
   if ( int64_t( boxW ) * imgH <= int64_t( boxH ) * imgW )
   {
      w = boxW;
      h = int( (int64_t( boxW ) * imgH + imgW / 2) / imgW );
   }
   else
   {
      h = boxH;
      w = int( (int64_t( boxH ) * imgW + imgH / 2) / imgH );
   }
   const int x0 = (boxW - w) / 2, y0 = (boxH - h) / 2;
   return { x0, y0, x0 + w, y0 + h };
}

} // namespace planetary::preview

#endif // __PreviewPixels_h
