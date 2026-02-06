//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// PixelSelector - Intelligent pixel selection for integration

#include "PixelSelector.h"

#include <algorithm>
#include <cmath>
#include <omp.h>
#include <stdexcept>

namespace pcl
{

// ----------------------------------------------------------------------------
// TargetContext Implementation
// ----------------------------------------------------------------------------

void TargetContext::ParseFromHeaders( const FITSKeywordArray& keywords )
{
   for ( const FITSHeaderKeyword& kw : keywords )
   {
      IsoString name = kw.name.Uppercase();

      if ( name == "OBJECT" )
         objectName = kw.value.Trimmed();
      else if ( name == "FILTER" )
         filter = kw.value.Trimmed();
      else if ( name == "EXPTIME" || name == "EXPOSURE" )
         exposureTime = kw.value.ToDouble();
      else if ( name == "GAIN" )
         gain = kw.value.ToDouble();
      else if ( name == "CCD-TEMP" || name == "CCDTEMP" )
         temperature = kw.value.ToDouble();
      else if ( name == "TELESCOP" )
         telescope = kw.value.Trimmed();
      else if ( name == "INSTRUME" )
         instrument = kw.value.Trimmed();
   }

   // Infer expected features from object name
   InferExpectedFeatures();
}

// ----------------------------------------------------------------------------

void TargetContext::InferExpectedFeatures()
{
   if ( objectName.IsEmpty() && filter.IsEmpty() )
      return;

   // Infer from object name first
   if ( !objectName.IsEmpty() )
   {
      KnownObjects::GetExpectedFeatures( objectName,
         expectsEmissionNebula, expectsDarkNebula, expectsGalaxy,
         expectsStarCluster, expectsPlanetaryNebula );
   }

   // Apply filter-based adjustments
   if ( !filter.IsEmpty() )
   {
      String upperFilter = filter.Uppercase();

      // Detect narrowband filters
      bool isNarrowband = upperFilter.Contains( "HA" ) ||
                          upperFilter.Contains( "H-ALPHA" ) ||
                          upperFilter.Contains( "HALPHA" ) ||
                          upperFilter.Contains( "OIII" ) ||
                          upperFilter.Contains( "O3" ) ||
                          upperFilter.Contains( "SII" ) ||
                          upperFilter.Contains( "S2" ) ||
                          upperFilter.Contains( "NII" );

      if ( isNarrowband )
      {
         // Narrowband: stronger signal preservation, flatter background expected
         // Almost all narrowband signal IS emission nebulosity
         expectsEmissionNebula = true;
      }

      // Detect luminance filter - broadband, all features equally likely
      bool isLuminance = upperFilter.Contains( "LUM" ) ||
                         upperFilter.Contains( "CLEAR" ) ||
                         upperFilter == "L";

      // Luminance data doesn't change expectations, but we note it
      // for potential future use (e.g., luminance weighting)
      (void)isLuminance;
   }
}

// ----------------------------------------------------------------------------
// PixelSelector Implementation
// ----------------------------------------------------------------------------

PixelSelector::PixelSelector()
   : m_config()
   , m_analyzer( m_config.stackConfig )
{
}

PixelSelector::PixelSelector( const PixelSelectorConfig& config )
   : m_config( config )
   , m_analyzer( config.stackConfig )
{
}

// ----------------------------------------------------------------------------

void PixelSelector::SetTargetContext( const FITSKeywordArray& keywords )
{
   m_targetContext.ParseFromHeaders( keywords );
}

// ----------------------------------------------------------------------------

void PixelSelector::SetFrameWeights( const std::vector<float>& weights )
{
   m_frameWeights = weights;

   // Also pass through to the analyzer's config so SelectBestValue can use them
   StackAnalysisConfig analyzerConfig = m_analyzer.Config();
   analyzerConfig.frameWeights = weights;
   m_analyzer.SetConfig( analyzerConfig );
}

// ----------------------------------------------------------------------------

void PixelSelector::SetSegmentation(
   const std::vector<int>& segmentationMap,
   const std::vector<float>& confidenceMap,
   int width, int height )
{
   m_segmentationMap = segmentationMap;
   m_confidenceMap = confidenceMap;
   m_segWidth = width;
   m_segHeight = height;
   m_hasSegmentation = ( width > 0 && height > 0 &&
      !segmentationMap.empty() &&
      segmentationMap.size() == static_cast<size_t>( width ) * height );
}

void PixelSelector::SetSegmentation(
   const std::vector<std::vector<int>>& segmentationMap,
   const std::vector<std::vector<float>>& confidenceMap )
{
   if ( segmentationMap.empty() || segmentationMap[0].empty() )
   {
      m_segWidth = 0;
      m_segHeight = 0;
      m_hasSegmentation = false;
      m_segmentationMap.clear();
      m_confidenceMap.clear();
      return;
   }

   int height = static_cast<int>( segmentationMap.size() );
   int width = static_cast<int>( segmentationMap[0].size() );

   // Flatten 2D input to 1D storage
   m_segmentationMap.resize( static_cast<size_t>( height ) * width );
   m_confidenceMap.resize( static_cast<size_t>( height ) * width );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         size_t idx = static_cast<size_t>( y ) * width + x;
         m_segmentationMap[idx] = segmentationMap[y][x];
         m_confidenceMap[idx] = ( y < static_cast<int>( confidenceMap.size() ) &&
                                  x < static_cast<int>( confidenceMap[y].size() ) )
                                ? confidenceMap[y][x] : 0.5f;
      }
   }

   m_segWidth = width;
   m_segHeight = height;
   m_hasSegmentation = true;
}

