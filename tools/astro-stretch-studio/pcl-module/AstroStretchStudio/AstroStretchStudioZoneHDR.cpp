// ----------------------------------------------------------------------------
// AstroStretchStudio Zone HDR Engine Implementation
// ----------------------------------------------------------------------------

#include "AstroStretchStudioZoneHDR.h"

#include <pcl/Math.h>
#include <cmath>

namespace pcl
{

// ----------------------------------------------------------------------------

ZoneHDREngine::ZoneHDREngine()
{
}

// ----------------------------------------------------------------------------

ZoneHDREngine::~ZoneHDREngine()
{
}

// ----------------------------------------------------------------------------
// Histogram Computation
// ----------------------------------------------------------------------------

void ZoneHDREngine::ComputeHistogram( const Image& luminance,
                                       FVector& histogram,
                                       int bins ) const
{
   histogram = FVector( bins, float( 0 ) );

   for ( Image::const_sample_iterator i( luminance ); i; ++i )
   {
      int bin = Min( bins - 1, Max( 0, RoundInt( double( *i ) * ( bins - 1 ) ) ) );
      histogram[bin]++;
   }

   // Normalize
   double total = luminance.Width() * luminance.Height();
   if ( total > 0 )
      for ( int i = 0; i < bins; ++i )
         histogram[i] /= float( total );
}

// ----------------------------------------------------------------------------
// Histogram Smoothing
// ----------------------------------------------------------------------------

void ZoneHDREngine::SmoothHistogram( FVector& histogram, int kernelSize ) const
{
   if ( kernelSize < 3 )
      return;

   int halfSize = kernelSize / 2;
   int n = histogram.Length();
   FVector smoothed( n );

   // Create gaussian kernel
   FVector kernel( kernelSize );
   double sigma = kernelSize / 6.0;
   double sum = 0;
   for ( int i = 0; i < kernelSize; ++i )
   {
      double x = i - halfSize;
      kernel[i] = float( std::exp( -0.5 * x * x / ( sigma * sigma ) ) );
      sum += kernel[i];
   }
   for ( int i = 0; i < kernelSize; ++i )
      kernel[i] /= float( sum );

   // Apply convolution
   for ( int i = 0; i < n; ++i )
   {
      double val = 0;
      for ( int k = 0; k < kernelSize; ++k )
      {
         int idx = i + k - halfSize;
         idx = Max( 0, Min( n - 1, idx ) ); // Clamp to bounds
         val += kernel[k] * histogram[idx];
      }
      smoothed[i] = float( val );
   }

   histogram = smoothed;
}

// ----------------------------------------------------------------------------
// Peak Detection
// ----------------------------------------------------------------------------

void ZoneHDREngine::FindHistogramPeaks( const FVector& histogram,
                                         Array<int>& peakBins ) const
{
   peakBins.Clear();
   int n = histogram.Length();

   if ( n < 5 )
      return;

   // Compute first derivative
   FVector derivative( n );
   derivative[0] = 0;
   for ( int i = 1; i < n; ++i )
      derivative[i] = histogram[i] - histogram[i-1];

   // Find zero crossings (peaks are where derivative goes from + to -)
   // Also require the peak to be above a threshold
   double maxHist = 0;
   for ( int i = 0; i < n; ++i )
      maxHist = Max( maxHist, double( histogram[i] ) );

   double peakThreshold = maxHist * 0.02; // Peak must be at least 2% of max

   for ( int i = 2; i < n - 2; ++i )
   {
      // Check for local maximum
      if ( derivative[i-1] > 0 && derivative[i+1] < 0 )
      {
         // Verify it's above threshold
         if ( histogram[i] >= peakThreshold )
         {
            // Verify it's a true local maximum in a 5-bin window
            bool isMax = true;
            for ( int j = -2; j <= 2 && isMax; ++j )
               if ( j != 0 && histogram[i+j] > histogram[i] )
                  isMax = false;

            if ( isMax )
               peakBins.Add( i );
         }
      }
   }

   // Always ensure we have at least the background peak if histogram has content
   if ( peakBins.IsEmpty() && maxHist > 0 )
   {
      // Find the overall maximum
      int maxBin = 0;
      for ( int i = 1; i < n; ++i )
         if ( histogram[i] > histogram[maxBin] )
            maxBin = i;
      peakBins.Add( maxBin );
   }

   // Sort peaks by bin position
   peakBins.Sort();

   // Merge peaks that are too close together (within 5% of histogram range)
   int minSeparation = n / 20;
   Array<int> mergedPeaks;
   for ( size_type i = 0; i < peakBins.Length(); ++i )
   {
      if ( mergedPeaks.IsEmpty() ||
           peakBins[i] - mergedPeaks[mergedPeaks.Length()-1] >= minSeparation )
      {
         mergedPeaks.Add( peakBins[i] );
      }
      else
      {
         // Keep the higher peak
         int lastIdx = int( mergedPeaks.Length() ) - 1;
         if ( histogram[peakBins[i]] > histogram[mergedPeaks[lastIdx]] )
            mergedPeaks[lastIdx] = peakBins[i];
      }
   }

   peakBins = mergedPeaks;
}

// ----------------------------------------------------------------------------
// Zone Boundary Detection
// ----------------------------------------------------------------------------

void ZoneHDREngine::FindZoneBoundaries( const FVector& histogram,
                                         const Array<int>& peakBins,
                                         Array<double>& boundaries ) const
{
   boundaries.Clear();
   int n = histogram.Length();

   if ( peakBins.IsEmpty() )
      return;

   // First boundary is at 0
   boundaries.Add( 0.0 );

   // Find valleys between adjacent peaks
   for ( size_type i = 0; i < peakBins.Length() - 1; ++i )
   {
      int start = peakBins[i];
      int end = peakBins[i+1];

      // Find minimum between these peaks
      int minBin = start;
      double minVal = histogram[start];
      for ( int j = start + 1; j < end; ++j )
      {
         if ( histogram[j] < minVal )
         {
            minVal = histogram[j];
            minBin = j;
         }
      }

      boundaries.Add( double( minBin ) / ( n - 1 ) );
   }

   // Last boundary is at 1
   boundaries.Add( 1.0 );
}

// ----------------------------------------------------------------------------
// Zone Name Generation
// ----------------------------------------------------------------------------

String ZoneHDREngine::GenerateZoneName( double centerValue,
                                         int zoneIndex,
                                         int totalZones ) const
{
   // Name based on luminance position
   if ( centerValue < 0.15 )
      return "Deep Shadows";
   else if ( centerValue < 0.25 )
      return "Shadows";
   else if ( centerValue < 0.40 )
      return "Low Midtones";
   else if ( centerValue < 0.55 )
      return "Midtones";
   else if ( centerValue < 0.70 )
      return "High Midtones";
   else if ( centerValue < 0.85 )
      return "Highlights";
   else
      return "Bright Core";
}

// ----------------------------------------------------------------------------
// Zone Detection (Main Entry Point)
// ----------------------------------------------------------------------------

int ZoneHDREngine::DetectZones( const Image& luminance )
{
   m_zones.Clear();
   m_masks.Clear();

   const int histogramBins = 256;

   // Compute histogram
   FVector histogram;
   ComputeHistogram( luminance, histogram, histogramBins );

   // Smooth histogram to reduce noise
   SmoothHistogram( histogram, 7 );

   // Find peaks
   Array<int> peakBins;
   FindHistogramPeaks( histogram, peakBins );

   // Ensure we have at least MinZones peaks by adding synthetic ones
   while ( int( peakBins.Length() ) < MinZones )
   {
      // Add evenly spaced peaks
      int newPeak;
      if ( peakBins.IsEmpty() )
         newPeak = histogramBins / 2;
      else if ( peakBins.Length() == 1 )
      {
         // Add one at 1/4 or 3/4 depending on where existing peak is
         if ( peakBins[0] < histogramBins / 2 )
            newPeak = histogramBins * 3 / 4;
         else
            newPeak = histogramBins / 4;
      }
      else
      {
         // Find largest gap and add peak there
         int maxGap = 0;
         int gapStart = 0;
         for ( size_type i = 0; i < peakBins.Length() - 1; ++i )
         {
            int gap = peakBins[i+1] - peakBins[i];
            if ( gap > maxGap )
            {
               maxGap = gap;
               gapStart = peakBins[i];
            }
         }
         newPeak = gapStart + maxGap / 2;
      }
      peakBins.Add( newPeak );
      peakBins.Sort();
   }

   // Limit to MaxZones by keeping most prominent peaks
   while ( int( peakBins.Length() ) > MaxZones )
   {
      // Find and remove the lowest peak
      int minIdx = 0;
      for ( size_type i = 1; i < peakBins.Length(); ++i )
         if ( histogram[peakBins[i]] < histogram[peakBins[minIdx]] )
            minIdx = int( i );
      peakBins.Remove( peakBins.At( minIdx ) );
   }

   // Find zone boundaries
   Array<double> boundaries;
   FindZoneBoundaries( histogram, peakBins, boundaries );

   // Create zones
   for ( size_type i = 0; i < peakBins.Length(); ++i )
   {
      double center = double( peakBins[i] ) / ( histogramBins - 1 );
      double lower = boundaries[i];
      double upper = boundaries[i+1];

      String name = GenerateZoneName( center, int( i ), int( peakBins.Length() ) );

      m_zones.Add( Zone( center, lower, upper, name ) );
   }

   return int( m_zones.Length() );
}

// ----------------------------------------------------------------------------
// Mask Weight Computation
// ----------------------------------------------------------------------------

double ZoneHDREngine::ComputeMaskWeight( double luminance, const Zone& zone ) const
{
   // Compute soft mask weight using smoothstep at boundaries
   if ( luminance < zone.lowerBound || luminance > zone.upperBound )
      return 0.0;

   double weight = 1.0;

   // Smooth transition at lower boundary
   double transitionWidth = ( zone.upperBound - zone.lowerBound ) * 0.15;
   transitionWidth = Max( 0.02, transitionWidth ); // At least 2% transition

   if ( luminance < zone.lowerBound + transitionWidth )
   {
      double t = ( luminance - zone.lowerBound ) / transitionWidth;
      weight *= t * t * ( 3 - 2 * t ); // smoothstep
   }

   // Smooth transition at upper boundary
   if ( luminance > zone.upperBound - transitionWidth )
   {
      double t = ( zone.upperBound - luminance ) / transitionWidth;
      weight *= t * t * ( 3 - 2 * t ); // smoothstep
   }

   return weight;
}

// ----------------------------------------------------------------------------
// Mask Generation
// ----------------------------------------------------------------------------

void ZoneHDREngine::GenerateMasks( const Image& luminance )
{
   m_masks.Clear();

   if ( m_zones.IsEmpty() )
      return;

   int width = luminance.Width();
   int height = luminance.Height();

   // Generate mask for each zone
   for ( size_type z = 0; z < m_zones.Length(); ++z )
   {
      Image mask;
      mask.AllocateData( width, height );

      const Zone& zone = m_zones[z];

      for ( int y = 0; y < height; ++y )
      {
         for ( int x = 0; x < width; ++x )
         {
            double lum = luminance( x, y );
            mask( x, y ) = float( ComputeMaskWeight( lum, zone ) );
         }
      }

      m_masks.Add( mask );
   }

   // Normalize masks so they sum to 1 at each pixel
   // This ensures smooth blending without gaps or overlaps
   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double sum = 0;
         for ( size_type z = 0; z < m_masks.Length(); ++z )
            sum += m_masks[z]( x, y );

         if ( sum > 0 )
         {
            for ( size_type z = 0; z < m_masks.Length(); ++z )
               m_masks[z]( x, y ) /= float( sum );
         }
      }
   }
}

