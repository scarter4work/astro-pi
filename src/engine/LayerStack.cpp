//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "LayerStack.h"

#include <pcl/Console.h>
#include <pcl/File.h>

#include <algorithm>
#include <set>

namespace pcl
{

// ----------------------------------------------------------------------------
// Layer Implementation
// ----------------------------------------------------------------------------

Layer::Layer( RegionClass regionClass, float depth )
   : m_regionClass( regionClass )
   , m_depth( depth )
   , m_name( RegionClassDisplayName( regionClass ) )
{
}

bool Layer::IsStarLayer() const
{
   return m_regionClass == RegionClass::BrightCompact ||
          m_regionClass == RegionClass::FaintCompact ||
          m_regionClass == RegionClass::StarHalo;
}

bool Layer::IsNebulaLayer() const
{
   return m_regionClass == RegionClass::BrightExtended ||
          m_regionClass == RegionClass::DarkExtended;
}

bool Layer::IsBackground() const
{
   return m_regionClass == RegionClass::Background;
}

double Layer::GetCoverage() const
{
   if ( m_mask.Width() == 0 || m_mask.Height() == 0 )
      return 0;

   double sum = 0;
   for ( int y = 0; y < m_mask.Height(); ++y )
      for ( int x = 0; x < m_mask.Width(); ++x )
         sum += m_mask( x, y, 0 );

   return sum / (m_mask.Width() * m_mask.Height());
}

double Layer::GetMeanOpacity() const
{
   if ( m_alpha.Width() == 0 || m_alpha.Height() == 0 )
      return 1.0;

   double sum = 0;
   int count = 0;

   for ( int y = 0; y < m_alpha.Height(); ++y )
   {
      for ( int x = 0; x < m_alpha.Width(); ++x )
      {
         if ( m_mask.Width() > 0 && m_mask( x, y, 0 ) > 0.1 )
         {
            sum += m_alpha( x, y, 0 );
            count++;
         }
      }
   }

   return count > 0 ? sum / count : 1.0;
}

// ----------------------------------------------------------------------------
// LayerStack Implementation
// ----------------------------------------------------------------------------

LayerStack::LayerStack( int width, int height )
   : m_width( width )
   , m_height( height )
{
}

void LayerStack::SetDimensions( int width, int height )
{
   m_width = width;
   m_height = height;
}

void LayerStack::AddLayer( const Layer& layer )
{
   m_layers.push_back( layer );
   SortByDepth();
}

void LayerStack::AddLayer( Layer&& layer )
{
   m_layers.push_back( std::move( layer ) );
   SortByDepth();
}

void LayerStack::InsertLayer( size_t index, const Layer& layer )
{
   if ( index >= m_layers.size() )
      m_layers.push_back( layer );
   else
      m_layers.insert( m_layers.begin() + index, layer );
   SortByDepth();
}

void LayerStack::RemoveLayer( size_t index )
{
   if ( index < m_layers.size() )
      m_layers.erase( m_layers.begin() + index );
}

void LayerStack::Clear()
{
   m_layers.clear();
}

Layer* LayerStack::GetLayerByClass( RegionClass rc )
{
   for ( auto& layer : m_layers )
      if ( layer.GetRegionClass() == rc )
         return &layer;
   return nullptr;
}

const Layer* LayerStack::GetLayerByClass( RegionClass rc ) const
{
   for ( const auto& layer : m_layers )
      if ( layer.GetRegionClass() == rc )
         return &layer;
   return nullptr;
}

std::vector<Layer*> LayerStack::GetLayersAtPixel( int x, int y, float threshold )
{
   std::vector<Layer*> result;

   for ( auto& layer : m_layers )
   {
      const Image& mask = layer.GetMask();
      if ( mask.Width() > 0 && x >= 0 && x < mask.Width() &&
           y >= 0 && y < mask.Height() )
      {
         if ( mask( x, y, 0 ) > threshold )
            result.push_back( &layer );
      }
   }

   return result;
}

void LayerStack::SortByDepth()
{
   std::sort( m_layers.begin(), m_layers.end(),
              []( const Layer& a, const Layer& b ) {
                 return a.GetDepth() < b.GetDepth();
              } );
}

void LayerStack::CompositePixel( int x, int y,
                                  const std::vector<const Layer*>& layers,
                                  double& r, double& g, double& b ) const
{
   r = g = b = 0;
   double remainingOpacity = 1.0;

   // Composite back to front
   for ( const auto* layer : layers )
   {
      const Image& intensity = layer->GetIntensity();
      const Image& alpha = layer->GetAlpha();

      if ( intensity.Width() == 0 || x >= intensity.Width() || y >= intensity.Height() )
         continue;

      double layerAlpha = 1.0;
      if ( alpha.Width() > 0 && x < alpha.Width() && y < alpha.Height() )
         layerAlpha = alpha( x, y, 0 );

      double contribution = layerAlpha * remainingOpacity;

      r += intensity( x, y, 0 ) * contribution;
      g += intensity( x, y, Min(1, intensity.NumberOfChannels()-1) ) * contribution;
      b += intensity( x, y, Min(2, intensity.NumberOfChannels()-1) ) * contribution;

      remainingOpacity *= (1.0 - layerAlpha);
   }
}

Image LayerStack::Composite() const
{
   Image result;
   result.AllocateData( m_width, m_height, 3, ColorSpace::RGB );

   // Collect layer pointers sorted by depth
   std::vector<const Layer*> layerPtrs;
   for ( const auto& layer : m_layers )
      layerPtrs.push_back( &layer );

   for ( int y = 0; y < m_height; ++y )
   {
      for ( int x = 0; x < m_width; ++x )
      {
         double r, g, b;
         CompositePixel( x, y, layerPtrs, r, g, b );
         result( x, y, 0 ) = r;
         result( x, y, 1 ) = g;
         result( x, y, 2 ) = b;
      }
   }

   return result;
}

Image LayerStack::CompositeWithout( size_t excludeLayerIndex ) const
{
   Image result;
   result.AllocateData( m_width, m_height, 3, ColorSpace::RGB );

   std::vector<const Layer*> layerPtrs;
   for ( size_t i = 0; i < m_layers.size(); ++i )
      if ( i != excludeLayerIndex )
         layerPtrs.push_back( &m_layers[i] );

   for ( int y = 0; y < m_height; ++y )
   {
      for ( int x = 0; x < m_width; ++x )
      {
         double r, g, b;
         CompositePixel( x, y, layerPtrs, r, g, b );
         result( x, y, 0 ) = r;
         result( x, y, 1 ) = g;
         result( x, y, 2 ) = b;
      }
   }

   return result;
}

Image LayerStack::CompositeRange( size_t startLayer, size_t endLayer ) const
{
   Image result;
   result.AllocateData( m_width, m_height, 3, ColorSpace::RGB );

   std::vector<const Layer*> layerPtrs;
   for ( size_t i = startLayer; i < Min( endLayer, m_layers.size() ); ++i )
      layerPtrs.push_back( &m_layers[i] );

   for ( int y = 0; y < m_height; ++y )
   {
      for ( int x = 0; x < m_width; ++x )
      {
         double r, g, b;
         CompositePixel( x, y, layerPtrs, r, g, b );
         result( x, y, 0 ) = r;
         result( x, y, 1 ) = g;
         result( x, y, 2 ) = b;
      }
   }

   return result;
}

Image LayerStack::RevealBehind( size_t frontLayerIndex ) const
{
   if ( frontLayerIndex >= m_layers.size() )
      return Image();

   const Layer& frontLayer = m_layers[frontLayerIndex];
   Image result;
   result.AllocateData( m_width, m_height, 3, ColorSpace::RGB );

   // Composite everything except and behind the front layer
   Image withoutFront = CompositeWithout( frontLayerIndex );

   const Image& frontMask = frontLayer.GetMask();
   const Image& frontAlpha = frontLayer.GetAlpha();
   const Image& frontIntensity = frontLayer.GetIntensity();

   for ( int y = 0; y < m_height; ++y )
   {
      for ( int x = 0; x < m_width; ++x )
      {
         bool inFrontLayer = frontMask.Width() > 0 &&
                             x < frontMask.Width() && y < frontMask.Height() &&
                             frontMask( x, y, 0 ) > 0.1;

         if ( inFrontLayer )
         {
            // Estimate what's behind by removing front layer contribution
            double alpha = 1.0;
            if ( frontAlpha.Width() > 0 )
               alpha = frontAlpha( x, y, 0 );

            // recovered = (observed - front * alpha) / (1 - alpha)
            // But we use the composite without front as estimate
            result( x, y, 0 ) = withoutFront( x, y, 0 );
            result( x, y, 1 ) = withoutFront( x, y, 1 );
            result( x, y, 2 ) = withoutFront( x, y, 2 );
         }
         else
         {
            // Outside front layer, use original composite
            result( x, y, 0 ) = withoutFront( x, y, 0 );
            result( x, y, 1 ) = withoutFront( x, y, 1 );
            result( x, y, 2 ) = withoutFront( x, y, 2 );
         }
      }
   }

   return result;
}

Image LayerStack::SubtractLayer( size_t layerIndex ) const
{
   return CompositeWithout( layerIndex );
}

Image LayerStack::SubtractPSF( size_t starLayerIndex ) const
{
   if ( starLayerIndex >= m_layers.size() )
      return Composite();

   const Layer& starLayer = m_layers[starLayerIndex];
   if ( !starLayer.HasPSF() )
      return Composite();  // No PSF to subtract

   Image result = Composite();
   PSFSubtractor subtractor;

   return subtractor.SubtractStar( result, starLayer.GetPSF() );
}

Image LayerStack::RecoverOccludedRegion( size_t starLayerIndex,
                                          float recoveryStrength ) const
{
   if ( starLayerIndex >= m_layers.size() )
      return Composite();

   const Layer& starLayer = m_layers[starLayerIndex];
   if ( !starLayer.HasPSF() )
      return Composite();

   const PSFParameters& psf = starLayer.GetPSF();

   // Find layers that might be behind this star
   Image result = Composite();
   Image contextMask;
   contextMask.AllocateData( m_width, m_height, 1, ColorSpace::Gray );

   // Build context mask from nebula layers
   for ( const auto& layer : m_layers )
   {
      if ( layer.IsNebulaLayer() && layer.GetDepth() < starLayer.GetDepth() )
      {
         const Image& mask = layer.GetMask();
         if ( mask.Width() > 0 )
         {
            for ( int y = 0; y < m_height; ++y )
               for ( int x = 0; x < m_width; ++x )
                  contextMask( x, y, 0 ) = Max( contextMask( x, y, 0 ),
                                                 mask( x, y, 0 ) );
         }
      }
   }

   // Perform PSF subtraction with recovery
   PSFSubtractor subtractor;
   PSFSubtractor::SubtractionResult subResult =
      subtractor.SubtractAndRecover( result, psf, contextMask );

   // Blend recovery based on strength
   for ( int y = 0; y < m_height; ++y )
   {
      for ( int x = 0; x < m_width; ++x )
      {
         float dx = x - psf.centerX;
         float dy = y - psf.centerY;
         float r = std::sqrt( dx * dx + dy * dy );
         float effectiveRadius = psf.GetEffectiveRadius( 0.01f );

         if ( r < effectiveRadius )
         {
            for ( int c = 0; c < 3; ++c )
            {
               double original = result( x, y, c );
               double recovered = subResult.recovered( x, y, c );
               result( x, y, c ) = original * (1 - recoveryStrength) +
                                   recovered * recoveryStrength;
            }
         }
      }
   }

   return result;
}

Image LayerStack::GetDepthMap() const
{
   Image depthMap;
   depthMap.AllocateData( m_width, m_height, 1, ColorSpace::Gray );

   for ( int y = 0; y < m_height; ++y )
   {
      for ( int x = 0; x < m_width; ++x )
      {
         float maxDepth = 0;

         for ( const auto& layer : m_layers )
         {
            const Image& mask = layer.GetMask();
            if ( mask.Width() > 0 && x < mask.Width() && y < mask.Height() )
            {
               if ( mask( x, y, 0 ) > 0.1 )
                  maxDepth = Max( maxDepth, layer.GetDepth() );
            }
         }

         depthMap( x, y, 0 ) = maxDepth;
      }
   }

   return depthMap;
}

Image LayerStack::GetOcclusionMap() const
{
   Image occlusionMap;
   occlusionMap.AllocateData( m_width, m_height, 1, ColorSpace::Gray );

   for ( int y = 0; y < m_height; ++y )
   {
      for ( int x = 0; x < m_width; ++x )
      {
         double totalOcclusion = 0;

         for ( const auto& layer : m_layers )
         {
            const Image& alpha = layer.GetAlpha();
            const Image& mask = layer.GetMask();

            if ( mask.Width() > 0 && x < mask.Width() && y < mask.Height() )
            {
               if ( mask( x, y, 0 ) > 0.1 )
               {
                  double layerAlpha = 1.0;
                  if ( alpha.Width() > 0 )
                     layerAlpha = alpha( x, y, 0 );
                  totalOcclusion += layerAlpha * (1 - totalOcclusion);
               }
            }
         }

         occlusionMap( x, y, 0 ) = totalOcclusion;
      }
   }

   return occlusionMap;
}

std::vector<std::pair<size_t, size_t>> LayerStack::FindOverlaps() const
{
   std::vector<std::pair<size_t, size_t>> overlaps;

   for ( size_t i = 0; i < m_layers.size(); ++i )
   {
      for ( size_t j = i + 1; j < m_layers.size(); ++j )
      {
         const Image& maskI = m_layers[i].GetMask();
         const Image& maskJ = m_layers[j].GetMask();

         if ( maskI.Width() == 0 || maskJ.Width() == 0 )
            continue;

         // Check for overlap
         bool hasOverlap = false;
         for ( int y = 0; y < m_height && !hasOverlap; ++y )
         {
            for ( int x = 0; x < m_width && !hasOverlap; ++x )
            {
               if ( x < maskI.Width() && y < maskI.Height() &&
                    x < maskJ.Width() && y < maskJ.Height() )
               {
                  if ( maskI( x, y, 0 ) > 0.3 && maskJ( x, y, 0 ) > 0.3 )
                     hasOverlap = true;
               }
            }
         }

         if ( hasOverlap )
            overlaps.push_back( { i, j } );
      }
   }

   return overlaps;
}

// ----------------------------------------------------------------------------
// LayerDecomposer Implementation
// ----------------------------------------------------------------------------

LayerDecomposer::LayerDecomposer( const Config& config )
   : m_config( config )
{
}

LayerStack LayerDecomposer::Decompose( const Image& inputImage,
                                        const Image& classProbabilities,
                                        const Image& alphaMap,
                                        const Image& depthMap ) const
{
   int width = inputImage.Width();
   int height = inputImage.Height();
   int numClasses = classProbabilities.NumberOfChannels();

   LayerStack stack( width, height );

   // Create a layer for each detected class
   for ( int c = 0; c < numClasses; ++c )
   {
      RegionClass rc = static_cast<RegionClass>( c );

      // Create mask for this class
      Image mask;
      mask.AllocateData( width, height, 1, ColorSpace::Gray );
      Image alpha;
      alpha.AllocateData( width, height, 1, ColorSpace::Gray );
      Image intensity;
      intensity.AllocateData( width, height, 3, ColorSpace::RGB );

      bool hasPixels = false;
      float sumDepth = 0;
      int depthCount = 0;

      for ( int y = 0; y < height; ++y )
      {
         for ( int x = 0; x < width; ++x )
         {
            float prob = float(classProbabilities( x, y, c ));

            if ( prob > m_config.classThreshold )
            {
               mask( x, y, 0 ) = prob;

               float pixelAlpha = 1.0f;
               if ( alphaMap.Width() > 0 )
                  pixelAlpha = float(alphaMap( x, y, 0 ));
               alpha( x, y, 0 ) = pixelAlpha * prob;

               // Estimate intensity contribution
               for ( int ch = 0; ch < Min( 3, inputImage.NumberOfChannels() ); ++ch )
                  intensity( x, y, ch ) = inputImage( x, y, ch ) * prob;

               hasPixels = true;

               if ( depthMap.Width() > 0 )
               {
                  sumDepth += float(depthMap( x, y, 0 ));
                  depthCount++;
               }
            }
         }
      }

      if ( hasPixels )
      {
         Layer layer( rc );
         layer.SetMask( mask );
         layer.SetAlpha( alpha );
         layer.SetIntensity( intensity );

         float avgDepth = depthCount > 0 ? sumDepth / depthCount : 0.5f;
         layer.SetDepth( avgDepth );

         // Estimate PSF for star layers
         if ( m_config.estimatePSF && layer.IsStarLayer() )
         {
            // Find brightest point in mask
            float maxBrightness = 0;
            int starX = 0, starY = 0;

            for ( int y = 0; y < height; ++y )
            {
               for ( int x = 0; x < width; ++x )
               {
                  if ( mask( x, y, 0 ) > 0.5 )
                  {
                     float brightness = float(inputImage( x, y, 0 ));
                     if ( brightness > maxBrightness )
                     {
                        maxBrightness = brightness;
                        starX = x;
                        starY = y;
                     }
                  }
               }
            }

            if ( maxBrightness > 0.1 )
            {
               PSFParameters psf = PSFModel::FitToStar( inputImage,
                                                         float(starX), float(starY) );
               layer.SetPSF( psf );
            }
         }

         stack.AddLayer( std::move( layer ) );
      }
   }

   return stack;
}

LayerStack LayerDecomposer::DecomposeFromSegmentation( const Image& inputImage,
                                                        const Image& classMap ) const
{
   int width = inputImage.Width();
   int height = inputImage.Height();

   LayerStack stack( width, height );

   // Find unique classes in the map
   std::set<int> uniqueClasses;
   for ( int y = 0; y < height; ++y )
      for ( int x = 0; x < width; ++x )
         uniqueClasses.insert( int(classMap( x, y, 0 ) * 255) );

   // Create layer for each class
   for ( int classIdx : uniqueClasses )
   {
      if ( classIdx >= static_cast<int>( RegionClass::Count ) )
         continue;

      RegionClass rc = static_cast<RegionClass>( classIdx );

      Image mask;
      mask.AllocateData( width, height, 1, ColorSpace::Gray );
      Image intensity;
      intensity.AllocateData( width, height, 3, ColorSpace::RGB );

      for ( int y = 0; y < height; ++y )
      {
         for ( int x = 0; x < width; ++x )
         {
            int pixelClass = int(classMap( x, y, 0 ) * 255);
            if ( pixelClass == classIdx )
            {
               mask( x, y, 0 ) = 1.0;
               for ( int c = 0; c < Min( 3, inputImage.NumberOfChannels() ); ++c )
                  intensity( x, y, c ) = inputImage( x, y, c );
            }
         }
      }

      Layer layer( rc );
      layer.SetMask( mask );
      layer.SetIntensity( intensity );

      // Assign default depths based on class (depth = 0 is back, 1 is front)
      float depth = 0.5f;
      switch ( rc )
      {
      case RegionClass::Background:      depth = 0.0f; break;
      case RegionClass::DarkExtended:    depth = 0.15f; break;
      case RegionClass::BrightExtended:  depth = 0.4f; break;
      case RegionClass::StarHalo:        depth = 0.7f; break;
      case RegionClass::FaintCompact:    depth = 0.8f; break;
      case RegionClass::BrightCompact:   depth = 0.95f; break;
      case RegionClass::Artifact:        depth = 0.5f; break;
      default:                           depth = 0.5f; break;
      }
      layer.SetDepth( depth );

      stack.AddLayer( std::move( layer ) );
   }

   return stack;
}

std::vector<RegionClass> LayerDecomposer::FindClassesAtPixel(
   const Image& probs, int x, int y ) const
{
   std::vector<RegionClass> classes;

   for ( int c = 0; c < probs.NumberOfChannels(); ++c )
   {
      if ( probs( x, y, c ) > m_config.classThreshold )
      {
         classes.push_back( static_cast<RegionClass>( c ) );
      }
   }

   return classes;
}

// ----------------------------------------------------------------------------

} // namespace pcl
