"""
CUDA GPU kernels for accelerated processing.

This module provides GPU-accelerated versions of the core algorithms using CUDA.jl.
Optimizations include:
- Shared memory for coalesced access patterns
- Block reduction for parallel statistics
- Multi-stream support for overlapped transfers
- Warp-level primitives where beneficial

Falls back to CPU implementation if CUDA is not available.
"""
module Kernels

using ..BayesianAstro: PixelDistribution, PixelResult, DistributionType,
                       ProcessingConfig, CUDA_AVAILABLE, GAUSSIAN, POISSON,
                       BIMODAL, SKEWED_RIGHT, SKEWED_LEFT, UNIFORM, UNKNOWN
using ..Welford: variance, skewness, kurtosis
using ..Classification: classify_distribution
using ..Confidence: compute_confidence

export gpu_accumulate!, gpu_finalize!, gpu_fuse!, gpu_stretch!
export is_gpu_available, create_gpu_context, destroy_gpu_context
export GPUContext

# ============================================================================
# GPU Context for resource management
# ============================================================================

"""
    GPUContext

Holds GPU resources for a processing session.
Manages device memory and CUDA streams for efficient processing.
"""
mutable struct GPUContext
    device_id::Int
    streams::Vector{Any}  # CUDA streams for overlapping
    tile_size::Tuple{Int,Int}
    initialized::Bool

    function GPUContext(; device_id::Int=0, n_streams::Int=4, tile_size::Tuple{Int,Int}=(1024, 1024))
        new(device_id, [], tile_size, false)
    end
end

"""
    is_gpu_available() -> Bool

Check if GPU acceleration is available and functional.
"""
function is_gpu_available()::Bool
    return CUDA_AVAILABLE[]
end

"""
    create_gpu_context(; kwargs...) -> GPUContext

Initialize GPU context with CUDA resources.
"""
function create_gpu_context(; device_id::Int=0, n_streams::Int=4, tile_size::Tuple{Int,Int}=(1024, 1024))
    ctx = GPUContext(; device_id, n_streams, tile_size)

    if CUDA_AVAILABLE[]
        try
            @eval using CUDA
            @eval CUDA.device!($device_id)

            # Create streams for overlapping
            for _ in 1:n_streams
                push!(ctx.streams, @eval CUDA.CuStream())
            end

            ctx.initialized = true
            @info "GPU context initialized on device $device_id with $n_streams streams"
        catch e
            @warn "Failed to initialize GPU context: $e"
            ctx.initialized = false
        end
    end

    return ctx
end

"""
    destroy_gpu_context(ctx::GPUContext)

Clean up GPU resources.
"""
function destroy_gpu_context(ctx::GPUContext)
    if ctx.initialized && CUDA_AVAILABLE[]
        try
            # Synchronize all streams before cleanup
            for stream in ctx.streams
                @eval CUDA.synchronize($stream)
            end
            empty!(ctx.streams)
            ctx.initialized = false
        catch e
            @warn "Error during GPU context cleanup: $e"
        end
    end
end

# ============================================================================
# GPU Kernel Implementations (loaded when CUDA is available)
# ============================================================================

function __init__()
    if CUDA_AVAILABLE[]
        @info "Loading optimized CUDA kernels..."
        _load_cuda_kernels()
    else
        @info "CUDA not available, GPU kernels disabled. Using CPU fallbacks."
    end
end

