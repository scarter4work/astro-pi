//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// PSF Model - Point Spread Function modeling for star halos

#ifndef __PSFModel_h
#define __PSFModel_h

#include <pcl/Image.h>
#include <pcl/Point.h>

#include <cmath>
#include <vector>

namespace pcl
{

// ----------------------------------------------------------------------------
// PSF Profile Types
// ----------------------------------------------------------------------------

enum class PSFProfile
{
   Gaussian,    // Simple Gaussian: exp(-r^2 / (2*sigma^2))
   Moffat,      // Moffat profile: (1 + (r/alpha)^2)^(-beta)
   Lorentzian,  // Lorentzian: 1 / (1 + (r/gamma)^2)
   Airy,        // Airy disk (diffraction-limited)
   Hybrid       // Gaussian core + Moffat wings
};

// ----------------------------------------------------------------------------
// PSF Parameters
// ----------------------------------------------------------------------------

struct PSFParameters
{
   // Core parameters
   float intensity = 1.0f;      // Peak brightness (0-1)
   float centerX = 0.0f;        // Center X coordinate
   float centerY = 0.0f;        // Center Y coordinate

   // Shape parameters
   PSFProfile profile = PSFProfile::Moffat;
   float fwhm = 5.0f;           // Full width at half maximum (pixels)
   float beta = 2.5f;           // Moffat beta (shape parameter)
   float sigma = 2.0f;          // Gaussian sigma (derived from fwhm)

   // Ellipticity (for elongated stars/tracking errors)
   float ellipticity = 0.0f;    // 0 = circular, 1 = fully elongated
   float positionAngle = 0.0f;  // Rotation angle in radians

   // Extended halo parameters
   float haloIntensity = 0.1f;  // Relative intensity of extended halo
   float haloRadius = 50.0f;    // Characteristic radius of halo

   // Color parameters (for chromatic halos)
   float colorR = 1.0f;         // Red channel multiplier
   float colorG = 1.0f;         // Green channel multiplier
   float colorB = 1.0f;         // Blue channel multiplier

   // Diffraction spikes (optional)
   int numSpikes = 0;           // Number of diffraction spikes (0, 4, 6, 8)
   float spikeIntensity = 0.0f; // Spike brightness relative to core
   float spikeWidth = 2.0f;     // Spike width in pixels
   float spikeAngle = 0.0f;     // Rotation of spike pattern

   // Derived parameters
   float GetAlpha() const;      // Moffat alpha from fwhm and beta
   float GetSigma() const;      // Gaussian sigma from fwhm

   // Evaluate PSF at distance from center
   float Evaluate( float r ) const;
   float Evaluate( float x, float y ) const;

   // Evaluate with diffraction spikes
   float EvaluateWithSpikes( float x, float y ) const;

   // Get effective radius (where PSF drops to threshold)
   float GetEffectiveRadius( float threshold = 0.01f ) const;
};

// ----------------------------------------------------------------------------
// Spatially-Varying PSF Grid
//
// Divides the image into a grid of zones with independently fitted PSF
// parameters, then interpolates between grid points to model field-varying
// aberrations (coma, field curvature, tilt, etc.).
// ----------------------------------------------------------------------------

struct PSFGrid
{
   int gridCols = 3;  // Number of columns in the grid
   int gridRows = 3;  // Number of rows in the grid
   std::vector<PSFParameters> gridParams;  // gridCols * gridRows PSF fits
   int imageWidth = 0;
   int imageHeight = 0;

   // Get interpolated PSF parameters at any image position
   [[nodiscard]] PSFParameters GetParametersAt( int x, int y ) const;
};

// ----------------------------------------------------------------------------
// PSF Model - Generate and manipulate PSF images
// ----------------------------------------------------------------------------

class PSFModel
{
public:

   PSFModel() = default;
   explicit PSFModel( const PSFParameters& params );

   // Parameters
   const PSFParameters& GetParameters() const { return m_params; }
   void SetParameters( const PSFParameters& params ) { m_params = params; }

   // Generate PSF image
   Image Generate( int width, int height ) const;
   Image GenerateCentered( int size ) const;  // Centered in size x size image

