"""
High-level pipeline orchestration for the Bayesian stacking workflow.
"""
module Pipeline

using ..BayesianAstro: PixelDistribution, PixelResult, DistributionType,
                       FrameMetadata, FusionStrategy, ProcessingConfig,
                       ImageStack, CUDA_AVAILABLE
using ..FitsIO: load_fits, save_fits, load_frame_sequence, find_fits_files
using ..Welford: accumulate!, finalize_statistics
using ..Classification: classify_distribution
using ..Confidence: compute_confidence, compute_pixel_result
using ..Strategies: fuse_mle, fuse_confidence_weighted
using ..Kernels: is_gpu_available, cpu_accumulate!, cpu_finalize!

export process_stack, process_directory, extract_values, extract_confidences

"""
    process_stack(stack::ImageStack, config::ProcessingConfig) -> Tuple{Matrix{Float32}, Matrix{Float32}}

Process an image stack and return fused image and confidence map.

# Arguments
- `stack`: ImageStack containing frames and metadata
- `config`: Processing configuration

# Returns
- Tuple of (fused_image, confidence_map) as Float32 matrices
"""
function process_stack(stack::ImageStack{T}, config::ProcessingConfig) where T
    height, width = stack.height, stack.width
    n_frames = length(stack)
    
    @info "Processing stack: $(width)×$(height) pixels, $n_frames frames"
    @info "Fusion strategy: $(config.fusion_strategy)"
    @info "GPU available: $(is_gpu_available() && config.use_gpu)"
    
    # Initialize distribution array
    distributions = [PixelDistribution() for _ in 1:height, _ in 1:width]
    
    # Phase 1: Accumulate statistics
    @info "Phase 1: Accumulating statistics..."
    t_start = time()
    
    for (frame_idx, frame) in enumerate(stack.frames)
        frame_f32 = Float32.(frame)
        
        if is_gpu_available() && config.use_gpu
            # GPU path (when implemented)
            # gpu_accumulate!(distributions_gpu, frame_gpu, frame_idx)
            cpu_accumulate!(distributions, frame_f32)
        else
            cpu_accumulate!(distributions, frame_f32)
        end
        
        if frame_idx % 10 == 0 || frame_idx == n_frames
            elapsed = time() - t_start
            fps = frame_idx / elapsed
            @info "  Frame $frame_idx/$n_frames ($(round(fps, digits=1)) fps)"
        end
    end
    
    @info "  Accumulation complete in $(round(time() - t_start, digits=2))s"
    
    # Phase 2: Finalize and fuse
    @info "Phase 2: Finalizing and fusing..."
    t_start = time()
    
    results = cpu_finalize!(distributions)
    
    @info "  Finalization complete in $(round(time() - t_start, digits=2))s"
    
    # Extract output arrays
    fused_image = extract_values(results)
    confidence_map = extract_confidences(results)
    
    # Log statistics
    log_result_statistics(results)
    
    return (fused_image, confidence_map)
end

"""
    process_directory(input_dir::String, output_path::String; 
                      config=ProcessingConfig()) -> Nothing

Process all FITS files in a directory and save results.

# Arguments
- `input_dir`: Directory containing FITS files
- `output_path`: Base path for output files (without extension)
- `config`: Processing configuration
"""
function process_directory(input_dir::String, output_path::String;
                           config::ProcessingConfig=ProcessingConfig())
    # Find FITS files
    files = find_fits_files(input_dir)
    
    if isempty(files)
        error("No FITS files found in $input_dir")
    end
    
    @info "Found $(length(files)) FITS files"
    
    # Load stack
    stack = load_frame_sequence(files)
    
    # Process
    fused, confidence = process_stack(stack, config)
    
    # Save outputs
    fused_path = output_path * "_fused.fits"
    conf_path = output_path * "_confidence.fits"
    
    save_fits(fused_path, fused; header_cards=Dict(
        "BAYESIAN" => true,
        "NFRAMES" => length(stack),
        "FUSION" => string(config.fusion_strategy)
    ))
    
    save_fits(conf_path, confidence; header_cards=Dict(
        "DATATYPE" => "CONFIDENCE",
        "RANGE" => "0.0-1.0"
    ))
    
    @info "Saved fused image to: $fused_path"
    @info "Saved confidence map to: $conf_path"
    
    return nothing
end

"""
    extract_values(results::Matrix{PixelResult}) -> Matrix{Float32}

Extract fused values from result matrix.
"""
function extract_values(results::Matrix{PixelResult})::Matrix{Float32}
    return Float32[r.value for r in results]
end

"""
    extract_confidences(results::Matrix{PixelResult}) -> Matrix{Float32}

Extract confidence values from result matrix.
"""
function extract_confidences(results::Matrix{PixelResult})::Matrix{Float32}
    return Float32[r.confidence for r in results]
end

"""
    extract_variances(results::Matrix{PixelResult}) -> Matrix{Float32}

Extract variance values from result matrix.
"""
function extract_variances(results::Matrix{PixelResult})::Matrix{Float32}
    return Float32[r.variance for r in results]
end

"""
    extract_classifications(results::Matrix{PixelResult}) -> Matrix{DistributionType}

Extract distribution classifications from result matrix.
"""
function extract_classifications(results::Matrix{PixelResult})::Matrix{DistributionType}
    return DistributionType[r.distribution_type for r in results]
end

"""
Log statistics about the processing results.
"""
function log_result_statistics(results::Matrix{PixelResult})
    n_pixels = length(results)
    
    # Count distribution types
    type_counts = Dict{DistributionType, Int}()
    total_confidence = 0.0f0
    
    for r in results
        type_counts[r.distribution_type] = get(type_counts, r.distribution_type, 0) + 1
        total_confidence += r.confidence
    end
    
    @info "Result statistics:"
    @info "  Total pixels: $n_pixels"
    @info "  Mean confidence: $(round(total_confidence / n_pixels, digits=3))"
    @info "  Distribution types:"
    
    for (dtype, count) in sort(collect(type_counts), by=x->-x[2])
        pct = round(100.0 * count / n_pixels, digits=1)
        @info "    $dtype: $count ($pct%)"
    end
end

end # module Pipeline
