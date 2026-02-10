//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "Segmentation.h"
#include "Constants.h"
#include "SegmentationPalette.h"

#include <pcl/Console.h>
#include <pcl/File.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace pcl
{

// ----------------------------------------------------------------------------
// SegmentationEngineResult Implementation
// ----------------------------------------------------------------------------

std::vector<std::pair<RegionClass, double>> SegmentationEngineResult::GetRegionsByCoverage() const
{
   auto coverage = segmentation.ComputeCoverage();

   std::vector<std::pair<RegionClass, double>> sorted;
   sorted.reserve( coverage.size() );

   for ( const auto& pair : coverage )
   {
      sorted.push_back( pair );
   }

   std::sort( sorted.begin(), sorted.end(),
              []( const auto& a, const auto& b ) { return a.second > b.second; } );

   return sorted;
}

// ----------------------------------------------------------------------------

RegionClass SegmentationEngineResult::GetDominantRegion() const
{
   auto sorted = GetRegionsByCoverage();

   // Skip background if there are other significant regions
   for ( const auto& pair : sorted )
   {
      if ( pair.first != RegionClass::Background && pair.second > 0.05 )
      {
         return pair.first;
      }
   }

   // Default to background if nothing else significant
   if ( !sorted.empty() )
   {
      return sorted[0].first;
   }

   return RegionClass::Background;
}

// ----------------------------------------------------------------------------
// SegmentationEngine Implementation
// ----------------------------------------------------------------------------

SegmentationEngine::SegmentationEngine()
{
}

// ----------------------------------------------------------------------------

SegmentationEngine::SegmentationEngine( const SegmentationEngineConfig& config )
{
   Initialize( config );
}

// ----------------------------------------------------------------------------

SegmentationEngine::~SegmentationEngine()
{
}

// ----------------------------------------------------------------------------

bool SegmentationEngine::Initialize( const SegmentationEngineConfig& config )
{
   m_config = config;

   // Create ONNX segmentation model (no fallback)
   m_model = SegmentationModelFactory::Create();
   if ( !m_model )
   {
      m_lastError = "ONNX runtime not available";
      return false;
   }

   if ( !m_model->Initialize( config.modelConfig ) )
   {
      m_lastError = m_model->GetLastError();
      m_model.reset();
      return false;
   }

   // Create region analyzer
   RegionAnalyzerConfig analyzerConfig;
   m_analyzer = std::make_unique<RegionAnalyzer>( analyzerConfig );
   ClearCache();
   return true;
}

// ----------------------------------------------------------------------------

void SegmentationEngine::ReportProgress( SegmentationEventType event,
                                          double progress, const String& message )
{
   if ( m_progressCallback )
   {
      m_progressCallback( event, progress, message );
   }
}

// ----------------------------------------------------------------------------

Image SegmentationEngine::PreprocessImage( const Image& image, double& scale ) const
{
   scale = 1.0;

   if ( !m_config.downsampleLargeImages )
      return image;

   int maxDim = std::max( image.Width(), image.Height() );

   // Determine effective max dimension based on adaptive settings
   int effectiveMaxDim = m_config.maxProcessingDimension;

   if ( m_config.useAdaptiveResolution )
   {
      // Use full resolution up to adaptive threshold
      if ( maxDim <= m_config.adaptiveThreshold )
      {
         Console().WriteLn( String().Format(
            "Segmentation: Using full resolution %dx%d (below adaptive threshold %d)",
            image.Width(), image.Height(), m_config.adaptiveThreshold ) );
         return image;
      }

      // Above threshold: scale proportionally, but respect maxProcessingDimension
      effectiveMaxDim = std::min( m_config.maxProcessingDimension,
                                   m_config.adaptiveThreshold );
      Console().WriteLn( String().Format(
         "Segmentation: Adaptive scaling - image %dx%d exceeds threshold %d, scaling to max %d",
         image.Width(), image.Height(), m_config.adaptiveThreshold, effectiveMaxDim ) );
   }

   if ( maxDim <= effectiveMaxDim )
      return image;

   // Calculate scale factor
   scale = static_cast<double>( effectiveMaxDim ) / maxDim;
   int newWidth = static_cast<int>( image.Width() * scale );
   int newHeight = static_cast<int>( image.Height() * scale );

   Console().WriteLn( String().Format(
      "Segmentation: Downsampling from %dx%d to %dx%d (scale=%.3f)",
      image.Width(), image.Height(), newWidth, newHeight, scale ) );

   // Create downsampled image using area averaging for better quality
   // (better than bilinear for downsampling - preserves small features)
   Image downsampled( newWidth, newHeight, image.ColorSpace() );

   double invScale = 1.0 / scale;
   double sampleRadius = invScale / 2.0;  // Half the downscale factor

   for ( int c = 0; c < image.NumberOfNominalChannels(); ++c )
   {
      for ( int y = 0; y < newHeight; ++y )
      {
         for ( int x = 0; x < newWidth; ++x )
         {
            // Area averaging: sample multiple pixels and average
            double srcCenterX = (x + 0.5) * invScale;
            double srcCenterY = (y + 0.5) * invScale;

            // Determine sampling area
            int x0 = std::max( 0, static_cast<int>( srcCenterX - sampleRadius ) );
            int x1 = std::min( image.Width() - 1, static_cast<int>( srcCenterX + sampleRadius ) );
            int y0 = std::max( 0, static_cast<int>( srcCenterY - sampleRadius ) );
            int y1 = std::min( image.Height() - 1, static_cast<int>( srcCenterY + sampleRadius ) );

            // Average all pixels in the sample area
            double sum = 0;
            int count = 0;
            for ( int sy = y0; sy <= y1; ++sy )
            {
               for ( int sx = x0; sx <= x1; ++sx )
               {
                  sum += image( sx, sy, c );
                  ++count;
               }
            }

            downsampled( x, y, c ) = (count > 0) ? (sum / count) : image( x0, y0, c );
         }
      }
   }

   return downsampled;
}

// ----------------------------------------------------------------------------

void SegmentationEngine::PostprocessMasks( SegmentationResult& result ) const
{
   for ( auto& pair : result.masks )
   {
      Image& mask = pair.second;

      // Apply erosion first (shrink)
      if ( m_config.maskErosionRadius > 0 )
      {
         mask = RegionMaskUtils::ErodeMask( mask,
            static_cast<int>( m_config.maskErosionRadius + 0.5 ) );
      }

      // Apply dilation (expand)
      if ( m_config.maskDilationRadius > 0 )
      {
         mask = RegionMaskUtils::DilateMask( mask,
            static_cast<int>( m_config.maskDilationRadius + 0.5 ) );
      }

      // Apply smoothing
      if ( m_config.maskSmoothingRadius > 0 )
      {
         mask = RegionMaskUtils::SmoothMask( mask, m_config.maskSmoothingRadius );
      }
   }

   // Remove masks with insufficient coverage
   if ( m_config.minMaskCoverage > 0 )
   {
      auto coverage = result.ComputeCoverage();
      std::vector<RegionClass> toRemove;

      for ( const auto& pair : coverage )
      {
         if ( pair.second < m_config.minMaskCoverage )
         {
            toRemove.push_back( pair.first );
         }
      }

      for ( RegionClass rc : toRemove )
      {
         result.masks.erase( rc );
      }
   }
}

// ----------------------------------------------------------------------------

void SegmentationEngine::AnalyzeRegions( const Image& image,
                                          SegmentationEngineResult& result ) const
{
   if ( !m_analyzer || !m_config.runAnalysis )
      return;

   auto analysisStart = std::chrono::high_resolution_clock::now();

   // Convert mask map to format expected by analyzer
   std::map<RegionClass, Image> maskMap;
   for ( const auto& pair : result.segmentation.masks )
   {
      maskMap[pair.first] = pair.second;
   }

   // Run analysis
   result.analysisResult = m_analyzer->Analyze( image, maskMap );

   // Copy region statistics
   result.regionStats = result.analysisResult.regionStats;

   auto analysisEnd = std::chrono::high_resolution_clock::now();
   result.analysisTimeMs = std::chrono::duration<double, std::milli>(
      analysisEnd - analysisStart ).count();
}

// ----------------------------------------------------------------------------

uint64_t SegmentationEngine::ComputeImageHash( const Image& image )
{
   // Simple hash based on image dimensions and sample pixels
   uint64_t hash = 0;
   hash ^= static_cast<uint64_t>( image.Width() ) << 32;
   hash ^= static_cast<uint64_t>( image.Height() ) << 16;
   hash ^= static_cast<uint64_t>( image.NumberOfNominalChannels() );

   // Sample some pixels
   int step = std::max( 1, std::min( image.Width(), image.Height() ) / 32 );

   for ( int y = 0; y < image.Height(); y += step )
   {
      for ( int x = 0; x < image.Width(); x += step )
      {
         double value = image( x, y, 0 );
         uint64_t bits;
         std::memcpy( &bits, &value, sizeof( bits ) );
         hash ^= bits + 0x9e3779b9 + (hash << 6) + (hash >> 2);
      }
   }

   return hash;
}

// ----------------------------------------------------------------------------

SegmentationEngineResult SegmentationEngine::Process( const Image& image )
{
   return Process( image, nullptr );
}

// ----------------------------------------------------------------------------

SegmentationEngineResult SegmentationEngine::Process( const Image& image,
                                                       SegmentationProgressCallback progressCallback )
{
   m_progressCallback = progressCallback;

   auto totalStart = std::chrono::high_resolution_clock::now();

   SegmentationEngineResult result;
   result.segmentation.width = image.Width();
   result.segmentation.height = image.Height();

   // Check cache
   if ( m_config.cacheResults )
   {
      uint64_t hash = ComputeImageHash( image );
      {
         std::lock_guard<std::mutex> lock( m_cacheMutex );
         if ( hash == m_cachedImageHash && m_cachedResult.isValid )
         {
            SegmentationEngineResult cachedCopy = m_cachedResult;
            ReportProgress( SegmentationEventType::Completed, 1.0, "Using cached result" );
            return cachedCopy;
         }
         m_cachedImageHash = hash;
      }
   }

   ReportProgress( SegmentationEventType::Started, 0.0, "Starting segmentation" );

   if ( !m_model )
   {
      result.errorMessage = "Segmentation model not initialized";
      ReportProgress( SegmentationEventType::Failed, 0.0, result.errorMessage );
      return result;
   }

   // Check if we should use tiled segmentation for very large images
   int maxDim = std::max( image.Width(), image.Height() );
   if ( m_config.useTiledSegmentation && maxDim > m_config.tiledSegmentationThreshold )
   {
      Console().WriteLn( String().Format(
         "Segmentation: Using tiled approach for large image %dx%d (threshold=%d)",
         image.Width(), image.Height(), m_config.tiledSegmentationThreshold ) );

      ReportProgress( SegmentationEventType::Preprocessing, 0.1, "Starting tiled segmentation" );

      auto segStart = std::chrono::high_resolution_clock::now();
      result.segmentation = ProcessTiled( image );
      auto segEnd = std::chrono::high_resolution_clock::now();

      result.segmentationTimeMs = std::chrono::duration<double, std::milli>(
         segEnd - segStart ).count();
      result.modelUsed = m_model->Name();

      if ( !result.segmentation.isValid )
      {
         ReportProgress( SegmentationEventType::Failed, 0.0, result.segmentation.errorMessage );
         return result;
      }

      // Skip the standard downsampling/upscaling path - tiled already produces full-res output
      RunPostprocessing( result, image, totalStart );
      return result;
   }

   // Preprocess (downsample if needed)
   ReportProgress( SegmentationEventType::Preprocessing, 0.1, "Preprocessing image" );

   {
   double scale;
   Image processImage = PreprocessImage( image, scale );

   // Run segmentation
   ReportProgress( SegmentationEventType::ModelRunning, 0.2, "Running segmentation model" );

   auto segStart = std::chrono::high_resolution_clock::now();
   result.segmentation = m_model->Segment( processImage );
   auto segEnd = std::chrono::high_resolution_clock::now();

   result.segmentationTimeMs = std::chrono::duration<double, std::milli>(
      segEnd - segStart ).count();
   result.modelUsed = m_model->Name();

   if ( !result.segmentation.isValid )
   {
      result.errorMessage = "Segmentation failed: " + result.segmentation.errorMessage;
      ReportProgress( SegmentationEventType::Failed, 0.0, result.errorMessage );
      return result;
   }

   // Resize masks back to original size if we downsampled
   ReportProgress( SegmentationEventType::Postprocessing, 0.7, "Postprocessing masks" );

   if ( scale < 1.0 )
   {
      int origWidth = image.Width();
      int origHeight = image.Height();

      Console().WriteLn( String().Format(
         "Segmentation: Upscaling masks from %dx%d to %dx%d",
         result.segmentation.masks.begin()->second.Width(),
         result.segmentation.masks.begin()->second.Height(),
         origWidth, origHeight ) );

      for ( auto& pair : result.segmentation.masks )
      {
         // Use edge-aware upscaling for better boundary preservation
         pair.second = UpscaleMaskEdgeAware( pair.second, origWidth, origHeight, scale );
      }

      // Also resize class map using nearest-neighbor (preserve class labels)
      if ( result.segmentation.classMap.Width() > 0 )
      {
         Image& classMap = result.segmentation.classMap;
         Image resized( origWidth, origHeight, pcl::ColorSpace::Gray );

         for ( int y = 0; y < origHeight; ++y )
         {
            for ( int x = 0; x < origWidth; ++x )
            {
               // Nearest-neighbor for class map (preserves class labels)
               int srcX = static_cast<int>( (x + 0.5) * scale );
               int srcY = static_cast<int>( (y + 0.5) * scale );
               srcX = std::min( srcX, classMap.Width() - 1 );
               srcY = std::min( srcY, classMap.Height() - 1 );

               resized( x, y, 0 ) = classMap( srcX, srcY, 0 );
            }
         }

         classMap = std::move( resized );
      }

      result.segmentation.width = origWidth;
      result.segmentation.height = origHeight;
   }

   } // End of standard (non-tiled) processing scope

   RunPostprocessing( result, image, totalStart );
   return result;
}

// ----------------------------------------------------------------------------

void SegmentationEngine::RunPostprocessing( SegmentationEngineResult& result,
                                             const Image& image,
                                             const std::chrono::high_resolution_clock::time_point& totalStart )
{
   // Apply mask postprocessing
   PostprocessMasks( result.segmentation );

   // Analyze regions
   ReportProgress( SegmentationEventType::AnalyzingRegions, 0.85, "Analyzing regions" );
   AnalyzeRegions( image, result );

   auto totalEnd = std::chrono::high_resolution_clock::now();
   result.totalTimeMs = std::chrono::duration<double, std::milli>(
      totalEnd - totalStart ).count();

   result.isValid = true;

   // Cache result
   if ( m_config.cacheResults )
   {
      std::lock_guard<std::mutex> lock( m_cacheMutex );
      m_cachedResult = result;
   }

   ReportProgress( SegmentationEventType::Completed, 1.0,
      String().Format( "Completed in %.1f ms", result.totalTimeMs ) );
}

// ----------------------------------------------------------------------------

std::optional<SegmentationEngineResult> SegmentationEngine::GetCachedResult() const
{
   std::lock_guard<std::mutex> lock( m_cacheMutex );
   if ( m_cachedResult.isValid )
      return m_cachedResult;  // Returns a copy under the lock
   return std::nullopt;
}

// ----------------------------------------------------------------------------

void SegmentationEngine::ClearCache()
{
   std::lock_guard<std::mutex> lock( m_cacheMutex );
   m_cachedResult = SegmentationEngineResult();
   m_cachedImageHash = 0;
}

// ----------------------------------------------------------------------------

std::optional<Image> SegmentationEngine::GetMask( RegionClass rc ) const
{
   std::lock_guard<std::mutex> lock( m_cacheMutex );
   if ( !m_cachedResult.isValid )
      return std::nullopt;

   const Image* mask = m_cachedResult.segmentation.GetMask( rc );
   if ( mask )
      return *mask;  // Returns a copy under the lock
   return std::nullopt;
}

// ----------------------------------------------------------------------------

std::map<RegionClass, Image> SegmentationEngine::GetAllMasks() const
{
   std::lock_guard<std::mutex> lock( m_cacheMutex );
   if ( !m_cachedResult.isValid )
      return {};

   return m_cachedResult.segmentation.masks;
}

// ----------------------------------------------------------------------------

String SegmentationEngine::GetModelName() const
{
   return m_model ? m_model->Name() : "None";
}

// ----------------------------------------------------------------------------

String SegmentationEngine::GetModelDescription() const
{
   return m_model ? m_model->Description() : "No model loaded";
}

// ----------------------------------------------------------------------------

bool SegmentationEngine::IsONNXAvailable()
{
   return SegmentationModelFactory::IsONNXAvailable();
}

// ----------------------------------------------------------------------------

std::vector<String> SegmentationEngine::FindAvailableModels()
{
   std::vector<String> models;

   // Get home directory for tilde expansion
   // Use IsoString (UTF-8) for environment variables and path operations
   // String (UTF-16) c_str() doesn't work with %s format specifier
   IsoString homeDir;
   const char* home = std::getenv( "HOME" );
   if ( home != nullptr )
      homeDir = IsoString( home );

   // Helper lambda to expand tilde in paths
   auto expandPath = [&homeDir]( const IsoString& path ) -> IsoString
   {
      if ( path.StartsWith( "~/" ) && !homeDir.IsEmpty() )
         return homeDir + path.Substring( 1 );
      return path;
   };

   // Common locations to search
   std::vector<IsoString> searchPaths = {
      "./models",
      "../models",
      "~/.nukex/models",
      "/usr/share/nukex/models",
      "/usr/local/share/nukex/models",
      // PixInsight installation directories
      "/opt/PixInsight/bin",
      "/opt/PixInsight/library"
   };

#ifdef __PI_MACOSX__
   searchPaths.push_back( "~/Library/Application Support/NukeX/models" );
   searchPaths.push_back( "/Applications/PixInsight/PixInsight.app/Contents/MacOS" );
#endif

#ifdef __PI_WINDOWS__
   searchPaths.push_back( "%APPDATA%/NukeX/models" );
   searchPaths.push_back( "C:/Program Files/PixInsight/bin" );
#endif

   // Search for .onnx files
   Console console;
   console.WriteLn( String().Format( "Searching for ONNX models (HOME=%s)...", homeDir.c_str() ) );

   for ( const IsoString& basePath : searchPaths )
   {
      IsoString expandedPath = expandPath( basePath );
      IsoString modelPath = expandedPath + "/nukex_segmentation.onnx";
      console.WriteLn( String().Format( "  Checking: %s", modelPath.c_str() ) );
      if ( File::Exists( String( modelPath ) ) )
      {
         console.WriteLn( String().Format( "  -> FOUND: %s", modelPath.c_str() ) );
         models.push_back( String( modelPath ) );
      }
   }

   if ( models.empty() )
      console.WarningLn( "No ONNX models found in search paths" );

   return models;
}

// ----------------------------------------------------------------------------

String SegmentationEngine::GetDefaultModelPath()
{
   // First check environment variable
   const char* envPath = std::getenv( "NUKEX_MODEL_PATH" );
   if ( envPath != nullptr )
   {
      String path( envPath );
      if ( File::Exists( path ) )
         return path;
   }

   // Search common locations
   auto models = FindAvailableModels();
   if ( !models.empty() )
      return models[0];

   // Last resort: check alongside the module in PixInsight bin
   String pixinsightPath = "/opt/PixInsight/bin/nukex_segmentation.onnx";
   if ( File::Exists( pixinsightPath ) )
      return pixinsightPath;

   // Return empty string if not found
   return String();
}

// ----------------------------------------------------------------------------
// SegmentationVisualizer Implementation
// ----------------------------------------------------------------------------

void SegmentationVisualizer::GetRegionColor( RegionClass rc, double& r, double& g, double& b )
{
   // Use the unified segmentation palette for consistent colors
   // between C++ and Python visualization code
   SegmentationPalette::GetColorForRegion( rc, r, g, b );
}

// ----------------------------------------------------------------------------

Image SegmentationVisualizer::CreateClassMapVisualization( const SegmentationResult& result )
{
   if ( !result.isValid || result.classMap.Width() == 0 )
      return Image();

   int width = result.classMap.Width();
   int height = result.classMap.Height();

   Image visual( width, height, pcl::ColorSpace::RGB );

   int numClasses = static_cast<int>( RegionClass::Count );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         // Class is encoded as value/(numClasses-1), decode with matching factor
         int classIdx = static_cast<int>( result.classMap( x, y, 0 ) * (numClasses - 1) + 0.5 );
         classIdx = std::max( 0, std::min( numClasses - 1, classIdx ) );

         RegionClass rc = static_cast<RegionClass>( classIdx );

         double r, g, b;
         GetRegionColor( rc, r, g, b );

         visual( x, y, 0 ) = r;
         visual( x, y, 1 ) = g;
         visual( x, y, 2 ) = b;
      }
   }

   return visual;
}