   // Generate at specific location
   void AddToImage( Image& target, float x, float y, float scale = 1.0f ) const;

   // Subtract PSF from image
   void SubtractFromImage( Image& target, float x, float y, float scale = 1.0f ) const;

   // Fit PSF to star in image
   [[nodiscard]] static PSFParameters FitToStar( const Image& image,
                                                   float centerX, float centerY,
                                                   float searchRadius = 20.0f );

   // Fit PSF with known center (more accurate)
   [[nodiscard]] static PSFParameters FitToStarAtCenter( const Image& image,
                                                          float centerX, float centerY,
                                                          float maxRadius = 100.0f );

private:

   PSFParameters m_params;

   // Fitting helpers
   static float ComputeResidual( const Image& image,
                                  float cx, float cy,
                                  const PSFParameters& params,
                                  float maxRadius );
};

// ----------------------------------------------------------------------------
// PSF Subtraction Engine - Remove star halos from images
// ----------------------------------------------------------------------------

class PSFSubtractor
{
public:

   struct Config
   {
      float protectionRadius = 3.0f;    // Don't subtract within N*fwhm of center
      float blendRadius = 5.0f;         // Blend zone width in pixels
      float maxSubtraction = 0.95f;     // Maximum fraction to subtract
      bool preserveCore = true;         // Keep star core intact
      bool subtractSpikes = true;       // Also subtract diffraction spikes
      float backgroundSafety = 0.02f;   // Don't subtract below this level
   };

   PSFSubtractor() = default;
   explicit PSFSubtractor( const Config& config );

   // Subtract single star PSF
   Image SubtractStar( const Image& input,
                       const PSFParameters& psf ) const;

   // Subtract multiple stars
   Image SubtractStars( const Image& input,
                        const std::vector<PSFParameters>& stars ) const;

   // Subtract with recovery of underlying structure
   struct SubtractionResult
   {
      Image subtracted;      // Image with PSF removed
      Image recovered;       // Estimated underlying structure
      Image residual;        // What was removed
      float confidenceMap;   // Confidence in recovery
   };

   SubtractionResult SubtractAndRecover( const Image& input,
                                          const PSFParameters& psf,
                                          const Image& contextMask ) const;

   // Configuration
   const Config& GetConfig() const { return m_config; }
   void SetConfig( const Config& config ) { m_config = config; }

private:

   Config m_config;

   // Internal methods
   Image CreateBlendMask( const PSFParameters& psf, int width, int height ) const;
   Image EstimateBackground( const Image& input,
                              const PSFParameters& psf,
                              const Image& contextMask ) const;
};

// ----------------------------------------------------------------------------
// Utility functions
// ----------------------------------------------------------------------------

namespace PSFUtils
{

// Convert between FWHM and sigma
inline float FWHMToSigma( float fwhm )
{
   return fwhm / (2.0f * std::sqrt( 2.0f * std::log( 2.0f ) ));
}

inline float SigmaToFWHM( float sigma )
{
   return sigma * 2.0f * std::sqrt( 2.0f * std::log( 2.0f ) );
}

// Moffat alpha from FWHM and beta
inline float MoffatAlpha( float fwhm, float beta )
{
   return fwhm / (2.0f * std::sqrt( std::pow( 2.0f, 1.0f/beta ) - 1.0f ));
}

// Estimate FWHM from image
float EstimateFWHM( const Image& star, float centerX, float centerY );

// Find star centroid
DPoint FindCentroid( const Image& image, float x, float y, float radius );

// Detect stars in image
std::vector<DPoint> DetectStars( const Image& image,
                                  float threshold = 5.0f,  // sigma above background
                                  float minSeparation = 10.0f );

// Fit a spatially-varying PSF grid from detected stars.
// Assigns each star to its nearest grid cell, fits PSF parameters per cell,
// and fills empty cells with interpolated values from neighbors.
[[nodiscard]] PSFGrid FitSpatiallyVaryingPSF( const Image& image,
                                               const std::vector<DPoint>& stars,
                                               int gridCols = 3, int gridRows = 3 );

} // namespace PSFUtils

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __PSFModel_h
