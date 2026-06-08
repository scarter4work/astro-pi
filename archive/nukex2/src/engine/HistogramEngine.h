//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// High-Performance Histogram Engine

#ifndef __HistogramEngine_h
#define __HistogramEngine_h

#include "RegionStatistics.h"

#include <pcl/Image.h>
#include <vector>
#include <memory>

namespace pcl
{

// ----------------------------------------------------------------------------
// Histogram Engine Configuration
// ----------------------------------------------------------------------------

struct HistogramConfig
{
   int numBins = 65536;           // Number of histogram bins
   bool useParallel = true;       // Enable parallel computation
   int numThreads = 0;            // 0 = auto-detect
   double clipLow = 0.0;          // Ignore values below this
   double clipHigh = 1.0;         // Ignore values above this
   bool computePercentiles = true; // Compute percentile values
   bool computeRobustStats = true; // Compute MAD, Sn, etc.
};

// ----------------------------------------------------------------------------
// Histogram Engine
//
// Fast, parallel histogram computation with support for:
// - Masked regions (soft masks with weights)
// - Per-channel and luminance histograms
// - Percentile computation
// - Robust statistics (MAD, biweight midvariance)
// ----------------------------------------------------------------------------

class HistogramEngine
{
public:

   HistogramEngine( const HistogramConfig& config = HistogramConfig() );

   // Compute histogram for an entire image
   NXHistogram ComputeHistogram( const Image& image, int channel = 0 ) const;

   // Compute histogram with mask (soft mask, values 0-1)
   NXHistogram ComputeHistogram( const Image& image, const Image& mask,
                               int channel = 0, int maskChannel = 0 ) const;

   // Compute histogram from raw pixel values
   NXHistogram ComputeHistogram( const std::vector<double>& values,
                               const std::vector<double>& weights = {} ) const;

   // Compute full statistics for an image region
   RegionStatistics ComputeStatistics( const Image& image, int channel = 0 ) const;

   // Compute statistics with mask
   RegionStatistics ComputeStatistics( const Image& image, const Image& mask,
                                       int channel = 0, int maskChannel = 0 ) const;

   // Compute statistics from raw values (for internal use)
   RegionStatistics ComputeStatistics( const std::vector<double>& values,
                                       const std::vector<double>& weights = {} ) const;

   // Compute per-channel statistics for color image
   ChannelStatistics ComputeChannelStatistics( const Image& image,
                                               const Image* mask = nullptr ) const;

   // Quick statistics (no histogram, faster)
   void ComputeQuickStats( const Image& image, const Image* mask,
                           int channel, int maskChannel,
                           double& min, double& max,
                           double& mean, double& stdDev ) const;

   // Configuration
   const HistogramConfig& Config() const { return m_config; }
   void SetConfig( const HistogramConfig& config ) { m_config = config; }

private:

   HistogramConfig m_config;

   // Helper to compute robust statistics
   void ComputeRobustStats( const std::vector<double>& sortedValues,
                            const std::vector<double>& weights,
                            RegionStatistics& stats ) const;

   // Compute MAD (Median Absolute Deviation)
   double ComputeMAD( const std::vector<double>& sortedValues,
                      double median ) const;

   // Compute Sn scale estimator
   double ComputeSn( const std::vector<double>& values ) const;

   // Compute biweight midvariance
   double ComputeBiweightMidvariance( const std::vector<double>& values,
                                      double median, double mad ) const;

   // Estimate SNR from statistics
   double EstimateSNR( const RegionStatistics& stats ) const;

   // Helper for luminance computation
   double ComputeLuminance( double r, double g, double b ) const;
};

// ----------------------------------------------------------------------------
// Parallel Histogram Accumulator
//
// Thread-local histogram for parallel computation
// ----------------------------------------------------------------------------

class ParallelHistogramAccumulator
{
public:

   ParallelHistogramAccumulator( int numBins );

   // Add a value
   void Add( double value, double weight = 1.0 );

   // Merge another accumulator into this one
   void Merge( const ParallelHistogramAccumulator& other );

   // Get histogram
   const std::vector<double>& Bins() const { return m_bins; }

   // Get counts
   double TotalCount() const { return m_count; }
   double TotalWeight() const { return m_weight; }

   // Statistics accumulators
   double SumValues() const { return m_sumValues; }
   double SumSquares() const { return m_sumSquares; }
   double MinValue() const { return m_min; }
   double MaxValue() const { return m_max; }

private:

   std::vector<double> m_bins;
   double m_count = 0;
   double m_weight = 0;
   double m_sumValues = 0;
   double m_sumSquares = 0;
   double m_min = 1.0;
   double m_max = 0.0;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __HistogramEngine_h
