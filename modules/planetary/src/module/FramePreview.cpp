// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "FramePreview.h"

#include <pcl/Brush.h>
#include <pcl/ButtonCodes.h>
#include <pcl/Console.h>
#include <pcl/Cursor.h>
#include <pcl/Exception.h>
#include <pcl/Graphics.h>
#include <pcl/Pen.h>

#include <stdexcept>
#include <vector>

#include "PreviewPixels.h"
#include "VideoControls.h"

namespace preview = planetary::preview;

namespace pcl
{

namespace
{

// `argb()` (PreviewPixels.h) as a bitmap; its std::invalid_argument becomes pcl::Error "<what>: <reason>".
template <class F>
Bitmap ArgbBitmap( const planetary::ser::FloatImage& image, const char* what, F&& argb )
{
   std::vector<uint32_t> words;
   try
   {
      words = argb();
   }
   catch ( const std::invalid_argument& e )
   {
      throw Error( String( what ) + ": " + e.what() );
   }
   Bitmap bitmap( image.width, image.height, BitmapFormat::RGB32 );
   const uint32_t* src = words.data();
   for ( int y = 0; y < image.height; ++y, src += image.width )
   {
      RGBA* row = bitmap.ScanLine( y );
      for ( int x = 0; x < image.width; ++x )
         row[x] = RGBA( src[x] );
   }
   return bitmap;
}

} // namespace

Bitmap MakePreviewBitmap( const planetary::ser::FloatImage& image, const char* what )
{
   return ArgbBitmap( image, what, [&image]() { return preview::PreviewArgb( image ); } );
}

Bitmap MakeStackPreviewBitmap( const planetary::ser::FloatImage& image )
{
   return ArgbBitmap( image, "PlanetaryStack: cannot show the stack", [&image]()
   {
      const preview::DisplayRange r = preview::StackDisplayRange( image );
      return preview::PreviewArgb( image, r.black, r.white );
   } );
}

Bitmap MakeDisplayPreviewBitmap( const planetary::ser::FloatImage& image )
{
   return ArgbBitmap( image, "PlanetaryStack: cannot show the mineral moon", [&image]()
   {
      return preview::PreviewArgb( image, 0.0f, 1.0f );
   } );
}

// ----------------------------------------------------------------------------

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
   const preview::FitBox fit = preview::FitRect( r.Width(), r.Height(), m_bitmap.Width(), m_bitmap.Height() );
   if ( fit.x1 <= fit.x0 || fit.y1 <= fit.y0 )
      return;
   G.EnableSmoothInterpolation();
   G.DrawScaledBitmap( r.x0 + fit.x0, r.y0 + fit.y0, r.x0 + fit.x1, r.y0 + fit.y1, m_bitmap );
}

// ----------------------------------------------------------------------------

ZoomPreview::ZoomPreview( Control& parent )
   : Control( parent )
   , m_bitmap( Bitmap::Null() )
{
   OnPaint( (Control::paint_event_handler)&ZoomPreview::e_Paint, *this );
   OnResize( (Control::resize_event_handler)&ZoomPreview::e_Resize, *this );
   OnMousePress( (Control::mouse_button_event_handler)&ZoomPreview::e_MousePress, *this );
   OnMouseMove( (Control::mouse_event_handler)&ZoomPreview::e_MouseMove, *this );
   OnMouseRelease( (Control::mouse_button_event_handler)&ZoomPreview::e_MouseRelease, *this );
}

void ZoomPreview::SetBitmap( const Bitmap& bitmap )
{
   m_bitmap = bitmap;
   m_note.Clear();
   m_dragging = false;
   m_fit = true;
   m_cx = m_bitmap.Width() / 2.0;
   m_cy = m_bitmap.Height() / 2.0;
   UpdateCursor();
   Update();
}

void ZoomPreview::ReplaceBitmap( const Bitmap& bitmap )
{
   if ( m_bitmap.IsNull() || bitmap.Width() != m_bitmap.Width() || bitmap.Height() != m_bitmap.Height() )
   {
      SetBitmap( bitmap );
      return;
   }
   m_bitmap = bitmap;
   m_note.Clear();
   Update();
}

void ZoomPreview::Clear( const String& note )
{
   m_bitmap = Bitmap::Null();
   m_note = note;
   m_dragging = false;
   m_fit = true;
   UpdateCursor();
   Update();
}

double ZoomPreview::ShownZoom() const
{
   if ( !m_fit || m_bitmap.IsNull() )
      return m_zoom;
   return preview::FitZoom( Width(), Height(), m_bitmap.Width(), m_bitmap.Height() );
}

void ZoomPreview::ZoomToFit()
{
   m_fit = true;
   m_cx = m_bitmap.Width() / 2.0;
   m_cy = m_bitmap.Height() / 2.0;
   UpdateCursor();
   Update();
}