// ----------------------------------------------------------------------------

Image SegmentationVisualizer::CreateMaskOverlay( const Image& original,
                                                   const SegmentationResult& result,
                                                   double opacity )
{
   if ( !result.isValid || original.Width() == 0 )
      return original;

   int width = original.Width();
   int height = original.Height();
   bool isColor = original.NumberOfNominalChannels() >= 3;

   Image overlay( width, height, pcl::ColorSpace::RGB );

   // Copy original (convert to RGB if grayscale)
   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         if ( isColor )
         {
            overlay( x, y, 0 ) = original( x, y, 0 );
            overlay( x, y, 1 ) = original( x, y, 1 );
            overlay( x, y, 2 ) = original( x, y, 2 );
         }
         else
         {
            double v = original( x, y, 0 );
            overlay( x, y, 0 ) = v;
            overlay( x, y, 1 ) = v;
            overlay( x, y, 2 ) = v;
         }
      }
   }

   // Blend region colors
   for ( const auto& pair : result.masks )
   {
      if ( pair.first == RegionClass::Background )
         continue;  // Skip background overlay

      const Image& mask = pair.second;
      if ( mask.Width() != width || mask.Height() != height )
         continue;

      double colorR, colorG, colorB;
      GetRegionColor( pair.first, colorR, colorG, colorB );

      for ( int y = 0; y < height; ++y )
      {
         for ( int x = 0; x < width; ++x )
         {
            double maskValue = mask( x, y, 0 ) * opacity;
            if ( maskValue < 0.01 )
               continue;

            overlay( x, y, 0 ) = overlay( x, y, 0 ) * (1 - maskValue) + colorR * maskValue;
            overlay( x, y, 1 ) = overlay( x, y, 1 ) * (1 - maskValue) + colorG * maskValue;
            overlay( x, y, 2 ) = overlay( x, y, 2 ) * (1 - maskValue) + colorB * maskValue;
         }
      }
   }

   return overlay;
}

