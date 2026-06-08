// ----------------------------------------------------------------------------
// FlatCalibrator.cpp — Median-stack flat frames, normalize, calibrate lights
//
// Algorithm:
//   1. Collect N flat frames per channel.
//   2. For each pixel position, take the median across all N frames.
//   3. Normalize each channel by dividing by the channel's median pixel value,
//      yielding master flat values ~ 1.0 for uniform regions.
//   4. Clamp to MIN_FLAT_VALUE to prevent division by near-zero.
//   5. Calibrate lights by dividing each pixel by the master flat pixel.
//
// Copyright (c) 2026 Scott Carter
// ----------------------------------------------------------------------------

#include "engine/FlatCalibrator.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace nukex {

// ============================================================================
// Median helper — O(N) via nth_element, operates on a mutable copy
// ============================================================================

static float medianOf( std::vector<float>& v )
{
   if ( v.empty() )
      return 0.0f;

   size_t n = v.size();
   size_t mid = n / 2;
   std::nth_element( v.begin(), v.begin() + mid, v.end() );

   if ( n % 2 == 1 )
      return v[mid];

   // Even count: average of two middle values
   float upper = v[mid];
   float lower = *std::max_element( v.begin(), v.begin() + mid );
   return 0.5f * ( lower + upper );
}

// ============================================================================
// addFrame — accumulate a debayered flat frame
// ============================================================================

void FlatCalibrator::addFrame( const float* r, const float* g, const float* b,
                                int width, int height )
{
   if ( width <= 0 || height <= 0 )
      throw std::invalid_argument( "FlatCalibrator::addFrame: invalid dimensions" );

   if ( m_frameCount == 0 )
   {
      m_width  = width;
      m_height = height;
   }
   else if ( width != m_width || height != m_height )
   {
      throw std::invalid_argument( "FlatCalibrator::addFrame: dimension mismatch" );
   }

   const size_t N = static_cast<size_t>( width ) * height;

   m_framesR.emplace_back( r, r + N );
   m_framesG.emplace_back( g, g + N );
   m_framesB.emplace_back( b, b + N );

   ++m_frameCount;
}

// ============================================================================
// buildMasterFlat — median-stack + normalize
// ============================================================================

void FlatCalibrator::buildMasterFlat( LogCallback log )
{
   if ( m_frameCount == 0 )
   {
      if ( log )
         log( "FlatCalibrator: no frames added, nothing to build." );
      return;
   }

   const size_t N = static_cast<size_t>( m_width ) * m_height;

   m_masterR.resize( N );
   m_masterG.resize( N );
   m_masterB.resize( N );

   // ------------------------------------------------------------------
   // Step 1: Per-pixel median across all frames
   // ------------------------------------------------------------------

   std::vector<float> column( m_frameCount );

   auto medianStack = [&]( const std::vector<std::vector<float>>& frames,
                           std::vector<float>& master )
   {
      for ( size_t i = 0; i < N; ++i )
      {
         for ( int f = 0; f < m_frameCount; ++f )
            column[f] = frames[f][i];
         master[i] = medianOf( column );
      }
   };

   medianStack( m_framesR, m_masterR );
   medianStack( m_framesG, m_masterG );
   medianStack( m_framesB, m_masterB );

   // ------------------------------------------------------------------
   // Step 2: Compute per-channel median for normalization
   // ------------------------------------------------------------------

   auto channelMedian = []( const std::vector<float>& master )
   {
      std::vector<float> tmp( master );
      return medianOf( tmp );
   };

   float medR = channelMedian( m_masterR );
   float medG = channelMedian( m_masterG );
   float medB = channelMedian( m_masterB );

   // ------------------------------------------------------------------
   // Step 3: Normalize + clamp
   // ------------------------------------------------------------------

   auto normalizeChannel = [&]( std::vector<float>& master, float med,
                                float& outMin, float& outMax )
   {
      if ( med < MIN_FLAT_VALUE )
         med = MIN_FLAT_VALUE;

      float mn = 1e30f, mx = -1e30f;
      for ( size_t i = 0; i < N; ++i )
      {
         master[i] /= med;
         if ( master[i] < MIN_FLAT_VALUE )
            master[i] = MIN_FLAT_VALUE;
         // Cap correction factor — a flat value below 1/MAX_FLAT_CORRECTION
         // means the correction (1/flat) would exceed MAX_FLAT_CORRECTION,
         // amplifying noise more than it helps. Clamp to preserve some
         // vignetting rather than amplify edge noise.
         float minAllowed = 1.0f / MAX_FLAT_CORRECTION;
         if ( master[i] < minAllowed )
            master[i] = minAllowed;
         mn = std::min( mn, master[i] );
         mx = std::max( mx, master[i] );
      }
      outMin = mn;
      outMax = mx;
   };

   float minR, maxR, minG, maxG, minB, maxB;
   normalizeChannel( m_masterR, medR, minR, maxR );
   normalizeChannel( m_masterG, medG, minG, maxG );
   normalizeChannel( m_masterB, medB, minB, maxB );

   // ------------------------------------------------------------------
   // Step 4: Free individual frame storage
   // ------------------------------------------------------------------

   m_framesR.clear();  m_framesR.shrink_to_fit();
   m_framesG.clear();  m_framesG.shrink_to_fit();
   m_framesB.clear();  m_framesB.shrink_to_fit();

   m_ready = true;

   // ------------------------------------------------------------------
   // Step 5: Diagnostics
   // ------------------------------------------------------------------

   if ( log )
   {
      std::ostringstream ss;
      ss << "FlatCalibrator: built master flat from " << m_frameCount
         << " frames (" << m_width << "x" << m_height << ")\n"
         << "  R median=" << medR << "  min=" << minR << "  max=" << maxR << "\n"
         << "  G median=" << medG << "  min=" << minG << "  max=" << maxG << "\n"
         << "  B median=" << medB << "  min=" << minB << "  max=" << maxB;
      log( ss.str() );
   }
}

// ============================================================================
// calibrate — divide each light pixel by master flat
// ============================================================================

void FlatCalibrator::calibrate( float* r, float* g, float* b,
                                 int width, int height ) const
{
   if ( !m_ready )
      throw std::runtime_error( "FlatCalibrator::calibrate: master flat not built" );

   if ( width != m_width || height != m_height )
      throw std::invalid_argument( "FlatCalibrator::calibrate: dimension mismatch" );

   const size_t N = static_cast<size_t>( width ) * height;

   for ( size_t i = 0; i < N; ++i )
   {
      r[i] /= m_masterR[i];
      g[i] /= m_masterG[i];
      b[i] /= m_masterB[i];
   }
}

} // namespace nukex
