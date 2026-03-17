// src/engine/cuda/CudaRemediation.cu
// GPU remediation kernels for post-stack artifact correction.
// Three kernels: trail re-selection, dust neighbor-ratio, vignetting multiply.
//
// All device functions are pure CUDA -- no host-side library dependencies.
// Copyright (c) 2026 Scott Carter

#include "cuda/CudaRemediation.h"

#include <cuda_runtime.h>
#include <cstdio>
#include <cmath>
#include <initializer_list>

namespace nukex {
namespace cuda {

// ============================================================================
// Constants
// ============================================================================

static constexpr int MAX_SUBS = 128;

__device__ constexpr double MAD_TO_SIGMA = 1.4826;

// ============================================================================
// Device helpers
// ============================================================================

__device__ static void insertionSortF( float* arr, int n )
{
   for ( int i = 1; i < n; ++i ) {
      float key = arr[i];
      int j = i - 1;
      while ( j >= 0 && arr[j] > key ) {
         arr[j + 1] = arr[j];
         --j;
      }
      arr[j + 1] = key;
   }
}

__device__ static float medianF( const float* sorted, int n )
{
   if ( n == 0 ) return 0.0f;
   if ( n % 2 == 0 )
      return ( sorted[n / 2 - 1] + sorted[n / 2] ) * 0.5f;
   return sorted[n / 2];
}

// ============================================================================
// Trail re-selection kernel
// One thread per trail pixel (sparse launch).
// Reads Z-column, computes median + MAD, rejects outliers, returns clean median.
// ============================================================================

__global__ void trailRemediationKernel(
   const float* __restrict__ cubeData,
   const int*   __restrict__ trailX,
   const int*   __restrict__ trailY,
   int    numTrailPixels,
   int    nSubs,
   int    height,
   int    width,
   float  outlierSigma,
   float* __restrict__ outputPixels )
{
   int tid = blockIdx.x * blockDim.x + threadIdx.x;
   if ( tid >= numTrailPixels ) return;

   int x = trailX[tid];
   int y = trailY[tid];

   // Column-major layout: element (z, y, x) = cubeData[z + y*nSubs + x*nSubs*height]
   const float* zCol = cubeData + y * nSubs + x * nSubs * height;

   // Read Z-column into local array
   float local[MAX_SUBS];
   for ( int z = 0; z < nSubs; ++z )
      local[z] = zCol[z];

   // Sort a copy to compute median
   float sorted[MAX_SUBS];
   for ( int z = 0; z < nSubs; ++z )
      sorted[z] = local[z];
   insertionSortF( sorted, nSubs );
   float med = medianF( sorted, nSubs );

   // Compute absolute deviations for MAD
   float deviations[MAX_SUBS];
   for ( int z = 0; z < nSubs; ++z )
      deviations[z] = fabsf( local[z] - med );
   insertionSortF( deviations, nSubs );
   float mad = medianF( deviations, nSubs );

   float scaledMAD = static_cast<float>( outlierSigma * MAD_TO_SIGMA ) * mad;

   // If MAD is zero (all identical), use range-based fallback
   if ( scaledMAD < 1e-15f ) {
      float range = sorted[nSubs - 1] - sorted[0];
      if ( range < 1e-15f ) {
         outputPixels[tid] = med;
         return;
      }
      scaledMAD = range * 0.1f;
   }

   // Upper-only threshold: trails are bright outliers, reject values above median + scaled MAD
   float threshold = med + scaledMAD;

   // Build clean array (keep only values <= threshold)
   float clean[MAX_SUBS];
   int nClean = 0;
   for ( int z = 0; z < nSubs; ++z ) {
      if ( local[z] <= threshold ) {
         clean[nClean++] = local[z];
      }
   }

   // Fallback: if all excluded, use original median
   if ( nClean == 0 ) {
      outputPixels[tid] = med;
      return;
   }

   // Compute median of clean values
   insertionSortF( clean, nClean );
   outputPixels[tid] = medianF( clean, nClean );
}

// ============================================================================
// Dust neighbor-ratio correction kernel
// One thread per pixel (full image launch). Non-dust pixels pass through.
// ============================================================================

__global__ void dustRemediationKernel(
   const float*   __restrict__ channelResult,
   const uint8_t* __restrict__ dustMask,
   int width, int height,
   int neighborRadius,
   float maxRatio,
   float* __restrict__ correctedOutput )
{
   int pixelIdx = blockIdx.x * blockDim.x + threadIdx.x;
   int totalPixels = width * height;
   if ( pixelIdx >= totalPixels ) return;

   float pixVal = channelResult[pixelIdx];

   // Non-dust pixels pass through unchanged
   if ( dustMask[pixelIdx] == 0 ) {
      correctedOutput[pixelIdx] = pixVal;
      return;
   }

   // Dust pixel: compute neighbor mean
   if ( pixVal <= 0.0f ) {
      correctedOutput[pixelIdx] = pixVal;
      return;
   }

   int y = pixelIdx / width;
   int x = pixelIdx % width;

   double neighborSum = 0.0;
   int neighborCount = 0;

   for ( int dy = -neighborRadius; dy <= neighborRadius; ++dy ) {
      for ( int dx = -neighborRadius; dx <= neighborRadius; ++dx ) {
         int ny = y + dy;
         int nx = x + dx;
         if ( ny < 0 || ny >= height || nx < 0 || nx >= width ) continue;
         int nIdx = ny * width + nx;
         if ( dustMask[nIdx] != 0 ) continue; // skip other dust pixels
         float nVal = channelResult[nIdx];
         if ( nVal <= 1e-6f ) continue; // skip near-zero
         neighborSum += static_cast<double>( nVal );
         ++neighborCount;
      }
   }

   if ( neighborCount == 0 ) {
      correctedOutput[pixelIdx] = pixVal;
      return;
   }

   float neighborMean = static_cast<float>( neighborSum / neighborCount );
   float ratio = neighborMean / pixVal;
   ratio = fmaxf( 1.0f, fminf( ratio, maxRatio ) );
   correctedOutput[pixelIdx] = pixVal * ratio;
}

// ============================================================================
// Vignetting multiplicative correction kernel
// One thread per pixel: output[i] = input[i] * correctionMap[i]
// ============================================================================

__global__ void vignettingRemediationKernel(
   const float* __restrict__ channelResult,
   const float* __restrict__ correctionMap,
   int totalPixels,
   float* __restrict__ correctedOutput )
{
   int idx = blockIdx.x * blockDim.x + threadIdx.x;
   if ( idx >= totalPixels ) return;

   correctedOutput[idx] = channelResult[idx] * correctionMap[idx];
}

// ============================================================================
// CUDA cleanup helpers
// ============================================================================

static void freeAll( std::initializer_list<void*> ptrs )
{
   for ( void* p : ptrs )
      if ( p ) cudaFree( p );
}

#define CUDA_CHECK_CLEANUP(call, ...)                     \
   do {                                                   \
      cudaError_t err = (call);                           \
      if ( err != cudaSuccess ) {                         \
         freeAll( { __VA_ARGS__ } );                      \
         return false;                                    \
      }                                                   \
   } while (0)

// ============================================================================
// Host wrappers
// ============================================================================

bool remediateTrailsGPU(
   const float* cubeData,
   size_t nSubs, size_t height, size_t width,
   const std::vector<TrailPixel>& trailPixels,
   double trailOutlierSigma,
   float* outputPixels )
{

   int numTrail = static_cast<int>( trailPixels.size() );
   if ( numTrail == 0 ) return true; // nothing to do
   if ( nSubs > MAX_SUBS ) return false; // too many subs for local arrays

   size_t cubeSize = nSubs * height * width;

   // Prepare host arrays for trail coordinates (SoA for GPU)
   std::vector<int> hostX( numTrail ), hostY( numTrail );
   for ( int i = 0; i < numTrail; ++i ) {
      hostX[i] = trailPixels[i].x;
      hostY[i] = trailPixels[i].y;
   }

   // Allocate device memory
   float* d_cube     = nullptr;
   int*   d_trailX   = nullptr;
   int*   d_trailY   = nullptr;
   float* d_output   = nullptr;

   CUDA_CHECK_CLEANUP( cudaMalloc( &d_cube,   cubeSize * sizeof(float) ),
                        d_cube, d_trailX, d_trailY, d_output );
   CUDA_CHECK_CLEANUP( cudaMalloc( &d_trailX, numTrail * sizeof(int) ),
                        d_cube, d_trailX, d_trailY, d_output );
   CUDA_CHECK_CLEANUP( cudaMalloc( &d_trailY, numTrail * sizeof(int) ),
                        d_cube, d_trailX, d_trailY, d_output );
   CUDA_CHECK_CLEANUP( cudaMalloc( &d_output, numTrail * sizeof(float) ),
                        d_cube, d_trailX, d_trailY, d_output );

   CUDA_CHECK_CLEANUP( cudaMemcpy( d_cube, cubeData,
                                    cubeSize * sizeof(float), cudaMemcpyHostToDevice ),
                        d_cube, d_trailX, d_trailY, d_output );
   CUDA_CHECK_CLEANUP( cudaMemcpy( d_trailX, hostX.data(),
                                    numTrail * sizeof(int), cudaMemcpyHostToDevice ),
                        d_cube, d_trailX, d_trailY, d_output );
   CUDA_CHECK_CLEANUP( cudaMemcpy( d_trailY, hostY.data(),
                                    numTrail * sizeof(int), cudaMemcpyHostToDevice ),
                        d_cube, d_trailX, d_trailY, d_output );

   constexpr int BLOCK_SIZE = 256;
   int gridSize = ( numTrail + BLOCK_SIZE - 1 ) / BLOCK_SIZE;

   trailRemediationKernel<<<gridSize, BLOCK_SIZE>>>(
      d_cube, d_trailX, d_trailY,
      numTrail,
      static_cast<int>( nSubs ),
      static_cast<int>( height ),
      static_cast<int>( width ),
      static_cast<float>( trailOutlierSigma ),
      d_output );

   CUDA_CHECK_CLEANUP( cudaGetLastError(),
                        d_cube, d_trailX, d_trailY, d_output );
   CUDA_CHECK_CLEANUP( cudaDeviceSynchronize(),
                        d_cube, d_trailX, d_trailY, d_output );

   CUDA_CHECK_CLEANUP( cudaMemcpy( outputPixels, d_output,
                                    numTrail * sizeof(float), cudaMemcpyDeviceToHost ),
                        d_cube, d_trailX, d_trailY, d_output );

   freeAll( { d_cube, d_trailX, d_trailY, d_output } );
   return true;
}

bool remediateDustGPU(
   const float* channelResult,
   int width, int height,
   const uint8_t* dustMask,
   int neighborRadius,
   float maxRatio,
   float* correctedOutput )
{
   int totalPixels = width * height;
   if ( totalPixels == 0 ) return true;

   float*   d_input  = nullptr;
   uint8_t* d_mask   = nullptr;
   float*   d_output = nullptr;

   CUDA_CHECK_CLEANUP( cudaMalloc( &d_input,  totalPixels * sizeof(float) ),
                        d_input, d_mask, d_output );
   CUDA_CHECK_CLEANUP( cudaMalloc( &d_mask,   totalPixels * sizeof(uint8_t) ),
                        d_input, d_mask, d_output );
   CUDA_CHECK_CLEANUP( cudaMalloc( &d_output, totalPixels * sizeof(float) ),
                        d_input, d_mask, d_output );

   CUDA_CHECK_CLEANUP( cudaMemcpy( d_input, channelResult,
                                    totalPixels * sizeof(float), cudaMemcpyHostToDevice ),
                        d_input, d_mask, d_output );
   CUDA_CHECK_CLEANUP( cudaMemcpy( d_mask, dustMask,
                                    totalPixels * sizeof(uint8_t), cudaMemcpyHostToDevice ),
                        d_input, d_mask, d_output );

   constexpr int BLOCK_SIZE = 256;
   int gridSize = ( totalPixels + BLOCK_SIZE - 1 ) / BLOCK_SIZE;

   dustRemediationKernel<<<gridSize, BLOCK_SIZE>>>(
      d_input, d_mask,
      width, height,
      neighborRadius,
      maxRatio,
      d_output );

   CUDA_CHECK_CLEANUP( cudaGetLastError(),
                        d_input, d_mask, d_output );
   CUDA_CHECK_CLEANUP( cudaDeviceSynchronize(),
                        d_input, d_mask, d_output );

   CUDA_CHECK_CLEANUP( cudaMemcpy( correctedOutput, d_output,
                                    totalPixels * sizeof(float), cudaMemcpyDeviceToHost ),
                        d_input, d_mask, d_output );

   freeAll( { d_input, d_mask, d_output } );
   return true;
}

bool remediateVignettingGPU(
   const float* channelResult,
   const float* correctionMap,
   int width, int height,
   float* correctedOutput )
{
   int totalPixels = width * height;
   if ( totalPixels == 0 ) return true;

   float* d_input   = nullptr;
   float* d_corrMap  = nullptr;
   float* d_output  = nullptr;

   CUDA_CHECK_CLEANUP( cudaMalloc( &d_input,   totalPixels * sizeof(float) ),
                        d_input, d_corrMap, d_output );
   CUDA_CHECK_CLEANUP( cudaMalloc( &d_corrMap,  totalPixels * sizeof(float) ),
                        d_input, d_corrMap, d_output );
   CUDA_CHECK_CLEANUP( cudaMalloc( &d_output,  totalPixels * sizeof(float) ),
                        d_input, d_corrMap, d_output );

   CUDA_CHECK_CLEANUP( cudaMemcpy( d_input, channelResult,
                                    totalPixels * sizeof(float), cudaMemcpyHostToDevice ),
                        d_input, d_corrMap, d_output );
   CUDA_CHECK_CLEANUP( cudaMemcpy( d_corrMap, correctionMap,
                                    totalPixels * sizeof(float), cudaMemcpyHostToDevice ),
                        d_input, d_corrMap, d_output );

   constexpr int BLOCK_SIZE = 256;
   int gridSize = ( totalPixels + BLOCK_SIZE - 1 ) / BLOCK_SIZE;

   vignettingRemediationKernel<<<gridSize, BLOCK_SIZE>>>(
      d_input, d_corrMap,
      totalPixels,
      d_output );

   CUDA_CHECK_CLEANUP( cudaGetLastError(),
                        d_input, d_corrMap, d_output );
   CUDA_CHECK_CLEANUP( cudaDeviceSynchronize(),
                        d_input, d_corrMap, d_output );

   CUDA_CHECK_CLEANUP( cudaMemcpy( correctedOutput, d_output,
                                    totalPixels * sizeof(float), cudaMemcpyDeviceToHost ),
                        d_input, d_corrMap, d_output );

   freeAll( { d_input, d_corrMap, d_output } );
   return true;
}

#undef CUDA_CHECK_CLEANUP

} // namespace cuda
} // namespace nukex