// ----------------------------------------------------------------------------

Image SegmentationVisualizer::CreateRegionVisualization( const Image& original,
                                                          const Image& mask,
                                                          RegionClass rc,
                                                          double opacity )
{
   if ( original.Width() == 0 || mask.Width() == 0 )
      return original;

   int width = original.Width();
   int height = original.Height();
   bool isColor = original.NumberOfNominalChannels() >= 3;

   Image visual( width, height, pcl::ColorSpace::RGB );

   double colorR, colorG, colorB;
   GetRegionColor( rc, colorR, colorG, colorB );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double origR, origG, origB;

         if ( isColor )
         {
            origR = original( x, y, 0 );
            origG = original( x, y, 1 );
            origB = original( x, y, 2 );
         }
         else
         {
            origR = origG = origB = original( x, y, 0 );
         }

         // Get mask value, handling size mismatch
         double maskValue = 0;
         if ( x < mask.Width() && y < mask.Height() )
         {
            maskValue = mask( x, y, 0 ) * opacity;
         }

         visual( x, y, 0 ) = origR * (1 - maskValue) + colorR * maskValue;
         visual( x, y, 1 ) = origG * (1 - maskValue) + colorG * maskValue;
         visual( x, y, 2 ) = origB * (1 - maskValue) + colorB * maskValue;
      }
   }

   return visual;
}

