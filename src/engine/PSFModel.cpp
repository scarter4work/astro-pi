//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "PSFModel.h"

#include <pcl/Math.h>

#include <algorithm>
#include <numeric>

// Use PCL math constants
static const float PCL_PI = float(pcl::Pi());
static const float PCL_TWO_PI = float(pcl::TwoPi());

namespace pcl
{

// ----------------------------------------------------------------------------
// PSFParameters Implementation
// ----------------------------------------------------------------------------

float PSFParameters::GetAlpha() const
{
   return PSFUtils::MoffatAlpha( fwhm, beta );
}

float PSFParameters::GetSigma() const
{
   return PSFUtils::FWHMToSigma( fwhm );
}

float PSFParameters::Evaluate( float r ) const
{
   if ( r < 0 )
      r = -r;

   float value = 0.0f;

   switch ( profile )
   {
   case PSFProfile::Gaussian:
      {
         float s = GetSigma();
         value = intensity * std::exp( -r * r / (2.0f * s * s) );
      }
      break;

   case PSFProfile::Moffat:
      {
         float a = GetAlpha();
         value = intensity * std::pow( 1.0f + (r * r) / (a * a), -beta );
      }
      break;

   case PSFProfile::Lorentzian:
      {
         float gamma = fwhm / 2.0f;
         value = intensity / (1.0f + (r * r) / (gamma * gamma));
      }
      break;

   case PSFProfile::Airy:
      {
         float x = r * 3.14159265f / fwhm;
         if ( x < 0.001f )
            value = intensity;
         else
         {
            // J1 Bessel function approximation (polynomial fit)
            float j1;
            float ax = std::abs( x );
            if ( ax < 3.0f )
            {
               // Small argument: series approximation
               float t = x / 3.0f;
               float t2 = t * t;
               j1 = x * (0.5f + t2 * (-0.56249985f + t2 * (0.21093573f + t2 * (-0.03954289f + t2 * 0.00443319f))));
            }
            else
            {
               // Large argument: asymptotic approximation
               float t = 3.0f / ax;
               float t2 = t * t;
               float p1 = 1.0f + t2 * (-0.00000156f + t2 * (0.00001659f + t2 * (-0.00017105f + t2 * 0.00249511f)));
               float q1 = 0.04166397f + t2 * (-0.00003954f + t2 * (0.00262573f + t2 * (-0.00054125f + t2 * (-0.00029333f))));
               float theta = ax - 2.356194491f; // ax - 3*pi/4
               j1 = std::sqrt( 0.636619772f / ax ) * (p1 * std::cos( theta ) - t * q1 * std::sin( theta ));
               if ( x < 0 ) j1 = -j1;
            }
            float airy = (std::abs( x ) > 0.001f) ? 2.0f * j1 / x : 1.0f;
            value = intensity * airy * airy;
         }
      }
      break;

   case PSFProfile::Hybrid:
      {
         // Gaussian core + Moffat wings
         float s = GetSigma();
         float a = GetAlpha() * 2.0f;  // Wider wings
         float gaussianPart = std::exp( -r * r / (2.0f * s * s) );
         float moffatPart = std::pow( 1.0f + (r * r) / (a * a), -beta );

         // Transition at ~2 sigma
         float transition = 2.0f * s;
         float weight = 1.0f / (1.0f + std::exp( (r - transition) / s ));
         value = intensity * (weight * gaussianPart + (1.0f - weight) * moffatPart * 0.5f);
      }
      break;
   }

   // Add extended halo
   if ( haloIntensity > 0 && haloRadius > 0 )
   {
      float halo = haloIntensity * intensity *
                   std::exp( -r / haloRadius );
      value += halo;
   }

   return value;
}

float PSFParameters::Evaluate( float x, float y ) const
{
   // Offset from center
   float dx = x - centerX;
   float dy = y - centerY;

   // Apply ellipticity
   if ( ellipticity > 0.001f )
   {
      float cosPA = std::cos( positionAngle );
      float sinPA = std::sin( positionAngle );

      // Rotate to ellipse axes
      float u = dx * cosPA + dy * sinPA;
      float v = -dx * sinPA + dy * cosPA;

      // Apply ellipticity (stretch minor axis)
      float axisRatio = 1.0f - ellipticity;
      v /= axisRatio;

      // Rotate back
      dx = u * cosPA - v * sinPA;
      dy = u * sinPA + v * cosPA;
   }

   float r = std::sqrt( dx * dx + dy * dy );
   return Evaluate( r );
}

float PSFParameters::EvaluateWithSpikes( float x, float y ) const
{
   float value = Evaluate( x, y );

   if ( numSpikes > 0 && spikeIntensity > 0 )
   {
      float dx = x - centerX;
      float dy = y - centerY;
      float r = std::sqrt( dx * dx + dy * dy );

      if ( r > fwhm * 0.5f )  // Spikes start outside core
      {
         float angle = std::atan2( dy, dx ) - spikeAngle;
         float spikeAngleStep = PCL_TWO_PI / numSpikes;

         // Find distance to nearest spike
         float minDist = spikeAngleStep / 2.0f;
         for ( int i = 0; i < numSpikes; ++i )
         {
            float spikeAng = i * spikeAngleStep;
            float diff = std::abs( angle - spikeAng );
            diff = Min( diff, PCL_TWO_PI - diff );
            minDist = Min( minDist, diff );
         }

         // Spike contribution (Gaussian cross-section)
         float angularWidth = spikeWidth / r;
         float spikeContrib = spikeIntensity * intensity *
                              std::exp( -minDist * minDist / (2.0f * angularWidth * angularWidth) ) *
                              std::exp( -r / (haloRadius * 2.0f) );  // Fade with distance
         value += spikeContrib;
      }
   }

   return value;
}

float PSFParameters::GetEffectiveRadius( float threshold ) const
{
   // Binary search for radius where PSF drops to threshold
   float lo = 0.0f;
   float hi = Max( fwhm * 20.0f, haloRadius * 5.0f );

   while ( hi - lo > 0.5f )
   {
      float mid = (lo + hi) / 2.0f;
      if ( Evaluate( mid ) > threshold * intensity )
         lo = mid;
      else
         hi = mid;
   }

   return hi;
}

// ----------------------------------------------------------------------------
// PSFModel Implementation
// ----------------------------------------------------------------------------

PSFModel::PSFModel( const PSFParameters& params )
   : m_params( params )
{
}

Image PSFModel::Generate( int width, int height ) const
{
   Image psf;
   psf.AllocateData( width, height, 3, ColorSpace::RGB );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         float value = m_params.EvaluateWithSpikes( float(x), float(y) );
         psf( x, y, 0 ) = value * m_params.colorR;
         psf( x, y, 1 ) = value * m_params.colorG;
         psf( x, y, 2 ) = value * m_params.colorB;
      }
   }

   return psf;
}

