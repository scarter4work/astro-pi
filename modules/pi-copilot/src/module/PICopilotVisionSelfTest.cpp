// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PICopilotVisionSelfTest.h"
#include "AnthropicClient.h"
#include "ProcessCatalog.h"
#include "Utf8.h"
#include "ViewContext.h"
#include "ViewPreview.h"
#include "VisionTurn.h"

#include <pcl/AutoViewLock.h>
#include <pcl/Bitmap.h>
#include <pcl/ByteArray.h>
#include <pcl/Color.h>
#include <pcl/Exception.h>
#include <pcl/File.h>
#include <pcl/FITSHeaderKeyword.h>
#include <pcl/Image.h>
#include <pcl/ImageVariant.h>
#include <pcl/ImageWindow.h>
#include <pcl/View.h>

#include <cstdlib>
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

// Writes the synthetic scene into an image of any real sample type: a dim
// grey gradient with a square that is pure red (RGB) or full white (mono).
// P::ToSample converts the normalized [0,1] value to the native sample range.
template <class P>
void FillSynthetic( GenericImage<P>& img )
{
   const int n = img.NumberOfChannels();
   for ( int y = 0; y < kSynthH; ++y )
      for ( int x = 0; x < kSynthW; ++x )
      {
         const bool sq = x >= kSquareX0 && x < kSquareX0 + kSquareSize
                      && y >= kSquareY0 && y < kSquareY0 + kSquareSize;
         const double bg = SynthBackground( x );
         for ( int c = 0; c < n; ++c )
            img.Pixel( x, y, c ) = P::ToSample( sq ? (c == 0 ? 1.0 : 0.0) : bg );
      }
}

// Fills an already-created window with the synthetic test image. Takes the
// window by reference so its caller keeps whatever ownership guard
// (WindowCloser) it already established -- this function must never be the
// first thing done with a freshly constructed window.
void FillSyntheticWindow( ImageWindow& window, int channels, int bitsPerSample, bool floatSample )
{
   View view = window.MainView();
   AutoViewLock lock( view );
   ImageVariant v = view.Image();
   if ( !v || v.IsComplexSample() || v.IsFloatSample() != floatSample
        || v.BitsPerSample() != bitsPerSample || v.NumberOfChannels() != channels )
      throw Error( "synthetic window does not have the requested sample format" );
#define FILL_SYNTHETIC( I ) FillSynthetic( static_cast<I&>( *v ) )
   SOLVE_TEMPLATE_REAL_2( v, FILL_SYNTHETIC )
#undef FILL_SYNTHETIC
}

