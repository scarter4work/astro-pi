// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __FramePreview_h
#define __FramePreview_h

#include <pcl/Bitmap.h>
#include <pcl/Control.h>

#include "Debayer.h"

namespace pcl
{

// The SERViewer preview bitmap: a linear stretch of the frame from 0 to its
// maximum onto 8-bit RGB (maximum <= 0 renders black), one channel replicated
// to grey. The pixel math is planetary::preview::PreviewArgb (PreviewPixels.h,
// unit tested). Throws pcl::Error for an image it cannot show.
Bitmap MakePreviewBitmap( const planetary::ser::FloatImage& image );

// Paints one bitmap scaled to fit its client area, keeping the aspect ratio,
// letterboxed on black. Black and empty until SetBitmap().
class FramePreview : public Control
{
public:
   FramePreview( Control& parent = Control::Null() );

   void SetBitmap( const Bitmap& bitmap );
   void Clear();
   bool HasBitmap() const
   {
      return !m_bitmap.IsNull();
   }

private:
   Bitmap m_bitmap;

   void e_Paint( Control& sender, const pcl::Rect& updateRect );
};

} // namespace pcl

#endif // __FramePreview_h