// ----------------------------------------------------------------------------

Image SegmentationVisualizer::CreateLegend( int width, int height )
{
   Image legend( width, height, pcl::ColorSpace::RGB );
   legend.White();

   int numClasses = static_cast<int>( RegionClass::Count );
   int boxHeight = height / numClasses;
   int boxWidth = width / 4;
   int margin = 5;

   for ( int i = 0; i < numClasses; ++i )
   {
      RegionClass rc = static_cast<RegionClass>( i );

      double r, g, b;
      GetRegionColor( rc, r, g, b );

      int y0 = i * boxHeight + margin;
      int y1 = (i + 1) * boxHeight - margin;

      // Draw color box
      for ( int y = y0; y < y1 && y < height; ++y )
      {
         for ( int x = margin; x < boxWidth - margin && x < width; ++x )
         {
            legend( x, y, 0 ) = r;
            legend( x, y, 1 ) = g;
            legend( x, y, 2 ) = b;
         }
      }

      // Note: Text rendering would require font support
      // For now, the color boxes serve as the legend
   }

   return legend;
}

// ----------------------------------------------------------------------------
// Tiled Segmentation Implementation
// ----------------------------------------------------------------------------

SegmentationResult SegmentationEngine::ProcessTiled( const Image& image )
{
   SegmentationResult result;
   result.width = image.Width();
   result.height = image.Height();

   int imageWidth = image.Width();
   int imageHeight = image.Height();
   int tileSize = m_config.tileSize;
   int overlap = m_config.tileOverlap;

   // Calculate number of tiles needed
   int effectiveTileSize = tileSize - overlap;
   if ( effectiveTileSize <= 0 )
   {
      result.errorMessage = "Tile overlap must be less than tile size";
      result.isValid = false;
      return result;
   }
   int numTilesX = (imageWidth + effectiveTileSize - 1) / effectiveTileSize;
   int numTilesY = (imageHeight + effectiveTileSize - 1) / effectiveTileSize;

   Console().WriteLn( String().Format(
      "Segmentation: Processing %dx%d tiles (size=%d, overlap=%d)",
      numTilesX, numTilesY, tileSize, overlap ) );

   // Initialize result masks
   for ( int i = 0; i < static_cast<int>( RegionClass::Count ); ++i )
   {
      RegionClass rc = static_cast<RegionClass>( i );
      result.masks[rc].AllocateData( imageWidth, imageHeight, 1, ColorSpace::Gray );
      result.masks[rc].Zero();
   }
   result.classMap.AllocateData( imageWidth, imageHeight, 1, ColorSpace::Gray );
   result.classMap.Zero();

   // Weight map for blending (accumulates contribution counts)
   Image weightMap( imageWidth, imageHeight, ColorSpace::Gray );
   weightMap.Zero();

   int totalTiles = numTilesX * numTilesY;
   int processedTiles = 0;

   // TODO: Batch tile inference optimization
   // Currently each tile is processed individually via m_model->Segment().
   // For ONNX models with dynamic batch support, collect tiles into batches
   // of BATCH_SIZE (e.g., 4) and use ONNXSession::RunBatch() for significantly
   // faster throughput. This requires splitting Segment() into separate
   // preprocess/inference/postprocess steps so that only the inference step
   // is batched. See ONNXSession::RunBatch() in ONNXInference.h.

   // Process each tile
   for ( int tileY = 0; tileY < numTilesY; ++tileY )
   {
      for ( int tileX = 0; tileX < numTilesX; ++tileX )
      {
         // Calculate tile boundaries with overlap
         int x0 = tileX * effectiveTileSize;
         int y0 = tileY * effectiveTileSize;
         int x1 = std::min( x0 + tileSize, imageWidth );
         int y1 = std::min( y0 + tileSize, imageHeight );
         int tileWidth = x1 - x0;
         int tileHeight = y1 - y0;

         // Extract tile from image
         Image tile( tileWidth, tileHeight, image.ColorSpace() );
         for ( int c = 0; c < image.NumberOfNominalChannels(); ++c )
         {
            for ( int y = 0; y < tileHeight; ++y )
            {
               for ( int x = 0; x < tileWidth; ++x )
               {
                  tile( x, y, c ) = image( x0 + x, y0 + y, c );
               }
            }
         }

         // Progress report
         processedTiles++;
         double progress = 0.2 + 0.6 * processedTiles / totalTiles;
         ReportProgress( SegmentationEventType::ModelRunning, progress,
            String().Format( "Processing tile %d/%d", processedTiles, totalTiles ) );

         // Run segmentation on tile
         SegmentationResult tileResult = m_model->Segment( tile );

         if ( !tileResult.isValid )
         {
            Console().WarningLn( String().Format(
               "Segmentation: Tile %d,%d failed: %s",
               tileX, tileY, IsoString( tileResult.errorMessage ).c_str() ) );
            continue;
         }

         // Calculate overlap regions for blending
         int overlapLeft = (tileX > 0) ? overlap / 2 : 0;
         int overlapTop = (tileY > 0) ? overlap / 2 : 0;
         int overlapRight = (tileX < numTilesX - 1) ? overlap / 2 : 0;
         int overlapBottom = (tileY < numTilesY - 1) ? overlap / 2 : 0;

         // Merge tile result into full result with blending
         MergeTileResult( result, tileResult, x0, y0, tileWidth, tileHeight,
                          overlapLeft, overlapTop, overlapRight, overlapBottom );

         // Update weight map
         for ( int y = 0; y < tileHeight; ++y )
         {
            for ( int x = 0; x < tileWidth; ++x )
            {
               // Calculate blend weight (ramp down at edges)
               double wx = 1.0;
               double wy = 1.0;

               if ( overlapLeft > 0 && x < overlapLeft )
                  wx = static_cast<double>( x ) / overlapLeft;
               else if ( overlapRight > 0 && x >= tileWidth - overlapRight )
                  wx = static_cast<double>( tileWidth - 1 - x ) / overlapRight;

               if ( overlapTop > 0 && y < overlapTop )
                  wy = static_cast<double>( y ) / overlapTop;
               else if ( overlapBottom > 0 && y >= tileHeight - overlapBottom )
                  wy = static_cast<double>( tileHeight - 1 - y ) / overlapBottom;

               double weight = wx * wy;

               int px = x0 + x;
               int py = y0 + y;
               if ( px < imageWidth && py < imageHeight )
               {
                  weightMap( px, py, 0 ) += weight;
               }
            }
         }
      }
   }

   // Normalize masks by weight map
   for ( auto& pair : result.masks )
   {
      Image& mask = pair.second;
      for ( int y = 0; y < imageHeight; ++y )
      {
         for ( int x = 0; x < imageWidth; ++x )
         {
            double w = weightMap( x, y, 0 );
            if ( w > 0 )
               mask( x, y, 0 ) /= w;
         }
      }
   }

   // Recompute class map from normalized masks
   for ( int y = 0; y < imageHeight; ++y )
   {
      for ( int x = 0; x < imageWidth; ++x )
      {
         double maxProb = 0;
         int maxClass = 0;

         int classIdx = 0;
         for ( const auto& pair : result.masks )
         {
            double prob = pair.second( x, y, 0 );
            if ( prob > maxProb )
            {
               maxProb = prob;
               maxClass = static_cast<int>( pair.first );
            }
            classIdx++;
         }

         result.classMap( x, y, 0 ) = static_cast<double>( maxClass ) / static_cast<double>( static_cast<int>( RegionClass::Count ) - 1 );
      }
   }

   result.isValid = true;
   Console().WriteLn( String().Format(
      "Segmentation: Tiled processing complete, processed %d tiles", totalTiles ) );

   return result;
}

