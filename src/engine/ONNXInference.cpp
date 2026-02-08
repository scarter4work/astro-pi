//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "ONNXInference.h"
#include "Constants.h"

#include <pcl/Image.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

// Conditionally include ONNX Runtime
#ifdef NUKEX_USE_ONNX
#include <onnxruntime_cxx_api.h>
#define ONNX_AVAILABLE 1
#else
#define ONNX_AVAILABLE 0
#endif

namespace pcl
{

// ----------------------------------------------------------------------------
// ONNXSession Implementation
// ----------------------------------------------------------------------------

struct ONNXSession::Impl
{
#if ONNX_AVAILABLE
   std::unique_ptr<Ort::Session> session;
   std::unique_ptr<Ort::MemoryInfo> memoryInfo;
   std::vector<std::string> inputNames;
   std::vector<std::string> outputNames;
   std::vector<const char*> inputNamePtrs;
   std::vector<const char*> outputNamePtrs;
#endif
};

// ----------------------------------------------------------------------------

ONNXSession::ONNXSession()
   : m_impl( std::make_unique<Impl>() )
{
}

// ----------------------------------------------------------------------------

ONNXSession::~ONNXSession()
{
   Unload();
}

// ----------------------------------------------------------------------------

bool ONNXSession::LoadModel( const String& modelPath, const ONNXConfig& config )
{
#if ONNX_AVAILABLE
   try
   {
      // Check runtime availability
      if ( !ONNXRuntime::Instance().IsAvailable() )
      {
         m_lastError = "ONNX Runtime not available: " + ONNXRuntime::Instance().GetLastError();
         return false;
      }

      Ort::Env* env = static_cast<Ort::Env*>( ONNXRuntime::Instance().GetEnvironment() );

      // Configure session options
      Ort::SessionOptions sessionOptions;

      sessionOptions.SetIntraOpNumThreads( config.numThreads > 0 ? config.numThreads : 0 );

      // Deterministic mode: force single-threaded for bit-identical results
      if ( config.deterministic )
      {
         sessionOptions.SetIntraOpNumThreads( 1 );
         sessionOptions.SetInterOpNumThreads( 1 );
      }

      sessionOptions.SetGraphOptimizationLevel(
         static_cast<GraphOptimizationLevel>( config.graphOptimizationLevel ) );

      if ( config.enableMemoryPattern )
         sessionOptions.EnableMemPattern();

      if ( config.enableCpuMemArena )
         sessionOptions.EnableCpuMemArena();

      // Try GPU if requested
      if ( config.useGPU )
      {
#ifdef USE_CUDA
         OrtCUDAProviderOptions cudaOptions;
         cudaOptions.device_id = config.gpuDeviceId;
         sessionOptions.AppendExecutionProvider_CUDA( cudaOptions );
         Console().WriteLn( "ONNX: Using CUDA execution provider" );
#else
         Console().WarningLn( "ONNX: CUDA requested but not available, using CPU" );
#endif
      }

      // Convert path to wide string for Windows compatibility
      std::string pathStr( modelPath.ToUTF8().c_str() );

#ifdef _WIN32
      std::wstring wpath( pathStr.begin(), pathStr.end() );
      m_impl->session = std::make_unique<Ort::Session>( *env, wpath.c_str(), sessionOptions );
#else
      m_impl->session = std::make_unique<Ort::Session>( *env, pathStr.c_str(), sessionOptions );
#endif

      // Create memory info
      m_impl->memoryInfo = std::make_unique<Ort::MemoryInfo>(
         Ort::MemoryInfo::CreateCpu( OrtArenaAllocator, OrtMemTypeDefault ) );

      m_modelPath = modelPath;
      m_isLoaded = true;

      // Parse metadata
      if ( !ParseMetadata() )
      {
         Unload();
         return false;
      }

      Console().WriteLn( String().Format( "ONNX: Loaded model '%s'", IsoString( modelPath ).c_str() ) );
      Console().WriteLn( String().Format( "ONNX: %zu inputs, %zu outputs",
                                           m_inputs.size(), m_outputs.size() ) );

      return true;
   }
   catch ( const Ort::Exception& e )
   {
      m_lastError = String( "ONNX Exception: " ) + e.what();
      Console().CriticalLn( m_lastError );
      return false;
   }
   catch ( const std::exception& e )
   {
      m_lastError = String( "Exception: " ) + e.what();
      Console().CriticalLn( m_lastError );
      return false;
   }
#else
   m_lastError = "ONNX Runtime not compiled into this build";
   Console().WarningLn( m_lastError );
   return false;
#endif
}

// ----------------------------------------------------------------------------

bool ONNXSession::ParseMetadata()
{
#if ONNX_AVAILABLE
   if ( !m_impl->session )
      return false;

   try
   {
      Ort::AllocatorWithDefaultOptions allocator;

      // Get input info
      size_t numInputs = m_impl->session->GetInputCount();
      m_inputs.resize( numInputs );
      m_impl->inputNames.resize( numInputs );
      m_impl->inputNamePtrs.resize( numInputs );

      for ( size_t i = 0; i < numInputs; ++i )
      {
         // Get name
         auto namePtr = m_impl->session->GetInputNameAllocated( i, allocator );
         m_impl->inputNames[i] = namePtr.get();
         m_impl->inputNamePtrs[i] = m_impl->inputNames[i].c_str();
         m_inputs[i].name = String( m_impl->inputNames[i].c_str() );

         // Get type info
         auto typeInfo = m_impl->session->GetInputTypeInfo( i );
         auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();

         // Get shape
         auto shape = tensorInfo.GetShape();
         m_inputs[i].shape.dims = shape;

         // Check for dynamic dimensions
         for ( int64_t dim : shape )
         {
            if ( dim < 0 )
            {
               m_inputs[i].isDynamic = true;
               break;
            }
         }

         // Get element type
         auto elemType = tensorInfo.GetElementType();
         switch ( elemType )
         {
         case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
            m_inputs[i].elementType = "float";
            break;
         case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:
            m_inputs[i].elementType = "int64";
            break;
         case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:
            m_inputs[i].elementType = "int32";
            break;
         default:
            m_inputs[i].elementType = "unknown";
            break;
         }
      }

      // Get output info
      size_t numOutputs = m_impl->session->GetOutputCount();
      m_outputs.resize( numOutputs );
      m_impl->outputNames.resize( numOutputs );
      m_impl->outputNamePtrs.resize( numOutputs );

      for ( size_t i = 0; i < numOutputs; ++i )
      {
         auto namePtr = m_impl->session->GetOutputNameAllocated( i, allocator );
         m_impl->outputNames[i] = namePtr.get();
         m_impl->outputNamePtrs[i] = m_impl->outputNames[i].c_str();
         m_outputs[i].name = String( m_impl->outputNames[i].c_str() );

         auto typeInfo = m_impl->session->GetOutputTypeInfo( i );
         auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();

         auto shape = tensorInfo.GetShape();
         m_outputs[i].shape.dims = shape;

         for ( int64_t dim : shape )
         {
            if ( dim < 0 )
            {
               m_outputs[i].isDynamic = true;
               break;
            }
         }

         auto elemType = tensorInfo.GetElementType();
         switch ( elemType )
         {
         case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
            m_outputs[i].elementType = "float";
            break;
         case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:
            m_outputs[i].elementType = "int64";
            break;
         default:
            m_outputs[i].elementType = "unknown";
            break;
         }
      }

      return true;
   }
   catch ( const Ort::Exception& e )
   {
      m_lastError = String( "Failed to parse metadata: " ) + e.what();
      return false;
   }
#else
   return false;
#endif
}

// ----------------------------------------------------------------------------

bool ONNXSession::Run( const std::vector<FloatTensor>& inputs,
                        std::vector<FloatTensor>& outputs )
{
#if ONNX_AVAILABLE
   if ( !m_isLoaded || !m_impl->session )
   {
      m_lastError = "No model loaded";
      return false;
   }

   if ( inputs.size() != m_inputs.size() )
   {
      m_lastError = String().Format( "Expected %zu inputs, got %zu",
                                      m_inputs.size(), inputs.size() );
      return false;
   }

   try
   {
      // Create input tensors
      std::vector<Ort::Value> inputTensors;
      inputTensors.reserve( inputs.size() );

      // Create mutable copies of input data for ONNX Runtime (avoids const_cast)
      std::vector<std::vector<float>> inputDataCopies( inputs.size() );

      for ( size_t i = 0; i < inputs.size(); ++i )
      {
         const FloatTensor& input = inputs[i];

         // Copy input data to a mutable buffer
         inputDataCopies[i].assign( input.Data(), input.Data() + input.Size() );

         // Create tensor with mutable data
         inputTensors.push_back( Ort::Value::CreateTensor<float>(
            *m_impl->memoryInfo,
            inputDataCopies[i].data(),
            inputDataCopies[i].size(),
            input.shape.dims.data(),
            input.shape.dims.size() ) );
      }

      // Run inference
      auto outputTensors = m_impl->session->Run(
         Ort::RunOptions{ nullptr },
         m_impl->inputNamePtrs.data(),
         inputTensors.data(),
         inputs.size(),
         m_impl->outputNamePtrs.data(),
         m_outputs.size() );

      // Extract outputs
      outputs.resize( outputTensors.size() );

      for ( size_t i = 0; i < outputTensors.size(); ++i )
      {
         auto& tensor = outputTensors[i];

         // Get shape
         auto typeInfo = tensor.GetTensorTypeAndShapeInfo();
         auto shape = typeInfo.GetShape();
         outputs[i].shape.dims = shape;

         // Get data
         const float* data = tensor.GetTensorData<float>();
         size_t numElements = typeInfo.GetElementCount();

         // Validate output size to prevent allocation crashes
         if ( numElements > 500000000 )  // 500M elements max (~2GB for float)
            throw std::runtime_error( "ONNX output tensor too large: " + std::to_string( numElements ) + " elements" );

         outputs[i].data.resize( numElements );
         std::copy( data, data + numElements, outputs[i].data.begin() );
      }

      return true;
   }
   catch ( const Ort::Exception& e )
   {
      m_lastError = String( "Inference failed: " ) + e.what();
      Console().CriticalLn( m_lastError );
      return false;
   }
#else
   m_lastError = "ONNX Runtime not available";
   return false;
#endif
}

// ----------------------------------------------------------------------------

bool ONNXSession::Run( const FloatTensor& input, FloatTensor& output )
{
   std::vector<FloatTensor> inputs = { input };
   std::vector<FloatTensor> outputs;

   if ( !Run( inputs, outputs ) )
      return false;

   if ( outputs.empty() )
   {
      m_lastError = "No output returned";
      return false;
   }

   output = std::move( outputs[0] );
   return true;
}

// ----------------------------------------------------------------------------

bool ONNXSession::RunBatch( const std::vector<FloatTensor>& batchInputs,
                             std::vector<FloatTensor>& batchOutputs,
                             int batchSize )
{
#if ONNX_AVAILABLE
   if ( !m_isLoaded || !m_impl->session )
   {
      m_lastError = "No model loaded";
      return false;
   }

   if ( batchInputs.empty() )
   {
      m_lastError = "Empty batch input";
      return false;
   }

   if ( batchSize <= 0 || batchSize > static_cast<int>( batchInputs.size() ) )
      batchSize = static_cast<int>( batchInputs.size() );

   // If batch size is 1, delegate to the standard Run() method
   if ( batchSize == 1 )
   {
      std::vector<FloatTensor> singleInput = { batchInputs[0] };
      std::vector<FloatTensor> singleOutput;
      if ( !Run( singleInput, singleOutput ) )
         return false;
      batchOutputs = std::move( singleOutput );
      return true;
   }

   try
   {
      // Validate all inputs have the same shape (except batch dim, which should be 1)
      const TensorShape& refShape = batchInputs[0].shape;
      if ( refShape.Rank() != 4 )
      {
         m_lastError = "RunBatch expects 4D tensors [N, C, H, W]";
         return false;
      }

      int64_t channels = refShape[1];
      int64_t height = refShape[2];
      int64_t width = refShape[3];
      int64_t elementsPerSample = channels * height * width;

      for ( int i = 1; i < batchSize; ++i )
      {
         const TensorShape& s = batchInputs[i].shape;
         if ( s.Rank() != 4 || s[1] != channels || s[2] != height || s[3] != width )
         {
            m_lastError = String().Format(
               "RunBatch: Input %d shape %s does not match expected [*, %lld, %lld, %lld]",
               i, IsoString( s.ToString() ).c_str(), channels, height, width );
            return false;
         }
      }

      // Concatenate individual inputs into a single batched tensor [N, C, H, W]
      int64_t totalElements = static_cast<int64_t>( batchSize ) * elementsPerSample;
      if ( totalElements > 500000000 )
      {
         m_lastError = String().Format( "RunBatch: Combined tensor too large (%lld elements)", totalElements );
         return false;
      }

      std::vector<float> batchedData( static_cast<size_t>( totalElements ) );

      for ( int i = 0; i < batchSize; ++i )
      {
         const float* src = batchInputs[i].Data();
         float* dst = batchedData.data() + i * elementsPerSample;
         std::copy( src, src + elementsPerSample, dst );
      }

      // Create batched tensor shape
      std::vector<int64_t> batchedShape = { static_cast<int64_t>( batchSize ), channels, height, width };

      // Create ONNX input tensor
      std::vector<Ort::Value> inputTensors;
      inputTensors.push_back( Ort::Value::CreateTensor<float>(
         *m_impl->memoryInfo,
         batchedData.data(),
         batchedData.size(),
         batchedShape.data(),
         batchedShape.size() ) );

      // Run inference
      auto outputTensors = m_impl->session->Run(
         Ort::RunOptions{ nullptr },
         m_impl->inputNamePtrs.data(),
         inputTensors.data(),
         1,  // Single batched input
         m_impl->outputNamePtrs.data(),
         m_outputs.size() );

      // Split batched output back into individual results
      if ( outputTensors.empty() )
      {
         m_lastError = "RunBatch: No output tensors returned";
         return false;
      }

      auto& outTensor = outputTensors[0];
      auto typeInfo = outTensor.GetTensorTypeAndShapeInfo();
      auto outShape = typeInfo.GetShape();

      if ( outShape.size() != 4 || outShape[0] != batchSize )
      {
         m_lastError = String().Format(
            "RunBatch: Unexpected output shape, expected batch=%d, got batch=%lld",
            batchSize, outShape.empty() ? -1 : outShape[0] );
         return false;
      }

      const float* outData = outTensor.GetTensorData<float>();
      int64_t outChannels = outShape[1];
      int64_t outHeight = outShape[2];
      int64_t outWidth = outShape[3];
      int64_t outElementsPerSample = outChannels * outHeight * outWidth;

      batchOutputs.resize( batchSize );
      for ( int i = 0; i < batchSize; ++i )
      {
         TensorShape singleOutShape = { 1, outChannels, outHeight, outWidth };
         batchOutputs[i].Resize( singleOutShape );

         const float* src = outData + i * outElementsPerSample;
         std::copy( src, src + outElementsPerSample, batchOutputs[i].Data() );
      }

      return true;
   }
   catch ( const Ort::Exception& e )
   {
      m_lastError = String( "RunBatch inference failed: " ) + e.what();
      Console().CriticalLn( m_lastError );
      return false;
   }
   catch ( const std::exception& e )
   {
      m_lastError = String( "RunBatch exception: " ) + e.what();
      Console().CriticalLn( m_lastError );
      return false;
   }
#else
   m_lastError = "ONNX Runtime not available";
   return false;
#endif
}

// ----------------------------------------------------------------------------

void ONNXSession::Unload()
{
#if ONNX_AVAILABLE
   m_impl->session.reset();
   m_impl->memoryInfo.reset();
   m_impl->inputNames.clear();
   m_impl->outputNames.clear();
   m_impl->inputNamePtrs.clear();
   m_impl->outputNamePtrs.clear();
#endif

   m_inputs.clear();
   m_outputs.clear();
   m_isLoaded = false;
   m_modelPath.Clear();
}

// ----------------------------------------------------------------------------

bool ONNXSession::IsRuntimeAvailable()
{
   return ONNXRuntime::Instance().IsAvailable();
}

// ----------------------------------------------------------------------------

String ONNXSession::GetRuntimeVersion()
{
   return ONNXRuntime::Instance().GetVersion();
}

// ----------------------------------------------------------------------------
// ONNXRuntime Singleton Implementation
// ----------------------------------------------------------------------------

ONNXRuntime& ONNXRuntime::Instance()
{
   static ONNXRuntime instance;
   return instance;
}

// ----------------------------------------------------------------------------

ONNXRuntime::ONNXRuntime()
{
#if ONNX_AVAILABLE
   try
   {
      static Ort::Env globalEnv( ORT_LOGGING_LEVEL_WARNING, "NukeX" );

      m_env = &globalEnv;
      m_isAvailable = true;
      m_version = String( ORT_API_VERSION );

      Console().WriteLn( String().Format( "ONNX Runtime initialized (API version: %s)",
                                           m_version.c_str() ) );
   }
   catch ( const Ort::Exception& e )
   {
      m_lastError = String( "Failed to initialize ONNX Runtime: " ) + e.what();
      m_isAvailable = false;
   }
   catch ( const std::exception& e )
   {
      m_lastError = String( "Failed to initialize ONNX Runtime: " ) + e.what();
      m_isAvailable = false;
   }
#else
   m_isAvailable = false;
   m_version = "N/A";
   m_lastError = "ONNX Runtime not compiled into this build. "
                  "Rebuild with NUKEX_USE_ONNX=1 and link against onnxruntime.";
#endif
}

// ----------------------------------------------------------------------------

ONNXRuntime::~ONNXRuntime()
{
   // Environment is static, don't delete
}

// ----------------------------------------------------------------------------
// ONNXUtils Implementation
// ----------------------------------------------------------------------------

namespace ONNXUtils
{

FloatTensor ImageToTensor( const Image& image, int targetWidth, int targetHeight )
{
   int srcWidth = image.Width();
   int srcHeight = image.Height();
   int numChannels = image.NumberOfNominalChannels();

   // Determine output size
   int outWidth = (targetWidth > 0) ? targetWidth : srcWidth;
   int outHeight = (targetHeight > 0) ? targetHeight : srcHeight;

   // Create NCHW tensor (batch=1, channels, height, width)
   TensorShape shape = { 1, numChannels, outHeight, outWidth };
   FloatTensor tensor( shape );

   // Scale factors
   double scaleX = static_cast<double>( srcWidth ) / outWidth;
   double scaleY = static_cast<double>( srcHeight ) / outHeight;

   // Convert image to tensor with bilinear interpolation if resizing
   for ( int c = 0; c < numChannels; ++c )
   {
      for ( int y = 0; y < outHeight; ++y )
      {
         for ( int x = 0; x < outWidth; ++x )
         {
            double srcX = x * scaleX;
            double srcY = y * scaleY;

            // Bilinear interpolation
            int x0 = static_cast<int>( srcX );
            int y0 = static_cast<int>( srcY );
            int x1 = std::min( x0 + 1, srcWidth - 1 );
            int y1 = std::min( y0 + 1, srcHeight - 1 );

            double fx = srcX - x0;
            double fy = srcY - y0;

            double v00 = image( x0, y0, c );
            double v10 = image( x1, y0, c );
            double v01 = image( x0, y1, c );
            double v11 = image( x1, y1, c );

            double value = Interpolation::BilinearInterpolate( v00, v10, v01, v11, fx, fy );

            // NCHW layout: index = ((n * C + c) * H + y) * W + x
            size_t idx = (c * outHeight + y) * outWidth + x;
            tensor[idx] = static_cast<float>( value );
         }
      }
   }

   return tensor;
}

// ----------------------------------------------------------------------------

void TensorToImage( const FloatTensor& tensor, Image& image )
{
   if ( tensor.shape.Rank() != 4 )
      return;  // Expected NCHW format

   int numChannels = static_cast<int>( tensor.shape[1] );
   int height = static_cast<int>( tensor.shape[2] );
   int width = static_cast<int>( tensor.shape[3] );

   // Determine color space
   ColorSpace::value_type colorSpace = (numChannels >= 3) ? ColorSpace::RGB : ColorSpace::Gray;

   image.AllocateData( width, height, numChannels, colorSpace );

   for ( int c = 0; c < numChannels; ++c )
   {
      for ( int y = 0; y < height; ++y )
      {
         for ( int x = 0; x < width; ++x )
         {
            size_t idx = (c * height + y) * width + x;
            image( x, y, c ) = static_cast<double>( tensor[idx] );
         }
      }
   }
}

// ----------------------------------------------------------------------------

FloatTensor ResizeTensor( const FloatTensor& input, int newWidth, int newHeight )
{
   if ( input.shape.Rank() != 4 )
      return input;  // Expected NCHW format

   int batch = static_cast<int>( input.shape[0] );
   int channels = static_cast<int>( input.shape[1] );
   int oldHeight = static_cast<int>( input.shape[2] );
   int oldWidth = static_cast<int>( input.shape[3] );

   TensorShape newShape = { batch, channels, newHeight, newWidth };
   FloatTensor output( newShape );

   double scaleX = static_cast<double>( oldWidth ) / newWidth;
   double scaleY = static_cast<double>( oldHeight ) / newHeight;

   for ( int n = 0; n < batch; ++n )
   {
      for ( int c = 0; c < channels; ++c )
      {
         for ( int y = 0; y < newHeight; ++y )
         {
            for ( int x = 0; x < newWidth; ++x )
            {
               double srcX = x * scaleX;
               double srcY = y * scaleY;

               int x0 = static_cast<int>( srcX );
               int y0 = static_cast<int>( srcY );
               int x1 = std::min( x0 + 1, oldWidth - 1 );
               int y1 = std::min( y0 + 1, oldHeight - 1 );

               double fx = srcX - x0;
               double fy = srcY - y0;

               size_t idx00 = ((n * channels + c) * oldHeight + y0) * oldWidth + x0;
               size_t idx10 = ((n * channels + c) * oldHeight + y0) * oldWidth + x1;
               size_t idx01 = ((n * channels + c) * oldHeight + y1) * oldWidth + x0;
               size_t idx11 = ((n * channels + c) * oldHeight + y1) * oldWidth + x1;

               float value = static_cast<float>(
                  Interpolation::BilinearInterpolate(
                     input[idx00], input[idx10], input[idx01], input[idx11], fx, fy ) );

               size_t outIdx = ((n * channels + c) * newHeight + y) * newWidth + x;
               output[outIdx] = value;
            }
         }
      }
   }

   return output;
}

// ----------------------------------------------------------------------------

void NormalizeTensor( FloatTensor& tensor )
{
   if ( tensor.Size() == 0 )
      return;

   float minVal = tensor[0];
   float maxVal = tensor[0];

   for ( size_t i = 1; i < tensor.Size(); ++i )
   {
      minVal = std::min( minVal, tensor[i] );
      maxVal = std::max( maxVal, tensor[i] );
   }

   if ( maxVal > minVal )
   {
      float scale = 1.0f / (maxVal - minVal);
      for ( size_t i = 0; i < tensor.Size(); ++i )
      {
         tensor[i] = (tensor[i] - minVal) * scale;
      }
   }
}

// ----------------------------------------------------------------------------

void SoftmaxChannels( FloatTensor& tensor )
{
   if ( tensor.shape.Rank() != 4 )
      return;

   int batch = static_cast<int>( tensor.shape[0] );
   int channels = static_cast<int>( tensor.shape[1] );
   int height = static_cast<int>( tensor.shape[2] );
   int width = static_cast<int>( tensor.shape[3] );

   for ( int n = 0; n < batch; ++n )
   {
      for ( int y = 0; y < height; ++y )
      {
         for ( int x = 0; x < width; ++x )
         {
            // Find max for numerical stability
            float maxVal = -1e30f;
            for ( int c = 0; c < channels; ++c )
            {
               size_t idx = ((n * channels + c) * height + y) * width + x;
               maxVal = std::max( maxVal, tensor[idx] );
            }

            // Compute exp and sum
            float expSum = 0;
            for ( int c = 0; c < channels; ++c )
            {
               size_t idx = ((n * channels + c) * height + y) * width + x;
               tensor[idx] = std::exp( tensor[idx] - maxVal );
               expSum += tensor[idx];
            }

            // Normalize
            if ( expSum > 0 )
            {
               for ( int c = 0; c < channels; ++c )
               {
                  size_t idx = ((n * channels + c) * height + y) * width + x;
                  tensor[idx] /= expSum;
               }
            }
         }
      }
   }
}

// ----------------------------------------------------------------------------

std::vector<int> ArgmaxChannels( const FloatTensor& tensor )
{
   if ( tensor.shape.Rank() != 4 )
      return {};

   int batch = static_cast<int>( tensor.shape[0] );
   int channels = static_cast<int>( tensor.shape[1] );
   int height = static_cast<int>( tensor.shape[2] );
   int width = static_cast<int>( tensor.shape[3] );

   // Validate dimensions to prevent allocation crash
   if ( batch <= 0 || channels <= 0 || height <= 0 || width <= 0 )
      throw std::runtime_error( "Invalid tensor dimensions for argmax" );
   if ( batch > 100 || height > 100000 || width > 100000 )
      throw std::runtime_error( "Tensor dimensions out of range for argmax" );

   size_t resultSize = static_cast<size_t>( batch ) * height * width;
   if ( resultSize > 500000000 )  // 500M elements max
      throw std::runtime_error( "Argmax result too large: " + std::to_string( resultSize ) + " elements" );

   std::vector<int> result( resultSize );

   for ( int n = 0; n < batch; ++n )
   {
      for ( int y = 0; y < height; ++y )
      {
         for ( int x = 0; x < width; ++x )
         {
            int maxChannel = 0;
            float maxVal = -1e30f;

            for ( int c = 0; c < channels; ++c )
            {
               size_t idx = ((n * channels + c) * height + y) * width + x;
               if ( tensor[idx] > maxVal )
               {
                  maxVal = tensor[idx];
                  maxChannel = c;
               }
            }

            size_t outIdx = (n * height + y) * width + x;
            result[outIdx] = maxChannel;
         }
      }
   }

   return result;
}

} // namespace ONNXUtils

// ----------------------------------------------------------------------------

} // namespace pcl