void ZoomPreview::SetZoom( double zoom )
{
   if ( m_bitmap.IsNull() || !(zoom > 0) )
      return;
   m_zoom = zoom;
   m_fit = false;
   preview::ClampCentre( Width(), Height(), m_bitmap.Width(), m_bitmap.Height(), m_zoom, m_cx, m_cy );
   UpdateCursor();
   Update();
}

void ZoomPreview::ZoomIn()
{
   SetZoom( preview::ZoomStep( ShownZoom(), true/*in*/ ) );
}

void ZoomPreview::ZoomOut()
{
   SetZoom( preview::ZoomStep( ShownZoom(), false/*in*/ ) );
}

bool ZoomPreview::CanPan() const
{
   return !m_fit && !m_bitmap.IsNull() &&
          (m_bitmap.Width() * m_zoom > Width() || m_bitmap.Height() * m_zoom > Height());
}

void ZoomPreview::UpdateCursor()
{
   SetCursor( pcl::Cursor( m_dragging ? StdCursor::ClosedHand : CanPan() ? StdCursor::OpenHand : StdCursor::Arrow ) );
}

namespace
{

// Every handler body runs through this: nothing may cross into the core
// (VideoControls.h), and a failure is said on the Process Console.
template <class F>
void PreviewGuard( F&& f )
{
   GuardedCall( f, []( const String& message )
   {
      Console().CriticalLn( "<end><cbr>*** PlanetaryStack preview: " + message.EncodedHTMLSpecialChars() );
   } );
}

} // namespace

void ZoomPreview::e_Paint( Control& sender, const pcl::Rect& /*updateRect*/ )
{
   PreviewGuard( [this]()
   {
      const pcl::Rect r = BoundsRect();
      Graphics G( *this );
      G.FillRect( r, Brush( RGBAColor( 0, 0, 0 ) ) );
      if ( m_bitmap.IsNull() )
      {
         if ( !m_note.IsEmpty() )
         {
            G.SetPen( Pen( RGBAColor( 160, 160, 160 ) ) );
            G.DrawTextRect( r, m_note, TextAlign::Center|TextAlign::VertCenter );
         }
         return;
      }
      if ( m_fit )
      {
         const preview::FitBox fit = preview::FitRect( r.Width(), r.Height(), m_bitmap.Width(), m_bitmap.Height() );
         if ( fit.x1 <= fit.x0 || fit.y1 <= fit.y0 )
            return;
         G.EnableSmoothInterpolation();
         G.DrawScaledBitmap( r.x0 + fit.x0, r.y0 + fit.y0, r.x0 + fit.x1, r.y0 + fit.y1, m_bitmap );
         return;
      }
      const preview::ZoomView v = preview::ZoomRect( r.Width(), r.Height(), m_bitmap.Width(), m_bitmap.Height(),
                                                     m_zoom, m_cx, m_cy );
      if ( v.Empty() )
         return;
      G.EnableSmoothInterpolation( m_zoom < 1 );
      G.DrawScaledBitmapRect( r.x0 + v.dx0, r.y0 + v.dy0, r.x0 + v.dx1, r.y0 + v.dy1, m_bitmap,
                              v.sx0, v.sy0, v.sx1, v.sy1 );
   } );
}

void ZoomPreview::e_Resize( Control& sender, int newWidth, int newHeight, int oldWidth, int oldHeight )
{
   PreviewGuard( [=]()
   {
      if ( !m_fit && !m_bitmap.IsNull() )
         preview::ClampCentre( newWidth, newHeight, m_bitmap.Width(), m_bitmap.Height(), m_zoom, m_cx, m_cy );
      UpdateCursor();
   } );
}

void ZoomPreview::e_MousePress( Control& sender, const pcl::Point& pos, int button, unsigned buttons, unsigned modifiers )
{
   PreviewGuard( [=]()
   {
      if ( button != MouseButton::Left || !CanPan() )
         return;
      m_dragging = true;
      m_dragFrom = pos;
      m_dragCx = m_cx;
      m_dragCy = m_cy;
      UpdateCursor();
   } );
}

void ZoomPreview::e_MouseMove( Control& sender, const pcl::Point& pos, unsigned buttons, unsigned modifiers )
{
   PreviewGuard( [=]()
   {
      if ( !m_dragging )
         return;
      // The image follows the pointer: the centre moves the other way, in image pixels.
      m_cx = m_dragCx - (pos.x - m_dragFrom.x) / m_zoom;
      m_cy = m_dragCy - (pos.y - m_dragFrom.y) / m_zoom;
      preview::ClampCentre( Width(), Height(), m_bitmap.Width(), m_bitmap.Height(), m_zoom, m_cx, m_cy );
      Update();
   } );
}

void ZoomPreview::e_MouseRelease( Control& sender, const pcl::Point& pos, int button, unsigned buttons, unsigned modifiers )
{
   PreviewGuard( [=]()
   {
      if ( button != MouseButton::Left || !m_dragging )
         return;
      m_dragging = false;
      UpdateCursor();
   } );
}

} // namespace pcl