// ----------------------------------------------------------------------------

void SegmentationEngine::MergeTileResult( SegmentationResult& target,
                                           const SegmentationResult& tile,
                                           int tileX, int tileY,
                                           int tileWidth, int tileHeight,
                                           int overlapLeft, int overlapTop,
                                           int overlapRight, int overlapBottom )
{
   // Merge each mask from tile into target with blending weights
   for ( const auto& pair : tile.masks )
   {
      RegionClass rc = pair.first;
      const Image& tileMask = pair.second;

      auto it = target.masks.find( rc );
      if ( it == target.masks.end() )
         continue;

      Image& targetMask = it->second;

      for ( int y = 0; y < tileHeight && y < tileMask.Height(); ++y )
      {
         for ( int x = 0; x < tileWidth && x < tileMask.Width(); ++x )
         {
            int px = tileX + x;
            int py = tileY + y;

            if ( px >= target.width || py >= target.height )
               continue;

            // Calculate blend weight (linear ramp in overlap regions)
            double wx = 1.0;
            double wy = 1.0;

            if ( overlapLeft > 0 && x < overlapLeft )
               wx = static_cast<double>( x ) / overlapLeft;
            else if ( overlapRight > 0 && x >= tileWidth - overlapRight )
               wx = static_cast<double>( tileWidth - 1 - x ) / overlapRight;

            if ( overlapTop > 0 && y < overlapTop )
               wy = static_cast<double>( y ) / overlapTop;
            else if ( overlapBottom > 0 && y >= tileHeight - overlapBottom )
               wy = static_cast<double>( tileHeight - 1 - y ) / overlapBottom;

            double weight = wx * wy;
            double tileValue = tileMask( x, y, 0 );

            // Accumulate weighted value
            targetMask( px, py, 0 ) += tileValue * weight;
         }
      }
   }
}

