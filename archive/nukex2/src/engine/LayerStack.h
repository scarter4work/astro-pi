//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Layer Stack - Multi-layer image decomposition

#ifndef __LayerStack_h
#define __LayerStack_h

#include "RegionStatistics.h"
#include "PSFModel.h"

#include <pcl/Image.h>
#include <pcl/String.h>

#include <vector>
#include <memory>
#include <optional>

namespace pcl
{

// ----------------------------------------------------------------------------
// Layer - Single decomposed image layer
// ----------------------------------------------------------------------------

class Layer
{
public:

   Layer() = default;
   Layer( RegionClass regionClass, float depth = 0.0f );

   // Layer properties
   RegionClass GetRegionClass() const { return m_regionClass; }
   void SetRegionClass( RegionClass rc ) { m_regionClass = rc; }

   float GetDepth() const { return m_depth; }
   void SetDepth( float d ) { m_depth = d; }

   String GetName() const { return m_name; }
   void SetName( const String& name ) { m_name = name; }

   // Mask operations (spatial extent of this layer)
   const Image& GetMask() const { return m_mask; }
   Image& GetMask() { return m_mask; }
   void SetMask( const Image& mask ) { m_mask = mask; }

   // Alpha/opacity map (per-pixel opacity)
   const Image& GetAlpha() const { return m_alpha; }
   Image& GetAlpha() { return m_alpha; }
   void SetAlpha( const Image& alpha ) { m_alpha = alpha; }

   // Intensity contribution (RGB values this layer contributes)
   const Image& GetIntensity() const { return m_intensity; }
   Image& GetIntensity() { return m_intensity; }
   void SetIntensity( const Image& intensity ) { m_intensity = intensity; }

   // PSF parameters (for star layers)
   bool HasPSF() const { return m_psf.has_value(); }
   const PSFParameters& GetPSF() const { return m_psf.value(); }
   void SetPSF( const PSFParameters& psf ) { m_psf = psf; }
   void ClearPSF() { m_psf.reset(); }

   // Utility methods
   bool IsStarLayer() const;
   bool IsNebulaLayer() const;
   bool IsBackground() const;

   // Coverage statistics
   double GetCoverage() const;  // Fraction of image covered by this layer
   double GetMeanOpacity() const;  // Average opacity where mask > 0

private:

   RegionClass m_regionClass = RegionClass::Background;
   float m_depth = 0.0f;  // 0 = back, 1 = front
   String m_name;

   Image m_mask;       // Binary/soft mask showing where layer exists
   Image m_alpha;      // Per-pixel opacity (0-1)
   Image m_intensity;  // RGB intensity contribution

   std::optional<PSFParameters> m_psf;  // PSF params for star layers
};

// ----------------------------------------------------------------------------
// LayerStack - Collection of layers forming a complete image decomposition
// ----------------------------------------------------------------------------

class LayerStack
{
public:

   LayerStack() = default;
   LayerStack( int width, int height );

   // Dimensions
   int Width() const { return m_width; }
   int Height() const { return m_height; }
   void SetDimensions( int width, int height );

   // Layer management
   size_t NumLayers() const { return m_layers.size(); }
   bool IsEmpty() const { return m_layers.empty(); }

   void AddLayer( const Layer& layer );
   void AddLayer( Layer&& layer );
   void InsertLayer( size_t index, const Layer& layer );
   void RemoveLayer( size_t index );
   void Clear();

   // Layer access (sorted by depth, back to front)
   Layer& operator[]( size_t index ) { return m_layers[index]; }
   const Layer& operator[]( size_t index ) const { return m_layers[index]; }

   Layer* GetLayerByClass( RegionClass rc );
   const Layer* GetLayerByClass( RegionClass rc ) const;

   std::vector<Layer*> GetLayersAtPixel( int x, int y, float threshold = 0.1f );

   // Iteration
   auto begin() { return m_layers.begin(); }
   auto end() { return m_layers.end(); }
   auto begin() const { return m_layers.begin(); }
   auto end() const { return m_layers.end(); }

   // Sorting
   void SortByDepth();  // Sort layers back-to-front

   // Compositing - render layers to single image
   Image Composite() const;
   Image CompositeWithout( size_t excludeLayerIndex ) const;
   Image CompositeRange( size_t startLayer, size_t endLayer ) const;

   // Layer separation operations
   Image RevealBehind( size_t frontLayerIndex ) const;
   Image SubtractLayer( size_t layerIndex ) const;
   Image SubtractPSF( size_t starLayerIndex ) const;

   // Reconstruct what's behind a star halo
   Image RecoverOccludedRegion( size_t starLayerIndex,
                                 float recoveryStrength = 1.0f ) const;

   // Analysis
   Image GetDepthMap() const;       // Depth at each pixel
   Image GetOcclusionMap() const;   // Total occlusion at each pixel
   std::vector<std::pair<size_t, size_t>> FindOverlaps() const;  // Which layers overlap

   // Serialization
   void SaveToDirectory( const String& path ) const;
   static LayerStack LoadFromDirectory( const String& path );

private:

   int m_width = 0;
   int m_height = 0;
   std::vector<Layer> m_layers;

   // Internal compositing helper
   void CompositePixel( int x, int y,
                        const std::vector<const Layer*>& layers,
                        double& r, double& g, double& b ) const;
};

// ----------------------------------------------------------------------------
// LayerDecomposer - Decompose an image into layers using model predictions
// ----------------------------------------------------------------------------

class LayerDecomposer
{
public:

   struct Config
   {
      float classThreshold = 0.3f;      // Min probability to include class
      float alphaThreshold = 0.1f;      // Min opacity to include pixel
      float depthQuantization = 0.1f;   // Depth resolution for layer grouping
      bool detectOverlaps = true;       // Look for multi-class pixels
      bool estimatePSF = true;          // Compute PSF for star layers
   };

   LayerDecomposer() = default;
   explicit LayerDecomposer( const Config& config );

   // Decompose image using model predictions
   LayerStack Decompose( const Image& inputImage,
                         const Image& classProbabilities,  // H x W x 18
                         const Image& alphaMap,            // H x W x 1
                         const Image& depthMap ) const;    // H x W x 1

   // Simplified decomposition from single-label segmentation
   LayerStack DecomposeFromSegmentation( const Image& inputImage,
                                          const Image& classMap ) const;

   // Configuration
   const Config& GetConfig() const { return m_config; }
   void SetConfig( const Config& config ) { m_config = config; }

private:

   Config m_config;

   // Internal methods
   std::vector<RegionClass> FindClassesAtPixel( const Image& probs,
                                                  int x, int y ) const;
   float EstimateLocalAlpha( const Image& input,
                              const Image& probs,
                              int x, int y,
                              RegionClass foreground,
                              RegionClass background ) const;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __LayerStack_h
