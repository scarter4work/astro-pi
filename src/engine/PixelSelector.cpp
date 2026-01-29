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
   if ( objectName.IsEmpty() )
      return;

   KnownObjects::GetExpectedFeatures( objectName,
      expectsEmissionNebula, expectsDarkNebula, expectsGalaxy,
      expectsStarCluster, expectsPlanetaryNebula );
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

void PixelSelector::SetSegmentation(
   const std::vector<std::vector<int>>& segmentationMap,
   const std::vector<std::vector<float>>& confidenceMap )
{
   m_segmentationMap = segmentationMap;
   m_confidenceMap = confidenceMap;

   if ( !segmentationMap.empty() && !segmentationMap[0].empty() )
   {
      m_segHeight = static_cast<int>( segmentationMap.size() );
      m_segWidth = static_cast<int>( segmentationMap[0].size() );
      m_hasSegmentation = true;
   }
   else
   {
      m_segWidth = 0;
      m_segHeight = 0;
      m_hasSegmentation = false;
   }
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

   m_segmentationMap.resize( height, std::vector<int>( width ) );
   m_confidenceMap.resize( height, std::vector<float>( width, 1.0f ) );

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
         m_segmentationMap[y][x] = static_cast<int>( classData[idx] * (static_cast<int>( RegionClass::Count ) - 1) + 0.5f );
         if ( confData )
            m_confidenceMap[y][x] = static_cast<float>( confData[idx] );
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
   std::vector<std::vector<PixelSelectionResult>> metadata;
   return ProcessStackWithMetadata( frames, channel, metadata );
}

// ----------------------------------------------------------------------------

Image PixelSelector::ProcessStackWithMetadata(
   const std::vector<const Image*>& frames,
   int channel,
   std::vector<std::vector<PixelSelectionResult>>& metadata ) const
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

   // Allocate metadata grid
   metadata.resize( height, std::vector<PixelSelectionResult>( width ) );

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
         metadata[y][x] = result_pixel;
      }
   }

   return result;
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

   int classInt = m_segmentationMap[y][x];
   if ( classInt < 0 || classInt >= static_cast<int>( RegionClass::Count ) )
      return RegionClass::Background;

   return static_cast<RegionClass>( classInt );
}

// ----------------------------------------------------------------------------

float PixelSelector::GetClassConfidence( int x, int y ) const
{
   if ( !m_hasSegmentation || x < 0 || x >= m_segWidth || y < 0 || y >= m_segHeight )
      return 0.0f;

   return m_confidenceMap[y][x];
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
      if ( upper.Contains( "42" ) || upper.Contains( "ORION" ) ||
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
        (upper.StartsWith( "M" ) && (upper.Contains( "45" ) || upper.Contains( "7" ) ||
         upper.Contains( "35" ) || upper.Contains( "36" ) || upper.Contains( "37" ) ||
         upper.Contains( "38" ) || upper.Contains( "44" ) || upper.Contains( "67" ))) )
      cluster = true;
}

bool IsMessierEmissionNebula( const String& name )
{
   // Common Messier emission nebulae
   return name.Contains( "M1" ) ||   // Crab
          name.Contains( "M8" ) ||   // Lagoon
          name.Contains( "M16" ) ||  // Eagle
          name.Contains( "M17" ) ||  // Omega/Swan
          name.Contains( "M20" ) ||  // Trifid
          name.Contains( "M42" ) ||  // Orion
          name.Contains( "M43" ) ||  // De Mairan's
          name.Contains( "M76" ) ||  // Little Dumbbell
          name.Contains( "M78" );    // Reflection but often grouped
}

bool IsMessierGalaxy( const String& name )
{
   // Common Messier galaxies
   return name.Contains( "M31" ) ||  // Andromeda
          name.Contains( "M32" ) ||
          name.Contains( "M33" ) ||  // Triangulum
          name.Contains( "M51" ) ||  // Whirlpool
          name.Contains( "M63" ) ||  // Sunflower
          name.Contains( "M64" ) ||  // Black Eye
          name.Contains( "M65" ) ||
          name.Contains( "M66" ) ||
          name.Contains( "M74" ) ||
          name.Contains( "M77" ) ||
          name.Contains( "M81" ) ||  // Bode's
          name.Contains( "M82" ) ||  // Cigar
          name.Contains( "M83" ) ||
          name.Contains( "M101" ) || // Pinwheel
          name.Contains( "M104" ) || // Sombrero
          name.Contains( "M106" );
}

bool IsNGCEmissionNebula( const String& name )
{
   return name.Contains( "NGC281" ) ||   // Pacman
          name.Contains( "NGC1499" ) ||  // California
          name.Contains( "NGC2024" ) ||  // Flame
          name.Contains( "NGC2237" ) ||  // Rosette
          name.Contains( "NGC2244" ) ||  // Rosette cluster
          name.Contains( "NGC6888" ) ||  // Crescent
          name.Contains( "NGC7000" ) ||  // North America
          name.Contains( "NGC7635" ) ||  // Bubble
          name.Contains( "IC1396" ) ||   // Elephant Trunk
          name.Contains( "IC1805" ) ||   // Heart
          name.Contains( "IC1848" ) ||   // Soul
          name.Contains( "IC2177" ) ||   // Seagull
          name.Contains( "IC5070" ) ||   // Pelican
          name.Contains( "SH2-" );       // Sharpless catalog
}

bool IsNGCGalaxy( const String& name )
{
   return name.Contains( "NGC224" ) ||   // M31
          name.Contains( "NGC598" ) ||   // M33
          name.Contains( "NGC891" ) ||
          name.Contains( "NGC1300" ) ||
          name.Contains( "NGC2403" ) ||
          name.Contains( "NGC4565" ) ||  // Needle
          name.Contains( "NGC5128" ) ||  // Centaurus A
          name.Contains( "NGC6744" ) ||
          name.Contains( "NGC7331" );
}

bool IsPlanetaryNebula( const String& name )
{
   return name.Contains( "M27" ) ||      // Dumbbell
          name.Contains( "M57" ) ||      // Ring
          name.Contains( "M97" ) ||      // Owl
          name.Contains( "NGC6543" ) ||  // Cat's Eye
          name.Contains( "NGC6720" ) ||  // Ring
          name.Contains( "NGC6826" ) ||  // Blinking
          name.Contains( "NGC7009" ) ||  // Saturn
          name.Contains( "NGC7293" ) ||  // Helix
          name.Contains( "NGC7662" ) ||  // Blue Snowball
          name.Contains( "ABELL" ) ||    // Abell planetaries
          name.Contains( "PN " ) ||      // Explicit PN designation
          name.Contains( "PLANETARY" );
}

} // namespace KnownObjects

// ----------------------------------------------------------------------------

} // namespace pcl