Image PSFModel::GenerateCentered( int size ) const
{
   PSFParameters centeredParams = m_params;
   centeredParams.centerX = size / 2.0f;
   centeredParams.centerY = size / 2.0f;

   PSFModel centeredModel( centeredParams );
   return centeredModel.Generate( size, size );
}

void PSFModel::AddToImage( Image& target, float x, float y, float scale ) const
{
   PSFParameters localParams = m_params;
   localParams.centerX = x;
   localParams.centerY = y;
   localParams.intensity *= scale;

   float radius = localParams.GetEffectiveRadius( 0.001f );
   int x0 = Max( 0, int(x - radius) );
   int y0 = Max( 0, int(y - radius) );
   int x1 = Min( target.Width(), int(x + radius + 1) );
   int y1 = Min( target.Height(), int(y + radius + 1) );

   for ( int py = y0; py < y1; ++py )
   {
      for ( int px = x0; px < x1; ++px )
      {
         float value = localParams.EvaluateWithSpikes( float(px), float(py) );
         target( px, py, 0 ) = float(Min( 1.0, double(target( px, py, 0 )) + value * localParams.colorR ));
         target( px, py, 1 ) = float(Min( 1.0, double(target( px, py, 1 )) + value * localParams.colorG ));
         target( px, py, 2 ) = float(Min( 1.0, double(target( px, py, 2 )) + value * localParams.colorB ));
      }
   }
}

