// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.
//
// SERViewer and PlanetaryStack preview pixel math. PCL-free and header-only, so it is unit
// tested without PixInsight (test/unit/test_preview.cpp); FramePreview.h
// wraps it in a pcl::Bitmap.

#ifndef __PreviewPixels_h
#define __PreviewPixels_h

#include <algorithm>
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

// ---- PlanetaryStack's zoomable preview (ZoomPreview) ----------------------
// Zoom = screen pixels per image pixel.

// The zoom levels Zoom in / Zoom out step through.
constexpr double kZoomLevels[] = { 1.0/16, 1.0/12, 1.0/8, 1.0/6, 1.0/4, 1.0/3, 1.0/2, 2.0/3,
                                   1, 1.5, 2, 3, 4, 6, 8, 12, 16 };

// The next level above (in) or below (out) `zoom`, which need not be a level
// (Fit's zoom); the ladder's end when there is none.
inline double ZoomStep( double zoom, bool in )
{
   const size_t n = sizeof kZoomLevels / sizeof kZoomLevels[0];
   if ( in )
   {
      for ( size_t i = 0; i < n; ++i )
         if ( kZoomLevels[i] > zoom * (1 + 1e-9) )
            return kZoomLevels[i];
      return kZoomLevels[n - 1];
   }
   for ( size_t i = n; i-- > 0; )
      if ( kZoomLevels[i] < zoom * (1 - 1e-9) )
         return kZoomLevels[i];
   return kZoomLevels[0];
}

// The zoom at which the image fits the box (FitRect's scale); 0 when any size <= 0.
inline double FitZoom( int boxW, int boxH, int imgW, int imgH )
{
   if ( boxW <= 0 || boxH <= 0 || imgW <= 0 || imgH <= 0 )
      return 0;
   return std::min( double( boxW ) / imgW, double( boxH ) / imgH );
}

// Keeps the view's centre (cx, cy, image coordinates) on the image at `zoom`:
// on an axis where the zoomed image is no larger than the box, the image's
// middle (it shows whole, centred); otherwise within half a view of its edges.
inline void ClampCentre( int boxW, int boxH, int imgW, int imgH, double zoom, double& cx, double& cy )
{
   auto clamp = []( double c, int box, int img, double zoom )
   {
      const double half = box / (2 * zoom);
      if ( img <= 2 * half )
         return img / 2.0;
      return std::min( std::max( c, half ), img - half );
   };
   if ( zoom > 0 )
   {
      cx = clamp( cx, boxW, imgW, zoom );
      cy = clamp( cy, boxH, imgH, zoom );
   }
}

// What a box shows of the image at `zoom` with image point (cx, cy) at its
// centre: the whole image pixels that cover the box, src [sx0, sx1) x [sy0, sy1),
// and where they land, dst [dx0, dx1) x [dy0, dy1) in box coordinates (rounded
// to the nearest pixel; it may reach past the box, which the painter clips).
struct ZoomView
{
   int sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
   int dx0 = 0, dy0 = 0, dx1 = 0, dy1 = 0;
   bool Empty() const
   {
      return sx1 <= sx0 || sy1 <= sy0;
   }
};

inline ZoomView ZoomRect( int boxW, int boxH, int imgW, int imgH, double zoom, double cx, double cy )
{
   ZoomView v;
   if ( boxW <= 0 || boxH <= 0 || imgW <= 0 || imgH <= 0 || !(zoom > 0) )
      return v;
   const double hw = boxW / (2 * zoom), hh = boxH / (2 * zoom);
   v.sx0 = std::max( 0, int( std::floor( cx - hw ) ) );
   v.sx1 = std::min( imgW, int( std::ceil( cx + hw ) ) );
   v.sy0 = std::max( 0, int( std::floor( cy - hh ) ) );
   v.sy1 = std::min( imgH, int( std::ceil( cy + hh ) ) );
   v.dx0 = int( std::lround( boxW / 2.0 + (v.sx0 - cx) * zoom ) );
   v.dx1 = int( std::lround( boxW / 2.0 + (v.sx1 - cx) * zoom ) );
   v.dy0 = int( std::lround( boxH / 2.0 + (v.sy0 - cy) * zoom ) );
   v.dy1 = int( std::lround( boxH / 2.0 + (v.sy1 - cy) * zoom ) );
   return v;
}

} // namespace planetary::preview

#endif // __PreviewPixels_h
