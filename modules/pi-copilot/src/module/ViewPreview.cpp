// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ViewPreview.h"

#include <pcl/AutoViewLock.h>
#include <pcl/Bitmap.h>
#include <pcl/DisplayFunction.h>
#include <pcl/Exception.h>
#include <pcl/File.h>
#include <pcl/Image.h>
#include <pcl/ImageVariant.h>
#include <pcl/PixelInterpolation.h>
#include <pcl/Resample.h>
#include <pcl/Vector.h>

#include <algorithm>

namespace pcl
{

namespace
{

struct TempFileGuard
{
   String path;
   ~TempFileGuard()
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

// k x k integer block average of the source's first n channels into dst
// (already allocated as floor(W/k) x floor(H/k) x n). The source is only
// read. P::FromSample normalizes integer samples to [0,1] (e.g. uint16/65535),
// so every sample type lands on the same float scale. stride > 1 sums only
// every stride-th row of each block (k*ceil(k/stride) samples per output pixel).
template <class P>
void BlockAverage( const GenericImage<P>& src, Image& dst, int k, int n, int stride )
{
   const int sw = src.Width();
   const int dw = dst.Width();
   const int dh = dst.Height();
   const int rowsUsed = (k + stride - 1)/stride;
   const double norm = 1.0/(double( k )*rowsUsed);
   for ( int c = 0; c < n; ++c )
   {
      const typename P::sample* s = src.PixelData( c );
      float* d = dst.PixelData( c );
      for ( int y = 0; y < dh; ++y )
         for ( int x = 0; x < dw; ++x )
         {
            double sum = 0;
            for ( int j = 0; j < k; j += stride )
            {
               const typename P::sample* row = s + size_type( y*k + j )*sw + size_type( x )*k;
               for ( int i = 0; i < k; ++i )
               {
                  double v;
                  P::FromSample( v, row[i] );
                  sum += v;
               }
            }
            *d++ = float( sum*norm );
         }
   }
}

} // namespace

Image BlockAveragedCopy( const View& view, int maxEdge, int& blockFactor, int rowStride, int* samplesPerBlock )
{
   View v = view;                   // alias; locking needs a non-const View
   AutoViewWriteLock lock( v );
   ImageVariant src = v.Image();
   if ( !src )
      throw Error( "view has no image" );
   if ( src.IsComplexSample() )
      throw Error( "complex-sample images cannot be previewed" );
   const int w = src.Width();
   const int h = src.Height();
   const int n = src.NumberOfNominalChannels();   // 1 (grey) or 3 (RGB)
   const int k = std::max( 1, (std::max( w, h ) + maxEdge - 1)/maxEdge );
   if ( w/k < 1 || h/k < 1 )
      throw Error( String().Format( "image is too thin to preview (%dx%d)", w, h ) );
   const int stride = std::max( 1, rowStride );
   blockFactor = k;
   if ( samplesPerBlock != nullptr )
      *samplesPerBlock = k*((k + stride - 1)/stride);
   Image work;
   work.AllocateData( w/k, h/k, n, src.IsColor() ? ColorSpace::RGB : ColorSpace::Gray );
#define BLOCK_AVERAGE( I ) BlockAverage( static_cast<const I&>( *src ), work, k, n, stride )
   SOLVE_TEMPLATE_REAL_2( src, BLOCK_AVERAGE )
#undef BLOCK_AVERAGE
   return work;
}

Bitmap StretchAndRender( Image& work, int maxEdge )
{
   // 2. Downscale to a long edge <= maxEdge.
   const int longEdge = std::max( work.Width(), work.Height() );
   if ( longEdge > maxEdge )
   {
      BicubicSplinePixelInterpolation bicubic;
      Resample resample( bicubic, double( maxEdge )/longEdge );
      resample >> work;
   }

   // 3. Auto-STF, statistics measured on the small copy.
   const int n = work.NumberOfNominalChannels();
   const Rect r = work.Bounds();
   DVector center( n ), sigma( n );
   for ( int c = 0; c < n; ++c )
   {
      center[c] = work.Median( r, c, c );
      sigma[c] = PICopilotMadToSigma*work.MAD( center[c], r, c, c );
   }
   DisplayFunction stf;
   stf.SetLinkedRGB( false );
   stf.ComputeAutoStretch( sigma, center );
   stf >> work;

   // 4. Render to an 8-bit bitmap.
   return Bitmap::Render( ImageVariant( &work ), 1/*zoom*/, DisplayChannel::RGBK, false/*transparency*/ );
}

ViewPreviewResult RenderViewPreview( const View& view )
{
   ViewPreviewResult res;
   TempFileGuard guard;
   try
   {
      if ( view.IsNull() )
      {
         res.error = "no view to preview";
         return res;
      }

      // 1. Read-only block average into a new small float image (shared with StepStats).
      Image work;
      try
      {
         work = BlockAveragedCopy( view, PICopilotPreviewBlockEdge, res.blockFactor );
      }
      catch ( const Error& x )
      {
         res.error = x.Message();   // exact increment-3 wording (the earlier self-tests pin it)
         return res;
      }

      // 2-4. Downscale, auto-STF, render (shared with the journey thumbnail).
      Bitmap bmp = StretchAndRender( work, PICopilotPreviewMaxEdge );
      res.width = bmp.Width();
      res.height = bmp.Height();
      if ( std::max( res.width, res.height ) > PICopilotPreviewMaxEdge )
      {
         res.error = String().Format( "preview is %dx%d, over the %d px limit",
                                      res.width, res.height, PICopilotPreviewMaxEdge );
         return res;
      }

      // 5. JPEG through a temp file (PCL has no in-memory JPEG encoder).
      guard.path = File::UniqueFileName( File::SystemTempDirectory(), 12, "picopilot-preview-", ".jpg" );
      res.tempPath = guard.path;
      bmp.Save( guard.path, PICopilotPreviewJpegQuality );
      const ByteArray jpeg = File::ReadFile( guard.path );
      File::Remove( guard.path );   // eager; the guard covers every other exit
      if ( jpeg.Length() < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8 )
      {
         res.error = "Bitmap::Save did not produce a JPEG";
         return res;
      }
      res.jpegBytes = jpeg.Length();
      res.base64 = IsoString::ToBase64( jpeg );
      if ( res.base64.Length() > PICopilotMaxImageBase64Bytes )
      {
         res.error = String().Format( "preview JPEG is too large to send (%u base64 bytes)",
                                      unsigned( res.base64.Length() ) );
         res.base64.Clear();
         return res;
      }
      res.ok = true;
   }
   catch ( const pcl::Exception& x )
   {
      res.error = "preview failed: " + x.Message();
   }
   catch ( const std::exception& x )
   {
      res.error = String( "preview failed: " ) + String( x.what() );
   }
   catch ( ... )
   {
      res.error = "preview failed: unknown error";
   }
   if ( !res.ok )
      res.base64.Clear();
   return res;
}

} // namespace pcl