// ----------------------------------------------------------------------------

void PixelSelector::SetSegmentation( const Image& segmentationImage )
{
   if ( segmentationImage.IsEmpty() || segmentationImage.NumberOfChannels() < 1 )
   {
      m_hasSegmentation = false;
      return;
   }

   int width = segmentationImage.Width();
   int height = segmentationImage.Height();

   // Validate dimensions to prevent allocation crash
   if ( width <= 0 || height <= 0 || width > 100000 || height > 100000 )
      throw std::runtime_error( "Invalid segmentation image dimensions" );

   size_t numPixels = static_cast<size_t>( width ) * height;
   if ( numPixels > 500000000 )  // 500M pixels max
      throw std::runtime_error( "Segmentation image too large" );

   m_segmentationMap.resize( numPixels );
   m_confidenceMap.resize( numPixels, 1.0f );

   const Image::sample* classData = segmentationImage.PixelData( 0 );
   const Image::sample* confData = (segmentationImage.NumberOfChannels() >= 2)
                                   ? segmentationImage.PixelData( 1 ) : nullptr;

   // OpenMP parallelization for segmentation map conversion
   #pragma omp parallel for schedule(static)
   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         size_t idx = static_cast<size_t>( y ) * width + x;
         // Class stored as normalized float, convert to int
         m_segmentationMap[idx] = static_cast<int>( classData[idx] * (static_cast<int>( RegionClass::Count ) - 1) + 0.5f );
         if ( confData )
            m_confidenceMap[idx] = static_cast<float>( confData[idx] );
      }
   }

   m_segWidth = width;
   m_segHeight = height;
   m_hasSegmentation = true;
}

// ----------------------------------------------------------------------------

Image PixelSelector::ProcessStack(
   const std::vector<const Image*>& frames,
   int channel ) const
{
   std::vector<PixelSelectionResult> metadata;
   return ProcessStackWithMetadata( frames, channel, metadata );
}

// ----------------------------------------------------------------------------

Image PixelSelector::ProcessStackWithMetadata(
   const std::vector<const Image*>& frames,
   int channel,
   std::vector<PixelSelectionResult>& metadata ) const
{
   if ( frames.empty() || frames[0] == nullptr )
      return Image();

   int width = frames[0]->Width();
   int height = frames[0]->Height();
   int numFrames = static_cast<int>( frames.size() );

   // Validate dimensions to prevent allocation crashes
   if ( width <= 0 || height <= 0 || width > 100000 || height > 100000 )
      throw std::runtime_error( "Invalid image dimensions for pixel selection" );

   size_t numPixels = static_cast<size_t>( width ) * height;
   if ( numPixels > 500000000 )  // 500M pixels max
      throw std::runtime_error( "Image too large for pixel selection" );

   // Validate frame count
   if ( numFrames <= 0 || numFrames > 10000 )
      throw std::runtime_error( "Invalid number of frames: " + std::to_string( numFrames ) );

   // Validate dimensions
   for ( const Image* frame : frames )
   {
      if ( frame == nullptr || frame->Width() != width || frame->Height() != height )
         return Image();
   }

   // Create output image (single channel for now)
   Image result( width, height, ColorSpace::Gray );
   result.Zero();

   // Allocate flat metadata vector indexed as [y * width + x]
   metadata.resize( numPixels );

   // Process each pixel
   Image::sample* outData = result.PixelData( 0 );

   // OpenMP parallelization - process rows in parallel
   // Each thread gets its own pixelValues buffer (thread-local)
   #pragma omp parallel for schedule(dynamic)
   for ( int y = 0; y < height; ++y )
   {
      // Thread-local buffer for pixel values
      std::vector<float> pixelValues( numFrames );

      for ( int x = 0; x < width; ++x )
      {
         // Collect values from all frames
         for ( int f = 0; f < numFrames; ++f )
         {
            const Image::sample* data = frames[f]->PixelData( channel );
            size_t idx = static_cast<size_t>( y ) * width + x;
            pixelValues[f] = static_cast<float>( data[idx] );
         }

         // Select best pixel
         PixelSelectionResult result_pixel = SelectPixel( pixelValues, x, y );

         // Store result - no race condition since each thread works on different rows
         size_t outIdx = static_cast<size_t>( y ) * width + x;
         outData[outIdx] = result_pixel.value;
         metadata[outIdx] = result_pixel;
      }
   }

   return result;
}

