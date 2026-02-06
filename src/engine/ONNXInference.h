//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// ONNX Runtime Wrapper

#ifndef __ONNXInference_h
#define __ONNXInference_h

#include <pcl/String.h>
#include <pcl/Console.h>
#include <pcl/Image.h>

#include <vector>
#include <memory>
#include <string>

// Forward declarations for ONNX Runtime types
// This allows compilation without ONNX Runtime headers in client code
namespace Ort
{
   struct Env;
   struct Session;
   struct SessionOptions;
   struct MemoryInfo;
   struct Value;
}

namespace pcl
{

// ----------------------------------------------------------------------------
// ONNX Inference Configuration
// ----------------------------------------------------------------------------

struct ONNXConfig
{
   bool useGPU = false;               // Use CUDA execution provider
   int gpuDeviceId = 0;               // GPU device ID if using CUDA
   int numThreads = 0;                // 0 = auto-detect
   bool enableMemoryPattern = true;   // Memory optimization
   bool enableCpuMemArena = true;     // CPU memory arena
   int graphOptimizationLevel = 99;   // ORT_ENABLE_ALL = 99
   String logId = "NukeX";            // Session log identifier
};

// ----------------------------------------------------------------------------
// Tensor Shape
// ----------------------------------------------------------------------------

struct TensorShape
{
   std::vector<int64_t> dims;

   TensorShape() = default;
   TensorShape( std::initializer_list<int64_t> d ) : dims( d ) {}
   TensorShape( const std::vector<int64_t>& d ) : dims( d ) {}

   int64_t NumElements() const
   {
      if ( dims.empty() )
         return 0;

      int64_t n = 1;
      for ( int64_t d : dims )
      {
         // Validate dimension is positive and reasonable
         if ( d <= 0 || d > 1000000000 )
            return 0;

         // Check for overflow before multiplying
         if ( n > std::numeric_limits<int64_t>::max() / d )
            return 0;

         n *= d;
      }
      return n;
   }

   size_t Rank() const { return dims.size(); }

   int64_t operator[]( size_t i ) const { return dims[i]; }
   int64_t& operator[]( size_t i ) { return dims[i]; }

   String ToString() const
   {
      String s = "[";
      for ( size_t i = 0; i < dims.size(); ++i )
      {
         if ( i > 0 ) s += ", ";
         s += String( dims[i] );
      }
      s += "]";
      return s;
   }
};

// ----------------------------------------------------------------------------
// Tensor Data (CPU-side buffer)
// ----------------------------------------------------------------------------

template <typename T>
struct Tensor
{
   std::vector<T> data;
   TensorShape shape;

   Tensor() = default;
   Tensor( const TensorShape& s ) : shape( s )
   {
      int64_t numElements = s.NumElements();
      if ( numElements <= 0 )
         throw std::runtime_error( "Invalid tensor shape: empty or invalid dimensions" );
      if ( numElements > 500000000 )  // Max 500M elements (~2GB for float)
         throw std::runtime_error( "Tensor too large: " + std::to_string( numElements ) + " elements" );
      data.resize( static_cast<size_t>( numElements ) );
   }
   Tensor( const TensorShape& s, const T& initVal ) : shape( s )
   {
      int64_t numElements = s.NumElements();
      if ( numElements <= 0 )
         throw std::runtime_error( "Invalid tensor shape: empty or invalid dimensions" );
      if ( numElements > 500000000 )
         throw std::runtime_error( "Tensor too large: " + std::to_string( numElements ) + " elements" );
      data.resize( static_cast<size_t>( numElements ), initVal );
   }

   size_t Size() const { return data.size(); }
   T* Data() { return data.data(); }
   const T* Data() const { return data.data(); }
   T* MutableData() { return data.data(); }

   int64_t NumElements() const { return shape.NumElements(); }

   T& operator[]( size_t i ) { return data[i]; }
   const T& operator[]( size_t i ) const { return data[i]; }

   void Resize( const TensorShape& newShape )
   {
      int64_t numElements = newShape.NumElements();
      if ( numElements <= 0 )
         throw std::runtime_error( "Invalid tensor shape for resize: empty or invalid dimensions" );
      if ( numElements > 500000000 )  // Max 500M elements
         throw std::runtime_error( "Tensor resize too large: " + std::to_string( numElements ) + " elements" );

      shape = newShape;
      data.resize( static_cast<size_t>( numElements ) );
   }

