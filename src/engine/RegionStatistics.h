//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Region Statistics Data Structures

#ifndef __RegionStatistics_h
#define __RegionStatistics_h

#include <pcl/String.h>
#include <vector>
#include <cmath>

namespace pcl
{

// ----------------------------------------------------------------------------
// Region Class Enumeration
// 23 total classes (21 original + 2 extended: StarHalo, GalacticCirrus)
// ----------------------------------------------------------------------------

enum class RegionClass
{
   // Background (0)
   Background = 0,         // Sky background

   // Star classes (1-4)
   StarBright,             // Bright stellar cores (top 10% intensity)
   StarMedium,             // Medium brightness stars (10-50%)
   StarFaint,              // Faint stars (bottom 50%)
   StarSaturated,          // Saturated star cores (>0.95 intensity)

   // Nebula classes (5-8)
   NebulaEmission,         // Emission nebulae (H-alpha, OIII)
   NebulaReflection,       // Blue reflection nebulae
   NebulaDark,             // Dark nebulae, absorption
   NebulaPlanetary,        // Planetary nebula shells

   // Galaxy classes (9-12)
   GalaxySpiral,           // Spiral galaxies
   GalaxyElliptical,       // Elliptical galaxies
   GalaxyIrregular,        // Irregular galaxies
   GalaxyCore,             // Bright galactic nuclei/AGN

   // Structural classes (13-15)
   DustLane,               // Dark dust lanes
   StarClusterOpen,        // Open star clusters
   StarClusterGlobular,    // Globular clusters

   // Artifact classes (16-20)
   ArtifactHotPixel,       // Hot pixels, cosmic rays
   ArtifactSatellite,      // Satellite/airplane trails
   ArtifactDiffraction,    // Diffraction spikes
   ArtifactGradient,       // Background gradients/vignetting
   ArtifactNoise,          // Noise patterns

   // Extended classes (21-22) - added for star-nebula transitions and IFN
   StarHalo,               // Star halos - diffuse glow around stars
   GalacticCirrus,         // Integrated Flux Nebulae / Galactic cirrus

   Count                   // Number of region classes (23)
};

// Convert region class to string (matches model training names)
inline IsoString RegionClassToString( RegionClass rc )
{
   switch ( rc )
   {
   case RegionClass::Background:          return "background";
   case RegionClass::StarBright:          return "star_bright";
   case RegionClass::StarMedium:          return "star_medium";
   case RegionClass::StarFaint:           return "star_faint";
   case RegionClass::StarSaturated:       return "star_saturated";
   case RegionClass::NebulaEmission:      return "nebula_emission";
   case RegionClass::NebulaReflection:    return "nebula_reflection";
   case RegionClass::NebulaDark:          return "nebula_dark";
   case RegionClass::NebulaPlanetary:     return "nebula_planetary";
   case RegionClass::GalaxySpiral:        return "galaxy_spiral";
   case RegionClass::GalaxyElliptical:    return "galaxy_elliptical";
   case RegionClass::GalaxyIrregular:     return "galaxy_irregular";
   case RegionClass::GalaxyCore:          return "galaxy_core";
   case RegionClass::DustLane:            return "dust_lane";
   case RegionClass::StarClusterOpen:     return "star_cluster_open";
   case RegionClass::StarClusterGlobular: return "star_cluster_globular";
   case RegionClass::ArtifactHotPixel:    return "artifact_hot_pixel";
   case RegionClass::ArtifactSatellite:   return "artifact_satellite";
   case RegionClass::ArtifactDiffraction: return "artifact_diffraction";
   case RegionClass::ArtifactGradient:    return "artifact_gradient";
   case RegionClass::ArtifactNoise:       return "artifact_noise";
   case RegionClass::StarHalo:            return "star_halo";
   case RegionClass::GalacticCirrus:      return "galactic_cirrus";
   default:                               return "unknown";
   }
}

// Convert region class to display name
inline String RegionClassDisplayName( RegionClass rc )
{
   switch ( rc )
   {
   case RegionClass::Background:          return "Background";
   case RegionClass::StarBright:          return "Bright Stars";
   case RegionClass::StarMedium:          return "Medium Stars";
   case RegionClass::StarFaint:           return "Faint Stars";
   case RegionClass::StarSaturated:       return "Saturated Stars";
   case RegionClass::NebulaEmission:      return "Emission Nebula";
   case RegionClass::NebulaReflection:    return "Reflection Nebula";
   case RegionClass::NebulaDark:          return "Dark Nebula";
   case RegionClass::NebulaPlanetary:     return "Planetary Nebula";
   case RegionClass::GalaxySpiral:        return "Spiral Galaxy";
   case RegionClass::GalaxyElliptical:    return "Elliptical Galaxy";
   case RegionClass::GalaxyIrregular:     return "Irregular Galaxy";
   case RegionClass::GalaxyCore:          return "Galaxy Core";
   case RegionClass::DustLane:            return "Dust Lane";
   case RegionClass::StarClusterOpen:     return "Open Cluster";
   case RegionClass::StarClusterGlobular: return "Globular Cluster";
   case RegionClass::ArtifactHotPixel:    return "Hot Pixel";
   case RegionClass::ArtifactSatellite:   return "Satellite Trail";
   case RegionClass::ArtifactDiffraction: return "Diffraction Spike";
   case RegionClass::ArtifactGradient:    return "Gradient";
   case RegionClass::ArtifactNoise:       return "Noise";
   case RegionClass::StarHalo:            return "Star Halo";
   case RegionClass::GalacticCirrus:      return "Galactic Cirrus / IFN";
   default:                               return "Unknown";
   }
}

// Classification helper: is this a star-related class?
inline bool IsStarRelatedClass( RegionClass rc )
{
   return rc == RegionClass::StarBright ||
          rc == RegionClass::StarMedium ||
          rc == RegionClass::StarFaint ||
          rc == RegionClass::StarSaturated ||
          rc == RegionClass::StarHalo;
}

// Classification helper: is this an extended emission class?
inline bool IsExtendedEmission( RegionClass rc )
{
   return rc == RegionClass::NebulaEmission ||
          rc == RegionClass::NebulaReflection ||
          rc == RegionClass::NebulaPlanetary ||
          rc == RegionClass::GalacticCirrus;
}

// ----------------------------------------------------------------------------
// Histogram Data Structure
// ----------------------------------------------------------------------------

class NXHistogram
{
public:

