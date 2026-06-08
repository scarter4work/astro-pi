"""
Distribution classification based on statistical moments.

Uses skewness, kurtosis, and other properties to classify the behavior
of each pixel's distribution across frames.
"""
module Classification

using ..BayesianAstro: PixelDistribution, DistributionType
using ..Welford: variance, skewness, kurtosis

export classify_distribution

# Classification thresholds (tunable)
const SKEWNESS_THRESHOLD = 0.5f0
const KURTOSIS_LOW = -0.5f0       # Platykurtic threshold
const KURTOSIS_HIGH = 1.0f0       # Leptokurtic threshold
const MIN_SAMPLES = 5             # Minimum samples for reliable classification
const VARIANCE_RATIO_THRESHOLD = 0.1f0  # For detecting uniform/saturated

"""
    classify_distribution(dist::PixelDistribution) -> DistributionType

Classify a pixel's distribution based on its statistical moments.

# Classification Logic
- UNKNOWN: Insufficient samples (n < 5)
- UNIFORM: Very low variance relative to range (saturated/dead pixel)
- GAUSSIAN: Low skewness, normal kurtosis
- POISSON: Positive skewness, variance ≈ mean (low signal)
- SKEWED_RIGHT: High positive skewness (cosmic ray/hot pixel)
- SKEWED_LEFT: High negative skewness (dark artifact)
- BIMODAL: High kurtosis (star + background, or artifact)
"""
function classify_distribution(dist::PixelDistribution)::DistributionType
    # Insufficient data
    if dist.n < MIN_SAMPLES
        return UNKNOWN
    end
    
    var = variance(dist)
    skew = skewness(dist)
    kurt = kurtosis(dist)
    range = dist.max - dist.min
    
    # Check for uniform/saturated (very low variance relative to range)
    if range > 0 && var / (range * range) < VARIANCE_RATIO_THRESHOLD
        return UNIFORM
    end
    
    # Check for Poisson-like (low signal regime)
    # In Poisson, variance ≈ mean
    if dist.mean > 0 && abs(var - dist.mean) / dist.mean < 0.3f0 && skew > 0
        return POISSON
    end
    
    # Check for bimodal (high excess kurtosis can indicate bimodality)
    # Note: Bimodal distributions often have negative excess kurtosis
    if kurt < KURTOSIS_LOW
        return BIMODAL
    end
    
    # Check for heavy tails (leptokurtic) + skewness
    if abs(skew) > SKEWNESS_THRESHOLD
        if skew > 0
            return SKEWED_RIGHT  # Potential cosmic ray / hot pixel
        else
            return SKEWED_LEFT   # Potential dark artifact
        end
    end
    
    # Default to Gaussian if nothing else matches
    if abs(skew) <= SKEWNESS_THRESHOLD && abs(kurt) <= KURTOSIS_HIGH
        return GAUSSIAN
    end
    
    # Catch-all
    return UNKNOWN
end

"""
    classify_distribution(stats::NamedTuple) -> DistributionType

Classify from finalized statistics tuple.
"""
function classify_distribution(stats::NamedTuple)::DistributionType
    # Create temporary distribution from stats
    dist = PixelDistribution()
    dist.n = UInt16(stats.n)
    dist.mean = stats.mean
    dist.m2 = stats.variance * (stats.n - 1)  # Reverse Bessel correction
    dist.min = stats.min
    dist.max = stats.max
    # Note: m3, m4 not preserved in stats tuple, so we use the values directly
    
    # Simplified classification when we have pre-computed skewness/kurtosis
    if stats.n < MIN_SAMPLES
        return UNKNOWN
    end
    
    range = stats.max - stats.min
    if range > 0 && stats.variance / (range * range) < VARIANCE_RATIO_THRESHOLD
        return UNIFORM
    end
    
    if stats.mean > 0 && abs(stats.variance - stats.mean) / stats.mean < 0.3f0 && stats.skewness > 0
        return POISSON
    end
    
    if stats.kurtosis < KURTOSIS_LOW
        return BIMODAL
    end
    
    if abs(stats.skewness) > SKEWNESS_THRESHOLD
        return stats.skewness > 0 ? SKEWED_RIGHT : SKEWED_LEFT
    end
    
    if abs(stats.skewness) <= SKEWNESS_THRESHOLD && abs(stats.kurtosis) <= KURTOSIS_HIGH
        return GAUSSIAN
    end
    
    return UNKNOWN
end

"""
    is_artifact_candidate(dtype::DistributionType) -> Bool

Check if a distribution type suggests the pixel may be affected by artifacts.
"""
function is_artifact_candidate(dtype::DistributionType)::Bool
    return dtype == SKEWED_RIGHT || dtype == SKEWED_LEFT || dtype == UNIFORM
end

"""
    is_reliable(dtype::DistributionType) -> Bool

Check if a distribution type indicates reliable signal.
"""
function is_reliable(dtype::DistributionType)::Bool
    return dtype == GAUSSIAN || dtype == POISSON
end

end # module Classification
