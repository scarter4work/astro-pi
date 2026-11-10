// Star detection for the solver. Port of ImageSolver 6.6.0's detectImageStars(),
// detectStarsForBlindRetry(), binnedImage() and medianBrightFWHM() (ImageSolverEngine.js;
// Copyright (c) 2013-2026 Andres del Pozo, Copyright (c) 2019-2026 Juan Conejero (PTeam);
// PCL License 2.0).
#include "SolverDetect.h"

#include "core/Parallel.h"

#include <pcl/ErrorHandler.h>
#include <pcl/Image.h>
#include <pcl/PSFFit.h>
#include <pcl/StarDetector.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace pcl
{

ImageVariant WorkingGrayscale( const ImageVariant& image )
{
   ImageVariant Y;
   Y.CreateFloatImage( 32 );
   // GetIntensity: HSI intensity (min + max)/2 of a colour image, the gray channel otherwise --
   // ImageSolver's HSI channel 2 of a copy of the image.
   image.GetIntensity( Y );
   return Y;
}

namespace
{

/// binnedImage(): the mean of each k x k block; the incomplete right/bottom blocks are dropped.
ImageVariant Binned( const Image& src, int k )
{
   const int w = src.Width(), h = src.Height(), bw = w/k, bh = h/k;
   ImageVariant out;
   out.CreateFloatImage( 32 );
   out.AllocateImage( bw, bh, 1, ColorSpace::Gray );
   Image& b = static_cast<Image&>( *out );
   b.Zero();
   for ( int y = 0; y < bh*k; ++y )
   {
      const float* s = src.ScanLine( y, 0 );
      float* d = b.ScanLine( y/k, 0 );
      for ( int j = 0; j < bw; ++j )
      {
         double sum = 0;
         for ( int i = 0; i < k; ++i )
            sum += *s++;
         d[j] += float( sum );
      }
   }
   const float f = 1.0f/(k*k);
   for ( float* p = b.PixelData( 0 ), * e = p + size_type( bw )*bh; p < e; ++p )
      *p *= f;
   return out;
}

} // namespace

DetectedStars DetectImageStars( const ImageVariant& gray, const DetectParams& p, int binning )
{
   if ( !gray || gray.IsComplexSample() || !gray.IsFloatSample() || gray.BitsPerSample() != 32 || gray.NumberOfChannels() != 1 )
      throw Error( "DetectImageStars: a one-channel 32-bit floating point image is required" );

   if ( binning > 1 )
   {
      ImageVariant b = Binned( static_cast<const Image&>( *gray ), binning );
      DetectedStars d = DetectImageStars( b, p, 1 );
      for ( gdg::Pt& q : d.pos )
      {
         q.x *= binning;
         q.y *= binning;
      }
      for ( gdg::Box& r : d.rect )
      {
         r.x0 *= binning; r.y0 *= binning;
         r.x1 *= binning; r.y1 *= binning;
      }
      d.brightFWHM *= binning;
      d.tree = std::make_unique<gdg::RectIndex>( d.rect );
      return d;
   }

   // Step 1: star detection (ImageSolver's StarDetector settings; PSF fitting separately).
   StarDetector D;
   D.SetStructureLayers( p.structureLayers );
   D.SetHotPixelFilterRadius( 1 );
   D.SetNoiseReductionFilterRadius( 0 );
   D.SetSensitivity( 0.5f );
   D.SetPeakResponse( float( p.peakResponse ) );
   D.DisableClusteredSources();
   D.SetMaxDistortion( float( p.maxStarDistortion ) );
   D.SetBrightThreshold( float( p.brightThreshold ) );
   D.SetMinStructureSize( 0 );
   D.DisablePSFFitting();
   const StarDetector::star_list S = D.DetectStars( gray );

   DetectedStars out;
   out.rawCount = int( S.Length() );

   // Step 2: PSF fitting (PSF.fitStars: Gaussian, elliptic, tolerance 1e-8, growth 1, in
   // parallel), sorted by decreasing signal.
   struct Fit { bool ok = false; double x, y, sx, sy, signal, fwhm; };
   std::vector<Fit> fits( S.Length() );
   gdg::parallel_for_dynamic( int( S.Length() ), 16, [&]( int i, int )
   {
      const StarDetector::Star& s = S[i];
      PSFFit F( gray, s.pos, DRect( s.rect ), PSFunction::Gaussian, false/*circular*/ );
      if ( F.psf )
         fits[i] = Fit{ true, F.psf.c0.x, F.psf.c0.y, F.psf.sx, F.psf.sy, F.psf.signal,
                        std::sqrt( F.psf.FWHMx()*F.psf.FWHMy() ) };
   } );
   std::vector<size_t> order;
   for ( size_t i = 0; i < fits.size(); ++i )
      if ( fits[i].ok )
         order.push_back( i );
   std::stable_sort( order.begin(), order.end(), [&]( size_t a, size_t b ) { return fits[a].signal > fits[b].signal; } );
   out.psfFits = int( order.size() );

   std::vector<double> signal, fwhm;
   std::vector<gdg::Pt> pos;
   std::vector<gdg::Box> rect;
   std::vector<double> flux;
   for ( size_t i : order )
   {
      const Fit& f = fits[i];
      signal.push_back( f.signal );
      fwhm.push_back( f.fwhm );
      pos.push_back( { f.x, f.y } );
      rect.push_back( { f.x - f.sx/2, f.y - f.sy/2, f.x + f.sx/2, f.y + f.sy/2 } );
      flux.push_back( f.signal );
   }
   out.brightFWHM = gdg::median_bright_fwhm( signal, fwhm );

   // Step 3: remove conflicting sources.
   for ( int i : gdg::remove_conflicting_sources( pos, rect ) )
   {
      out.pos.push_back( pos[i] );
      out.rect.push_back( rect[i] );
      out.flux.push_back( flux[i] );
   }

   // Step 4: the search tree. Step 5: the matching tolerance from the smallest PSF box.
   out.tree = std::make_unique<gdg::RectIndex>( out.rect );
   if ( !out.rect.empty() )
   {
      double d = std::numeric_limits<double>::max();
      for ( const gdg::Box& r : out.rect )
         d = std::min( d, std::min( r.x1 - r.x0, r.y1 - r.y0 ) );
      out.tolerance = gdg::star_matching_tolerance( d );
   }
   return out;
}

} // namespace pcl