// ----------------------------------------------------------------------------

Image PixelSelector::ProcessStackWithMetadata(
   FrameStreamer& streamer,
   int channel,
   std::vector<PixelSelectionResult>& metadata ) const
{
   int width = streamer.Width();
   int height = streamer.Height();
   int numFrames = streamer.NumFrames();

   // Validate dimensions
   if ( width <= 0 || height <= 0 || width > 100000 || height > 100000 )
      throw std::runtime_error( "Invalid image dimensions for pixel selection" );

   size_t numPixels = static_cast<size_t>( width ) * height;
   if ( numPixels > 500000000 )
      throw std::runtime_error( "Image too large for pixel selection" );

   if ( numFrames <= 0 || numFrames > 10000 )
      throw std::runtime_error( "Invalid number of frames: " + std::to_string( numFrames ) );

   // Create output image (single channel)
   Image result( width, height, ColorSpace::Gray );
   result.Zero();

   // Allocate flat metadata vector
   metadata.resize( numPixels );

   Image::sample* outData = result.PixelData( 0 );

   // Streaming: process row-by-row (sequential I/O, parallel pixel processing)
   std::vector<std::vector<float>> rowData;

   for ( int y = 0; y < height; ++y )
   {
      // Sequential: read this row from all frames
      if ( !streamer.ReadRow( y, channel, rowData ) )
         throw std::runtime_error( "Failed to read row " + std::to_string( y ) );

      // Parallel: process pixels within this row
      #pragma omp parallel for schedule(static)
      for ( int x = 0; x < width; ++x )
      {
         // Collect values from the pre-read row data
         std::vector<float> pixelValues( numFrames );
         for ( int f = 0; f < numFrames; ++f )
            pixelValues[f] = rowData[f][x];

         // Select best pixel
         PixelSelectionResult result_pixel = SelectPixel( pixelValues, x, y );

         // Store result
         size_t outIdx = static_cast<size_t>( y ) * width + x;
         outData[outIdx] = result_pixel.value;
         metadata[outIdx] = result_pixel;
      }
   }

   return result;
}

// ----------------------------------------------------------------------------

Image PixelSelector::ProcessStack(
   FrameStreamer& streamer,
   int channel ) const
{
   std::vector<PixelSelectionResult> metadata;
   return ProcessStackWithMetadata( streamer, channel, metadata );
}

// ----------------------------------------------------------------------------

PixelSelectionResult PixelSelector::SelectPixel(
   const std::vector<float>& values,
   int x, int y ) const
{
   PixelSelectionResult result;

   if ( values.empty() )
      return result;

   // Get ML class if available
   RegionClass regionClass = RegionClass::Background;
   float classConfidence = 0.0f;
   bool hasValidClass = false;

   if ( m_hasSegmentation )
   {
      regionClass = GetRegionClass( x, y );
      classConfidence = GetClassConfidence( x, y );

      // Check if confidence meets threshold
      hasValidClass = (classConfidence >= m_config.minClassConfidence);
   }

   result.regionClass = regionClass;
   result.classConfidence = classConfidence;

   // Analyze pixel stack with or without class
   PixelStackMetadata stackMeta;
   if ( hasValidClass )
      stackMeta = m_analyzer.AnalyzePixelWithClass( values, regionClass, classConfidence );
   else
      stackMeta = m_analyzer.AnalyzePixel( values );

   result.value = stackMeta.selectedValue;
   result.sourceFrame = stackMeta.sourceFrame;
   result.confidence = stackMeta.confidence;
   result.outlierMask = stackMeta.outlierMask;
   result.outlierCount = stackMeta.outlierCount;
   result.totalFrames = stackMeta.totalFrames;
   result.distMu = stackMeta.distribution.mu;
   result.distSigma = stackMeta.distribution.sigma;

   // Apply target context adjustments
   if ( m_config.useTargetContext )
      ApplyTargetContextAdjustments( result, regionClass, values );

   // Apply spatial smoothing in transition zones
   if ( m_config.useSpatialContext && m_hasSegmentation )
   {
      SpatialContext context = GetSpatialContext( x, y );
      if ( context.isTransitionZone )
      {
         result.value = ApplySpatialSmoothing( values, context, result.value, result.sourceFrame );
      }
   }

   return result;
}