// ----------------------------------------------------------------------------

Image SegmentationEngine::UpscaleMaskEdgeAware( const Image& mask,
                                                  int targetWidth, int targetHeight,
                                                  double scale ) const
{
   Image resized( targetWidth, targetHeight, pcl::ColorSpace::Gray );

   // Edge-aware upscaling: use bilinear interpolation but preserve
   // sharp transitions by detecting edges and using nearest-neighbor there

   // First pass: compute gradient magnitude in source mask
   Image gradient( mask.Width(), mask.Height(), pcl::ColorSpace::Gray );

   for ( int y = 1; y < mask.Height() - 1; ++y )
   {
      for ( int x = 1; x < mask.Width() - 1; ++x )
      {
         // Sobel gradient
         double gx = -mask( x - 1, y - 1, 0 ) - 2 * mask( x - 1, y, 0 ) - mask( x - 1, y + 1, 0 )
                     +mask( x + 1, y - 1, 0 ) + 2 * mask( x + 1, y, 0 ) + mask( x + 1, y + 1, 0 );
         double gy = -mask( x - 1, y - 1, 0 ) - 2 * mask( x, y - 1, 0 ) - mask( x + 1, y - 1, 0 )
                     +mask( x - 1, y + 1, 0 ) + 2 * mask( x, y + 1, 0 ) + mask( x + 1, y + 1, 0 );
         gradient( x, y, 0 ) = std::sqrt( gx * gx + gy * gy );
      }
   }

   // Edge threshold (high gradient = edge)
   const double edgeThreshold = 0.3;

   // Second pass: upscale with edge-awareness
   for ( int y = 0; y < targetHeight; ++y )
   {
      for ( int x = 0; x < targetWidth; ++x )
      {
         double srcX = x * scale;
         double srcY = y * scale;

         int x0 = static_cast<int>( srcX );
         int y0 = static_cast<int>( srcY );
         int x1 = std::min( x0 + 1, mask.Width() - 1 );
         int y1 = std::min( y0 + 1, mask.Height() - 1 );

         // Check if we're near an edge in the source
         double gradVal = 0;
         if ( x0 > 0 && y0 > 0 && x0 < mask.Width() - 1 && y0 < mask.Height() - 1 )
         {
            gradVal = gradient( x0, y0, 0 );
         }

         double value;
         if ( gradVal > edgeThreshold )
         {
            // Near edge: use nearest-neighbor to preserve sharp boundaries
            int nearX = static_cast<int>( srcX + 0.5 );
            int nearY = static_cast<int>( srcY + 0.5 );
            nearX = std::min( nearX, mask.Width() - 1 );
            nearY = std::min( nearY, mask.Height() - 1 );
            value = mask( nearX, nearY, 0 );
         }
         else
         {
            // Smooth region: use bilinear interpolation
            double fx = srcX - x0;
            double fy = srcY - y0;

            value = Interpolation::BilinearInterpolate(
                       mask( x0, y0, 0 ), mask( x1, y0, 0 ),
                       mask( x0, y1, 0 ), mask( x1, y1, 0 ), fx, fy );
         }

         resized( x, y, 0 ) = value;
      }
   }

   return resized;
}

// ----------------------------------------------------------------------------

} // namespace pcl