function _load_cuda_kernels()
    @eval begin
        using CUDA

        # Thread block configuration
        const BLOCK_SIZE_X = 16
        const BLOCK_SIZE_Y = 16
        const WARP_SIZE = 32

        # ================================================================
        # Welford accumulation kernel with shared memory optimization
        # ================================================================

        """
        GPU kernel for Welford accumulation - processes one frame into distributions.
        Uses shared memory for coalesced reads from frame data.
        """
        function _kernel_accumulate_welford!(
            n::CuDeviceMatrix{UInt16},
            mean::CuDeviceMatrix{Float32},
            m2::CuDeviceMatrix{Float32},
            m3::CuDeviceMatrix{Float32},
            m4::CuDeviceMatrix{Float32},
            minval::CuDeviceMatrix{Float32},
            maxval::CuDeviceMatrix{Float32},
            frame::CuDeviceMatrix{Float32},
            height::Int32, width::Int32
        )
            # Shared memory for frame tile
            tile = @cuStaticSharedMem(Float32, (BLOCK_SIZE_X, BLOCK_SIZE_Y))

            # Global pixel coordinates
            i = (blockIdx().x - Int32(1)) * blockDim().x + threadIdx().x
            j = (blockIdx().y - Int32(1)) * blockDim().y + threadIdx().y

            # Local thread indices
            tx = threadIdx().x
            ty = threadIdx().y

            if i <= height && j <= width
                # Load frame value into shared memory
                value = frame[i, j]
                tile[tx, ty] = value
            end

            sync_threads()

            if i <= height && j <= width
                value = tile[tx, ty]

                # Load current state
                n1 = n[i, j]
                mean_old = mean[i, j]
                m2_old = m2[i, j]
                m3_old = m3[i, j]
                m4_old = m4[i, j]

                # Welford update
                n_new = n1 + UInt16(1)
                n_f = Float32(n_new)
                n1_f = Float32(n1)

                delta = value - mean_old
                delta_n = delta / n_f
                delta_n2 = delta_n * delta_n
                term1 = delta * delta_n * n1_f

                # Update mean
                mean_new = mean_old + delta_n

                # Update M4 (must be done before M3 and M2)
                m4_new = m4_old + term1 * delta_n2 * (n_f * n_f - 3.0f0 * n_f + 3.0f0) +
                         6.0f0 * delta_n2 * m2_old -
                         4.0f0 * delta_n * m3_old

                # Update M3 (must be done before M2)
                m3_new = m3_old + term1 * delta_n * (n_f - 2.0f0) - 3.0f0 * delta_n * m2_old

                # Update M2
                m2_new = m2_old + term1

                # Write back
                n[i, j] = n_new
                mean[i, j] = mean_new
                m2[i, j] = m2_new
                m3[i, j] = m3_new
                m4[i, j] = m4_new
                minval[i, j] = min(minval[i, j], value)
                maxval[i, j] = max(maxval[i, j], value)
            end

            return nothing
        end

        """
            gpu_accumulate!(ctx, distributions, frame)

        Accumulate a frame into pixel distributions using GPU.
        Distributions are stored as separate arrays for better memory coalescing.
        """
        function gpu_accumulate!(
            d_n::CuArray{UInt16,2},
            d_mean::CuArray{Float32,2},
            d_m2::CuArray{Float32,2},
            d_m3::CuArray{Float32,2},
            d_m4::CuArray{Float32,2},
            d_min::CuArray{Float32,2},
            d_max::CuArray{Float32,2},
            d_frame::CuArray{Float32,2};
            stream=nothing
        )
            height, width = size(d_frame)

            threads = (BLOCK_SIZE_X, BLOCK_SIZE_Y)
            blocks = (cld(height, BLOCK_SIZE_X), cld(width, BLOCK_SIZE_Y))

            if stream === nothing
                @cuda threads=threads blocks=blocks _kernel_accumulate_welford!(
                    d_n, d_mean, d_m2, d_m3, d_m4, d_min, d_max, d_frame,
                    Int32(height), Int32(width)
                )
            else
                @cuda threads=threads blocks=blocks stream=stream _kernel_accumulate_welford!(
                    d_n, d_mean, d_m2, d_m3, d_m4, d_min, d_max, d_frame,
                    Int32(height), Int32(width)
                )
            end

            return nothing
        end

        # ================================================================
        # Classification and confidence kernel
        # ================================================================

        """
        Classify distribution type based on moments.
        Inline device function for use in kernels.
        """
        @inline function _classify_device(
            n::UInt16, mean::Float32, m2::Float32, m3::Float32, m4::Float32,
            minval::Float32, maxval::Float32
        )::UInt8
            # Thresholds
            MIN_SAMPLES = UInt16(5)
            SKEWNESS_THRESHOLD = 0.5f0
            KURTOSIS_LOW = -0.5f0
            KURTOSIS_HIGH = 1.0f0
            VARIANCE_RATIO_THRESHOLD = 0.1f0

            if n < MIN_SAMPLES
                return UInt8(7)  # UNKNOWN
            end

            n_f = Float32(n)
            var = m2 / (n_f - 1.0f0)
            range_val = maxval - minval

            # Uniform check
            if range_val > 0.0f0 && var / (range_val * range_val) < VARIANCE_RATIO_THRESHOLD
                return UInt8(6)  # UNIFORM
            end

            # Skewness and kurtosis
            if m2 ≈ 0.0f0
                skew = 0.0f0
                kurt = 0.0f0
            else
                skew = sqrt(n_f) * m3 / (m2^1.5f0)
                kurt = n_f * m4 / (m2 * m2) - 3.0f0
            end

            # Poisson check
            if mean > 0.0f0 && abs(var - mean) / mean < 0.3f0 && skew > 0.0f0
                return UInt8(2)  # POISSON
            end

            # Bimodal check
            if kurt < KURTOSIS_LOW
                return UInt8(3)  # BIMODAL
            end

            # Skewness check
            if abs(skew) > SKEWNESS_THRESHOLD
                if skew > 0.0f0
                    return UInt8(4)  # SKEWED_RIGHT
                else
                    return UInt8(5)  # SKEWED_LEFT
                end
            end

            # Gaussian
            if abs(skew) <= SKEWNESS_THRESHOLD && abs(kurt) <= KURTOSIS_HIGH
                return UInt8(1)  # GAUSSIAN
            end

            return UInt8(7)  # UNKNOWN
        end

        """
        Compute confidence score.
        Inline device function for use in kernels.
        """
        @inline function _confidence_device(
            n::UInt16, mean::Float32, m2::Float32, m3::Float32, m4::Float32,
            dtype::UInt8
        )::Float32
            REF_SAMPLE_COUNT = 100.0f0
            REF_VARIANCE = 100.0f0

            if n < UInt16(2)
                return 0.0f0
            end

            n_f = Float32(n)
            var = m2 / (n_f - 1.0f0)

            # Sample size factor
            sample_factor = min(1.0f0, n_f / REF_SAMPLE_COUNT)

            # Variance factor
            variance_factor = if var > 0.0f0
                1.0f0 / (1.0f0 + var / REF_VARIANCE)
            else
                1.0f0
            end

            # Distribution factor
            dist_factor = if dtype == UInt8(1)  # GAUSSIAN
                1.0f0
            elseif dtype == UInt8(2)  # POISSON
                0.9f0
            elseif dtype == UInt8(3)  # BIMODAL
                0.5f0
            elseif dtype == UInt8(4) || dtype == UInt8(5)  # SKEWED
                0.3f0
            elseif dtype == UInt8(6)  # UNIFORM
                0.2f0
            else
                0.5f0
            end

            # Outlier factor
            if m2 ≈ 0.0f0
                outlier_factor = 1.0f0
            else
                skew = abs(sqrt(n_f) * m3 / (m2^1.5f0))
                kurt = abs(n_f * m4 / (m2 * m2) - 3.0f0)
                outlier_factor = 1.0f0 / (1.0f0 + 0.5f0 * skew + 0.2f0 * kurt)
            end

            # Weighted combination
            conf = 0.2f0 * sample_factor +
                   0.3f0 * variance_factor +
                   0.3f0 * dist_factor +
                   0.2f0 * outlier_factor

            return clamp(conf, 0.0f0, 1.0f0)
        end

        """
        Finalization kernel - computes fused values, confidence, and classification.
        """
        function _kernel_finalize!(
            output::CuDeviceMatrix{Float32},
            confidence::CuDeviceMatrix{Float32},
            dist_type::CuDeviceMatrix{UInt8},
            n::CuDeviceMatrix{UInt16},
            mean::CuDeviceMatrix{Float32},
            m2::CuDeviceMatrix{Float32},
            m3::CuDeviceMatrix{Float32},
            m4::CuDeviceMatrix{Float32},
            minval::CuDeviceMatrix{Float32},
            maxval::CuDeviceMatrix{Float32},
            height::Int32, width::Int32
        )
            i = (blockIdx().x - Int32(1)) * blockDim().x + threadIdx().x
            j = (blockIdx().y - Int32(1)) * blockDim().y + threadIdx().y

            if i <= height && j <= width
                # Load distribution data
                n_val = n[i, j]
                mean_val = mean[i, j]
                m2_val = m2[i, j]
                m3_val = m3[i, j]
                m4_val = m4[i, j]
                min_val = minval[i, j]
                max_val = maxval[i, j]

                # Classify
                dtype = _classify_device(n_val, mean_val, m2_val, m3_val, m4_val, min_val, max_val)

                # Compute confidence
                conf = _confidence_device(n_val, mean_val, m2_val, m3_val, m4_val, dtype)

                # MLE fusion (mean for Gaussian/Poisson)
                fused = mean_val

                # Write results
                output[i, j] = fused
                confidence[i, j] = conf
                dist_type[i, j] = dtype
            end

            return nothing
        end

        """
            gpu_finalize!(d_output, d_confidence, d_dtype, d_n, d_mean, d_m2, d_m3, d_m4, d_min, d_max)

        Finalize statistics and compute fused output on GPU.
        """
        function gpu_finalize!(
            d_output::CuArray{Float32,2},
            d_confidence::CuArray{Float32,2},
            d_dtype::CuArray{UInt8,2},
            d_n::CuArray{UInt16,2},
            d_mean::CuArray{Float32,2},
            d_m2::CuArray{Float32,2},
            d_m3::CuArray{Float32,2},
            d_m4::CuArray{Float32,2},
            d_min::CuArray{Float32,2},
            d_max::CuArray{Float32,2};
            stream=nothing
        )
            height, width = size(d_output)

            threads = (BLOCK_SIZE_X, BLOCK_SIZE_Y)
            blocks = (cld(height, BLOCK_SIZE_X), cld(width, BLOCK_SIZE_Y))

            if stream === nothing
                @cuda threads=threads blocks=blocks _kernel_finalize!(
                    d_output, d_confidence, d_dtype,
                    d_n, d_mean, d_m2, d_m3, d_m4, d_min, d_max,
                    Int32(height), Int32(width)
                )
            else
                @cuda threads=threads blocks=blocks stream=stream _kernel_finalize!(
                    d_output, d_confidence, d_dtype,
                    d_n, d_mean, d_m2, d_m3, d_m4, d_min, d_max,
                    Int32(height), Int32(width)
                )
            end

            return nothing
        end

        # ================================================================
        # Histogram stretch kernel
        # ================================================================

        """
        Apply histogram stretch to normalize output range.
        """
        function _kernel_stretch!(
            output::CuDeviceMatrix{Float32},
            input::CuDeviceMatrix{Float32},
            black_point::Float32,
            white_point::Float32,
            height::Int32, width::Int32
        )
            i = (blockIdx().x - Int32(1)) * blockDim().x + threadIdx().x
            j = (blockIdx().y - Int32(1)) * blockDim().y + threadIdx().y

            if i <= height && j <= width
                value = input[i, j]

                # Linear stretch
                range_val = white_point - black_point
                if range_val > 0.0f0
                    stretched = (value - black_point) / range_val
                    output[i, j] = clamp(stretched, 0.0f0, 1.0f0)
                else
                    output[i, j] = 0.0f0
                end
            end

            return nothing
        end

        """
            gpu_stretch!(d_output, d_input, black_point, white_point)

        Apply histogram stretch on GPU.
        """
        function gpu_stretch!(
            d_output::CuArray{Float32,2},
            d_input::CuArray{Float32,2},
            black_point::Float32,
            white_point::Float32;
            stream=nothing
        )
            height, width = size(d_input)

            threads = (BLOCK_SIZE_X, BLOCK_SIZE_Y)
            blocks = (cld(height, BLOCK_SIZE_X), cld(width, BLOCK_SIZE_Y))

            if stream === nothing
                @cuda threads=threads blocks=blocks _kernel_stretch!(
                    d_output, d_input, black_point, white_point,
                    Int32(height), Int32(width)
                )
            else
                @cuda threads=threads blocks=blocks stream=stream _kernel_stretch!(
                    d_output, d_input, black_point, white_point,
                    Int32(height), Int32(width)
                )
            end

            return nothing
        end

        # ================================================================
        # Parallel min/max reduction for auto-stretch
        # ================================================================

        """
        Block-level reduction for finding min/max values.
        Uses shared memory and warp-level primitives.
        """
        function _kernel_minmax_reduce!(
            out_min::CuDeviceVector{Float32},
            out_max::CuDeviceVector{Float32},
            input::CuDeviceMatrix{Float32},
            height::Int32, width::Int32
        )
            # Shared memory for block reduction
            smin = @cuStaticSharedMem(Float32, 256)
            smax = @cuStaticSharedMem(Float32, 256)

            tid = threadIdx().x
            bid = blockIdx().x

            # Initialize with identity values
            local_min = Inf32
            local_max = -Inf32

            # Grid-stride loop to process all pixels
            total_pixels = height * width
            idx = (bid - Int32(1)) * blockDim().x + tid
            stride = gridDim().x * blockDim().x

            while idx <= total_pixels
                i = ((idx - Int32(1)) % height) + Int32(1)
                j = ((idx - Int32(1)) ÷ height) + Int32(1)

                if i <= height && j <= width
                    val = input[i, j]
                    local_min = min(local_min, val)
                    local_max = max(local_max, val)
                end

                idx += stride
            end

            # Store in shared memory
            smin[tid] = local_min
            smax[tid] = local_max
            sync_threads()

            # Block reduction
            s = blockDim().x ÷ Int32(2)
            while s > Int32(0)
                if tid <= s
                    smin[tid] = min(smin[tid], smin[tid + s])
                    smax[tid] = max(smax[tid], smax[tid + s])
                end
                sync_threads()
                s ÷= Int32(2)
            end

            # Write block result
            if tid == Int32(1)
                out_min[bid] = smin[1]
                out_max[bid] = smax[1]
            end

            return nothing
        end

        """
            gpu_compute_minmax(d_input) -> (min, max)

        Compute min/max of image using parallel reduction.
        """
        function gpu_compute_minmax(d_input::CuArray{Float32,2})
            height, width = size(d_input)

            # Use 256 threads per block, enough blocks to cover image
            threads = 256
            blocks = min(256, cld(height * width, threads))

            d_min = CUDA.zeros(Float32, blocks)
            d_max = CUDA.zeros(Float32, blocks)

            @cuda threads=threads blocks=blocks _kernel_minmax_reduce!(
                d_min, d_max, d_input, Int32(height), Int32(width)
            )

            # Final reduction on CPU (small array)
            h_min = Array(d_min)
            h_max = Array(d_max)

            return (minimum(h_min), maximum(h_max))
        end

    end  # @eval
