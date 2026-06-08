"""
Welford's online algorithm for numerically stable running statistics.

This module implements single-pass computation of mean, variance, skewness,
and kurtosis using Welford's method, which avoids catastrophic cancellation
issues that plague naive sum-of-squares approaches.

Reference: Welford, B. P. (1962). "Note on a method for calculating corrected 
           sums of squares and products". Technometrics. 4 (3): 419–420.
"""
module Welford

using ..BayesianAstro: PixelDistribution

export accumulate!, finalize_statistics, reset!
export variance, stddev, skewness, kurtosis

"""
    accumulate!(dist::PixelDistribution, value::Float32)

Update running statistics with a new observation using Welford's algorithm.
Computes mean, variance, skewness, and kurtosis in a single pass.
"""
function accumulate!(dist::PixelDistribution, value::Float32)
    # Update count
    n1 = dist.n
    dist.n += 1
    n = dist.n
    
    # Update min/max
    dist.min = min(dist.min, value)
    dist.max = max(dist.max, value)
    
    # Welford's algorithm for mean and higher moments
    delta = value - dist.mean
    delta_n = delta / n
    delta_n2 = delta_n * delta_n
    term1 = delta * delta_n * n1
    
    # Update mean
    dist.mean += delta_n
    
    # Update M4 (must be done before M3 and M2)
    dist.m4 += term1 * delta_n2 * (n*n - 3*n + 3) + 
               6 * delta_n2 * dist.m2 - 
               4 * delta_n * dist.m3
    
    # Update M3 (must be done before M2)
    dist.m3 += term1 * delta_n * (n - 2) - 3 * delta_n * dist.m2
    
    # Update M2
    dist.m2 += term1
    
    return dist
end

"""
    accumulate!(dist::PixelDistribution, values::AbstractVector{Float32})

Batch update running statistics with multiple observations.
"""
function accumulate!(dist::PixelDistribution, values::AbstractVector{Float32})
    for value in values
        accumulate!(dist, value)
    end
    return dist
end

"""
    reset!(dist::PixelDistribution)

Reset distribution to initial state.
"""
function reset!(dist::PixelDistribution)
    dist.n = 0
    dist.mean = 0.0f0
    dist.m2 = 0.0f0
    dist.m3 = 0.0f0
    dist.m4 = 0.0f0
    dist.min = Inf32
    dist.max = -Inf32
    return dist
end

"""
    variance(dist::PixelDistribution; corrected=true) -> Float32

Compute variance from accumulated statistics.
Uses Bessel's correction (n-1) by default for sample variance.
"""
function variance(dist::PixelDistribution; corrected::Bool=true)::Float32
    if dist.n < 2
        return 0.0f0
    end
    
    if corrected
        return dist.m2 / (dist.n - 1)
    else
        return dist.m2 / dist.n
    end
end

"""
    stddev(dist::PixelDistribution; corrected=true) -> Float32

Compute standard deviation from accumulated statistics.
"""
function stddev(dist::PixelDistribution; corrected::Bool=true)::Float32
    return sqrt(variance(dist; corrected=corrected))
end

"""
    skewness(dist::PixelDistribution) -> Float32

Compute skewness (third standardized moment) from accumulated statistics.
Returns 0 for distributions with fewer than 3 samples.
"""
function skewness(dist::PixelDistribution)::Float32
    if dist.n < 3 || dist.m2 ≈ 0.0f0
        return 0.0f0
    end
    
    # Fisher's skewness: g1 = m3 / m2^(3/2) * sqrt(n)
    return sqrt(Float32(dist.n)) * dist.m3 / (dist.m2^1.5f0)
end

"""
    kurtosis(dist::PixelDistribution; excess=true) -> Float32

Compute kurtosis (fourth standardized moment) from accumulated statistics.
Returns excess kurtosis by default (normal distribution = 0).
Returns 0 for distributions with fewer than 4 samples.
"""
function kurtosis(dist::PixelDistribution; excess::Bool=true)::Float32
    if dist.n < 4 || dist.m2 ≈ 0.0f0
        return 0.0f0
    end
    
    # Kurt = n * m4 / m2^2
    kurt = Float32(dist.n) * dist.m4 / (dist.m2 * dist.m2)
    
    if excess
        return kurt - 3.0f0  # Excess kurtosis (normal = 0)
    else
        return kurt  # Raw kurtosis (normal = 3)
    end
end

"""
    finalize_statistics(dist::PixelDistribution) -> NamedTuple

Compute all final statistics from accumulated data.

# Returns
Named tuple with fields:
- `n`: Sample count
- `mean`: Sample mean  
- `variance`: Sample variance (Bessel corrected)
- `stddev`: Sample standard deviation
- `skewness`: Fisher's skewness
- `kurtosis`: Excess kurtosis
- `min`: Minimum value
- `max`: Maximum value
- `range`: max - min
"""
function finalize_statistics(dist::PixelDistribution)
    return (
        n = Int(dist.n),
        mean = dist.mean,
        variance = variance(dist),
        stddev = stddev(dist),
        skewness = skewness(dist),
        kurtosis = kurtosis(dist),
        min = dist.min,
        max = dist.max,
        range = dist.max - dist.min
    )
end

"""
    merge(dist1::PixelDistribution, dist2::PixelDistribution) -> PixelDistribution

Merge two PixelDistribution objects into one (parallel Welford).
Useful for combining statistics computed on separate data partitions.
"""
function Base.merge(dist1::PixelDistribution, dist2::PixelDistribution)::PixelDistribution
    result = PixelDistribution()
    
    if dist1.n == 0
        return deepcopy(dist2)
    elseif dist2.n == 0
        return deepcopy(dist1)
    end
    
    n1, n2 = Float32(dist1.n), Float32(dist2.n)
    n = n1 + n2
    
    delta = dist2.mean - dist1.mean
    delta2 = delta * delta
    delta3 = delta2 * delta
    delta4 = delta3 * delta
    
    result.n = UInt16(n)
    result.mean = (n1 * dist1.mean + n2 * dist2.mean) / n
    
    result.m2 = dist1.m2 + dist2.m2 + delta2 * n1 * n2 / n
    
    result.m3 = dist1.m3 + dist2.m3 + 
                delta3 * n1 * n2 * (n1 - n2) / (n * n) +
                3 * delta * (n1 * dist2.m2 - n2 * dist1.m2) / n
    
    result.m4 = dist1.m4 + dist2.m4 +
                delta4 * n1 * n2 * (n1*n1 - n1*n2 + n2*n2) / (n*n*n) +
                6 * delta2 * (n1*n1 * dist2.m2 + n2*n2 * dist1.m2) / (n*n) +
                4 * delta * (n1 * dist2.m3 - n2 * dist1.m3) / n
    
    result.min = min(dist1.min, dist2.min)
    result.max = max(dist1.max, dist2.max)
    
    return result
end

end # module Welford
