//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Blend Engine - Regional stretch blending

#ifndef __BlendEngine_h
#define __BlendEngine_h

#include "RegionStatistics.h"
#include "IStretchAlgorithm.h"

#include <pcl/Image.h>
#include <pcl/String.h>

#include <map>
#include <memory>
#include <functional>

namespace pcl
{

// ----------------------------------------------------------------------------
// Blend Configuration
// ----------------------------------------------------------------------------

struct BlendConfig
{
   double featherRadius = 3.0;         // Default mask edge softening radius
   double blendFalloff = 2.0;          // Falloff exponent for transitions
   bool normalizeWeights = true;       // Ensure weights sum to 1
   bool preserveBlackPoint = true;     // Don't stretch pure black
   bool clampOutput = true;            // Clamp output to [0, 1]

   // Per-region strength multipliers
   std::map<RegionClass, double> regionStrength;

   // Per-region feather radius (different astronomical features need different blending)
   // Stars: sharp boundaries (0.5-1.0), Nebulae: soft (4-6), Galaxies: medium (2-3)
   std::map<RegionClass, double> regionFeatherRadius;

   double GetRegionStrength( RegionClass rc ) const
   {
      auto it = regionStrength.find( rc );
      return (it != regionStrength.end()) ? it->second : 1.0;
   }

   // Get the feather radius for a specific region class
   // Returns region-specific value if set, otherwise returns sensible defaults
   // based on astronomical feature type
   double GetFeatherRadiusForRegion( RegionClass rc ) const
   {
      // Check for explicit override first
      auto it = regionFeatherRadius.find( rc );
      if ( it != regionFeatherRadius.end() )
         return it->second;

      // Return defaults based on region type
      switch ( rc )
      {
      // Stars: SHARP boundaries - minimal feathering (0.5-1.0)
      case RegionClass::StarBright:
      case RegionClass::StarMedium:
      case RegionClass::StarFaint:
      case RegionClass::StarSaturated:
         return 0.75;

      // Star clusters: slightly more feathering than individual stars
      case RegionClass::StarClusterOpen:
      case RegionClass::StarClusterGlobular:
         return 1.5;

      // Nebulae: SOFT boundaries - higher feathering (4-6)
      case RegionClass::NebulaEmission:
      case RegionClass::NebulaReflection:
      case RegionClass::NebulaPlanetary:
         return 5.0;

      // Dark nebulae: soft but slightly less than emission
      case RegionClass::NebulaDark:
         return 4.5;

      // Galaxies: MEDIUM feathering (2-3)
      case RegionClass::GalaxySpiral:
      case RegionClass::GalaxyElliptical:
      case RegionClass::GalaxyIrregular:
         return 2.5;

      // Galaxy cores: slightly sharper than outer regions
      case RegionClass::GalaxyCore:
         return 2.0;

      // Dust lanes: medium-soft (blend with galaxy/nebula context)
      case RegionClass::DustLane:
         return 3.0;

      // Artifacts: sharp boundaries - we want to isolate them
      case RegionClass::ArtifactHotPixel:
      case RegionClass::ArtifactSatellite:
      case RegionClass::ArtifactDiffraction:
         return 0.5;

      // Gradient artifacts: very soft blending
      case RegionClass::ArtifactGradient:
         return 6.0;

      // Noise: medium-soft
      case RegionClass::ArtifactNoise:
         return 4.0;

      // Background: use default
      case RegionClass::Background:
      default:
         return featherRadius;  // Use global default (3.0)
      }
   }
};

// ----------------------------------------------------------------------------
// Stretch Result for a Single Region
// ----------------------------------------------------------------------------

struct RegionStretchResult
{
   RegionClass region;
   Image stretchedImage;        // The fully stretched image for this region
   Image mask;                  // Soft mask (0-1)
   double coverage = 0;         // Fraction of image covered
};

// ----------------------------------------------------------------------------
// Blend Engine
//
// Combines multiple stretched images using soft masks.
// Handles overlapping regions with weighted blending.
// ----------------------------------------------------------------------------

class BlendEngine
{
public:

   BlendEngine( const BlendConfig& config = BlendConfig() );

   // Main blending function
   // Takes original image and per-region stretched results
   // Returns the blended output image
   Image Blend( const Image& original,
                const std::vector<RegionStretchResult>& stretchedRegions ) const;

   // Blend with algorithms applied during blend
   // More memory efficient for large images
   Image BlendWithAlgorithms(
      const Image& original,
      const std::map<RegionClass, Image>& masks,
      const std::map<RegionClass, IStretchAlgorithm*>& algorithms ) const;

   // Prepare masks (apply feathering, normalize)
   std::map<RegionClass, Image> PrepareMasks(
      const std::map<RegionClass, Image>& rawMasks ) const;

   // Apply feathering to a single mask (uses default featherRadius)
   Image FeatherMask( const Image& mask ) const;

   // Apply region-specific feathering to a mask
   // Different astronomical features need different feathering:
   // - Stars: sharp (0.5-1.0), Nebulae: soft (4-6), Galaxies: medium (2-3)
   Image FeatherMaskForRegion( const Image& mask, RegionClass region ) const;

   // Compute weight map from masks
   Image ComputeWeightMap( const std::map<RegionClass, Image>& masks,
                           RegionClass forRegion ) const;

   // Configuration
   const BlendConfig& Config() const { return m_config; }
   void SetConfig( const BlendConfig& config ) { m_config = config; }

private:

   BlendConfig m_config;

   // Blend a single pixel
   double BlendPixel( double original,
                      const std::vector<std::pair<double, double>>& stretchedWeights ) const;
};

// ----------------------------------------------------------------------------
// Parallel Blend Processor
//
// Optimized for multi-threaded blending of large images.
// ----------------------------------------------------------------------------

class ParallelBlendProcessor
{
public:

   ParallelBlendProcessor( const BlendConfig& config = BlendConfig() );

   // Process image in parallel
   Image Process( const Image& original,
                  const std::map<RegionClass, Image>& masks,
                  const std::map<RegionClass, IStretchAlgorithm*>& algorithms,
                  int numThreads = 0 ) const;

   // Progress callback: (progress 0-1, message)
   using ProgressCallback = std::function<void( double, const String& )>;

   // Process with progress reporting
   Image Process( const Image& original,
                  const std::map<RegionClass, Image>& masks,
                  const std::map<RegionClass, IStretchAlgorithm*>& algorithms,
                  ProgressCallback progress,
                  int numThreads = 0 ) const;

private:

   BlendConfig m_config;

   // Process a row range
   void ProcessRows( const Image& original,
                     Image& output,
                     const std::map<RegionClass, Image>& masks,
                     const std::map<RegionClass, IStretchAlgorithm*>& algorithms,
                     int startRow, int endRow,
                     int channel ) const;
};

// ----------------------------------------------------------------------------
// Mask Preparation Utilities
// ----------------------------------------------------------------------------

namespace MaskPrep
{
   // Apply Gaussian blur to mask edges
   Image GaussianFeather( const Image& mask, double sigma );

   // Apply power falloff to mask
   Image PowerFalloff( const Image& mask, double exponent );

   // Normalize multiple masks so they sum to 1 at each pixel
   void NormalizeMasks( std::map<RegionClass, Image>& masks );

   // Fill gaps in masks (ensure every pixel has weight)
   void FillMaskGaps( std::map<RegionClass, Image>& masks,
                      RegionClass defaultRegion = RegionClass::Background );

   // Detect and smooth transition zones between regions
   Image DetectTransitionZone( const std::map<RegionClass, Image>& masks );
}

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __BlendEngine_h