// ----------------------------------------------------------------------------

SpatialContext PixelSelector::GetSpatialContext( int x, int y ) const
{
   SpatialContext context;

   if ( !m_hasSegmentation )
      return context;

   context.centerClass = static_cast<int>( GetRegionClass( x, y ) );
   context.isEdgePixel = (x == 0 || y == 0 || x >= m_segWidth - 1 || y >= m_segHeight - 1);

   // Get 8-neighbor classes (clockwise from top)
   // Offsets: top, top-right, right, bottom-right, bottom, bottom-left, left, top-left
   const int dx[8] = { 0,  1, 1, 1, 0, -1, -1, -1 };
   const int dy[8] = {-1, -1, 0, 1, 1,  1,  0, -1 };

   int classCount[static_cast<int>( RegionClass::Count )] = {0};

   for ( int i = 0; i < 8; ++i )
   {
      int nx = x + dx[i];
      int ny = y + dy[i];

      if ( nx >= 0 && nx < m_segWidth && ny >= 0 && ny < m_segHeight )
      {
         context.neighborClasses[i] = static_cast<int>( GetRegionClass( nx, ny ) );
         context.neighborConfidences[i] = GetClassConfidence( nx, ny );

         if ( context.neighborClasses[i] >= 0 && context.neighborClasses[i] < static_cast<int>( RegionClass::Count ) )
            classCount[context.neighborClasses[i]]++;

         if ( context.neighborClasses[i] == context.centerClass )
            context.numMatchingNeighbors++;
      }
      else
      {
         context.neighborClasses[i] = -1;
         context.neighborConfidences[i] = 0.0f;
      }
   }

   // Find dominant neighbor class
   int maxCount = 0;
   for ( int c = 0; c < static_cast<int>( RegionClass::Count ); ++c )
   {
      if ( classCount[c] > maxCount )
      {
         maxCount = classCount[c];
         context.dominantNeighborClass = c;
      }
   }

   // Transition zone if fewer than 5 neighbors match (out of 8)
   context.isTransitionZone = (context.numMatchingNeighbors < 5);

   return context;
}

// ----------------------------------------------------------------------------

RegionClass PixelSelector::GetRegionClass( int x, int y ) const
{
   if ( !m_hasSegmentation || x < 0 || x >= m_segWidth || y < 0 || y >= m_segHeight )
      return RegionClass::Background;

   size_t idx = static_cast<size_t>( y ) * m_segWidth + x;
   if ( idx >= m_segmentationMap.size() )
      return RegionClass::Background;

   int classInt = m_segmentationMap[idx];
   if ( classInt < 0 || classInt >= static_cast<int>( RegionClass::Count ) )
      return RegionClass::Background;

   return static_cast<RegionClass>( classInt );
}

// ----------------------------------------------------------------------------

float PixelSelector::GetClassConfidence( int x, int y ) const
{
   if ( !m_hasSegmentation || x < 0 || x >= m_segWidth || y < 0 || y >= m_segHeight )
      return 0.0f;

   size_t idx = static_cast<size_t>( y ) * m_segWidth + x;
   if ( idx >= m_confidenceMap.size() )
      return 0.0f;

   return m_confidenceMap[idx];
}

// ----------------------------------------------------------------------------

