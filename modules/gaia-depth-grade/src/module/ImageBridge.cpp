#include "ImageBridge.h"

#include <pcl/ErrorHandler.h>

namespace pcl
{

namespace
{

template <class P>
gdg::Image CopyToCore( const GenericImage<P>& img )
{
   const int W = img.Width(), H = img.Height(), C = img.NumberOfNominalChannels();
   gdg::Image out( W, H, C );
   for ( int c = 0; c < C; ++c )
      for ( int yp = 0; yp < H; ++yp )
      {
         const typename P::sample* row = img.ScanLine( yp, c );
         const int y = yp; // the core frame keeps PixInsight's row order (see ImageBridge.h)
         for ( int x = 0; x < W; ++x )
         {
            double v;
            P::FromSample( v, row[x] );
            out.at( x, y, c ) = v;
         }
      }
   return out;
}

} // namespace

gdg::Image ImageBridge::ToCore( const ImageVariant& image )
{
   if ( !image || image->IsEmpty() )
      throw Error( "Gaia Depth Grade: empty image." );
   if ( image.IsComplexSample() )
      throw Error( "Gaia Depth Grade: complex images are not supported." );
   if ( image.IsFloatSample() )
      switch ( image.BitsPerSample() )
      {
      case 32: return CopyToCore( static_cast<const pcl::Image&>( *image ) );
      case 64: return CopyToCore( static_cast<const pcl::DImage&>( *image ) );
      }
   else
      switch ( image.BitsPerSample() )
      {
      case  8: return CopyToCore( static_cast<const UInt8Image&>( *image ) );
      case 16: return CopyToCore( static_cast<const UInt16Image&>( *image ) );
      case 32: return CopyToCore( static_cast<const UInt32Image&>( *image ) );
      }
   throw Error( "Gaia Depth Grade: unsupported image sample format." );
}

void ImageBridge::ToPcl( const gdg::Image& core, pcl::Image& out )
{
   if ( core.channels != 1 && core.channels != 3 )
      throw Error( "Gaia Depth Grade: only grayscale and RGB images are supported." );
   const int W = core.width, H = core.height, C = core.channels;
   out.AllocateData( W, H, C, C == 3 ? ColorSpace::RGB : ColorSpace::Gray );
   for ( int c = 0; c < C; ++c )
      for ( int yp = 0; yp < H; ++yp )
      {
         float* row = out.ScanLine( yp, c );
         const int y = yp;
         for ( int x = 0; x < W; ++x )
            row[x] = static_cast<float>( core.at( x, y, c ) );
      }
}

} // namespace pcl
