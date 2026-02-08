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
// 7 total classes (v2.0 taxonomy)
// ----------------------------------------------------------------------------

enum class RegionClass
{
   Background = 0,      // Sky background, gradients, noise
   BrightCompact,       // Bright/saturated stars, diffraction spikes
   FaintCompact,        // Medium/faint stars, star clusters
   BrightExtended,      // Emission/reflection/planetary nebulae, galaxies, cirrus
   DarkExtended,        // Dark nebulae, dust lanes
   Artifact,            // Hot pixels, satellite trails
   StarHalo,            // Diffuse glow around stars

   Count                // Number of region classes (7)
};

// Convert region class to string (matches model training names)
inline IsoString RegionClassToString( RegionClass rc )
{
   switch ( rc )
   {
   case RegionClass::Background:      return "background";
   case RegionClass::BrightCompact:   return "bright_compact";
   case RegionClass::FaintCompact:    return "faint_compact";
   case RegionClass::BrightExtended:  return "bright_extended";
   case RegionClass::DarkExtended:    return "dark_extended";
   case RegionClass::Artifact:        return "artifact";
   case RegionClass::StarHalo:        return "star_halo";
   default:                           return "unknown";
   }
}

// Convert region class to display name
inline String RegionClassDisplayName( RegionClass rc )
{
   switch ( rc )
   {
   case RegionClass::Background:      return "Background";
   case RegionClass::BrightCompact:   return "Bright Compact";
   case RegionClass::FaintCompact:    return "Faint Compact";
   case RegionClass::BrightExtended:  return "Bright Extended";
   case RegionClass::DarkExtended:    return "Dark Extended";
   case RegionClass::Artifact:        return "Artifact";
   case RegionClass::StarHalo:        return "Star Halo";
   default:                           return "Unknown";
   }
}

// Classification helper: is this a star-related class?
inline bool IsStarRelatedClass( RegionClass rc )
{
   return rc == RegionClass::BrightCompact ||
          rc == RegionClass::FaintCompact ||
          rc == RegionClass::StarHalo;
}

// Classification helper: is this an extended emission class?
inline bool IsExtendedEmission( RegionClass rc )
{
   return rc == RegionClass::BrightExtended;
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
