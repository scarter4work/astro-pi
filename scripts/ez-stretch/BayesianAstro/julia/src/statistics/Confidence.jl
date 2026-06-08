"""
Confidence scoring for fused pixel values.

Confidence is derived from distribution properties and indicates
how reliable the fused value is.
"""
module Confidence

using ..BayesianAstro: PixelDistribution, DistributionType, PixelResult
using ..Welford: variance, stddev, skewness, kurtosis
using ..Classification: classify_distribution, is_reliable, is_artifact_candidate

export compute_confidence, compute_pixel_result

# Confidence weights for different factors
const WEIGHT_SAMPLE_SIZE = 0.2f0
const WEIGHT_VARIANCE = 0.3f0
const WEIGHT_DISTRIBUTION = 0.3f0
const WEIGHT_OUTLIERS = 0.2f0

# Reference values for normalization
const REF_SAMPLE_COUNT = 100   # "Good" number of samples
const REF_VARIANCE = 100.0f0   # Reference variance for normalization

"""
    compute_confidence(dist::PixelDistribution) -> Float32

Compute confidence score (0.0 - 1.0) for a pixel based on its distribution.

# Factors considered:
1. Sample size: More samples = higher confidence
2. Variance: Lower variance = higher confidence  
3. Distribution type: Gaussian/Poisson = higher confidence
4. Outlier indicators: Skewness/kurtosis affect confidence
"""
function compute_confidence(dist::PixelDistribution)::Float32
    if dist.n < 2
        return 0.0f0
    end
    
    # Factor 1: Sample size contribution
    # Saturates at REF_SAMPLE_COUNT samples
    sample_factor = min(1.0f0, Float32(dist.n) / REF_SAMPLE_COUNT)
    
    # Factor 2: Variance contribution
    # Lower variance = higher confidence (inverse relationship)
    var = variance(dist)
    if var > 0
        variance_factor = 1.0f0 / (1.0f0 + var / REF_VARIANCE)
    else
        variance_factor = 1.0f0  # Zero variance is perfectly consistent
    end
    
    # Factor 3: Distribution type contribution
    dtype = classify_distribution(dist)
    distribution_factor = if dtype == GAUSSIAN
        1.0f0
    elseif dtype == POISSON
        0.9f0
    elseif dtype == BIMODAL
        0.5f0
    elseif dtype == SKEWED_RIGHT || dtype == SKEWED_LEFT
        0.3f0
    elseif dtype == UNIFORM
        0.2f0  # Saturated/dead
    else
        0.5f0  # UNKNOWN
    end
    
    # Factor 4: Outlier indicator
    # High absolute skewness or kurtosis reduces confidence
    skew = abs(skewness(dist))
    kurt = abs(kurtosis(dist))
    outlier_factor = 1.0f0 / (1.0f0 + 0.5f0 * skew + 0.2f0 * kurt)
    
    # Weighted combination
    confidence = WEIGHT_SAMPLE_SIZE * sample_factor +
                 WEIGHT_VARIANCE * variance_factor +
                 WEIGHT_DISTRIBUTION * distribution_factor +
                 WEIGHT_OUTLIERS * outlier_factor
    
    return clamp(confidence, 0.0f0, 1.0f0)
end

"""
    compute_confidence(stats::NamedTuple) -> Float32

Compute confidence from finalized statistics tuple.
"""
function compute_confidence(stats::NamedTuple)::Float32
    if stats.n < 2
        return 0.0f0
    end
    
    # Sample size factor
    sample_factor = min(1.0f0, Float32(stats.n) / REF_SAMPLE_COUNT)
    
    # Variance factor
    if stats.variance > 0
        variance_factor = 1.0f0 / (1.0f0 + stats.variance / REF_VARIANCE)
    else
        variance_factor = 1.0f0
    end
    
    # Distribution type factor
    dtype = classify_distribution(stats)
    distribution_factor = if dtype == GAUSSIAN
        1.0f0
    elseif dtype == POISSON
        0.9f0
    elseif dtype == BIMODAL
        0.5f0
    elseif is_artifact_candidate(dtype)
        0.3f0
    else
        0.5f0
    end
    
    # Outlier factor
    outlier_factor = 1.0f0 / (1.0f0 + 0.5f0 * abs(stats.skewness) + 0.2f0 * abs(stats.kurtosis))
    
    confidence = WEIGHT_SAMPLE_SIZE * sample_factor +
                 WEIGHT_VARIANCE * variance_factor +
                 WEIGHT_DISTRIBUTION * distribution_factor +
                 WEIGHT_OUTLIERS * outlier_factor
    
    return clamp(confidence, 0.0f0, 1.0f0)
end

"""
    compute_pixel_result(dist::PixelDistribution, fused_value::Float32) -> PixelResult

Create a complete PixelResult from distribution and fused value.
"""
function compute_pixel_result(dist::PixelDistribution, fused_value::Float32)::PixelResult
    return PixelResult(
        fused_value,
        compute_confidence(dist),
        variance(dist),
        classify_distribution(dist)
    )
end

"""
    confidence_weight(dist::PixelDistribution) -> Float32

Compute a weight suitable for weighted averaging based on confidence.
Higher confidence = higher weight.
"""
function confidence_weight(dist::PixelDistribution)::Float32
    conf = compute_confidence(dist)
    var = variance(dist)
    
    # Combine confidence with inverse variance weighting
    if var > 0
        return conf / var
    else
        return conf * 1000.0f0  # High weight for zero variance
    end
end

end # module Confidence
