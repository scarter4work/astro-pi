// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PICopilotVisionSelfTest.h"
#include "Utf8.h"

#include <pcl/AutoViewLock.h>
#include <pcl/Bitmap.h>
#include <pcl/ByteArray.h>
#include <pcl/Exception.h>
#include <pcl/File.h>
#include <pcl/Image.h>
#include <pcl/ImageVariant.h>
#include <pcl/ImageWindow.h>
#include <pcl/View.h>

#include <utility>

namespace pcl
{

namespace
{

// Synthetic test image: a dim horizontal grey gradient (identical in R,G,B)
// with a pure-red square. 2000 px wide so ViewPreview must downscale it.
constexpr int kSynthW = 2000, kSynthH = 1500;
constexpr int kSquareX0 = 900, kSquareY0 = 650, kSquareSize = 200;

double SynthBackground( int x )
{
   return 0.02 + 0.03*x/(kSynthW - 1);
}

bool IsJpeg( const ByteArray& b )
{
   const size_type n = b.Length();
   return n >= 4 && b[0] == 0xFF && b[1] == 0xD8 && b[n-2] == 0xFF && b[n-1] == 0xD9;
}

// Owns a (possibly null) ImageWindow and force-closes the core-side window on
// destruction. Move-only: ImageWindow is an alias handle (its own destructor
// does NOT close the core-side window -- see ImageWindow.h:388-393), so
// exactly one WindowCloser must be responsible for a given window at a time.
// A window must be handed to a WindowCloser IMMEDIATELY after construction,
// before any code that could throw runs -- see CreateSyntheticWindow().
struct WindowCloser
{
   ImageWindow window;

   WindowCloser() = default;
   explicit WindowCloser( ImageWindow w ) : window( std::move( w ) )
   {
   }

   WindowCloser( const WindowCloser& ) = delete;
   WindowCloser& operator=( const WindowCloser& ) = delete;

   WindowCloser( WindowCloser&& ) = default;
   WindowCloser& operator=( WindowCloser&& other )
   {
      if ( this != &other )
      {
         Close();
         window = std::move( other.window );
      }
      return *this;
   }

   ~WindowCloser()
   {
      Close();
   }

private:

   void Close()
   {
      try
      {
         if ( !window.IsNull() )
            window.ForceClose();
      }
      catch ( ... )
      {
      }
   }
};

// Fills an already-created window with the synthetic test image (dim grey
// gradient + pure-red square). Takes the window by reference so its caller
// keeps whatever ownership guard (WindowCloser) it already established --
// this function must never be the first thing done with a freshly
// constructed window.
void FillSyntheticWindow( ImageWindow& window )
{
   View view = window.MainView();
   AutoViewLock lock( view );
   ImageVariant v = view.Image();
   if ( !v || !v.IsFloatSample() || v.BitsPerSample() != 32 )
      throw Error( "synthetic window is not a 32-bit float image" );
   Image& img = static_cast<Image&>( *v );
   for ( int y = 0; y < kSynthH; ++y )
      for ( int x = 0; x < kSynthW; ++x )
      {
         const bool sq = x >= kSquareX0 && x < kSquareX0 + kSquareSize
                      && y >= kSquareY0 && y < kSquareY0 + kSquareSize;
         const float bg = float( SynthBackground( x ) );
         img.Pixel( x, y, 0 ) = sq ? 1.0f : bg;
         img.Pixel( x, y, 1 ) = sq ? 0.0f : bg;
         img.Pixel( x, y, 2 ) = sq ? 0.0f : bg;
      }
}

// Constructs the hidden 2000x1500 synthetic RGB test window and returns it
// already wrapped in a WindowCloser, so a caller can never observe a
// constructed-but-unguarded window: if FillSyntheticWindow() throws (or
// anything else does, in a later task that extends this function), the
// WindowCloser already owns the window and force-closes it on unwind.
WindowCloser CreateSyntheticWindow()
{
   // Hidden window (ImageWindow.h:348 -- "The new image window will be hidden").
   ImageWindow window( kSynthW, kSynthH, 3, 32, true/*floatSample*/, true/*color*/,
                       false/*initialProcessing*/, IsoString( "PICopilotSelfTest" ) );
   if ( window.IsNull() )
      throw Error( "ImageWindow construction returned a null window" );
   // Ownership transfers to the guard HERE, before anything else can throw.
   WindowCloser wc{ std::move( window ) };
   FillSyntheticWindow( wc.window );
   return wc;
}

struct SmokeTempGuard
{
   String path;
   ~SmokeTempGuard()
   {
      try
      {
         if ( !path.IsEmpty() && File::Exists( path ) )
            File::Remove( path );
      }
      catch ( ... )
      {
      }
   }
};

} // namespace

bool RunVisionSelfTest( nlohmann::json& out )
{
   bool allOk = true;

   // ---- Section 1: platform smoke (Task 1) --------------------------------
   // Proves, under --automation-mode: ImageWindow creation, View::Image()
   // read under a write lock, ImageVariant copy, Bitmap::Render, and
   // Bitmap::Save to a temp .jpg that reads back as a JPEG and is removed.
   {
      bool windowOk = false, readOk = false, renderOk = false, jpegOk = false;
      String error, path;
      try
      {
         WindowCloser wc = CreateSyntheticWindow();
         windowOk = true;
         View view = wc.window.MainView();
         ImageVariant copy;
         copy.CreateFloatImage( 32 );
         {
            AutoViewWriteLock lock( view );
            ImageVariant src = view.Image();
            readOk = bool( src ) && src.Width() == kSynthW && src.Height() == kSynthH
                  && src.NumberOfChannels() == 3;
            copy.CopyImage( src );
         }
         Bitmap bmp = Bitmap::Render( copy, -4/*1:4*/, DisplayChannel::RGBK, false/*transparency*/ );
         renderOk = !bmp.IsNull() && bmp.Width() == kSynthW/4 && bmp.Height() == kSynthH/4;
         SmokeTempGuard guard;
         guard.path = path = File::UniqueFileName( File::SystemTempDirectory(), 12, "picopilot-smoke-", ".jpg" );
         bmp.Save( path, 85 );
         jpegOk = IsJpeg( File::ReadFile( path ) );
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }
      const bool tempRemoved = !path.IsEmpty() && !File::Exists( path );
      const bool smokeOk = windowOk && readOk && renderOk && jpegOk && tempRemoved;
      out["smokeWindowOk"] = windowOk;
      out["smokeReadOk"] = readOk;
      out["smokeRenderOk"] = renderOk;
      out["smokeJpegOk"] = jpegOk;
      out["smokeTempRemoved"] = tempRemoved;
      out["smokeError"] = U8( error );
      out["visionSmokeOk"] = smokeOk;
      allOk = allOk && smokeOk;
   }

   // ---- inc3 sections end ----
   return allOk;
}

} // namespace pcl
