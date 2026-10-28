// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __FramePreview_h
#define __FramePreview_h

#include <pcl/Bitmap.h>
#include <pcl/Control.h>

#include "Debayer.h"

namespace pcl
{

// The preview bitmap of SERViewer's frames and PlanetaryStack's stack: a
// linear stretch from 0 to the image's maximum onto 8-bit RGB (maximum <= 0
// renders black), one channel replicated to grey -- a display copy only, the
// image itself is untouched. The pixel math is planetary::preview::PreviewArgb
// (PreviewPixels.h, unit tested). Throws pcl::Error "<what>: <reason>" for an
// image it cannot show.
Bitmap MakePreviewBitmap( const planetary::ser::FloatImage& image, const char* what = "SERViewer: cannot show the frame" );

// PlanetaryStack's preview of a stack: the same, stretched over
// preview::StackDisplayRange (black at the background, white at the 99.99th
// percentile) -- a display copy only.
Bitmap MakeStackPreviewBitmap( const planetary::ser::FloatImage& image );

// PlanetaryStack's preview of a display-ready image (the mineral moon, samples in
// [0, 1]): 0..1 onto 0..255 as it is, no stretch.
Bitmap MakeDisplayPreviewBitmap( const planetary::ser::FloatImage& image );

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

// PlanetaryStack's preview: one bitmap on black, at Fit (scaled to the client
// area, aspect kept, smooth) or at a zoom (screen pixels per image pixel:
// nearest neighbour from 100 %, so pixels stay sharp; smooth below) around a
// view centre the mouse drags. The view math is PreviewPixels.h's (FitRect,
// ZoomRect, ClampCentre, ZoomStep). Black and empty until SetBitmap(); while
// empty it may show a note (one grey line, centred).
class ZoomPreview : public Control
{
public:
   ZoomPreview( Control& parent = Control::Null() );

   // A new bitmap is shown at Fit.
   void SetBitmap( const Bitmap& bitmap );
   // Another view of the same image (the stack and its mineral moon, a new
   // strength): shown at the zoom and place of the one it replaces; a bitmap of
   // another size is shown at Fit (SetBitmap).
   void ReplaceBitmap( const Bitmap& bitmap );
   void Clear( const String& note = String() );
   bool HasBitmap() const
   {
      return !m_bitmap.IsNull();
   }

   void ZoomToFit();
   // `zoom` around the current view centre (the image's middle from Fit).
   void SetZoom( double zoom );
   void ZoomIn();
   void ZoomOut();
   bool IsFit() const
   {
      return m_fit;
   }
   // The zoom shown: Fit's for the current size when fitting.
   double ShownZoom() const;

private:
   Bitmap m_bitmap;
   String m_note;
   bool   m_fit = true;
   double m_zoom = 1;
   double m_cx = 0, m_cy = 0;   // view centre, image coordinates
   bool   m_dragging = false;
   pcl::Point m_dragFrom;
   double m_dragCx = 0, m_dragCy = 0;

   // The image is larger than the client area at the current zoom (dragging moves it).
   bool CanPan() const;
   void UpdateCursor();

   void e_Paint( Control& sender, const pcl::Rect& updateRect );
   void e_Resize( Control& sender, int newWidth, int newHeight, int oldWidth, int oldHeight );
   void e_MousePress( Control& sender, const pcl::Point& pos, int button, unsigned buttons, unsigned modifiers );
   void e_MouseMove( Control& sender, const pcl::Point& pos, unsigned buttons, unsigned modifiers );
   void e_MouseRelease( Control& sender, const pcl::Point& pos, int button, unsigned buttons, unsigned modifiers );
};

} // namespace pcl

#endif // __FramePreview_h