   static constexpr int DefaultBins = 65536;  // 16-bit equivalent

   NXHistogram( int numBins = DefaultBins );

   // Reset histogram to zero
   void Clear();

   // Add a value to the histogram
   void Add( double value, double weight = 1.0 );

   // Access bin counts
   double operator[]( int bin ) const { return m_bins[bin]; }
   double& operator[]( int bin ) { return m_bins[bin]; }

   // Properties
   int NumBins() const { return static_cast<int>( m_bins.size() ); }
   double TotalCount() const { return m_totalCount; }
   double TotalWeight() const { return m_totalWeight; }

   // Statistics from histogram
   double Mean() const;
   double Median() const;
   double StdDev() const;
   double Mode() const;
   double Percentile( double p ) const;  // p in [0, 1]

   // Peak analysis
   int PeakBin() const;
   double PeakValue() const;
   double PeakWidth( double threshold = 0.5 ) const;  // FWHM-like

   // Histogram shape metrics
   double Skewness() const;
   double Kurtosis() const;

   // Clipping analysis
   double ClippedLowPercent() const;   // Percent at bin 0
   double ClippedHighPercent() const;  // Percent at max bin

   // Normalization
   void Normalize();  // Normalize to sum = 1

   // Get raw data
   const std::vector<double>& Data() const { return m_bins; }

private:

   std::vector<double> m_bins;
   double m_totalCount = 0;
   double m_totalWeight = 0;
   mutable bool m_statsValid = false;
   mutable double m_cachedMean = 0;
   mutable double m_cachedVariance = 0;

   void ComputeBasicStats() const;
   int ValueToBin( double value ) const;
   double BinToValue( int bin ) const;
};

// ----------------------------------------------------------------------------
// Extended Region Statistics
// ----------------------------------------------------------------------------

struct RegionStatistics
{
   // Basic statistics
   double min = 0.0;
   double max = 1.0;
   double mean = 0.5;
   double median = 0.5;
   double stdDev = 0.1;

   // Robust statistics
   double mad = 0.1;              // Median Absolute Deviation
   double biweightMidvariance = 0.1;  // Robust variance estimate
   double sn = 0.1;               // Sn scale estimator

   // Histogram-based
   double mode = 0.5;
   double peakLocation = 0.5;
   double peakWidth = 0.1;        // FWHM of main peak
   double skewness = 0.0;
   double kurtosis = 0.0;

   // Dynamic range
   double dynamicRange = 1.0;     // log10(max/min) when min > 0
   double dynamicRangeDB = 0.0;   // 20 * log10(max/min)

   // Clipping
   double clippedLowPct = 0.0;    // Percent of pixels at 0
   double clippedHighPct = 0.0;   // Percent of pixels at 1
   double clippingPct = 0.0;      // Total clipped

   // Signal analysis
   double snrEstimate = 10.0;     // Signal-to-noise ratio
   double noiseEstimate = 0.01;   // Estimated noise level
   double signalEstimate = 0.1;   // Estimated signal level

   // Percentiles
   double p01 = 0.0;              // 1st percentile
   double p05 = 0.0;              // 5th percentile
   double p10 = 0.0;              // 10th percentile
   double p25 = 0.0;              // 25th percentile (Q1)
   double p75 = 0.0;              // 75th percentile (Q3)
   double p90 = 0.0;              // 90th percentile
   double p95 = 0.0;              // 95th percentile
   double p99 = 0.0;              // 99th percentile

   // Region metadata
   RegionClass regionClass = RegionClass::Background;
   size_t pixelCount = 0;
   double maskCoverage = 0.0;     // Fraction of image covered by mask

   // Histogram (optional, may be null for memory efficiency)
   NXHistogram histogram;

   // Default constructor
   RegionStatistics() = default;

   // Utility methods
   double InterquartileRange() const { return p75 - p25; }
   double Range() const { return max - min; }
   double CoefficientOfVariation() const { return (mean > 0) ? stdDev / mean : 0; }

   // Classification helpers
   bool IsBright() const { return median > 0.3; }
   bool IsFaint() const { return median < 0.05; }
   bool IsHighSNR() const { return snrEstimate > 20.0; }
   bool IsLowSNR() const { return snrEstimate < 5.0; }
   bool HasClipping() const { return clippingPct > 0.1; }
   bool IsHighDynamicRange() const { return dynamicRange > 3.0; }

   // String representation
   String ToString() const;
};

// ----------------------------------------------------------------------------
// Per-Channel Statistics
// ----------------------------------------------------------------------------

struct ChannelStatistics
{
   RegionStatistics luminance;
   RegionStatistics red;
   RegionStatistics green;
   RegionStatistics blue;

   // Color metrics
   double colorBalance = 1.0;     // R/B ratio
   double saturationMean = 0.0;
   double saturationMax = 0.0;
   double hueDominant = 0.0;      // Dominant hue angle (0-360)

   bool isColor = false;
   int numChannels = 1;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __RegionStatistics_h
