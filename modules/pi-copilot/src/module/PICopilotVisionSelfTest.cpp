// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PICopilotVisionSelfTest.h"
#include "AnthropicClient.h"
#include "PICopilotModule.h"
#include "PanelPlacement.h"
#include "ProcessCatalog.h"
#include "Utf8.h"
#include "ViewCapture.h"
#include "ViewContext.h"
#include "ViewPreview.h"
#include "VisionTurn.h"

#include <pcl/AutoViewLock.h>
#include <pcl/Bitmap.h>
#include <pcl/ByteArray.h>
#include <pcl/Color.h>
#include <pcl/ElapsedTime.h>
#include <pcl/Exception.h>
#include <pcl/File.h>
#include <pcl/FITSHeaderKeyword.h>
#include <pcl/Image.h>
#include <pcl/ImageVariant.h>
#include <pcl/ImageWindow.h>
#include <pcl/View.h>

#include <chrono>
#include <cstdlib>
#include <thread>
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

// True iff the reply is exactly the one word "red" (any case), ignoring
// surrounding whitespace and punctuation. "Red." passes; "not red", "reddish"
// and "red square" fail.
bool IsSingleWordRed( const String& reply )
{
   auto isLetter = []( char16_type c ) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
   size_type b = 0, e = reply.Length();
   while ( b < e && !isLetter( reply[b] ) )
      ++b;
   while ( e > b && !isLetter( reply[e-1] ) )
      --e;
   return reply.Substring( b, e - b ).Lowercase() == "red";
}

// Index of the first byte that breaks STRICT UTF-8 (RFC 3629: no overlongs,
// no UTF-16 surrogates U+D800..U+DFFF, nothing above U+10FFFF), or -1.
long FirstInvalidUtf8( const std::string& s )
{
   const auto* p = reinterpret_cast<const unsigned char*>( s.data() );
   const size_t n = s.size();
   for ( size_t i = 0; i < n; )
   {
      const unsigned char c = p[i];
      size_t len;
      unsigned lo = 0x80, hi = 0xBF;   // allowed range of the 2nd byte
      if ( c < 0x80 ) { ++i; continue; }
      else if ( c >= 0xC2 && c <= 0xDF ) len = 2;
      else if ( c == 0xE0 ) { len = 3; lo = 0xA0; }
      else if ( c == 0xED ) { len = 3; hi = 0x9F; }   // would be a surrogate
      else if ( c >= 0xE1 && c <= 0xEF ) len = 3;
      else if ( c == 0xF0 ) { len = 4; lo = 0x90; }
      else if ( c >= 0xF1 && c <= 0xF3 ) len = 4;
      else if ( c == 0xF4 ) { len = 4; hi = 0x8F; }
      else return long( i );
      if ( i + len > n || p[i+1] < lo || p[i+1] > hi )
         return long( i );
      for ( size_t k = 2; k < len; ++k )
         if ( (p[i+k] & 0xC0) != 0x80 )
            return long( i );
      i += len;
   }
   return -1;
}