void PSFModel::SubtractFromImage( Image& target, float x, float y, float scale ) const
{
   PSFParameters localParams = m_params;
   localParams.centerX = x;
   localParams.centerY = y;
   localParams.intensity *= scale;

   float radius = localParams.GetEffectiveRadius( 0.001f );
   int x0 = Max( 0, int(x - radius) );
   int y0 = Max( 0, int(y - radius) );
   int x1 = Min( target.Width(), int(x + radius + 1) );
   int y1 = Min( target.Height(), int(y + radius + 1) );

   for ( int py = y0; py < y1; ++py )
   {
      for ( int px = x0; px < x1; ++px )
      {
         float value = localParams.EvaluateWithSpikes( float(px), float(py) );
         target( px, py, 0 ) = float(Max( 0.0, double(target( px, py, 0 )) - value * localParams.colorR ));
         target( px, py, 1 ) = float(Max( 0.0, double(target( px, py, 1 )) - value * localParams.colorG ));
         target( px, py, 2 ) = float(Max( 0.0, double(target( px, py, 2 )) - value * localParams.colorB ));
      }
   }
}

PSFParameters PSFModel::FitToStar( const Image& image,
                                    float centerX, float centerY,
                                    float searchRadius )
{
   // Refine center position
   DPoint centroid = PSFUtils::FindCentroid( image, centerX, centerY, searchRadius );

   return FitToStarAtCenter( image, float(centroid.x), float(centroid.y), searchRadius * 3 );
}

PSFParameters PSFModel::FitToStarAtCenter( const Image& image,
                                            float centerX, float centerY,
                                            float maxRadius )
{
   PSFParameters result;
   result.centerX = centerX;
   result.centerY = centerY;

   // Get peak intensity
   int cx = int(centerX + 0.5f);
   int cy = int(centerY + 0.5f);
   if ( cx >= 0 && cx < image.Width() && cy >= 0 && cy < image.Height() )
   {
      result.intensity = float(image( cx, cy, 0 ));  // Use red channel
   }

   // Estimate FWHM
   result.fwhm = PSFUtils::EstimateFWHM( image, centerX, centerY );

   // Estimate Moffat beta by fitting
   // Simple grid search over beta values
   float bestBeta = 2.5f;
   float bestResidual = 1e10f;

   for ( float beta = 1.5f; beta <= 5.0f; beta += 0.25f )
   {
      PSFParameters testParams = result;
      testParams.beta = beta;
      float residual = ComputeResidual( image, centerX, centerY, testParams, maxRadius );

      if ( residual < bestResidual )
      {
         bestResidual = residual;
         bestBeta = beta;
      }
   }

   result.beta = bestBeta;

   // Estimate halo parameters
   float outerRadius = result.fwhm * 5;
   if ( outerRadius < maxRadius )
   {
      // Sample at outer radius
      float outerIntensity = 0;
      int samples = 0;
      for ( float angle = 0; angle < PCL_TWO_PI; angle += 0.2f )
      {
         int sx = int(centerX + outerRadius * std::cos(angle));
         int sy = int(centerY + outerRadius * std::sin(angle));
         if ( sx >= 0 && sx < image.Width() && sy >= 0 && sy < image.Height() )
         {
            outerIntensity += float(image( sx, sy, 0 ));
            samples++;
         }
      }

      if ( samples > 0 )
      {
         outerIntensity /= samples;
         float expectedPSF = result.Evaluate( outerRadius );

         if ( outerIntensity > expectedPSF * 1.5f )
         {
            // Extended halo present
            result.haloIntensity = (outerIntensity - expectedPSF) / result.intensity;
            result.haloRadius = outerRadius;
         }
      }
   }

   // Estimate color (chromatic aberration)
   if ( image.NumberOfChannels() >= 3 )
   {
      float rSum = 0, gSum = 0, bSum = 0;
      int count = 0;
      float sampleRadius = result.fwhm * 2;

      for ( float r = result.fwhm; r < sampleRadius; r += 1.0f )
      {
         for ( float angle = 0; angle < PCL_TWO_PI; angle += 0.3f )
         {
            int sx = int(centerX + r * std::cos(angle));
            int sy = int(centerY + r * std::sin(angle));
            if ( sx >= 0 && sx < image.Width() && sy >= 0 && sy < image.Height() )
            {
               rSum += float(image( sx, sy, 0 ));
               gSum += float(image( sx, sy, 1 ));
               bSum += float(image( sx, sy, 2 ));
               count++;
            }
         }
      }

      if ( count > 0 )
      {
         float total = rSum + gSum + bSum;
         if ( total > 0 )
         {
            result.colorR = (rSum / total) * 3.0f;
            result.colorG = (gSum / total) * 3.0f;
            result.colorB = (bSum / total) * 3.0f;
         }
      }
   }

   return result;
}