void PixelSelector::ApplyTargetContextAdjustments(
   PixelSelectionResult& result,
   RegionClass regionClass,
   const std::vector<float>& values ) const
{
   // Boost confidence if ML class matches expected features from object name
   bool classMatchesExpected = false;

   switch ( regionClass )
   {
   case RegionClass::NebulaEmission:
      if ( m_targetContext.expectsEmissionNebula )
         classMatchesExpected = true;
      break;

   case RegionClass::NebulaDark:
   case RegionClass::DustLane:
      if ( m_targetContext.expectsDarkNebula )
         classMatchesExpected = true;
      break;

   case RegionClass::GalaxyCore:
   case RegionClass::GalaxySpiral:
   case RegionClass::GalaxyElliptical:
   case RegionClass::GalaxyIrregular:
      if ( m_targetContext.expectsGalaxy )
         classMatchesExpected = true;
      break;

   case RegionClass::StarClusterOpen:
   case RegionClass::StarClusterGlobular:
      if ( m_targetContext.expectsStarCluster )
         classMatchesExpected = true;
      break;

   case RegionClass::NebulaPlanetary:
      if ( m_targetContext.expectsPlanetaryNebula )
         classMatchesExpected = true;
      break;

   default:
      break;
   }

   if ( classMatchesExpected )
   {
      // Boost confidence
      result.confidence = std::min( 1.0f, result.confidence + m_config.contextWeight );
   }
}

// ----------------------------------------------------------------------------

float PixelSelector::ApplySpatialSmoothing(
   const std::vector<float>& values,
   const SpatialContext& context,
   float selectedValue,
   int selectedFrame ) const
{
   // In transition zones, blend the selected value with the median
   // to reduce hard boundaries

   if ( !context.isTransitionZone || values.empty() )
      return selectedValue;

   // Compute median
   std::vector<float> sorted = values;
   std::sort( sorted.begin(), sorted.end() );
   float median = sorted[sorted.size() / 2];

   // Blend based on how much of a transition this is
   float transitionStrength = 1.0f - (context.numMatchingNeighbors / 8.0f);
   float blendWeight = m_config.transitionSmoothingWeight * transitionStrength;

   return selectedValue * (1.0f - blendWeight) + median * blendWeight;
}

// ----------------------------------------------------------------------------
// KnownObjects Implementation
// ----------------------------------------------------------------------------