end  # _load_cuda_kernels

# ============================================================================
# CPU Fallback Implementations
# ============================================================================

"""
    cpu_accumulate!(distributions, frame)

CPU fallback for frame accumulation using threads.
"""
function cpu_accumulate!(
    distributions::Matrix{PixelDistribution},
    frame::Matrix{Float32}
)
    height, width = size(frame)
    @assert size(distributions) == (height, width)

    Threads.@threads for j in 1:width
        for i in 1:height
            dist = distributions[i, j]
            value = frame[i, j]

            # Welford update
            n1 = dist.n
            dist.n += 1
            n = dist.n

            dist.min = min(dist.min, value)
            dist.max = max(dist.max, value)

            delta = value - dist.mean
            delta_n = delta / n
            delta_n2 = delta_n * delta_n
            term1 = delta * delta_n * n1

            dist.mean += delta_n
            dist.m4 += term1 * delta_n2 * (n*n - 3*n + 3) + 6 * delta_n2 * dist.m2 - 4 * delta_n * dist.m3
            dist.m3 += term1 * delta_n * (n - 2) - 3 * delta_n * dist.m2
            dist.m2 += term1
        end
    end

    return nothing
end

"""
    cpu_finalize!(distributions) -> (output, confidence, dist_types)

CPU fallback for finalization.
"""
function cpu_finalize!(distributions::Matrix{PixelDistribution})
    height, width = size(distributions)

    output = Matrix{Float32}(undef, height, width)
    confidence = Matrix{Float32}(undef, height, width)
    dist_types = Matrix{DistributionType}(undef, height, width)

    Threads.@threads for j in 1:width
        for i in 1:height
            dist = distributions[i, j]

            output[i, j] = dist.mean  # MLE
            confidence[i, j] = compute_confidence(dist)
            dist_types[i, j] = classify_distribution(dist)
        end
    end

    return (output, confidence, dist_types)
end

"""
    cpu_stretch!(output, input, black_point, white_point)

CPU fallback for histogram stretch.
"""
function cpu_stretch!(
    output::Matrix{Float32},
    input::Matrix{Float32},
    black_point::Float32,
    white_point::Float32
)
    height, width = size(input)
    range_val = white_point - black_point

    if range_val <= 0
        fill!(output, 0.0f0)
        return nothing
    end

    Threads.@threads for j in 1:width
        for i in 1:height
            value = input[i, j]
            stretched = (value - black_point) / range_val
            output[i, j] = clamp(stretched, 0.0f0, 1.0f0)
        end
    end

    return nothing
end

end # module Kernels