// ----------------------------------------------------------------------------
// Get Zone Mask
// ----------------------------------------------------------------------------

Image ZoneHDREngine::GetZoneMask( int zoneIndex ) const
{
   if ( zoneIndex >= 0 && zoneIndex < int( m_masks.Length() ) )
      return m_masks[zoneIndex];
   return Image();
}

// ----------------------------------------------------------------------------
// Apply Zone Adjustments
// ----------------------------------------------------------------------------

void ZoneHDREngine::ApplyZoneAdjustments( Image& luminance, Image& colorImage ) const
{
   if ( m_zones.IsEmpty() || m_masks.IsEmpty() )
      return;

   int width = luminance.Width();
   int height = luminance.Height();
   int numChannels = colorImage.NumberOfChannels();
   bool isColor = numChannels >= 3;

   // Store original luminance for color reconstruction
   Image origLuminance( luminance );

   // Apply intensity adjustments to luminance
   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double lum = luminance( x, y );
         double newLum = lum;

         // Accumulate adjustments from all zones weighted by masks
         for ( size_type z = 0; z < m_zones.Length(); ++z )
         {
            double mask = m_masks[z]( x, y );
            if ( mask <= 0 )
               continue;

            double intensity = m_zones[z].intensity;
            if ( Abs( intensity ) < 0.001 )
               continue;

            // Intensity adjustment: positive = brighten, negative = darken
            // Use a curve that preserves extremes
            double adjustment;
            if ( intensity > 0 )
            {
               // Brighten: lift shadows more than highlights
               adjustment = intensity * ( 1.0 - lum ) * 0.5;
            }
            else
            {
               // Darken: reduce highlights more than shadows
               adjustment = intensity * lum * 0.5;
            }

            newLum += adjustment * mask;
         }

         luminance( x, y ) = float( Range( newLum, 0.0, 1.0 ) );
      }
   }

   // Reconstruct color if applicable
   if ( isColor )
   {
      for ( int y = 0; y < height; ++y )
      {
         for ( int x = 0; x < width; ++x )
         {
            double oldLum = origLuminance( x, y );
            double newLum = luminance( x, y );

            if ( oldLum < 1e-10 )
               continue;

            double scale = newLum / oldLum;

            // Apply saturation adjustments
            double satAdj = 0;
            for ( size_type z = 0; z < m_zones.Length(); ++z )
            {
               double mask = m_masks[z]( x, y );
               satAdj += m_zones[z].saturation * mask;
            }

            // Get original RGB
            double r = colorImage( x, y, 0 );
            double g = colorImage( x, y, 1 );
            double b = colorImage( x, y, 2 );

            // Scale by luminance change
            r *= scale;
            g *= scale;
            b *= scale;

            // Apply saturation adjustment
            if ( Abs( satAdj ) > 0.001 )
            {
               double gray = 0.2126 * r + 0.7152 * g + 0.0722 * b;
               double satMult = 1.0 + satAdj; // Range: 0 to 2
               r = gray + ( r - gray ) * satMult;
               g = gray + ( g - gray ) * satMult;
               b = gray + ( b - gray ) * satMult;
            }

            colorImage( x, y, 0 ) = float( Range( r, 0.0, 1.0 ) );
            colorImage( x, y, 1 ) = float( Range( g, 0.0, 1.0 ) );
            colorImage( x, y, 2 ) = float( Range( b, 0.0, 1.0 ) );
         }
      }
   }
   else
   {
      // Grayscale: just copy luminance back
      for ( int c = 0; c < numChannels; ++c )
         for ( int y = 0; y < height; ++y )
            for ( int x = 0; x < width; ++x )
               colorImage( x, y, c ) = luminance( x, y );
   }
}

