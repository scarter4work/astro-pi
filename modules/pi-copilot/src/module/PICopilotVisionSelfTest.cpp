// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PICopilotVisionSelfTest.h"
#include "Utf8.h"
#include "ViewContext.h"

#include <pcl/AutoViewLock.h>
#include <pcl/Bitmap.h>
#include <pcl/ByteArray.h>
#include <pcl/Exception.h>
#include <pcl/File.h>
#include <pcl/FITSHeaderKeyword.h>
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

// 70 keywords: OBJECT, FILTER, then KW001..KW068 with 100-char values, so
// the 60-keyword cap and the 80-char value cap both engage.
void AddSyntheticKeywords( ImageWindow& window )
{
   FITSKeywordArray k;
   k.Add( FITSHeaderKeyword( "OBJECT", "'M42'", "synthetic" ) );
   k.Add( FITSHeaderKeyword( "FILTER", "'Full'", "synthetic" ) );
   const IsoString longValue = "'" + IsoString( 'x', 100 ) + "'";
   for ( int i = 1; i <= 68; ++i )
      k.Add( FITSHeaderKeyword( IsoString().Format( "KW%03d", i ), longValue, "" ) );
   window.SetKeywords( k );
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

   // ---- Section 2: ViewContext (Task 2) -----------------------------------
   {
      bool ctxOk = false;
      String error;
      nlohmann::json ctx;
      try
      {
         WindowCloser wc{ CreateSyntheticWindow() };
         AddSyntheticKeywords( wc.window );
         ctx = BuildViewContext( wc.window.MainView() );

         const nlohmann::json& g = ctx.at( "geometry" );
         const nlohmann::json& s = ctx.at( "channelStats" );
         const nlohmann::json& k = ctx.at( "fitsKeywords" );
         auto near = []( double a, double b, double tol ) { return a >= b - tol && a <= b + tol; };
         bool medians = s.size() == 3;
         for ( const nlohmann::json& c : s )
            medians = medians && c.at( "median" ).get<double>() > 0.03 && c.at( "median" ).get<double>() < 0.04
                              && c.at( "mad" ).get<double>() > 0;
         ctxOk = ctx.at( "viewId" ).get<std::string>().rfind( "PICopilotSelfTest", 0 ) == 0
              && ctx.at( "isPreview" ) == false
              && ctx.at( "filePath" ) == ""
              && !ctx.contains( "history" )
              && g.at( "width" ) == kSynthW && g.at( "height" ) == kSynthH
              && g.at( "channels" ) == 3 && g.at( "nominalChannels" ) == 3
              && g.at( "bitsPerSample" ) == 32 && g.at( "floatSample" ) == true && g.at( "color" ) == true
              && medians
              && near( s[0].at( "max" ).get<double>(), 1.0, 1e-6 )                  // red square
              && near( s[0].at( "min" ).get<double>(), SynthBackground( 0 ), 1e-6 )
              && near( s[1].at( "min" ).get<double>(), 0.0, 1e-9 )                  // square is 0 in G
              && near( s[1].at( "max" ).get<double>(), SynthBackground( kSynthW - 1 ), 1e-6 )
              && s[0].at( "mean" ).get<double>() > s[1].at( "mean" ).get<double>()
              && ctx.at( "fitsKeywordsTotal" ) == 70
              && ctx.at( "fitsKeywordsOmitted" ) == 10
              && k.size() == size_t( PICopilotMaxFitsKeywords )
              && k[0].at( "name" ) == "OBJECT" && k[0].at( "value" ) == "M42"
              && k[2].at( "name" ) == "KW001"
              && k[2].at( "value" ).get<std::string>().size() == size_t( PICopilotMaxFitsValueChars )
              && k[2].at( "valueTruncated" ) == true
              && !k[0].contains( "valueTruncated" );
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }

      // A null view must be rejected loudly, not produce an empty context.
      bool nullRejected = false;
      try { BuildViewContext( View() ); }
      catch ( const pcl::Exception& ) { nullRejected = true; }

      ctx.erase( "fitsKeywords" );   // keep the verdict readable; counts stay
      out["viewContext"] = ctx;
      out["viewContextError"] = U8( error );
      out["viewContextNullRejected"] = nullRejected;
      out["viewContextOk"] = ctxOk && nullRejected;
      allOk = allOk && ctxOk && nullRejected;
   }

   // ---- inc3 sections end ----
   return allOk;
}

} // namespace pcl