float PSFModel::ComputeResidual( const Image& image,
                                  float cx, float cy,
                                  const PSFParameters& params,
                                  float maxRadius )
{
   float sumSqDiff = 0;
   int count = 0;

   for ( float r = 1; r < maxRadius; r += 1.0f )
   {
      for ( float angle = 0; angle < PCL_TWO_PI; angle += 0.3f )
      {
         int px = int(cx + r * std::cos(angle));
         int py = int(cy + r * std::sin(angle));

         if ( px >= 0 && px < image.Width() && py >= 0 && py < image.Height() )
         {
            float observed = float(image( px, py, 0 ));
            float predicted = params.Evaluate( r );
            float diff = observed - predicted;
            sumSqDiff += diff * diff;
            count++;
         }
      }
   }

   return count > 0 ? sumSqDiff / count : 1e10f;
}

// ----------------------------------------------------------------------------
// PSFSubtractor Implementation
// ----------------------------------------------------------------------------

PSFSubtractor::PSFSubtractor( const Config& config )
   : m_config( config )
{
}

Image PSFSubtractor::SubtractStar( const Image& input,
                                    const PSFParameters& psf ) const
{
   Image result = input;

   float effectiveRadius = psf.GetEffectiveRadius( 0.001f );
   float protectionZone = psf.fwhm * m_config.protectionRadius;

   int x0 = Max( 0, int(psf.centerX - effectiveRadius) );
   int y0 = Max( 0, int(psf.centerY - effectiveRadius) );
   int x1 = Min( input.Width(), int(psf.centerX + effectiveRadius + 1) );
   int y1 = Min( input.Height(), int(psf.centerY + effectiveRadius + 1) );

   for ( int y = y0; y < y1; ++y )
   {
      for ( int x = x0; x < x1; ++x )
      {
         float dx = x - psf.centerX;
         float dy = y - psf.centerY;
         float r = std::sqrt( dx * dx + dy * dy );

         // Skip protection zone
         if ( m_config.preserveCore && r < protectionZone )
            continue;

         // Compute blend factor
         float blendFactor = 1.0f;
         if ( r < protectionZone + m_config.blendRadius )
         {
            blendFactor = (r - protectionZone) / m_config.blendRadius;
            blendFactor = blendFactor * blendFactor * (3 - 2 * blendFactor);  // Smoothstep
         }

         // Compute PSF value to subtract
         float psfValue = m_config.subtractSpikes ?
                          psf.EvaluateWithSpikes( float(x), float(y) ) :
                          psf.Evaluate( float(x), float(y) );

         float subtractAmount = psfValue * blendFactor * m_config.maxSubtraction;

         // Subtract from each channel
         for ( int c = 0; c < Min( 3, input.NumberOfChannels() ); ++c )
         {
            float current = float(result( x, y, c ));
            float colorMult = (c == 0) ? psf.colorR : (c == 1) ? psf.colorG : psf.colorB;
            float newValue = current - subtractAmount * colorMult;

            // Don't go below background safety level
            result( x, y, c ) = Max( double(m_config.backgroundSafety), double(newValue) );
         }
      }
   }

   return result;
}

Image PSFSubtractor::SubtractStars( const Image& input,
                                     const std::vector<PSFParameters>& stars ) const
{
   Image result = input;

   for ( const auto& star : stars )
   {
      result = SubtractStar( result, star );
   }

   return result;
}