// ----------------------------------------------------------------------------
// Preview Generation
// ----------------------------------------------------------------------------

void ZoneHDREngine::GeneratePreview( Image& previewImage,
                                      const Image& sourceImage,
                                      PreviewMode mode,
                                      int selectedZone ) const
{
   if ( mode == PreviewOff )
   {
      previewImage.Assign( sourceImage );
      return;
   }

   int width = sourceImage.Width();
   int height = sourceImage.Height();
   int numChannels = sourceImage.NumberOfChannels();
   bool isColor = numChannels >= 3;

   // Always allocate at least 3 channels for color preview overlay
   previewImage.AllocateData( width, height, Max( 3, numChannels ), ColorSpace::RGB );

   if ( mode == PreviewMaskOverlay )
   {
      // Overlay colored masks on the image
      // Zone colors: cycle through distinct colors
      static const double zoneColors[][3] = {
         { 0.2, 0.4, 1.0 },  // Blue - shadows
         { 0.2, 0.8, 0.4 },  // Green - low mids
         { 1.0, 1.0, 0.2 },  // Yellow - mids
         { 1.0, 0.6, 0.2 },  // Orange - high mids
         { 1.0, 0.3, 0.3 },  // Red - highlights
         { 1.0, 0.3, 1.0 },  // Magenta - bright core
         { 0.3, 1.0, 1.0 },  // Cyan
         { 0.8, 0.8, 0.8 }   // Gray
      };

      for ( int y = 0; y < height; ++y )
      {
         for ( int x = 0; x < width; ++x )
         {
            double r = isColor ? sourceImage( x, y, 0 ) : sourceImage( x, y, 0 );
            double g = isColor ? sourceImage( x, y, 1 ) : sourceImage( x, y, 0 );
            double b = isColor ? sourceImage( x, y, 2 ) : sourceImage( x, y, 0 );

            // Blend with zone colors based on masks
            for ( size_type z = 0; z < m_masks.Length() && z < 8; ++z )
            {
               double mask = m_masks[z]( x, y ) * 0.4; // 40% overlay strength
               int colorIdx = int( z ) % 8;
               r = r * ( 1 - mask ) + zoneColors[colorIdx][0] * mask;
               g = g * ( 1 - mask ) + zoneColors[colorIdx][1] * mask;
               b = b * ( 1 - mask ) + zoneColors[colorIdx][2] * mask;
            }

            previewImage( x, y, 0 ) = float( Range( r, 0.0, 1.0 ) );
            previewImage( x, y, 1 ) = float( Range( g, 0.0, 1.0 ) );
            previewImage( x, y, 2 ) = float( Range( b, 0.0, 1.0 ) );
         }
      }
   }
   else if ( mode == PreviewSoloZone )
   {
      // Show only the selected zone's mask effect
      if ( selectedZone < 0 || selectedZone >= int( m_masks.Length() ) )
      {
         previewImage.Assign( sourceImage );
         return;
      }

      for ( int y = 0; y < height; ++y )
      {
         for ( int x = 0; x < width; ++x )
         {
            double mask = m_masks[selectedZone]( x, y );

            double r = isColor ? sourceImage( x, y, 0 ) : sourceImage( x, y, 0 );
            double g = isColor ? sourceImage( x, y, 1 ) : sourceImage( x, y, 0 );
            double b = isColor ? sourceImage( x, y, 2 ) : sourceImage( x, y, 0 );

            // Desaturate and darken areas outside the zone
            double gray = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            double dimGray = gray * 0.3;

            r = dimGray + ( r - dimGray ) * mask;
            g = dimGray + ( g - dimGray ) * mask;
            b = dimGray + ( b - dimGray ) * mask;

            previewImage( x, y, 0 ) = float( Range( r, 0.0, 1.0 ) );
            previewImage( x, y, 1 ) = float( Range( g, 0.0, 1.0 ) );
            previewImage( x, y, 2 ) = float( Range( b, 0.0, 1.0 ) );
         }
      }
   }

   // Copy alpha channel if present
   if ( numChannels > 3 )
   {
      for ( int c = 3; c < numChannels; ++c )
         for ( int y = 0; y < height; ++y )
            for ( int x = 0; x < width; ++x )
               previewImage( x, y, c ) = sourceImage( x, y, c );
   }
}

// ----------------------------------------------------------------------------

} // namespace pcl

// ----------------------------------------------------------------------------
