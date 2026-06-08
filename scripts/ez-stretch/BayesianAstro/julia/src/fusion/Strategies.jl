"""
Pixel fusion strategies for combining frame data.

Provides multiple approaches to fusing per-pixel distributions into
final output values.
"""
module Strategies

using ..BayesianAstro: PixelDistribution, PixelResult, DistributionType, 
                       FrameMetadata, FusionStrategy, ImageStack
using ..Welford: variance, finalize_statistics
using ..Classification: classify_distribution, is_reliable
using ..Confidence: compute_confidence, compute_pixel_result

export fuse_mle, fuse_confidence_weighted, fuse_lucky, fuse_multiscale
export select_fusion_strategy

"""
    fuse_mle(dist::PixelDistribution) -> Float32

Maximum Likelihood Estimation fusion.
For Gaussian distributions, MLE estimate is simply the mean.
"""
function fuse_mle(dist::PixelDistribution)::Float32
    if dist.n == 0
        return 0.0f0
    end
    
    dtype = classify_distribution(dist)
    
    # For Gaussian, MLE = mean
    if dtype == GAUSSIAN
        return dist.mean
    end
    
    # For Poisson, MLE = mean (lambda)
    if dtype == POISSON
        return dist.mean
    end
    
    # For skewed distributions, median might be better
    # but we don't track median in online algorithm
    # Fall back to mean
    return dist.mean
end

"""
    fuse_confidence_weighted(dists::Vector{PixelDistribution}, 
                             values::Vector{Float32}) -> Float32

Confidence-weighted mean across frames.
Each frame's contribution is weighted by confidence and inverse variance.
"""
function fuse_confidence_weighted(dists::Vector{PixelDistribution}, 
                                   values::Vector{Float32})::Float32
    @assert length(dists) == length(values)
    
    if isempty(values)
        return 0.0f0
    end
    
    total_weight = 0.0f0
    weighted_sum = 0.0f0
    
    for (dist, value) in zip(dists, values)
        conf = compute_confidence(dist)
        var = variance(dist)
        
        # Weight = confidence / variance (inverse variance weighting)
        weight = if var > 0
            conf / var
        else
            conf * 1000.0f0
        end
        
        weighted_sum += weight * value
        total_weight += weight
    end
    
    if total_weight > 0
        return weighted_sum / total_weight
    else
        # Fall back to simple mean
        return sum(values) / length(values)
    end
end

"""
    fuse_lucky(values::Vector{Float32}, quality_scores::Vector{Float32}) -> Float32

Per-pixel lucky imaging: select value from frame with highest local quality.

# Arguments
- `values`: Pixel values from each frame
- `quality_scores`: Quality metric for each frame (e.g., local sharpness)
"""
function fuse_lucky(values::Vector{Float32}, quality_scores::Vector{Float32})::Float32
    @assert length(values) == length(quality_scores)
    
    if isempty(values)
        return 0.0f0
    end
    
    # Find index of best quality frame
    best_idx = argmax(quality_scores)
    return values[best_idx]
end

"""
    fuse_lucky(values::Vector{Float32}, metadata::Vector{FrameMetadata}) -> Float32

Per-pixel lucky imaging using frame metadata (FWHM-based).
Selects value from frame with best seeing (lowest FWHM).
"""
function fuse_lucky(values::Vector{Float32}, metadata::Vector{FrameMetadata})::Float32
    @assert length(values) == length(metadata)
    
    if isempty(values)
        return 0.0f0
    end
    
    # Find frame with lowest FWHM (best seeing)
    best_idx = 1
    best_fwhm = Inf32
    
    for (i, meta) in enumerate(metadata)
        if meta.fwhm > 0 && meta.fwhm < best_fwhm
            best_fwhm = meta.fwhm
            best_idx = i
        end
    end
    
    return values[best_idx]
end

"""
    fuse_multiscale(dist::PixelDistribution, 
                    spatial_frequency::Symbol) -> Float32

Multi-scale fusion: different strategies at different spatial frequencies.

# Arguments
- `dist`: Pixel distribution
- `spatial_frequency`: `:high`, `:mid`, or `:low`

# Strategy
- High frequency: Lucky imaging approach (preserve fine detail)
- Mid frequency: Confidence-weighted
- Low frequency: Mean (maximize SNR for smooth gradients)
"""
function fuse_multiscale(dist::PixelDistribution, spatial_frequency::Symbol)::Float32
    if dist.n == 0
        return 0.0f0
    end
    
    if spatial_frequency == :high
        # For high frequency, we want to preserve the sharpest values
        # Without per-frame data, use max or value closest to expected
        # This is a simplified version - full implementation needs frame data
        return dist.mean  # Placeholder
    elseif spatial_frequency == :mid
        # Confidence-weighted (single distribution version = MLE)
        return fuse_mle(dist)
    else  # :low
        # Simple mean for maximum SNR
        return dist.mean
    end
end

"""
    select_fusion_strategy(dtype::DistributionType) -> FusionStrategy

Automatically select appropriate fusion strategy based on distribution type.
"""
function select_fusion_strategy(dtype::DistributionType)::FusionStrategy
    if dtype == GAUSSIAN || dtype == POISSON
        return MLE
    elseif dtype == BIMODAL
        return LUCKY  # Try to pick the "right" mode
    elseif dtype == SKEWED_RIGHT || dtype == SKEWED_LEFT
        return CONFIDENCE_WEIGHTED  # Down-weight outliers
    else
        return CONFIDENCE_WEIGHTED  # Safe default
    end
end

"""
    fuse_image(stack::ImageStack, config::ProcessingConfig) -> Matrix{PixelResult}

Fuse an entire image stack into a result matrix.
CPU reference implementation.
"""
function fuse_image(stack::ImageStack{T}, strategy::FusionStrategy) where T
    height, width, n_frames = size(stack)
    
    # Initialize distribution array
    distributions = [PixelDistribution() for _ in 1:height, _ in 1:width]
    
    # Accumulate statistics from all frames
    @info "Accumulating statistics from $n_frames frames..."
    for (frame_idx, frame) in enumerate(stack.frames)
        for j in 1:width
            for i in 1:height
                accumulate!(distributions[i, j], Float32(frame[i, j]))
            end
        end
        
        if frame_idx % 10 == 0
            @info "  Processed $frame_idx/$n_frames frames"
        end
    end
    
    # Fuse each pixel
    @info "Fusing pixels..."
    results = Matrix{PixelResult}(undef, height, width)
    
    for j in 1:width
        for i in 1:height
            dist = distributions[i, j]
            
            fused_value = if strategy == MLE
                fuse_mle(dist)
            else
                # Default to MLE for now
                fuse_mle(dist)
            end
            
            results[i, j] = compute_pixel_result(dist, fused_value)
        end
    end
    
    return results
end

# Import accumulate! for internal use
using ..Welford: accumulate!

end # module Strategies