PSFSubtractor::SubtractionResult PSFSubtractor::SubtractAndRecover(
   const Image& input,
   const PSFParameters& psf,
   const Image& contextMask ) const
{
   SubtractionResult result;

   // First do basic subtraction
   result.subtracted = SubtractStar( input, psf );

   // Compute residual (what was removed)
   result.residual.AllocateData( input.Width(), input.Height(),
                                  input.NumberOfChannels(), input.ColorSpace() );

   for ( int c = 0; c < input.NumberOfChannels(); ++c )
   {
      for ( int y = 0; y < input.Height(); ++y )
      {
         for ( int x = 0; x < input.Width(); ++x )
         {
            result.residual( x, y, c ) = input( x, y, c ) - result.subtracted( x, y, c );
         }
      }
   }

   // Estimate background/underlying structure from context
   result.recovered = EstimateBackground( input, psf, contextMask );

   return result;
}

Image PSFSubtractor::EstimateBackground( const Image& input,
                                          const PSFParameters& psf,
                                          const Image& contextMask ) const
{
   Image recovered = input;

   float effectiveRadius = psf.GetEffectiveRadius( 0.01f );
   int cx = int(psf.centerX);
   int cy = int(psf.centerY);

   // Sample background from annulus around star
   float innerRadius = effectiveRadius * 0.8f;
   float outerRadius = effectiveRadius * 1.5f;

   std::vector<float> bgSamplesR, bgSamplesG, bgSamplesB;

   for ( float angle = 0; angle < PCL_TWO_PI; angle += 0.1f )
   {
      for ( float r = innerRadius; r < outerRadius; r += 2.0f )
      {
         int sx = int(psf.centerX + r * std::cos(angle));
         int sy = int(psf.centerY + r * std::sin(angle));

         if ( sx >= 0 && sx < input.Width() && sy >= 0 && sy < input.Height() )
         {
            // Check if this pixel is valid context (not another star)
            bool validContext = true;
            if ( contextMask.Width() > 0 )
            {
               validContext = contextMask( sx, sy, 0 ) > 0.5;
            }

            if ( validContext )
            {
               bgSamplesR.push_back( float(input( sx, sy, 0 )) );
               if ( input.NumberOfChannels() >= 3 )
               {
                  bgSamplesG.push_back( float(input( sx, sy, 1 )) );
                  bgSamplesB.push_back( float(input( sx, sy, 2 )) );
               }
            }
         }
      }
   }

   // Compute median background
   auto median = []( std::vector<float>& v ) -> float {
      if ( v.empty() ) return 0;
      std::sort( v.begin(), v.end() );
      return v[v.size() / 2];
   };

   float bgR = median( bgSamplesR );
   float bgG = bgSamplesG.empty() ? bgR : median( bgSamplesG );
   float bgB = bgSamplesB.empty() ? bgR : median( bgSamplesB );

   // Fill in recovered region with estimated background
   int x0 = Max( 0, int(psf.centerX - effectiveRadius) );
   int y0 = Max( 0, int(psf.centerY - effectiveRadius) );
   int x1 = Min( input.Width(), int(psf.centerX + effectiveRadius + 1) );
   int y1 = Min( input.Height(), int(psf.centerY + effectiveRadius + 1) );

   for ( int y = y0; y < y1; ++y )
   {
      for ( int x = x0; x < x1; ++x )
      {
         float dx = x - psf.centerX;
         float dy = y - psf.centerY;
         float r = std::sqrt( dx * dx + dy * dy );

         if ( r < effectiveRadius )
         {
            // Blend between observed and estimated background
            float psfOpacity = psf.Evaluate( r ) / psf.intensity;
            psfOpacity = Min( 1.0f, psfOpacity * 1.5f );  // Boost for better recovery

            // Estimated underlying = (observed - psf) / (1 - opacity)
            // But clamp to not go negative
            float recoveryFactor = 1.0f / Max( 0.1f, 1.0f - psfOpacity );

            recovered( x, y, 0 ) = bgR;  // Use background estimate
            if ( input.NumberOfChannels() >= 3 )
            {
               recovered( x, y, 1 ) = bgG;
               recovered( x, y, 2 ) = bgB;
            }
         }
      }
   }

   return recovered;
}

// ----------------------------------------------------------------------------
// PSFUtils Implementation
// ----------------------------------------------------------------------------