// Hex dump of up to `count` bytes of s starting at `at` (evidence only).
std::string HexAround( const std::string& s, size_t at, size_t count = 24 )
{
   static const char* const d = "0123456789ABCDEF";
   std::string r;
   for ( size_t i = at; i < s.size() && i < at + count; ++i )
   {
      const unsigned char c = static_cast<unsigned char>( s[i] );
      r += d[c >> 4];
      r += d[c & 15];
      r += ' ';
   }
   return r;
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

// 72 keywords: OBJECT, FILTER, two location/identity keywords that must be
// redacted (SITELAT, OBSERVER), then KW001..KW068 with 100-char values, so
// the 60-keyword cap and the 80-char value cap both engage.
constexpr const char* kSyntheticSiteLat  = "+39:55:12.3456";
constexpr const char* kSyntheticObserver = "Jane Q. Stargazer";

void AddSyntheticKeywords( ImageWindow& window )
{
   FITSKeywordArray k;
   k.Add( FITSHeaderKeyword( "OBJECT", "'M42'", "synthetic" ) );
   k.Add( FITSHeaderKeyword( "FILTER", "'Full'", "synthetic" ) );
   k.Add( FITSHeaderKeyword( "SITELAT", "'" + IsoString( kSyntheticSiteLat ) + "'", "synthetic" ) );
   k.Add( FITSHeaderKeyword( "OBSERVER", "'" + IsoString( kSyntheticObserver ) + "'", "synthetic" ) );
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
         // Location/identity keywords: neither name nor value may reach the model.
         const std::string dumped = ctx.dump();
         const bool redactedAbsent = dumped.find( "SITELAT" ) == std::string::npos
                                  && dumped.find( "OBSERVER" ) == std::string::npos
                                  && dumped.find( kSyntheticSiteLat ) == std::string::npos
                                  && dumped.find( kSyntheticObserver ) == std::string::npos;
         auto near = []( double a, double b, double tol ) { return a >= b - tol && a <= b + tol; };
         bool medians = s.size() == 3;
         for ( const nlohmann::json& c : s )
            medians = medians && c.at( "median" ).get<double>() > 0.03 && c.at( "median" ).get<double>() < 0.04
                              && c.at( "mad" ).get<double>() > 0;
         ctxOk = ctx.at( "viewId" ).get<std::string>().rfind( "PICopilotSelfTest", 0 ) == 0
              && ctx.at( "isPreview" ) == false
              && !ctx.contains( "filePath" )                  // never the full local path
              && ctx.at( "fileName" ) == ""
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
              && ctx.at( "fitsKeywordsTotal" ) == 72
              && ctx.at( "fitsKeywordsRedacted" ) == 2
              && ctx.at( "fitsKeywordsOmitted" ) == 10
              && redactedAbsent
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

      // Privacy helpers: file NAME only (no directory / account name), and
      // every listed location/identity keyword is redacted, any case.
      bool privacyOk = ViewContextFileName( "/home/jdoe/astro/2026-09-20/M42_stack.xisf" ) == "M42_stack.xisf"
                    && ViewContextFileName( String() ).IsEmpty()
                    && IsRedactedFitsKeyword( " sitelat " )
                    && !IsRedactedFitsKeyword( "OBJECT" )
                    && !IsRedactedFitsKeyword( "SITE" );
      int listed = 0;
      for ( const char* r : PICopilotRedactedFitsKeywords )
      {
         privacyOk = privacyOk && IsRedactedFitsKeyword( r );
         ++listed;
      }
      privacyOk = privacyOk && listed == 10;
      ctxOk = ctxOk && privacyOk;
      out["viewContextPrivacyOk"] = privacyOk;

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

         const nlohmann::json ctx = {
            { "viewId", "V" },
            { "fullId", "V" },
            { "geometry", { { "width", 10 }, { "height", 8 } } },
            { "channelStats", nlohmann::json::array( { { { "channel", 0 }, { "median", 0.5 } } } ) },
            { "fitsKeywords", nlohmann::json::array( { { { "name", "OBJECT" }, { "value", "M42" } } } ) }
         };
         const AnthropicMessage t = ComposeUserTurn( "what is this?", &ctx, "QUJD" );
         const AnthropicMessage plain = ComposeUserTurn( "hi", nullptr, IsoString() );
         composeOk = t.role == "user" && t.imageJpegBase64 == "QUJD"
                  && t.content.StartsWith( String( "[PixInsight view context]" ) )
                  && t.content.EndsWith( String( "what is this?" ) )
                  && t.content.Contains( String( "\"viewId\":\"V\"" ) )
                  && plain.content == "hi" && plain.imageJpegBase64.IsEmpty();

         // Older turns: image stripped AND context collapsed to fullId +
         // geometry (no stats/keywords re-sent); a context-only older turn
         // (preview failed) is collapsed too; the latest turn is untouched.
         const AnthropicMessage ctxOnly = ComposeUserTurn( "and now?", &ctx, IsoString() );
         Array<AnthropicMessage> hist;
         hist.Add( t );
         hist.Add( AnthropicMessage{ IsoString( "assistant" ), String( "an image" ), IsoString() } );
         hist.Add( ctxOnly );
         hist.Add( AnthropicMessage{ IsoString( "assistant" ), String( "same" ), IsoString() } );
         hist.Add( t );
         StripOlderImages( hist );
         const Array<AnthropicMessage> once = hist;
         StripOlderImages( hist );   // idempotent
         const String note = String::UTF8ToUTF16( kPICopilotImageOmittedNote );
         const String block = String( "[PixInsight view context]" );
         auto collapsed = [&]( const String& c )
         {
            return c.Contains( block )
                && c.Contains( String( "\"collapsed\":true" ) )
                && c.Contains( String( "\"fullId\":\"V\"" ) )
                && c.Contains( String( "\"geometry\":{\"height\":8,\"width\":10}" ) )
                && !c.Contains( String( "channelStats" ) )
                && !c.Contains( String( "fitsKeywords" ) )
                && !c.Contains( String( "\"viewId\"" ) );
         };
         bool unchangedByRerun = once.Length() == hist.Length();
         for ( size_type i = 0; unchangedByRerun && i < hist.Length(); ++i )
            unchangedByRerun = once[i].content == hist[i].content
                            && once[i].imageJpegBase64 == hist[i].imageJpegBase64;
         stripOk = hist[0].imageJpegBase64.IsEmpty()
                && hist[0].content.StartsWith( note + block )
                && hist[0].content.Find( note, note.Length() ) == String::notFound   // not doubled
                && collapsed( hist[0].content )
                && hist[0].content.EndsWith( String( "[/PixInsight view context]\n\nwhat is this?" ) )
                && hist[1].content == "an image"
                && hist[2].imageJpegBase64.IsEmpty()
                && hist[2].content.StartsWith( block )                               // no image note
                && collapsed( hist[2].content )
                && hist[2].content.EndsWith( String( "[/PixInsight view context]\n\nand now?" ) )
                && hist[3].content == "same"
                && hist[4].imageJpegBase64 == "QUJD"
                && hist[4].content == t.content                                     // latest: full context
                && unchangedByRerun;
         if ( !stripOk )
            error = hist[0].content + " || " + hist[2].content;
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
            // No view context: its per-channel stats (R > G,B) could answer
            // "red" from text alone. Only the pixels may answer here.
            const ViewPreviewResult p = RenderViewPreview( view );
            if ( !p.ok )
               error = "preview failed: " + p.error;
            else
            {
               AnthropicClient client{ String( key ) };
               const AnthropicResult r = client.Send( "You are a test. Answer with exactly one word.",
                  { ComposeUserTurn( "What colour is the square in this image? Answer with exactly one word.",
                                     nullptr, p.base64 ) } );
               answer = r.text;
               error = r.error;
               visionOk = r.ok && IsSingleWordRed( r.text );
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

   // ---- Section 7: panel capture + default placement (Task 6, no network) --
   // The pure halves of the panel's Send capture and right-edge placement.
   // The panel wiring itself (checkbox, OnShow, log) is GUI-only.
   {
      bool captureOk = false, noViewOk = false, failedViewOk = false, busyViewOk = false, placementOk = false;
      double busyMs = -1;
      String error;
      try
      {
         {
            WindowCloser wc{ CreateSyntheticWindow() };
            const View view = wc.window.MainView();
            StringList notes;
            const AnthropicMessage t = CaptureViewTurn( "what now?", &view, notes );
            captureOk = t.role == "user" && !t.imageJpegBase64.IsEmpty()
                     && t.content.StartsWith( String( "[PixInsight view context]" ) )
                     && t.content.EndsWith( String( "what now?" ) )
                     && notes.Length() == 1 && notes[0].StartsWith( String( "(attached " ) );
            if ( !captureOk )
               for ( const String& n : notes )
                  error += n + " | ";
         }
         {
            StringList notes;
            const AnthropicMessage t = CaptureViewTurn( "hello", nullptr, notes );
            noViewOk = t.content == "hello" && t.imageJpegBase64.IsEmpty()
                    && notes.Length() == 1
                    && notes[0] == String::UTF8ToUTF16( kPICopilotNoActiveImageNote );
         }
         {
            // A null View: both the context and the preview fail; each failure
            // is reported and the bare text still goes out.
            const View nullView = View::Null();
            StringList notes;
            const AnthropicMessage t = CaptureViewTurn( "still send", &nullView, notes );
            failedViewOk = t.content == "still send" && t.imageJpegBase64.IsEmpty()
                        && notes.Length() == 2
                        && notes[0].StartsWith( String( "View context failed: " ) )
                        && notes[1].StartsWith( String( "Preview failed: " ) );
            if ( !failedViewOk )
               for ( const String& n : notes )
                  error += n + " | ";
         }
         {
            // A view locked by a running process (View::Lock(), as processes
            // lock their targets): Send must not touch it. Expect exactly the
            // busy note, text only, and a prompt return.
            WindowCloser wc{ CreateSyntheticWindow() };
            const View view = wc.window.MainView();
            StringList notes;
            AnthropicMessage t;
            {
               View locked = view;
               AutoViewLock lock( locked );   // RAII: Lock() now, Unlock() on scope exit
               ElapsedTime et;
               t = CaptureViewTurn( "busy?", &view, notes );
               busyMs = et()*1000;
            }
            busyViewOk = t.content == "busy?" && t.imageJpegBase64.IsEmpty()
                      && notes.Length() == 1
                      && notes[0] == String::UTF8ToUTF16( kPICopilotViewBusyNote )
                      && busyMs >= 0 && busyMs < 2000
                      && view.CanRead() && view.CanWrite();   // unlocked again
            if ( !busyViewOk )
               for ( const String& n : notes )
                  error += n + " | ";
         }
         {
            // Placement is anchored to the primary screen's CENTER (Cx,Cy),
            // with conservative half-extents halfW = min(Cx, Cy*16/9) and
            // halfH = min(Cy, Cx*9/16): the primary screen's origin is not
            // known, so 2*Cx is NOT its right edge on multi-monitor desktops.
            auto halfW = []( int cx, int cy ) { return Min( cx, cy*16/9 ); };
            auto halfH = []( int cx, int cy ) { return Min( cy, cx*9/16 ); };
            // Inside [Cx-halfW, Cx+halfW] x [Cy-halfH, Cy+halfH], margins honoured.
            auto inside = [&]( const PanelPlacement& p, int cx, int cy )
            {
               return p.ok && p.x >= 0 && p.y >= 0
                   && p.x >= cx - halfW( cx, cy ) && p.x + p.width + 8 <= cx + halfW( cx, cy )
                   && p.y >= cy - halfH( cx, cy ) + 40 && p.y + p.height + 60 <= cy + halfH( cx, cy );
            };
            // Single 2560x1440 primary at the origin: exactly flush right, full height.
            const PanelPlacement a = ComputeDefaultPanelPlacement( 1280, 720, 420, 40, 60, 8 );
            // 2560x1440 primary to the RIGHT of a 1920 px monitor (origin 1920,0):
            // Cx = 3200. Must stay on the primary, i.e. right edge <= 4480.
            const PanelPlacement m = ComputeDefaultPanelPlacement( 3200, 720, 420, 40, 60, 8 );
            // 2560x1440 primary BELOW a 1440 px monitor (origin 0,1440): Cy = 2160.
            const PanelPlacement v = ComputeDefaultPanelPlacement( 1280, 2160, 420, 40, 60, 8 );
            // Width clamped to what fits beside the right margin.
            const PanelPlacement b = ComputeDefaultPanelPlacement( 200, 400, 420, 40, 60, 8 );
            // Unusable geometry: never move/resize.
            const PanelPlacement z = ComputeDefaultPanelPlacement( 0, 0, 420, 40, 60, 8 );
            const PanelPlacement s = ComputeDefaultPanelPlacement( 1280, 40, 420, 40, 60, 8 );
            placementOk = a.ok && a.x == 2560 - 420 - 8 && a.y == 40 && a.width == 420 && a.height == 1440 - 100
                       && inside( a, 1280, 720 )
                       && m.ok && m.x + m.width <= 3200 + halfW( 3200, 720 )
                       && m.x == 1920 + 2560 - 420 - 8 && m.y == 40 && m.height == 1440 - 100
                       && inside( m, 3200, 720 )
                       && v.ok && v.x == 2560 - 420 - 8 && v.y == 1440 + 40 && v.height == 1440 - 100
                       && inside( v, 1280, 2160 )
                       && b.ok && b.width == 392 && b.x == 0 && inside( b, 200, 400 )
                       && !z.ok && !s.ok;
            out["placementMulti"] = { m.x, m.y, m.width, m.height };
            out["placementStacked"] = { v.x, v.y, v.width, v.height };
         }
      }
      catch ( const pcl::Exception& x ) { error += x.Message(); }
      catch ( const std::exception& x ) { error += String( x.what() ); }
      catch ( ... )                     { error += "unknown exception"; }
      const bool ok = captureOk && noViewOk && failedViewOk && busyViewOk && placementOk;
      out["captureTurnOk"] = captureOk;
      out["captureBusyViewOk"] = busyViewOk;
      out["captureBusyMs"] = busyMs;
      out["captureNoViewOk"] = noViewOk;
      out["captureFailedViewOk"] = failedViewOk;
      out["placementOk"] = placementOk;
      out["captureError"] = U8( error );
      out["panelCaptureOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section 8: multi-turn request body is strict UTF-8 on the wire ---
   // Regression for the turn-2 "400: str is not valid UTF-8: surrogates not
   // allowed". Builds the exact turn-2 history the panel builds (turn 1 with
   // view context + image, a long markdown assistant reply, turn 2 with
   // context + image; StripOlderImages after each user turn, as the panel
   // does), then checks both the in-process body and the BYTES the core
   // actually POSTs (captured by the harness's loopback echo server, which
   // strict-decodes them exactly like the API does). Several variants
   // isolate which input carries the offending characters.
   {
      bool   echoSkipped = true, utf8Ok = true;
      String error;
      nlohmann::json variantsOut = nlohmann::json::array();
      try
      {
         // Realistic markdown reply. BMP-only punctuation vs non-BMP
         // (U+1F4F7 camera, U+1D6FC mathematical italic alpha).
         const char* const replyBmp =
            "## SPCC for an HaO3 dual-band OSC frame\n\n"
            "**Short answer** \xE2\x80\x94 yes, but set the filters first \xE2\x86\x92 `HaO3`.\n"
            "- White reference ~ average spiral galaxy; QE \xE2\x89\x88 0.8 at 656 nm\n"
            "- Pixel size 2.9 \xC2\xB5m, sensor at \xE2\x88\x92" "10 \xC2\xB0" "C\n";
         const std::string replyNonBmp = std::string( replyBmp )
            + "- Tip \xF0\x9F\x93\xB7: the \xF0\x9D\x9B\xBC channel ratio drives the Ha/OIII balance.\n";
         const char* const prompt1 = "what should I do next with this image?";
         const char* const prompt2 = "sho, feel free to set the params on spcc";
         const std::string promptNonBmp = std::string( prompt2 ) + " \xF0\x9F\x93\xB7";

         auto ctxWith = []( const std::string& filterValue )
         {
            return nlohmann::json{
               { "viewId", "Image01" }, { "fullId", "Image01" },
               { "geometry", { { "width", 3840 }, { "height", 2160 } } },
               { "fitsKeywords", nlohmann::json::array( {
                  { { "name", "FILTER" },   { "value", filterValue } },
                  { { "name", "BAYERPAT" }, { "value", "RGGB" } },
                  // Latin-1 FITS byte decoded the way ViewContext does it.
                  { { "name", "INSTRUME" }, { "value", U8( String( "ZWO ASI585MC \xB5" ) ) } } } ) }
            };
         };
         const nlohmann::json ctxPlain = ctxWith( "HaO3" );
         const nlohmann::json ctxNonBmp = ctxWith( "HaO3 \xF0\x9D\x9B\xBC" );
         const IsoString jpeg = "/9j/4AAQSkZJRgABAQ==";

         struct Variant { const char* name; std::string reply, prompt; const nlohmann::json* ctx; };
         const Variant variants[] = {
            { "replyBmpOnly",  replyBmp,    prompt2,      &ctxPlain  },
            { "replyNonBmp",   replyNonBmp, prompt2,      &ctxPlain  },
            { "promptNonBmp",  replyBmp,    promptNonBmp, &ctxPlain  },
            { "fitsNonBmp",    replyBmp,    prompt2,      &ctxNonBmp },
         };

         const char* echoUrl = std::getenv( "PICOPILOT_SELFTEST_ECHO_URL" );
         echoSkipped = echoUrl == nullptr || *echoUrl == '\0';
         if ( echoSkipped )
         {
            utf8Ok = false;
            error = "PICOPILOT_SELFTEST_ECHO_URL not set";
         }

         for ( const Variant& v : variants )
         {
            nlohmann::json vo = { { "variant", v.name } };
            // "value":"<FILTER>" exactly as nlohmann dumps it in the context block.
            const std::string wantFilter = "\"value\":\"" + (*v.ctx)["fitsKeywords"][0]["value"].get<std::string>() + "\"";
            // Exactly the panel's sequence (PICopilotInterface::SendCurrentInput
            // + e_Poll_Timer).
            Array<AnthropicMessage> h;
            h.Add( ComposeUserTurn( String::UTF8ToUTF16( prompt1 ), v.ctx, jpeg ) );
            StripOlderImages( h );
            h.Add( AnthropicMessage{ IsoString( "assistant" ), String::UTF8ToUTF16( v.reply.c_str() ), IsoString() } );
            h.Add( ComposeUserTurn( String::UTF8ToUTF16( v.prompt.c_str() ), v.ctx, jpeg ) );
            StripOlderImages( h );

            // (a) In-process: the serialized body must be strict UTF-8 and
            // must carry the reply/prompt text unaltered.
            bool inProcOk = false;
            try
            {
               const std::string body = BuildMessagesRequestBody( PICOPILOT_DEFAULT_MODEL, "sys", h );
               const long bad = FirstInvalidUtf8( body );
               vo["inProcFirstBadByte"] = bad;
               if ( bad >= 0 )
                  vo["inProcBadHex"] = HexAround( body, size_t( bad ) );
               const nlohmann::json j = nlohmann::json::parse( body );
               const nlohmann::json& m = j.at( "messages" );
               const std::string gotReply = m.at( 1 ).at( "content" ).get<std::string>();
               const std::string gotLast = m.at( 2 ).at( "content" ).at( 1 ).at( "text" ).get<std::string>();
               const bool replyExact = gotReply == v.reply;
               const bool promptExact = gotLast.size() >= v.prompt.size()
                  && gotLast.compare( gotLast.size() - v.prompt.size(), v.prompt.size(), v.prompt ) == 0
                  && gotLast.find( wantFilter ) != std::string::npos;   // FITS value intact
               vo["inProcReplyExact"] = replyExact;
               vo["inProcPromptExact"] = promptExact;
               if ( !replyExact )
                  vo["inProcReplyTailHex"] = HexAround( gotReply, gotReply.size() > 40 ? gotReply.size() - 40 : 0, 80 );
               inProcOk = bad < 0 && replyExact && promptExact;
            }
            catch ( const std::exception& x ) { vo["inProcError"] = x.what(); }
            vo["inProcOk"] = inProcOk;

            // (b) On the wire: what the core actually POSTs.
            bool wireOk = false;
            if ( !echoSkipped )
            {
               AnthropicRequest req( String( "sk-ant-invalid-selftest" ), PICOPILOT_DEFAULT_MODEL,
                                     String( "sys" ), h, String( echoUrl ), 30 );
               const AnthropicResult r = req.Perform();
               vo["wireHttpStatus"] = r.httpStatus;
               vo["wireError"] = U8( r.error );
               if ( r.ok )
               {
                  // The echo server replies with the messages array it
                  // parsed from our bytes.
                  try
                  {
                     const nlohmann::json m = nlohmann::json::parse( U8( r.text ) );
                     const std::string gotReply = m.at( 1 ).at( "content" ).get<std::string>();
                     const std::string gotLast = m.at( 2 ).at( "content" ).at( 1 ).at( "text" ).get<std::string>();
                     const bool replyExact = gotReply == v.reply;
                     const bool promptExact = gotLast.size() >= v.prompt.size()
                        && gotLast.compare( gotLast.size() - v.prompt.size(), v.prompt.size(), v.prompt ) == 0
                        && gotLast.find( wantFilter ) != std::string::npos;
                     vo["wireReplyExact"] = replyExact;
                     vo["wirePromptExact"] = promptExact;
                     wireOk = replyExact && promptExact;
                  }
                  catch ( const std::exception& x ) { vo["wireParseError"] = x.what(); }
               }
            }
            vo["wireOk"] = wireOk;
            utf8Ok = utf8Ok && inProcOk && wireOk;
            variantsOut.push_back( vo );
         }
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); utf8Ok = false; }
      catch ( const std::exception& x ) { error = String( x.what() ); utf8Ok = false; }
      catch ( ... )                     { error = "unknown exception"; utf8Ok = false; }
      out["utf8EchoSkipped"] = echoSkipped;
      out["utf8Variants"] = variantsOut;
      out["utf8Error"] = U8( error );
      out["utf8BodyOk"] = utf8Ok;
      allOk = allOk && utf8Ok;
   }

   // ---- Section 8b: gated REAL two-turn conversation ----------------------
   // Turn 1 (context + real preview) -> the model's reply is forced to carry
   // non-BMP text -> turn 2 (context + preview) must be accepted by the API.
   {
      bool twoTurnSkipped = true, twoTurnOk = true;
      String error;
      int status1 = 0, status2 = 0;
      if ( const char* key = std::getenv( "PICOPILOT_TEST_API_KEY" ) )
      {
         twoTurnSkipped = false;
         twoTurnOk = false;
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
               const String sys = "You are a test. Follow the user's formatting instructions exactly.";
               Array<AnthropicMessage> h;
               h.Add( ComposeUserTurn( String::UTF8ToUTF16(
                  "Describe this image in one short markdown line. Begin the line with the characters "
                  "\"\xF0\x9F\x93\xB7 \xF0\x9D\x9B\xBC \xE2\x80\x94 \xE2\x86\x92 \xE2\x89\x88\" exactly." ),
                  &ctx, p.base64 ) );
               StripOlderImages( h );
               const AnthropicResult r1 = client.Send( sys, h );
               status1 = r1.httpStatus;
               if ( !r1.ok )
                  error = "turn 1: " + r1.error;
               else
               {
                  // Keep the model's own text, but guarantee the non-BMP
                  // characters are present even if the model dropped them.
                  String reply = r1.text;
                  if ( !reply.Contains( String::UTF8ToUTF16( "\xF0\x9F\x93\xB7" ) ) )
                     reply += String::UTF8ToUTF16( " \xF0\x9F\x93\xB7 \xF0\x9D\x9B\xBC" );
                  h.Add( AnthropicMessage{ IsoString( "assistant" ), reply, IsoString() } );
                  h.Add( ComposeUserTurn( "Reply with exactly: WORKING", &ctx, p.base64 ) );
                  StripOlderImages( h );
                  const AnthropicResult r2 = client.Send( sys, h );
                  status2 = r2.httpStatus;
                  if ( !r2.ok )
                     error = "turn 2: " + r2.error;
                  twoTurnOk = r2.ok && r2.text.Trimmed().Contains( String( "WORKING" ) );
               }
            }
         }
         catch ( const pcl::Exception& x ) { error = x.Message(); }
         catch ( const std::exception& x ) { error = String( x.what() ); }
         catch ( ... )                     { error = "unknown exception"; }
      }
      out["twoTurnSkipped"] = twoTurnSkipped;
      out["twoTurnStatus1"] = status1;
      out["twoTurnStatus2"] = status2;
      out["twoTurnError"] = U8( error );
      out["twoTurnOk"] = twoTurnOk;
      allOk = allOk && twoTurnOk;
   }

   // ---- inc3 sections end ----

   // Every WindowCloser above has now force-closed its ImageWindow (the last
   // one just above, in Section 7). ImageWindow::ForceClose() only POSTS the
   // core-side teardown -- the actual window/view destruction runs later, off
   // the core's deferred-delete queue -- so with 9 windows torn down in quick
   // succession, that queued teardown can still be in flight when the caller
   // (PICopilotInstance::ExecuteGlobal) returns and the harness's
   // --force-exit tears down the process. That race was observed printing
   // "pthread_mutex_lock() failed" on core's teardown path when the harness
   // ran in a NON-isolated instance slot; it was NOT reproduced (0 hits
   // across 25+ runs) once the harness moved to an isolated test slot
   // (test/run-selftest.sh / test/run-load.sh), so this drain is a
   // defensive measure against a real, understood race rather than a fix
   // proven necessary against a reproduced local failure. Draining the event
   // queue here, on this root thread (required -- see the header comment),
   // gives that deferred teardown a chance to finish before we hand control
   // back; excludeUserInputEvents=true because this is a headless
   // automation-mode run with no user input to preserve. ProcessEvents()
   // docs ask for >=250 ms between calls from the root thread, hence the
   // sleep.
   for ( int i = 0; i < 4; ++i )
   {
      ThePICopilotModule->ProcessEvents( true/*excludeUserInputEvents*/ );
      std::this_thread::sleep_for( std::chrono::milliseconds( 250 ) );
   }

   return allOk;
}

} // namespace pcl
