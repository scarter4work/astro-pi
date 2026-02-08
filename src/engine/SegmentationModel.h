//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Segmentation Model Interface

#ifndef __SegmentationModel_h
#define __SegmentationModel_h

#include "RegionStatistics.h"
#include "ONNXInference.h"

#include <pcl/Image.h>
#include <pcl/String.h>

#include <memory>
#include <vector>
#include <map>

namespace pcl
{

// ----------------------------------------------------------------------------
// Segmentation Result
// ----------------------------------------------------------------------------

struct SegmentationResult
{
   // Per-region probability masks (soft masks, 0-1)
   // Key is region class, value is single-channel mask image
   std::map<RegionClass, Image> masks;

   // Argmax class map (each pixel assigned to one class)
   Image classMap;

   // Original image dimensions
   int width = 0;
   int height = 0;

   // Processing metadata
   double processingTimeMs = 0;
   bool isValid = false;
   String errorMessage = String();  // Must be initialized for PCL ABI

   // Get mask for a specific region class
   const Image* GetMask( RegionClass rc ) const
   {
      auto it = masks.find( rc );
      return (it != masks.end()) ? &it->second : nullptr;
   }

   // Get total coverage for each region
   std::map<RegionClass, double> ComputeCoverage() const;

   // Clear all masks
   void Clear()
   {
      masks.clear();
      classMap = Image();
      isValid = false;
   }
};

// ----------------------------------------------------------------------------
// Segmentation Model Configuration
// ----------------------------------------------------------------------------

struct SegmentationConfig
{
   // Model settings
   String modelPath;                    // Path to ONNX model file
   int inputWidth = 512;                // Model input width
   int inputHeight = 512;               // Model input height
   bool useGPU = false;                 // Use GPU acceleration

   // Determinism
   bool deterministic = false;          // Force deterministic inference

   // Processing settings
   bool applySoftmax = true;            // Apply softmax to model output
   double probabilityThreshold = 0.1;   // Min probability to include in mask
   double smoothingRadius = 0.0;        // Gaussian smoothing on masks

   // Region mapping
   // Maps model output channel index to region class (matches training)
   std::vector<RegionClass> channelMapping = {
      RegionClass::Background,      // 0
      RegionClass::BrightCompact,   // 1
      RegionClass::FaintCompact,    // 2
      RegionClass::BrightExtended,  // 3
      RegionClass::DarkExtended,    // 4
      RegionClass::Artifact,        // 5
      RegionClass::StarHalo         // 6
   };
};

// ----------------------------------------------------------------------------
// Segmentation Model Interface
//
// Abstract base class for all segmentation models.
// Implementations include:
// - ONNXSegmentationModel: ONNX Runtime-based neural network
// ----------------------------------------------------------------------------

class ISegmentationModel
{
public:

   virtual ~ISegmentationModel() = default;

   // Get model name/description
   virtual String Name() const = 0;
   virtual String Description() const = 0;

   // Check if model is ready
   virtual bool IsReady() const = 0;

   // Initialize model (load weights, etc.)
   virtual bool Initialize( const SegmentationConfig& config ) = 0;

   // Run segmentation on an image
   virtual SegmentationResult Segment( const Image& image ) = 0;

   // Get last error message
   virtual String GetLastError() const = 0;

   // Get expected input dimensions
   virtual int GetInputWidth() const = 0;
   virtual int GetInputHeight() const = 0;

   // Get number of output classes
   virtual int GetNumClasses() const = 0;
};

// ----------------------------------------------------------------------------
// ONNX Segmentation Model
//
// Neural network-based segmentation using ONNX Runtime.
// Supports various model architectures (U-Net, DeepLab, SAM, etc.)
// ----------------------------------------------------------------------------

class ONNXSegmentationModel : public ISegmentationModel
{
public:

   ONNXSegmentationModel();
   ~ONNXSegmentationModel();

   String Name() const override { return "ONNXSegmentation"; }
   String Description() const override
   {
      return "Neural network segmentation via ONNX Runtime";
   }

   bool IsReady() const override { return m_isReady; }
   bool Initialize( const SegmentationConfig& config ) override;

   SegmentationResult Segment( const Image& image ) override;

   String GetLastError() const override { return m_lastError; }

   int GetInputWidth() const override { return m_config.inputWidth; }
   int GetInputHeight() const override { return m_config.inputHeight; }
   int GetNumClasses() const override { return m_numClasses; }

private:

   SegmentationConfig m_config;
   String m_lastError;
   bool m_isReady = false;
   int m_numClasses = 7;
   int m_numInputChannels = 3;  // Detected from model (3=RGB)

   std::unique_ptr<ONNXSession> m_session;

   // Preprocessing
   FloatTensor PreprocessImage( const Image& image ) const;

   // Postprocessing
   SegmentationResult PostprocessOutput( const FloatTensor& output,
                                          int originalWidth, int originalHeight ) const;
};

// ----------------------------------------------------------------------------
// Segmentation Model Factory
// ----------------------------------------------------------------------------

class SegmentationModelFactory
{
public:

   // Create an ONNX segmentation model
   static std::unique_ptr<ISegmentationModel> Create();

   // Check if ONNX runtime is available
   static bool IsONNXAvailable();
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __SegmentationModel_h
