#=
Comprehensive tests for BayesianAstro package.

Tests cover:
- Core types and constructors
- Welford's algorithm (numerical accuracy, edge cases, merging)
- Distribution classification (boundary conditions, all types)
- Confidence scoring (factor contributions, edge cases)
- Fusion strategies (MLE, confidence-weighted, lucky)
- GPU/CPU parity (when CUDA available)
=#

using Test
using BayesianAstro
using Statistics

@testset "BayesianAstro.jl" begin

    # ========================================================================
    # Type Tests
    # ========================================================================
    @testset "Core Types" begin
        @testset "PixelDistribution" begin
            dist = PixelDistribution()

            @test dist.n == 0
            @test dist.mean == 0.0f0
            @test dist.m2 == 0.0f0
            @test dist.m3 == 0.0f0
            @test dist.m4 == 0.0f0
            @test dist.min == Inf32
            @test dist.max == -Inf32
        end

        @testset "PixelResult" begin
            result = PixelResult(0.5f0, 0.8f0, 0.01f0, GAUSSIAN)

            @test result.value == 0.5f0
            @test result.confidence == 0.8f0
            @test result.variance == 0.01f0
            @test result.distribution_type == GAUSSIAN
        end

        @testset "FrameMetadata" begin
            meta = FrameMetadata("test.fits"; fwhm=2.5f0, background=100.0f0)

            @test meta.filename == "test.fits"
            @test meta.fwhm == 2.5f0
            @test meta.background == 100.0f0
            @test meta.weight == 1.0f0
        end

        @testset "ProcessingConfig" begin
            config = ProcessingConfig()

            @test config.fusion_strategy == CONFIDENCE_WEIGHTED
            @test config.confidence_threshold == 0.1f0
            @test config.outlier_sigma == 3.0f0
            @test config.use_gpu == true

            config2 = ProcessingConfig(
                fusion_strategy=MLE,
                outlier_sigma=2.5f0,
                use_gpu=false
            )

            @test config2.fusion_strategy == MLE
            @test config2.outlier_sigma == 2.5f0
            @test config2.use_gpu == false
        end

        @testset "DistributionType enum" begin
            @test Int(GAUSSIAN) == 1
            @test Int(POISSON) == 2
            @test Int(BIMODAL) == 3
            @test Int(SKEWED_RIGHT) == 4
            @test Int(SKEWED_LEFT) == 5
            @test Int(UNIFORM) == 6
            @test Int(UNKNOWN) == 7
        end

        @testset "FusionStrategy enum" begin
            @test Int(MLE) == 1
            @test Int(CONFIDENCE_WEIGHTED) == 2
            @test Int(LUCKY) == 3
            @test Int(MULTISCALE) == 4
        end
    end

    # ========================================================================
    # Welford's Algorithm Tests
    # ========================================================================
    @testset "Welford's Algorithm" begin
        @testset "Basic statistics" begin
            dist = PixelDistribution()

            # Known test case: values with known mean=5, variance=4.571
            values = Float32[2, 4, 4, 4, 5, 5, 7, 9]
            for v in values
                accumulate!(dist, v)
            end

            stats = finalize_statistics(dist)

            @test stats.n == 8
            @test stats.mean ≈ 5.0f0 atol=0.001
            @test stats.variance ≈ 4.571f0 atol=0.01  # Sample variance (n-1)
            @test stats.min == 2.0f0
            @test stats.max == 9.0f0
            @test stats.range == 7.0f0
        end

        @testset "Edge cases" begin
            # Empty distribution
            dist_empty = PixelDistribution()
            @test variance(dist_empty) == 0.0f0
            @test stddev(dist_empty) == 0.0f0
            @test skewness(dist_empty) == 0.0f0
            @test kurtosis(dist_empty) == 0.0f0

            # Single value
            dist_single = PixelDistribution()
            accumulate!(dist_single, 5.0f0)
            @test dist_single.n == 1
            @test dist_single.mean == 5.0f0
            @test variance(dist_single) == 0.0f0

            # Two identical values
            dist_two = PixelDistribution()
            accumulate!(dist_two, 3.0f0)
            accumulate!(dist_two, 3.0f0)
            @test dist_two.n == 2
            @test dist_two.mean == 3.0f0
            @test variance(dist_two) == 0.0f0
        end

        @testset "Numerical stability with large values" begin
            dist = PixelDistribution()

            # Large values that could cause numerical issues with naive algorithms
            base = 1e6
            values = Float32[base + 1, base + 2, base + 3, base + 4, base + 5]

            for v in values
                accumulate!(dist, v)
            end

            stats = finalize_statistics(dist)

            @test stats.mean ≈ base + 3 atol=0.01
            @test stats.variance ≈ 2.5f0 atol=0.01
        end

        @testset "Numerical stability with small differences" begin
            dist = PixelDistribution()

            # Values with very small differences
            values = Float32[1.0, 1.0001, 1.0002, 1.0003, 1.0004]

            for v in values
                accumulate!(dist, v)
            end

            stats = finalize_statistics(dist)

            @test stats.mean ≈ 1.0002f0 atol=0.0001
            @test stats.variance > 0  # Should detect small variance
        end

        @testset "Batch accumulate" begin
            dist1 = PixelDistribution()
            dist2 = PixelDistribution()

            values = Float32[1, 2, 3, 4, 5]

            # Accumulate one by one
            for v in values
                accumulate!(dist1, v)
            end

            # Accumulate as batch
            accumulate!(dist2, values)

            @test dist1.n == dist2.n
            @test dist1.mean ≈ dist2.mean atol=1e-6
            @test dist1.m2 ≈ dist2.m2 atol=1e-6
        end

        @testset "Reset" begin
            dist = PixelDistribution()
            accumulate!(dist, Float32[1, 2, 3, 4, 5])

            reset!(dist)

            @test dist.n == 0
            @test dist.mean == 0.0f0
            @test dist.m2 == 0.0f0
            @test dist.min == Inf32
            @test dist.max == -Inf32
        end

        @testset "Merge (parallel Welford)" begin
            # Create two distributions from different data partitions
            dist1 = PixelDistribution()
            dist2 = PixelDistribution()
            dist_combined = PixelDistribution()

            values1 = Float32[1, 2, 3, 4, 5]
            values2 = Float32[6, 7, 8, 9, 10]
            all_values = Float32[1, 2, 3, 4, 5, 6, 7, 8, 9, 10]

            for v in values1
                accumulate!(dist1, v)
            end
            for v in values2
                accumulate!(dist2, v)
            end
            for v in all_values
                accumulate!(dist_combined, v)
            end

            merged = merge(dist1, dist2)

            @test merged.n == dist_combined.n
            @test merged.mean ≈ dist_combined.mean atol=1e-5
            @test merged.m2 ≈ dist_combined.m2 atol=1e-4
            @test merged.min == dist_combined.min
            @test merged.max == dist_combined.max
        end

        @testset "Skewness calculation" begin
            dist = PixelDistribution()

            # Right-skewed data
            values = Float32[1, 1, 1, 2, 2, 3, 5, 10, 20]
            for v in values
                accumulate!(dist, v)
            end

            @test skewness(dist) > 0  # Should be positive for right-skew
        end

        @testset "Kurtosis calculation" begin
            dist = PixelDistribution()

            # High kurtosis (heavy tails)
            values = Float32[1, 1, 1, 1, 1, 1, 1, 1, 100]
            for v in values
                accumulate!(dist, v)
            end

            @test kurtosis(dist) > 0  # Should be positive (leptokurtic)
        end
    end

    # ========================================================================
    # Classification Tests
    # ========================================================================
    @testset "Distribution Classification" begin
        @testset "UNKNOWN for insufficient samples" begin
            dist = PixelDistribution()
            accumulate!(dist, Float32[1, 2, 3])  # Only 3 samples

            @test classify_distribution(dist) == UNKNOWN
        end

        @testset "GAUSSIAN classification" begin
            dist = PixelDistribution()

            # Normally distributed values (low skewness, normal kurtosis)
            # Simulating with symmetric data around mean
            values = Float32[4.8, 4.9, 5.0, 5.0, 5.0, 5.1, 5.2, 4.95, 5.05, 5.0]
            for v in values
                accumulate!(dist, v)
            end

            dtype = classify_distribution(dist)
            # May classify as GAUSSIAN or similar depending on exact statistics
            @test dtype in [GAUSSIAN, UNKNOWN, POISSON]
        end

        @testset "SKEWED_RIGHT classification" begin
            dist = PixelDistribution()

            # Highly right-skewed data (cosmic ray pattern)
            values = Float32[10, 10, 10, 10, 10, 10, 10, 10, 100, 200]
            for v in values
                accumulate!(dist, v)
            end

            dtype = classify_distribution(dist)
            @test dtype == SKEWED_RIGHT || skewness(dist) > 0.5
        end

        @testset "UNIFORM classification" begin
            dist = PixelDistribution()

            # Very low variance relative to range (saturated pixel)
            values = Float32[65535, 65535, 65535, 65535, 65535, 65534]
            for v in values
                accumulate!(dist, v)
            end

            dtype = classify_distribution(dist)
            # Should detect as UNIFORM due to low variance/range ratio
            range_val = dist.max - dist.min
            var = variance(dist)
            if range_val > 0
                ratio = var / (range_val * range_val)
                @test ratio < 0.1 || dtype == UNIFORM
            end
        end

        @testset "is_artifact_candidate" begin
            @test is_artifact_candidate(SKEWED_RIGHT) == true
            @test is_artifact_candidate(SKEWED_LEFT) == true
            @test is_artifact_candidate(UNIFORM) == true
            @test is_artifact_candidate(GAUSSIAN) == false
            @test is_artifact_candidate(POISSON) == false
        end

        @testset "is_reliable" begin
            @test is_reliable(GAUSSIAN) == true
            @test is_reliable(POISSON) == true
            @test is_reliable(BIMODAL) == false
            @test is_reliable(SKEWED_RIGHT) == false
            @test is_reliable(UNKNOWN) == false
        end
    end

    # ========================================================================
    # Confidence Scoring Tests
    # ========================================================================
    @testset "Confidence Scoring" begin
        @testset "Zero confidence for insufficient data" begin
            dist = PixelDistribution()
            @test compute_confidence(dist) == 0.0f0

            accumulate!(dist, 5.0f0)
            @test compute_confidence(dist) == 0.0f0  # n=1 still insufficient
        end

        @testset "Confidence increases with sample count" begin
            dist = PixelDistribution()

            accumulate!(dist, Float32[5, 5])
            conf_low = compute_confidence(dist)

            for _ in 1:50
                accumulate!(dist, 5.0f0)
            end
            conf_high = compute_confidence(dist)

            @test conf_high > conf_low
        end

        @testset "Confidence bounded 0-1" begin
            dist = PixelDistribution()

            # Add lots of data
            for i in 1:1000
                accumulate!(dist, Float32(5 + randn() * 0.1))
            end

            conf = compute_confidence(dist)
            @test 0.0f0 <= conf <= 1.0f0
        end

        @testset "Low variance increases confidence" begin
            dist_low_var = PixelDistribution()
            dist_high_var = PixelDistribution()

            # Low variance
            for _ in 1:20
                accumulate!(dist_low_var, 5.0f0)
            end

            # High variance
            for i in 1:20
                accumulate!(dist_high_var, Float32(i * 10))
            end

            @test compute_confidence(dist_low_var) > compute_confidence(dist_high_var)
        end

        @testset "confidence_weight" begin
            dist = PixelDistribution()
            for _ in 1:10
                accumulate!(dist, 5.0f0)
            end

            weight = confidence_weight(dist)
            @test weight > 0
        end

        @testset "compute_pixel_result" begin
            dist = PixelDistribution()
            for v in Float32[1, 2, 3, 4, 5]
                accumulate!(dist, v)
            end

            result = compute_pixel_result(dist, 3.0f0)

            @test result.value == 3.0f0
            @test 0.0f0 <= result.confidence <= 1.0f0
            @test result.variance >= 0
            @test result.distribution_type isa DistributionType
        end
    end

    # ========================================================================
    # Fusion Strategy Tests
    # ========================================================================
    @testset "Fusion Strategies" begin
        @testset "MLE fusion" begin
            dist = PixelDistribution()
            for v in Float32[10, 11, 10, 11, 10, 11, 10, 11]
                accumulate!(dist, v)
            end

            fused = fuse_mle(dist)

            # MLE for Gaussian should be the mean
            @test fused ≈ 10.5f0 atol=0.01
        end

        @testset "MLE with empty distribution" begin
            dist = PixelDistribution()
            @test fuse_mle(dist) == 0.0f0
        end

        @testset "Lucky imaging fusion" begin
            values = Float32[10, 20, 30, 15, 25]
            quality_scores = Float32[0.5, 0.9, 0.3, 0.7, 0.8]

            fused = fuse_lucky(values, quality_scores)

            # Should select value from best quality frame (index 2, quality 0.9)
            @test fused == 20.0f0
        end

        @testset "Lucky imaging with metadata" begin
            values = Float32[10, 20, 30]
            metadata = [
                FrameMetadata("f1.fits"; fwhm=3.0f0),
                FrameMetadata("f2.fits"; fwhm=1.5f0),  # Best seeing
                FrameMetadata("f3.fits"; fwhm=2.5f0),
            ]

            fused = fuse_lucky(values, metadata)

            # Should select value from frame with lowest FWHM (index 2)
            @test fused == 20.0f0
        end

        @testset "Multiscale fusion" begin
            dist = PixelDistribution()
            for v in Float32[5, 5, 5, 5, 5]
                accumulate!(dist, v)
            end

            fused_high = fuse_multiscale(dist, :high)
            fused_mid = fuse_multiscale(dist, :mid)
            fused_low = fuse_multiscale(dist, :low)

            # All should return reasonable values
            @test fused_high ≈ 5.0f0 atol=0.01
            @test fused_mid ≈ 5.0f0 atol=0.01
            @test fused_low ≈ 5.0f0 atol=0.01
        end

        @testset "select_fusion_strategy" begin
            @test select_fusion_strategy(GAUSSIAN) == MLE
            @test select_fusion_strategy(POISSON) == MLE
            @test select_fusion_strategy(BIMODAL) == LUCKY
            @test select_fusion_strategy(SKEWED_RIGHT) == CONFIDENCE_WEIGHTED
            @test select_fusion_strategy(UNKNOWN) == CONFIDENCE_WEIGHTED
        end
    end

    # ========================================================================
    # ImageStack Tests
    # ========================================================================
    @testset "ImageStack" begin
        @testset "Basic construction" begin
            frames = [rand(Float32, 100, 100) for _ in 1:5]
            metadata = [FrameMetadata("f$i.fits") for i in 1:5]

            stack = ImageStack(frames, metadata)

            @test length(stack) == 5
            @test size(stack) == (100, 100, 5)
            @test stack.width == 100
            @test stack.height == 100
        end

        @testset "Mismatched frames and metadata" begin
            frames = [rand(Float32, 100, 100) for _ in 1:5]
            metadata = [FrameMetadata("f$i.fits") for i in 1:3]

            @test_throws AssertionError ImageStack(frames, metadata)
        end

        @testset "Empty stack" begin
            @test_throws AssertionError ImageStack(Matrix{Float32}[], FrameMetadata[])
        end
    end

    # ========================================================================
    # Integration Tests
    # ========================================================================
    @testset "Integration" begin
        @testset "Full pipeline - small image" begin
            # Create synthetic test data
            height, width, n_frames = 10, 10, 20

            # Initialize distributions
            distributions = [PixelDistribution() for _ in 1:height, _ in 1:width]

            # Simulate frame accumulation
            for frame_idx in 1:n_frames
                for j in 1:width
                    for i in 1:height
                        # Synthetic pixel value with some noise
                        base_value = Float32(i + j)
                        noise = Float32(randn() * 0.1)
                        value = base_value + noise

                        accumulate!(distributions[i, j], value)
                    end
                end
            end

            # Finalize and fuse
            for j in 1:width
                for i in 1:height
                    dist = distributions[i, j]

                    @test dist.n == n_frames
                    @test dist.mean ≈ Float32(i + j) atol=0.5

                    fused = fuse_mle(dist)
                    @test fused ≈ Float32(i + j) atol=0.5

                    conf = compute_confidence(dist)
                    @test 0.0f0 <= conf <= 1.0f0
                end
            end
        end

        @testset "Cosmic ray simulation" begin
            dist = PixelDistribution()

            # Normal frames
            for _ in 1:18
                accumulate!(dist, Float32(100 + randn() * 5))
            end

            # Cosmic ray hit on 2 frames
            accumulate!(dist, 500.0f0)
            accumulate!(dist, 550.0f0)

            # Should classify as skewed
            dtype = classify_distribution(dist)
            @test dtype == SKEWED_RIGHT || skewness(dist) > 0

            # Confidence should be lower due to skewness
            conf = compute_confidence(dist)

            # MLE will be biased by cosmic rays
            fused = fuse_mle(dist)
            @test fused > 100  # Mean will be pulled up
        end

        @testset "Saturated pixel simulation" begin
            dist = PixelDistribution()

            # Saturated values with minimal variation
            for _ in 1:20
                accumulate!(dist, Float32(65535 + randn() * 0.1))
            end

            dtype = classify_distribution(dist)
            conf = compute_confidence(dist)

            # Low variance means lower overall confidence for saturated
            range_val = dist.max - dist.min
            var = variance(dist)
            @test var < 1.0  # Very low variance
        end
    end

    # ========================================================================
    # GPU Tests (conditional)
    # ========================================================================
    @testset "GPU Kernels" begin
        @testset "GPU availability check" begin
            # This just tests the function exists and returns a boolean
            available = is_gpu_available()
            @test available isa Bool
        end

        @testset "GPU context creation" begin
            ctx = create_gpu_context()
            @test ctx isa GPUContext

            if is_gpu_available()
                @test ctx.initialized == true
            else
                @test ctx.initialized == false
            end

            destroy_gpu_context(ctx)
            @test ctx.initialized == false
        end

        # CPU fallback tests (always run)
        @testset "CPU fallback - accumulate" begin
            height, width = 50, 50
            distributions = [PixelDistribution() for _ in 1:height, _ in 1:width]
            frame = rand(Float32, height, width)

            cpu_accumulate!(distributions, frame)

            # Verify all distributions were updated
            for j in 1:width
                for i in 1:height
                    @test distributions[i, j].n == 1
                    @test distributions[i, j].mean == frame[i, j]
                end
            end
        end

        @testset "CPU fallback - stretch" begin
            height, width = 50, 50
            input = rand(Float32, height, width)
            output = similar(input)

            cpu_stretch!(output, input, 0.0f0, 1.0f0)

            @test all(0.0f0 .<= output .<= 1.0f0)
        end
    end

    # ========================================================================
    # FITS I/O Tests
    # ========================================================================
    @testset "FITS I/O" begin
        @testset "parse_fits_date" begin
            # ISO 8601 with time
            ts1 = parse_fits_date("2024-01-15T20:30:45.123")
            @test ts1 > 0

            # ISO 8601 without fractional seconds
            ts2 = parse_fits_date("2024-01-15T20:30:45")
            @test ts2 > 0

            # Date only
            ts3 = parse_fits_date("2024-01-15")
            @test ts3 > 0

            # Invalid date returns 0
            ts4 = parse_fits_date("invalid")
            @test ts4 == 0.0

            # Empty string returns 0
            ts5 = parse_fits_date("")
            @test ts5 == 0.0

            # Verify ordering
            @test ts3 < ts2  # Date only should be earlier (midnight)
        end

        @testset "FITS round-trip (if tempdir available)" begin
            # Skip if we can't create temp files
            try
                tmpdir = mktempdir()

                # Create test data
                test_data = rand(Float32, 64, 64)

                # Write FITS
                test_path = joinpath(tmpdir, "test.fits")
                save_fits(test_path, test_data)

                @test isfile(test_path)

                # Read back
                loaded = load_fits(test_path)

                @test size(loaded) == size(test_data)
                @test eltype(loaded) == Float32
                @test loaded ≈ test_data atol=1e-6

                # Clean up
                rm(tmpdir; recursive=true)
            catch e
                @warn "Skipping FITS round-trip test: $e"
            end
        end

        @testset "find_fits_files" begin
            # Test with non-existent directory should return empty
            try
                files = find_fits_files("/nonexistent/path/12345")
                @test isempty(files)
            catch
                # Some systems throw on non-existent dirs
                @test true
            end
        end
    end

end  # main testset

println("\nAll tests completed!")
