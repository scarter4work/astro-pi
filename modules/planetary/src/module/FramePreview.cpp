// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "FramePreview.h"

#include <pcl/Brush.h>
#include <pcl/Exception.h>
#include <pcl/Graphics.h>

#include <stdexcept>
#include <vector>

#include "PreviewPixels.h"

namespace pcl
{

Bitmap MakePreviewBitmap( const planetary::ser::FloatImage& image )
{
   std::vector<uint32_t> argb;
   try
   {
      argb = planetary::preview::PreviewArgb( image );
   }
   catch ( const std::invalid_argument& e )
   {
      throw Error( String( "SERViewer: cannot show the frame: " ) + e.what() );
   }
   Bitmap bitmap( image.width, image.height, BitmapFormat::RGB32 );
   const uint32_t* src = argb.data();
   for ( int y = 0; y < image.height; ++y, src += image.width )
   {
      RGBA* row = bitmap.ScanLine( y );
      for ( int x = 0; x < image.width; ++x )
         row[x] = RGBA( src[x] );
   }
   return bitmap;
}

FramePreview::FramePreview( Control& parent )
   : Control( parent )
   , m_bitmap( Bitmap::Null() )
{
   OnPaint( (Control::paint_event_handler)&FramePreview::e_Paint, *this );
}

void FramePreview::SetBitmap( const Bitmap& bitmap )
{
   m_bitmap = bitmap;
   Update();
}

void FramePreview::Clear()
{
   m_bitmap = Bitmap::Null();
   Update();
}

void FramePreview::e_Paint( Control& sender, const pcl::Rect& /*updateRect*/ )
{
   const pcl::Rect r = BoundsRect();
   Graphics G( *this );
   G.FillRect( r, Brush( RGBAColor( 0, 0, 0 ) ) );
   if ( m_bitmap.IsNull() )
      return;
   const planetary::preview::FitBox fit =
      planetary::preview::FitRect( r.Width(), r.Height(), m_bitmap.Width(), m_bitmap.Height() );
   if ( fit.x1 <= fit.x0 || fit.y1 <= fit.y0 )
      return;
   G.EnableSmoothInterpolation();
   G.DrawScaledBitmap( r.x0 + fit.x0, r.y0 + fit.y0, r.x0 + fit.x1, r.y0 + fit.y1, m_bitmap );
}

} // namespace pcl
