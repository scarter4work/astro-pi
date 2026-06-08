"""
Visualization utilities for debugging and analysis.

Generates confidence maps, distribution overlays, and diagnostic images.
"""
module ConfidenceMaps

using ..BayesianAstro: PixelResult, DistributionType,
                       GAUSSIAN, POISSON, BIMODAL, SKEWED_RIGHT, SKEWED_LEFT, UNIFORM, UNKNOWN
using Images: RGB, Gray, colorview

export generate_confidence_map, generate_classification_map
export apply_confidence_colormap, overlay_confidence

# Color maps for distribution types
const DTYPE_COLORS = Dict{DistributionType, Tuple{Float32,Float32,Float32}}(
    GAUSSIAN => (0.0f0, 1.0f0, 0.0f0),      # Green - reliable
    POISSON => (0.0f0, 0.8f0, 0.2f0),       # Light green - reliable
    BIMODAL => (1.0f0, 1.0f0, 0.0f0),       # Yellow - caution
    SKEWED_RIGHT => (1.0f0, 0.5f0, 0.0f0),  # Orange - artifact likely
    SKEWED_LEFT => (1.0f0, 0.3f0, 0.0f0),   # Red-orange - artifact likely
    UNIFORM => (1.0f0, 0.0f0, 0.0f0),       # Red - bad pixel
    UNKNOWN => (0.5f0, 0.5f0, 0.5f0)        # Gray - unknown
)

"""
    generate_confidence_map(results::Matrix{PixelResult}) -> Matrix{Float32}

Generate a grayscale confidence map from results.
Values range from 0.0 (low confidence) to 1.0 (high confidence).
"""
function generate_confidence_map(results::Matrix{PixelResult})::Matrix{Float32}
    return Float32[r.confidence for r in results]
end

"""
    generate_classification_map(results::Matrix{PixelResult}) -> Matrix{RGB{Float32}}

Generate a color-coded map showing distribution classifications.
"""
function generate_classification_map(results::Matrix{PixelResult})
    height, width = size(results)
    
    output = Matrix{RGB{Float32}}(undef, height, width)
    
    for j in 1:width
        for i in 1:height
            dtype = results[i, j].distribution_type
            r, g, b = get(DTYPE_COLORS, dtype, (0.5f0, 0.5f0, 0.5f0))
            output[i, j] = RGB{Float32}(r, g, b)
        end
    end
    
    return output
end

"""
    apply_confidence_colormap(confidence::Matrix{Float32}; 
                               colormap=:viridis) -> Matrix{RGB{Float32}}

Apply a colormap to confidence values for visualization.

# Arguments
- `confidence`: Matrix of confidence values (0.0 - 1.0)
- `colormap`: Color scheme (:viridis, :plasma, :inferno, :magma, :hot, :cool)
"""
function apply_confidence_colormap(confidence::Matrix{Float32}; 
                                    colormap::Symbol=:viridis)
    height, width = size(confidence)
    output = Matrix{RGB{Float32}}(undef, height, width)
    
    for j in 1:width
        for i in 1:height
            c = clamp(confidence[i, j], 0.0f0, 1.0f0)
            r, g, b = colormap_value(c, colormap)
            output[i, j] = RGB{Float32}(r, g, b)
        end
    end
    
    return output
end

"""
Apply colormap to a single value.
"""
function colormap_value(v::Float32, cmap::Symbol)::Tuple{Float32,Float32,Float32}
    if cmap == :viridis
        # Simplified viridis approximation
        r = clamp(0.267f0 + v * (0.993f0 - 0.267f0), 0f0, 1f0)
        g = clamp(0.004f0 + v * 0.906f0, 0f0, 1f0)
        b = clamp(0.329f0 + v * (0.143f0 - 0.329f0 + 0.5f0 * sin(v * 3.14159f0)), 0f0, 1f0)
        return (r, g, b)
    elseif cmap == :hot
        # Hot colormap: black -> red -> yellow -> white
        r = clamp(3.0f0 * v, 0f0, 1f0)
        g = clamp(3.0f0 * v - 1.0f0, 0f0, 1f0)
        b = clamp(3.0f0 * v - 2.0f0, 0f0, 1f0)
        return (r, g, b)
    elseif cmap == :cool
        # Cool colormap: cyan -> magenta
        return (v, 1.0f0 - v, 1.0f0)
    else
        # Default grayscale
        return (v, v, v)
    end
end

"""
    overlay_confidence(image::Matrix{Float32}, confidence::Matrix{Float32};
                       alpha=0.3, threshold=0.5) -> Matrix{RGB{Float32}}

Overlay confidence information on an image.
Low confidence regions are highlighted in red.

# Arguments
- `image`: Grayscale image (stretched)
- `confidence`: Confidence map
- `alpha`: Blend factor for overlay
- `threshold`: Confidence threshold below which to highlight
"""
function overlay_confidence(image::Matrix{Float32}, confidence::Matrix{Float32};
                            alpha::Float32=0.3f0, threshold::Float32=0.5f0)
    height, width = size(image)
    @assert size(confidence) == (height, width)
    
    output = Matrix{RGB{Float32}}(undef, height, width)
    
    for j in 1:width
        for i in 1:height
            img_val = clamp(image[i, j], 0f0, 1f0)
            conf = confidence[i, j]
            
            if conf < threshold
                # Blend with red for low confidence
                blend = alpha * (threshold - conf) / threshold
                r = img_val * (1 - blend) + blend
                g = img_val * (1 - blend)
                b = img_val * (1 - blend)
            else
                r = g = b = img_val
            end
            
            output[i, j] = RGB{Float32}(r, g, b)
        end
    end
    
    return output
end

"""
    confidence_histogram(confidence::Matrix{Float32}; bins=50) -> Tuple{Vector{Float32}, Vector{Int}}

Compute histogram of confidence values.

# Returns
- Tuple of (bin_centers, counts)
"""
function confidence_histogram(confidence::Matrix{Float32}; bins::Int=50)
    edges = range(0.0f0, 1.0f0, length=bins+1)
    centers = Float32[(edges[i] + edges[i+1]) / 2 for i in 1:bins]
    counts = zeros(Int, bins)
    
    for c in confidence
        bin = clamp(Int(floor(c * bins)) + 1, 1, bins)
        counts[bin] += 1
    end
    
    return (centers, counts)
end

"""
    classification_summary(results::Matrix{PixelResult}) -> Dict{DistributionType, Float64}

Compute percentage breakdown of distribution classifications.
"""
function classification_summary(results::Matrix{PixelResult})::Dict{DistributionType,Float64}
    n_total = length(results)
    counts = Dict{DistributionType, Int}()
    
    for r in results
        counts[r.distribution_type] = get(counts, r.distribution_type, 0) + 1
    end
    
    return Dict(k => 100.0 * v / n_total for (k, v) in counts)
end

end # module ConfidenceMaps