   void Fill( const T& value )
   {
      std::fill( data.begin(), data.end(), value );
   }
};

using FloatTensor = Tensor<float>;

// ----------------------------------------------------------------------------
// Model Input/Output Metadata
// ----------------------------------------------------------------------------

struct TensorInfo
{
   String name = String();           // Must be initialized for PCL ABI
   TensorShape shape;                // Shape (may include -1 for dynamic dims)
   String elementType = String();    // "float", "int64", etc. - Must be initialized
   bool isDynamic = false;           // Has dynamic dimensions
};

// ----------------------------------------------------------------------------
// ONNX Inference Session
//
// Wraps ONNX Runtime for model loading and inference.
// Designed to be exception-safe and provide graceful degradation.
// ----------------------------------------------------------------------------

class ONNXSession
{
public:

   ONNXSession();
   ~ONNXSession();

   // Load a model from file
   // Returns true on success, false on failure
   bool LoadModel( const String& modelPath, const ONNXConfig& config = ONNXConfig() );

   // Check if model is loaded
   bool IsLoaded() const { return m_isLoaded; }

   // Get model metadata
   String GetModelPath() const { return m_modelPath; }
   std::vector<TensorInfo> GetInputInfo() const { return m_inputs; }
   std::vector<TensorInfo> GetOutputInfo() const { return m_outputs; }

   // Run inference
   // Input: vector of float tensors (one per model input)
   // Output: vector of float tensors (one per model output)
   bool Run( const std::vector<FloatTensor>& inputs,
             std::vector<FloatTensor>& outputs );

   // Single input/output convenience method
   bool Run( const FloatTensor& input, FloatTensor& output );

   // Run inference on a batch of inputs
   // Concatenates multiple [1,C,H,W] tensors into [N,C,H,W] and runs a single inference call.
   // All inputs must have the same shape (except batch dimension).
   // The model must support dynamic batch size.
   bool RunBatch( const std::vector<FloatTensor>& batchInputs,
                  std::vector<FloatTensor>& batchOutputs,
                  int batchSize );

   // Get last error message
   String GetLastError() const { return m_lastError; }

   // Unload model and release resources
   void Unload();

   // Check if ONNX Runtime is available
   static bool IsRuntimeAvailable();

   // Get ONNX Runtime version
   static String GetRuntimeVersion();

private:

   bool m_isLoaded = false;
   String m_modelPath;
   String m_lastError;

   std::vector<TensorInfo> m_inputs;
   std::vector<TensorInfo> m_outputs;

   // ONNX Runtime objects (pimpl pattern for isolation)
   struct Impl;
   std::unique_ptr<Impl> m_impl;

   // Parse model metadata
   bool ParseMetadata();
};

// ----------------------------------------------------------------------------
// ONNX Runtime Manager (Singleton)
//
// Manages the global ONNX Runtime environment.
// Thread-safe initialization.
// ----------------------------------------------------------------------------

class ONNXRuntime
{
public:

   // Get singleton instance
   static ONNXRuntime& Instance();

   // Check if runtime is available
   bool IsAvailable() const { return m_isAvailable; }

   // Get environment (for session creation)
   void* GetEnvironment() const { return m_env; }

   // Get runtime version
   String GetVersion() const { return m_version; }

   // Get last error
   String GetLastError() const { return m_lastError; }

private:

   ONNXRuntime();
   ~ONNXRuntime();

   ONNXRuntime( const ONNXRuntime& ) = delete;
   ONNXRuntime& operator=( const ONNXRuntime& ) = delete;

   bool m_isAvailable = false;
   void* m_env = nullptr;
   String m_version;
   String m_lastError;
};

// ----------------------------------------------------------------------------
// Utility Functions
// ----------------------------------------------------------------------------

namespace ONNXUtils
{
   // Convert PCL Image to float tensor (NCHW format)
   FloatTensor ImageToTensor( const Image& image, int targetWidth = -1, int targetHeight = -1 );

   // Convert float tensor to PCL Image
   void TensorToImage( const FloatTensor& tensor, Image& image );

   // Resize tensor spatially (bilinear interpolation)
   FloatTensor ResizeTensor( const FloatTensor& input, int newWidth, int newHeight );

   // Normalize tensor values to [0, 1]
   void NormalizeTensor( FloatTensor& tensor );

   // Apply softmax to tensor (along channel dimension for segmentation)
   void SoftmaxChannels( FloatTensor& tensor );

   // Get argmax along channel dimension (for class prediction)
   std::vector<int> ArgmaxChannels( const FloatTensor& tensor );
}

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __ONNXInference_h
