//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "Segmentation.h"

#include <pcl/Console.h>
#include <pcl/File.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>

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

   // Create segmentation model
   if ( config.autoFallback )
   {
      m_model = SegmentationModelFactory::CreateWithFallback( config.modelConfig );
   }
   else
   {
      // Try ONNX first if path is specified
      if ( !config.modelConfig.modelPath.IsEmpty() )
      {
         m_model = SegmentationModelFactory::Create( SegmentationModelFactory::ModelType::ONNX );
         if ( m_model && !m_model->Initialize( config.modelConfig ) )
         {
            m_lastError = m_model->GetLastError();
            m_model.reset();
            return false;
         }
      }
      else
      {
         m_model = SegmentationModelFactory::Create( SegmentationModelFactory::ModelType::Mock );
         m_model->Initialize( config.modelConfig );
      }
   }

   if ( !m_model )
   {
      m_lastError = "Failed to create segmentation model";
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
   if ( maxDim <= m_config.maxProcessingDimension )
      return image;

   // Calculate scale factor
   scale = static_cast<double>( m_config.maxProcessingDimension ) / maxDim;
   int newWidth = static_cast<int>( image.Width() * scale );
   int newHeight = static_cast<int>( image.Height() * scale );

   // Create downsampled image
   Image downsampled( newWidth, newHeight, image.ColorSpace() );

   double invScale = 1.0 / scale;

   for ( int c = 0; c < image.NumberOfNominalChannels(); ++c )
   {
      for ( int y = 0; y < newHeight; ++y )
      {
         for ( int x = 0; x < newWidth; ++x )
         {
            // Bilinear sampling
            double srcX = x * invScale;
            double srcY = y * invScale;

            int x0 = static_cast<int>( srcX );
            int y0 = static_cast<int>( srcY );
            int x1 = std::min( x0 + 1, image.Width() - 1 );
            int y1 = std::min( y0 + 1, image.Height() - 1 );

            double fx = srcX - x0;
            double fy = srcY - y0;

            double value = (1 - fx) * (1 - fy) * image( x0, y0, c ) +
                           fx * (1 - fy) * image( x1, y0, c ) +
                           (1 - fx) * fy * image( x0, y1, c ) +
                           fx * fy * image( x1, y1, c );

            downsampled( x, y, c ) = value;
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
         uint64_t bits = *reinterpret_cast<uint64_t*>( &value );
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
      if ( hash == m_cachedImageHash && m_cachedResult.isValid )
      {
         ReportProgress( SegmentationEventType::Completed, 1.0, "Using cached result" );
         return m_cachedResult;
      }
      m_cachedImageHash = hash;
   }

   ReportProgress( SegmentationEventType::Started, 0.0, "Starting segmentation" );

   if ( !m_model )
   {
      result.errorMessage = "Segmentation model not initialized";
      ReportProgress( SegmentationEventType::Failed, 0.0, result.errorMessage );
      return result;
   }

   // Preprocess (downsample if needed)
   ReportProgress( SegmentationEventType::Preprocessing, 0.1, "Preprocessing image" );

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
      result.usedFallback = true;

      // Try fallback if this was ONNX
      if ( m_config.autoFallback && dynamic_cast<ONNXSegmentationModel*>( m_model.get() ) )
      {
         Console().WarningLn( "ONNX segmentation failed, trying mock fallback" );
         auto mockModel = std::make_unique<MockSegmentationModel>();
         mockModel->Initialize( m_config.modelConfig );

         result.segmentation = mockModel->Segment( processImage );
         result.modelUsed = mockModel->Name();
      }

      if ( !result.segmentation.isValid )
      {
         ReportProgress( SegmentationEventType::Failed, 0.0, result.errorMessage );
         return result;
      }
   }

   // Resize masks back to original size if we downsampled
   ReportProgress( SegmentationEventType::Postprocessing, 0.7, "Postprocessing masks" );

   if ( scale < 1.0 )
   {
      int origWidth = image.Width();
      int origHeight = image.Height();

      for ( auto& pair : result.segmentation.masks )
      {
         Image& mask = pair.second;
         Image resized( origWidth, origHeight, pcl::ColorSpace::Gray );

         double invScale = 1.0 / scale;

         for ( int y = 0; y < origHeight; ++y )
         {
            for ( int x = 0; x < origWidth; ++x )
            {
               double srcX = x * scale;
               double srcY = y * scale;

               int x0 = static_cast<int>( srcX );
               int y0 = static_cast<int>( srcY );
               int x1 = std::min( x0 + 1, mask.Width() - 1 );
               int y1 = std::min( y0 + 1, mask.Height() - 1 );

               double fx = srcX - x0;
               double fy = srcY - y0;

               double value = (1 - fx) * (1 - fy) * mask( x0, y0, 0 ) +
                              fx * (1 - fy) * mask( x1, y0, 0 ) +
                              (1 - fx) * fy * mask( x0, y1, 0 ) +
                              fx * fy * mask( x1, y1, 0 );

               resized( x, y, 0 ) = value;
            }
         }

         mask = std::move( resized );
      }

      // Also resize class map
      if ( result.segmentation.classMap.Width() > 0 )
      {
         Image& classMap = result.segmentation.classMap;
         Image resized( origWidth, origHeight, pcl::ColorSpace::Gray );

         for ( int y = 0; y < origHeight; ++y )
         {
            for ( int x = 0; x < origWidth; ++x )
            {
               int srcX = static_cast<int>( x * scale );
               int srcY = static_cast<int>( y * scale );
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
      m_cachedResult = result;
   }

   ReportProgress( SegmentationEventType::Completed, 1.0,
      String().Format( "Completed in %.1f ms", result.totalTimeMs ) );

   return result;
}

// ----------------------------------------------------------------------------

const SegmentationEngineResult* SegmentationEngine::GetCachedResult() const
{
   return m_cachedResult.isValid ? &m_cachedResult : nullptr;
}

// ----------------------------------------------------------------------------

void SegmentationEngine::ClearCache()
{
   m_cachedResult = SegmentationEngineResult();
   m_cachedImageHash = 0;
}

// ----------------------------------------------------------------------------

const Image* SegmentationEngine::GetMask( RegionClass rc ) const
{
   if ( !m_cachedResult.isValid )
      return nullptr;

   return m_cachedResult.segmentation.GetMask( rc );
}

// ----------------------------------------------------------------------------

std::map<RegionClass, Image> SegmentationEngine::GetAllMasks() const
{
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
   String homeDir;
   const char* home = std::getenv( "HOME" );
   if ( home != nullptr )
      homeDir = String( home );

   // Helper lambda to expand tilde in paths
   auto expandPath = [&homeDir]( const String& path ) -> String
   {
      if ( path.StartsWith( "~/" ) && !homeDir.IsEmpty() )
         return homeDir + path.Substring( 1 );
      return path;
   };

   // Common locations to search
   std::vector<String> searchPaths = {
      "./models",
      "../models",
      "~/.nukex/models",
      "/usr/share/nukex/models",
      "/usr/local/share/nukex/models"
   };

#ifdef __PI_MACOSX__
   searchPaths.push_back( "~/Library/Application Support/NukeX/models" );
#endif

#ifdef __PI_WINDOWS__
   searchPaths.push_back( "%APPDATA%/NukeX/models" );
#endif

   // Search for .onnx files
   Console console;
   console.WriteLn( String().Format( "Searching for ONNX models (HOME=%s)...", homeDir.c_str() ) );

   for ( const String& basePath : searchPaths )
   {
      String expandedPath = expandPath( basePath );
      String modelPath = expandedPath + "/nukex_segmentation.onnx";
      console.WriteLn( String().Format( "  Checking: %s", modelPath.c_str() ) );
      if ( File::Exists( modelPath ) )
      {
         console.WriteLn( String().Format( "  -> FOUND: %s", modelPath.c_str() ) );
         models.push_back( modelPath );
      }
   }

   if ( models.empty() )
      console.WarningLn( "No ONNX models found in search paths" );

   return models;
}

// ----------------------------------------------------------------------------
// SegmentationVisualizer Implementation
// ----------------------------------------------------------------------------

void SegmentationVisualizer::GetRegionColor( RegionClass rc, double& r, double& g, double& b )
{
   // Distinct colors for each region class (21 classes)
   switch ( rc )
   {
   case RegionClass::Background:
      r = 0.1; g = 0.1; b = 0.2;
      break;

   // Star classes (1-4) - yellow/orange tones
   case RegionClass::StarBright:
      r = 1.0; g = 1.0; b = 0.0;
      break;
   case RegionClass::StarMedium:
      r = 1.0; g = 0.8; b = 0.2;
      break;
   case RegionClass::StarFaint:
      r = 0.9; g = 0.7; b = 0.4;
      break;
   case RegionClass::StarSaturated:
      r = 1.0; g = 1.0; b = 1.0;
      break;

   // Nebula classes (5-8) - red/pink/purple tones
   case RegionClass::NebulaEmission:
      r = 1.0; g = 0.2; b = 0.4;
      break;
   case RegionClass::NebulaReflection:
      r = 0.4; g = 0.6; b = 1.0;
      break;
   case RegionClass::NebulaDark:
      r = 0.2; g = 0.1; b = 0.15;
      break;
   case RegionClass::NebulaPlanetary:
      r = 0.0; g = 0.8; b = 0.8;
      break;

   // Galaxy classes (9-12) - purple/orange tones
   case RegionClass::GalaxySpiral:
      r = 0.2; g = 0.6; b = 1.0;
      break;
   case RegionClass::GalaxyElliptical:
      r = 0.6; g = 0.4; b = 0.8;
      break;
   case RegionClass::GalaxyIrregular:
      r = 0.5; g = 0.5; b = 0.9;
      break;
   case RegionClass::GalaxyCore:
      r = 1.0; g = 0.6; b = 0.0;
      break;

   // Structural classes (13-15)
   case RegionClass::DustLane:
      r = 0.4; g = 0.2; b = 0.1;
      break;
   case RegionClass::StarClusterOpen:
      r = 0.8; g = 0.9; b = 0.3;
      break;
   case RegionClass::StarClusterGlobular:
      r = 0.9; g = 0.8; b = 0.2;
      break;

   // Artifact classes (16-20) - gray/muted tones
   case RegionClass::ArtifactHotPixel:
      r = 1.0; g = 0.0; b = 0.0;
      break;
   case RegionClass::ArtifactSatellite:
      r = 0.0; g = 1.0; b = 0.0;
      break;
   case RegionClass::ArtifactDiffraction:
      r = 0.8; g = 0.8; b = 0.0;
      break;
   case RegionClass::ArtifactGradient:
      r = 0.5; g = 0.3; b = 0.3;
      break;
   case RegionClass::ArtifactNoise:
      r = 0.4; g = 0.4; b = 0.4;
      break;

   default:
      r = 0.5; g = 0.5; b = 0.5;
      break;
   }
}

// ----------------------------------------------------------------------------

Image SegmentationVisualizer::CreateClassMapVisualization( const SegmentationResult& result )
{
   if ( !result.isValid || result.classMap.Width() == 0 )
      return Image();

   int width = result.classMap.Width();
   int height = result.classMap.Height();

   Image visual( width, height, pcl::ColorSpace::RGB );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         // Class is encoded as value/(numClasses-1)
         int classIdx = static_cast<int>( result.classMap( x, y, 0 ) * 8 + 0.5 );
         classIdx = std::max( 0, std::min( 8, classIdx ) );

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

} // namespace pcl