// Constructs a hidden 2000x1500 synthetic test window (default: 32-bit float
// RGB; channels=1 gives a mono image) and returns it already wrapped in a
// WindowCloser, so a caller can never observe a constructed-but-unguarded
// window: if FillSyntheticWindow() throws (or anything else does, in a later
// task that extends this function), the WindowCloser already owns the window
// and force-closes it on unwind.
WindowCloser CreateSyntheticWindow( int channels = 3, int bitsPerSample = 32, bool floatSample = true )
{
   // Hidden window (ImageWindow.h:348 -- "The new image window will be hidden").
   ImageWindow window( kSynthW, kSynthH, channels, bitsPerSample, floatSample, channels >= 3/*color*/,
                       false/*initialProcessing*/, IsoString( "PICopilotSelfTest" ) );
   if ( window.IsNull() )
      throw Error( "ImageWindow construction returned a null window" );
   // Ownership transfers to the guard HERE, before anything else can throw.
   WindowCloser wc{ std::move( window ) };
   FillSyntheticWindow( wc.window, channels, bitsPerSample, floatSample );
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

// Order-dependent 64-bit hash of every channel's native pixel buffer
// (GenericImage::Hash64, Image.h:13786), dispatched on the real sample type
// so it is exact for float and integer images alike.
template <class P>
uint64 HashImage( const GenericImage<P>& img )
{
   uint64 h = 0;
   for ( int c = 0; c < img.NumberOfChannels(); ++c )
      h = img.Hash64( c, h );
   return h;
}

uint64 ViewImageHash( View view )
{
   AutoViewWriteLock lock( view );
   ImageVariant v = view.Image();
   if ( !v || v.IsComplexSample() )
      throw Error( "ViewImageHash: view has no real-sample image" );
   uint64 h = 0;
#define HASH_IMAGE( I ) h = HashImage( static_cast<const I&>( *v ) )
   SOLVE_TEMPLATE_REAL_2( v, HASH_IMAGE )
#undef HASH_IMAGE
   return h;
}

// Large synthetic RGB window (long edge > PICopilotPreviewBlockEdge) so
// ViewPreview must take its k >= 2 block-average path. Dim diagonal gradient,
// slightly different per channel so the stretch has real dispersion.
constexpr int kLargeW = 4200, kLargeH = 2800;

WindowCloser CreateLargeSyntheticWindow()
{
   ImageWindow window( kLargeW, kLargeH, 3, 32, true/*floatSample*/, true/*color*/,
                       false/*initialProcessing*/, IsoString( "PICopilotSelfTestLarge" ) );
   if ( window.IsNull() )
      throw Error( "ImageWindow construction returned a null window (large)" );
   WindowCloser wc{ std::move( window ) };
   View view = wc.window.MainView();
   AutoViewLock lock( view );
   ImageVariant v = view.Image();
   if ( !v || !v.IsFloatSample() || v.BitsPerSample() != 32 )
      throw Error( "large synthetic window is not a 32-bit float image" );
   Image& img = static_cast<Image&>( *v );
   for ( int c = 0; c < 3; ++c )
   {
      float* f = img.PixelData( c );
      for ( int y = 0; y < kLargeH; ++y )
         for ( int x = 0; x < kLargeW; ++x )
            *f++ = float( 0.02 + 0.01*c + 0.03*(double( x )/kLargeW + double( y )/kLargeH)/2 );
   }
   return wc;
}

// Renders a preview of the small synthetic scene in the given sample format
// and samples the decoded JPEG at the square's centre and the background.
struct ScenePreviewCheck
{
   ViewPreviewResult p;
   bool   formatOk = false;      // JPEG, sizes, base64 consistency
   bool   unchanged = false;     // user image hash identical before/after
   bool   tempRemoved = false;
   int    sq[3] = { -1, -1, -1 };
   int    bg[3] = { -1, -1, -1 };
   String error;
};

ScenePreviewCheck CheckScenePreview( int channels, int bitsPerSample, bool floatSample )
{
   ScenePreviewCheck r;
   try
   {
      WindowCloser wc{ CreateSyntheticWindow( channels, bitsPerSample, floatSample ) };
      const View view = wc.window.MainView();
      const uint64 before = ViewImageHash( view );
      r.p = RenderViewPreview( view );
      r.unchanged = ViewImageHash( view ) == before;
      r.tempRemoved = !r.p.tempPath.IsEmpty() && !File::Exists( r.p.tempPath );
      if ( r.p.ok )
      {
         const ByteArray jpeg = r.p.base64.FromBase64();
         Bitmap decoded( jpeg.Begin(), jpeg.Length(), "JPG" );
         const double s = double( r.p.width )/kSynthW;
         const RGBA q = decoded.Pixel( int( (kSquareX0 + kSquareSize/2)*s ), int( (kSquareY0 + kSquareSize/2)*s ) );
         const RGBA b = decoded.Pixel( int( 200*s ), int( 200*s ) );
         r.sq[0] = Red( q ); r.sq[1] = Green( q ); r.sq[2] = Blue( q );
         r.bg[0] = Red( b ); r.bg[1] = Green( b ); r.bg[2] = Blue( b );
         r.formatOk = IsJpeg( jpeg )
                   && jpeg.Length() == r.p.jpegBytes
                   && Max( r.p.width, r.p.height ) <= PICopilotPreviewMaxEdge
                   && Max( r.p.width, r.p.height ) >= PICopilotPreviewMaxEdge - 8
                   && r.p.base64.Length() == 4*((r.p.jpegBytes + 2)/3)
                   && r.p.base64.Length() > 1000
                   && r.p.base64.Length() <= PICopilotMaxImageBase64Bytes;
      }
      else
         r.error = r.p.error;
   }
   catch ( const pcl::Exception& x ) { r.error = x.Message(); }
   catch ( const std::exception& x ) { r.error = String( x.what() ); }
   catch ( ... )                     { r.error = "unknown exception"; }
   return r;
}

// Stretched background: neutral grey, neither black nor saturated. The
// upper bound catches integer samples that were not normalized to [0,1]
// (the whole frame then clips to white).
bool BackgroundNeutralGrey( const int bg[3] )
{
   const int lo = Min( bg[0], Min( bg[1], bg[2] ) );
   const int hi = Max( bg[0], Max( bg[1], bg[2] ) );
   return lo > 4 && hi < 200 && hi - lo < 24;
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

   // ---- Section 3: ViewPreview (Task 3) -----------------------------------
   {
      bool previewOk = false, unchanged = false, tempRemoved = false, pixelsOk = false;
      String error;
      ViewPreviewResult p;
      int sqR = -1, sqG = -1, sqB = -1, bgR = -1, bgG = -1, bgB = -1;
      try
      {
         WindowCloser wc{ CreateSyntheticWindow() };
         const View view = wc.window.MainView();
         const uint64 before = ViewImageHash( view );
         p = RenderViewPreview( view );
         unchanged = ViewImageHash( view ) == before;
         tempRemoved = !p.tempPath.IsEmpty() && !File::Exists( p.tempPath );
         if ( p.ok )
         {
            const ByteArray jpeg = p.base64.FromBase64();
            // Decode the JPEG and sample it: the red square must read red and
            // the stretched background must be neutral grey, not black.
            Bitmap decoded( jpeg.Begin(), jpeg.Length(), "JPG" );
            const double s = double( p.width )/kSynthW;
            const RGBA sq = decoded.Pixel( int( (kSquareX0 + kSquareSize/2)*s ), int( (kSquareY0 + kSquareSize/2)*s ) );
            const RGBA bg = decoded.Pixel( int( 200*s ), int( 200*s ) );
            sqR = Red( sq ); sqG = Green( sq ); sqB = Blue( sq );
            bgR = Red( bg ); bgG = Green( bg ); bgB = Blue( bg );
            pixelsOk = sqR > 180 && sqG < 80 && sqB < 80
                    && Min( bgR, Min( bgG, bgB ) ) > 4
                    && Max( bgR, Max( bgG, bgB ) ) - Min( bgR, Min( bgG, bgB ) ) < 24;
            previewOk = IsJpeg( jpeg )
                     && jpeg.Length() == p.jpegBytes
                     && Max( p.width, p.height ) <= PICopilotPreviewMaxEdge
                     && Max( p.width, p.height ) >= PICopilotPreviewMaxEdge - 8
                     && p.base64.Length() == 4*((p.jpegBytes + 2)/3)
                     && p.base64.Length() > 1000
                     && p.base64.Length() <= PICopilotMaxImageBase64Bytes;
         }
         else
            error = p.error;
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }

      const ViewPreviewResult nullResult = RenderViewPreview( View() );
      const bool nullRejected = !nullResult.ok && !nullResult.error.IsEmpty() && nullResult.base64.IsEmpty();

      // Large source (long edge > 2048): must take the k >= 2 block-average
      // path, still land at <= 1024 px, and leave the user image untouched.
      bool largeOk = false, largeUnchanged = false, largeTempRemoved = false;
      String largeError;
      ViewPreviewResult lp;
      try
      {
         WindowCloser wc{ CreateLargeSyntheticWindow() };
         const View view = wc.window.MainView();
         const uint64 before = ViewImageHash( view );
         lp = RenderViewPreview( view );
         largeUnchanged = ViewImageHash( view ) == before;
         largeTempRemoved = !lp.tempPath.IsEmpty() && !File::Exists( lp.tempPath );
         if ( lp.ok )
            largeOk = lp.blockFactor >= 2
                   && IsJpeg( lp.base64.FromBase64() )
                   && Max( lp.width, lp.height ) <= PICopilotPreviewMaxEdge
                   && Max( lp.width, lp.height ) >= PICopilotPreviewMaxEdge - 8
                   && lp.base64.Length() <= PICopilotMaxImageBase64Bytes;
         else
            largeError = lp.error;
      }
      catch ( const pcl::Exception& x ) { largeError = x.Message(); }
      catch ( const std::exception& x ) { largeError = String( x.what() ); }
      catch ( ... )                     { largeError = "unknown exception"; }
      largeOk = largeOk && largeUnchanged && largeTempRemoved;

      const bool ok = previewOk && unchanged && tempRemoved && pixelsOk && nullRejected && largeOk;
      out["previewWidth"] = p.width;
      out["previewHeight"] = p.height;
      out["previewBlockFactor"] = p.blockFactor;
      out["previewJpegBytes"] = p.jpegBytes;
      out["previewBase64Len"] = p.base64.Length();
      out["previewTempRemoved"] = tempRemoved;
      out["previewUserImageUnchanged"] = unchanged;
      out["previewSquareRGB"] = { sqR, sqG, sqB };
      out["previewBackgroundRGB"] = { bgR, bgG, bgB };
      out["previewNullRejected"] = nullRejected;
      out["previewError"] = U8( error );
      out["previewLargeWidth"] = lp.width;
      out["previewLargeHeight"] = lp.height;
      out["previewLargeBlockFactor"] = lp.blockFactor;
      out["previewLargeUserImageUnchanged"] = largeUnchanged;
      out["previewLargeTempRemoved"] = largeTempRemoved;
      out["previewLargeError"] = U8( largeError );
      out["previewLargeOk"] = largeOk;
      out["previewOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section 3b: ViewPreview on 16-bit integer images (Task 3 fix) -----
   // Raw subs are uint16 and the user shoots a mono camera: prove the typed
   // block-average normalizes integer samples and handles 1 channel.
   {
      const ScenePreviewCheck rgb = CheckScenePreview( 3, 16, false );
      const bool rgbOk = rgb.p.ok && rgb.formatOk && rgb.unchanged && rgb.tempRemoved
                      && rgb.sq[0] > 180 && rgb.sq[1] < 80 && rgb.sq[2] < 80
                      && BackgroundNeutralGrey( rgb.bg );
      out["previewU16Width"] = rgb.p.width;
      out["previewU16Height"] = rgb.p.height;
      out["previewU16SquareRGB"] = { rgb.sq[0], rgb.sq[1], rgb.sq[2] };
      out["previewU16BackgroundRGB"] = { rgb.bg[0], rgb.bg[1], rgb.bg[2] };
      out["previewU16UserImageUnchanged"] = rgb.unchanged;
      out["previewU16TempRemoved"] = rgb.tempRemoved;
      out["previewU16Error"] = U8( rgb.error );
      out["previewU16Ok"] = rgbOk;

      const ScenePreviewCheck mono = CheckScenePreview( 1, 16, false );
      const bool monoOk = mono.p.ok && mono.formatOk && mono.unchanged && mono.tempRemoved
                       && mono.sq[0] > mono.bg[0] + 32
                       && BackgroundNeutralGrey( mono.bg );
      out["previewMonoWidth"] = mono.p.width;
      out["previewMonoHeight"] = mono.p.height;
      out["previewMonoSquareRGB"] = { mono.sq[0], mono.sq[1], mono.sq[2] };
      out["previewMonoBackgroundRGB"] = { mono.bg[0], mono.bg[1], mono.bg[2] };
      out["previewMonoUserImageUnchanged"] = mono.unchanged;
      out["previewMonoTempRemoved"] = mono.tempRemoved;
      out["previewMonoError"] = U8( mono.error );
      out["previewMonoOk"] = monoOk;

      allOk = allOk && rgbOk && monoOk;
   }

   // ---- Section 4: ProcessCatalog (Task 4) --------------------------------
   {
      bool listOk = false, pmOk = false, htOk = false, unknownOk = false;
      String error;
      size_t count = 0, summaries = 0;
      nlohmann::json unknownSummaryIds = nlohmann::json::array();
      try
      {
         const nlohmann::json& sums = CompiledProcessSummaries();
         summaries = sums.size();

         const nlohmann::json list = ListProcesses();
         count = list.at( "count" ).get<size_t>();
         bool sawPM = false, sawHT = false;
         std::string prev;
         bool sorted = true;
         for ( const nlohmann::json& row : list.at( "processes" ) )
         {
            const std::string id = row.at( "id" ).get<std::string>();
            sorted = sorted && prev <= id;
            prev = id;
            if ( id == "PixelMath" )
               sawPM = row.value( "summary", std::string() ) == sums.at( "PixelMath" ).get<std::string>();
            if ( id == "HistogramTransformation" )
               sawHT = row.contains( "summary" );
         }
         // Informational: summary keys that name no installed process.
         for ( auto it = sums.begin(); it != sums.end(); ++it )
         {
            bool found = false;
            for ( const nlohmann::json& row : list.at( "processes" ) )
               if ( row.at( "id" ) == it.key() ) { found = true; break; }
            if ( !found )
               unknownSummaryIds.push_back( it.key() );
         }
         listOk = summaries == 25 && count > 50 && count == list.at( "processes" ).size()
               && sorted && sawPM && sawHT;

         const nlohmann::json pm = DescribeProcess( "PixelMath" );
         for ( const nlohmann::json& prm : pm.at( "parameters" ) )
            if ( prm.at( "id" ) == "expression" && prm.at( "type" ) == "String" )
               pmOk = true;
         pmOk = pmOk && pm.value( "summary", std::string() ) == "Arbitrary per-pixel expression evaluation.";

         const nlohmann::json ht = DescribeProcess( "HistogramTransformation" );
         for ( const nlohmann::json& prm : ht.at( "parameters" ) )
            if ( prm.at( "id" ) == "H" && prm.at( "type" ) == "Table" && !prm.value( "columns", nlohmann::json::array() ).empty() )
               htOk = true;

         const nlohmann::json bad = DescribeProcess( "NoSuchProcessXYZ" );
         unknownOk = bad.contains( "error" )
                  && bad.at( "error" ).get<std::string>().rfind( "unknown process id: NoSuchProcessXYZ", 0 ) == 0;
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }

      const bool ok = listOk && pmOk && htOk && unknownOk;
      out["catalogCount"] = count;
      out["catalogSummaries"] = summaries;
      out["catalogUnknownSummaryIds"] = unknownSummaryIds;
      out["catalogListOk"] = listOk;
      out["catalogPixelMathOk"] = pmOk;
      out["catalogHistogramTransformationOk"] = htOk;
      out["catalogUnknownIdOk"] = unknownOk;
      out["catalogError"] = U8( error );
      out["catalogOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section 5: request shape + history stripping (Task 5, no network) --
   {
      bool shapeOk = false, stripOk = false, composeOk = false;
      String error;
      try
      {
         AnthropicMessage img;
         img.role = "user";
         img.content = "look";
         img.imageJpegBase64 = "QUJD";
         Array<AnthropicMessage> h;
         h.Add( AnthropicMessage{ IsoString( "user" ), String( "plain" ), IsoString() } );
         h.Add( AnthropicMessage{ IsoString( "assistant" ), String( "ok" ), IsoString() } );
         h.Add( img );
         const nlohmann::json j = nlohmann::json::parse( BuildMessagesRequestBody( PICOPILOT_DEFAULT_MODEL, "sys", h ) );
         const nlohmann::json& m = j.at( "messages" );
         shapeOk = !j.contains( "stream" )
                && m[0].at( "content" ).is_string() && m[0].at( "content" ) == "plain"
                && m[1].at( "content" ) == "ok"
                && m[2].at( "content" ).is_array() && m[2].at( "content" ).size() == 2
                && m[2]["content"][0].at( "type" ) == "image"
                && m[2]["content"][0].at( "source" ).at( "type" ) == "base64"
                && m[2]["content"][0].at( "source" ).at( "media_type" ) == "image/jpeg"
                && m[2]["content"][0].at( "source" ).at( "data" ) == "QUJD"
                && m[2]["content"][1].at( "type" ) == "text"
                && m[2]["content"][1].at( "text" ) == "look";

         const nlohmann::json ctx = { { "viewId", "V" } };
         const AnthropicMessage t = ComposeUserTurn( "what is this?", &ctx, "QUJD" );
         const AnthropicMessage plain = ComposeUserTurn( "hi", nullptr, IsoString() );
         composeOk = t.role == "user" && t.imageJpegBase64 == "QUJD"
                  && t.content.StartsWith( String( "[PixInsight view context]" ) )
                  && t.content.EndsWith( String( "what is this?" ) )
                  && t.content.Contains( String( "\"viewId\":\"V\"" ) )
                  && plain.content == "hi" && plain.imageJpegBase64.IsEmpty();

         Array<AnthropicMessage> hist;
         hist.Add( t );
         hist.Add( AnthropicMessage{ IsoString( "assistant" ), String( "an image" ), IsoString() } );
         hist.Add( t );
         StripOlderImages( hist );
         StripOlderImages( hist );   // idempotent
         const String note = String::UTF8ToUTF16( kPICopilotImageOmittedNote );
         stripOk = hist[0].imageJpegBase64.IsEmpty()
                && hist[0].content.StartsWith( note )
                && hist[0].content.Find( note, note.Length() ) == String::notFound   // not doubled
                && hist[0].content.EndsWith( String( "what is this?" ) )
                && hist[1].content == "an image"
                && hist[2].imageJpegBase64 == "QUJD";
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }
      const bool ok = shapeOk && composeOk && stripOk;
      out["requestShapeOk"] = shapeOk;
      out["composeTurnOk"] = composeOk;
      out["historyStripOk"] = stripOk;
      out["turnError"] = U8( error );
      out["visionTurnOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section 6: gated REAL vision round-trip (Task 5) -------------------
   // Runs only when PICOPILOT_TEST_API_KEY is set (harness: keyring -> file).
   {
      bool visionSkipped = true, visionOk = true;
      String answer, error;
      if ( const char* key = std::getenv( "PICOPILOT_TEST_API_KEY" ) )
      {
         visionSkipped = false;
         visionOk = false;
         try
         {
            WindowCloser wc{ CreateSyntheticWindow() };
            const View view = wc.window.MainView();
            const nlohmann::json ctx = BuildViewContext( view );
            const ViewPreviewResult p = RenderViewPreview( view );
            if ( !p.ok )
               error = "preview failed: " + p.error;
            else
            {
               AnthropicClient client{ String( key ) };
               const AnthropicResult r = client.Send( "You are a test. Answer with exactly one word.",
                  { ComposeUserTurn( "What colour is the square in this image? Answer with exactly one word.",
                                     &ctx, p.base64 ) } );
               answer = r.text;
               error = r.error;
               visionOk = r.ok && r.text.ContainsIC( String( "red" ) );
            }
         }
         catch ( const pcl::Exception& x ) { error = x.Message(); }
         catch ( const std::exception& x ) { error = String( x.what() ); }
         catch ( ... )                     { error = "unknown exception"; }
      }
      out["visionSkipped"] = visionSkipped;
      out["visionAnswer"] = U8( answer );
      out["visionError"] = U8( error );
      out["visionOk"] = visionOk;
      allOk = allOk && visionOk;
   }

   // ---- inc3 sections end ----
   return allOk;
}

} // namespace pcl