namespace KnownObjects
{

static bool MatchesDesignation( const String& name, const String& prefix, const String& number )
{
   String designator = prefix + number;
   String upper = name.Uppercase();
   String upperDes = designator.Uppercase();
   int pos = upper.Find( upperDes );
   while ( pos >= 0 )
   {
      int endPos = pos + int( upperDes.Length() );
      if ( endPos >= int( upper.Length() ) )
         return true;
      char nextChar = upper[endPos];
      if ( nextChar < '0' || nextChar > '9' )
         return true;
      pos = upper.Find( upperDes, endPos );
   }
   return false;
}

void GetExpectedFeatures( const String& objectName,
                          bool& emission, bool& dark, bool& galaxy,
                          bool& cluster, bool& planetary )
{
   emission = false;
   dark = false;
   galaxy = false;
   cluster = false;
   planetary = false;

   String upper = objectName.Uppercase();

   // Check for emission nebulae
   if ( IsMessierEmissionNebula( upper ) || IsNGCEmissionNebula( upper ) )
   {
      emission = true;
      // Many emission nebulae also have dark regions
      if ( MatchesDesignation( upper, "M", "42" ) || upper.Contains( "ORION" ) ||
           upper.Contains( "HORSEHEAD" ) || upper.Contains( "BARNARD" ) )
         dark = true;
   }

   // Check for galaxies
   if ( IsMessierGalaxy( upper ) || IsNGCGalaxy( upper ) ||
        upper.Contains( "GALAXY" ) || upper.Contains( "ANDROMEDA" ) )
      galaxy = true;

   // Check for planetary nebulae
   if ( IsPlanetaryNebula( upper ) )
      planetary = true;

   // Check for star clusters
   if ( upper.Contains( "CLUSTER" ) ||
        MatchesDesignation( upper, "M", "45" ) || MatchesDesignation( upper, "M", "7" ) ||
        MatchesDesignation( upper, "M", "35" ) || MatchesDesignation( upper, "M", "36" ) ||
        MatchesDesignation( upper, "M", "37" ) || MatchesDesignation( upper, "M", "38" ) ||
        MatchesDesignation( upper, "M", "44" ) || MatchesDesignation( upper, "M", "67" ) )
      cluster = true;
}

bool IsMessierEmissionNebula( const String& name )
{
   // Common Messier emission nebulae
   return MatchesDesignation( name, "M", "1" ) ||   // Crab
          MatchesDesignation( name, "M", "8" ) ||   // Lagoon
          MatchesDesignation( name, "M", "16" ) ||  // Eagle
          MatchesDesignation( name, "M", "17" ) ||  // Omega/Swan
          MatchesDesignation( name, "M", "20" ) ||  // Trifid
          MatchesDesignation( name, "M", "42" ) ||  // Orion
          MatchesDesignation( name, "M", "43" ) ||  // De Mairan's
          MatchesDesignation( name, "M", "76" ) ||  // Little Dumbbell
          MatchesDesignation( name, "M", "78" );    // Reflection but often grouped
}

bool IsMessierGalaxy( const String& name )
{
   // Common Messier galaxies
   return MatchesDesignation( name, "M", "31" ) ||  // Andromeda
          MatchesDesignation( name, "M", "32" ) ||
          MatchesDesignation( name, "M", "33" ) ||  // Triangulum
          MatchesDesignation( name, "M", "51" ) ||  // Whirlpool
          MatchesDesignation( name, "M", "63" ) ||  // Sunflower
          MatchesDesignation( name, "M", "64" ) ||  // Black Eye
          MatchesDesignation( name, "M", "65" ) ||
          MatchesDesignation( name, "M", "66" ) ||
          MatchesDesignation( name, "M", "74" ) ||
          MatchesDesignation( name, "M", "77" ) ||
          MatchesDesignation( name, "M", "81" ) ||  // Bode's
          MatchesDesignation( name, "M", "82" ) ||  // Cigar
          MatchesDesignation( name, "M", "83" ) ||
          MatchesDesignation( name, "M", "101" ) || // Pinwheel
          MatchesDesignation( name, "M", "104" ) || // Sombrero
          MatchesDesignation( name, "M", "106" );
}

bool IsNGCEmissionNebula( const String& name )
{
   return MatchesDesignation( name, "NGC", "281" ) ||   // Pacman
          MatchesDesignation( name, "NGC", "1499" ) ||  // California
          MatchesDesignation( name, "NGC", "2024" ) ||  // Flame
          MatchesDesignation( name, "NGC", "2237" ) ||  // Rosette
          MatchesDesignation( name, "NGC", "2244" ) ||  // Rosette cluster
          MatchesDesignation( name, "NGC", "6888" ) ||  // Crescent
          MatchesDesignation( name, "NGC", "7000" ) ||  // North America
          MatchesDesignation( name, "NGC", "7635" ) ||  // Bubble
          MatchesDesignation( name, "IC", "1396" ) ||   // Elephant Trunk
          MatchesDesignation( name, "IC", "1805" ) ||   // Heart
          MatchesDesignation( name, "IC", "1848" ) ||   // Soul
          MatchesDesignation( name, "IC", "2177" ) ||   // Seagull
          MatchesDesignation( name, "IC", "5070" ) ||   // Pelican
          name.Contains( "SH2-" );                      // Sharpless catalog
}

bool IsNGCGalaxy( const String& name )
{
   return MatchesDesignation( name, "NGC", "224" ) ||   // M31
          MatchesDesignation( name, "NGC", "598" ) ||   // M33
          MatchesDesignation( name, "NGC", "891" ) ||
          MatchesDesignation( name, "NGC", "1300" ) ||
          MatchesDesignation( name, "NGC", "2403" ) ||
          MatchesDesignation( name, "NGC", "4565" ) ||  // Needle
          MatchesDesignation( name, "NGC", "5128" ) ||  // Centaurus A
          MatchesDesignation( name, "NGC", "6744" ) ||
          MatchesDesignation( name, "NGC", "7331" );
}

bool IsPlanetaryNebula( const String& name )
{
   return MatchesDesignation( name, "M", "27" ) ||      // Dumbbell
          MatchesDesignation( name, "M", "57" ) ||      // Ring
          MatchesDesignation( name, "M", "97" ) ||      // Owl
          MatchesDesignation( name, "NGC", "6543" ) ||  // Cat's Eye
          MatchesDesignation( name, "NGC", "6720" ) ||  // Ring
          MatchesDesignation( name, "NGC", "6826" ) ||  // Blinking
          MatchesDesignation( name, "NGC", "7009" ) ||  // Saturn
          MatchesDesignation( name, "NGC", "7293" ) ||  // Helix
          MatchesDesignation( name, "NGC", "7662" ) ||  // Blue Snowball
          name.Contains( "ABELL" ) ||                   // Abell planetaries
          name.Contains( "PN " ) ||                     // Explicit PN designation
          name.Contains( "PLANETARY" );
}

} // namespace KnownObjects

// ----------------------------------------------------------------------------

} // namespace pcl