namespace PSFUtils
{

float EstimateFWHM( const Image& star, float centerX, float centerY )
{
   int cx = int(centerX + 0.5f);
   int cy = int(centerY + 0.5f);

   if ( cx < 0 || cx >= star.Width() || cy < 0 || cy >= star.Height() )
      return 5.0f;  // Default

   float peak = float(star( cx, cy, 0 ));
   float halfMax = peak / 2.0f;

   // Search outward for half-max
   float fwhmSum = 0;
   int fwhmCount = 0;

   for ( float angle = 0; angle < PCL_TWO_PI; angle += 0.2f )
   {
      for ( float r = 1; r < 50; r += 0.5f )
      {
         int sx = int(centerX + r * std::cos(angle));
         int sy = int(centerY + r * std::sin(angle));

         if ( sx >= 0 && sx < star.Width() && sy >= 0 && sy < star.Height() )
         {
            if ( float(star( sx, sy, 0 )) < halfMax )
            {
               fwhmSum += r * 2;  // Diameter
               fwhmCount++;
               break;
            }
         }
      }
   }

   return fwhmCount > 0 ? fwhmSum / fwhmCount : 5.0f;
}

DPoint FindCentroid( const Image& image, float x, float y, float radius )
{
   double sumX = 0, sumY = 0, sumW = 0;

   int x0 = Max( 0, int(x - radius) );
   int y0 = Max( 0, int(y - radius) );
   int x1 = Min( image.Width(), int(x + radius + 1) );
   int y1 = Min( image.Height(), int(y + radius + 1) );

   for ( int py = y0; py < y1; ++py )
   {
      for ( int px = x0; px < x1; ++px )
      {
         float dx = px - x;
         float dy = py - y;
         if ( dx * dx + dy * dy <= radius * radius )
         {
            double w = image( px, py, 0 );
            sumX += px * w;
            sumY += py * w;
            sumW += w;
         }
      }
   }

   if ( sumW > 0 )
      return DPoint( sumX / sumW, sumY / sumW );
   else
      return DPoint( x, y );
}

std::vector<DPoint> DetectStars( const Image& image,
                                  float threshold,
                                  float minSeparation )
{
   std::vector<DPoint> stars;

   // Compute statistics
   double mean = 0, stdDev = 0;
   size_t count = 0;

   for ( int y = 0; y < image.Height(); ++y )
   {
      for ( int x = 0; x < image.Width(); ++x )
      {
         mean += image( x, y, 0 );
         count++;
      }
   }
   mean /= count;

   for ( int y = 0; y < image.Height(); ++y )
   {
      for ( int x = 0; x < image.Width(); ++x )
      {
         double diff = image( x, y, 0 ) - mean;
         stdDev += diff * diff;
      }
   }
   stdDev = std::sqrt( stdDev / count );

   float detectThreshold = float(mean + threshold * stdDev);

   // Find local maxima above threshold
   for ( int y = 2; y < image.Height() - 2; ++y )
   {
      for ( int x = 2; x < image.Width() - 2; ++x )
      {
         float val = float(image( x, y, 0 ));

         if ( val > detectThreshold )
         {
            // Check if local maximum
            bool isMax = true;
            for ( int dy = -2; dy <= 2 && isMax; ++dy )
            {
               for ( int dx = -2; dx <= 2 && isMax; ++dx )
               {
                  if ( dx == 0 && dy == 0 ) continue;
                  if ( image( x + dx, y + dy, 0 ) > val )
                     isMax = false;
               }
            }

            if ( isMax )
            {
               // Check separation from existing stars
               bool tooClose = false;
               for ( const auto& existing : stars )
               {
                  float dist = std::sqrt( float((x - existing.x) * (x - existing.x) +
                                                 (y - existing.y) * (y - existing.y)) );
                  if ( dist < minSeparation )
                  {
                     tooClose = true;
                     break;
                  }
               }

               if ( !tooClose )
               {
                  // Refine centroid
                  DPoint centroid = FindCentroid( image, float(x), float(y), 5 );
                  stars.push_back( centroid );
               }
            }
         }
      }
   }

   return stars;
}

} // namespace PSFUtils

// ----------------------------------------------------------------------------

} // namespace pcl
