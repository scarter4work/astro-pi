#include "nukex/gpu/gpu_executor.hpp"
#include "nukex/gpu/fit_heartbeat.hpp"
#include "nukex/stacker/frame_cache.hpp"
#include "nukex/core/progress_observer.hpp"
#include <omp.h>

#if NUKEX_HAS_OPENCL
#define CL_TARGET_OPENCL_VERSION 300
#include <CL/cl.h>
#endif

#include <iostream>
#include <algorithm>
#include <cstring>

namespace nukex {

GPUExecutor::GPUExecutor(const GPUExecutorConfig& config)
    : context_(GPUContext::create(config)) {

    if (context_.is_gpu_available()) {
        if (!kernels_.compile(context_)) {
            std::cerr << "NukeX GPU: Kernel compilation failed, falling back to CPU\n";
            // Context stays valid but kernels aren't compiled — will use CPU path
        }
    }
}

// ══════════════════════════════════════════════════════════════════════
// CPU batch execution (reference path)
// ══════════════════════════════════════════════════════════════════════

void GPUExecutor::execute_batch_cpu(
    ShadowBuffers& buf, const FrameStats* fs,
    const WeightConfig& wc, int batch_size, int n_channels, int n_frames) {

    GPUCPUFallback::classify_weights(buf, fs, wc, batch_size, n_channels, n_frames);
    GPUCPUFallback::robust_stats(buf, batch_size, n_channels, n_frames);
}

// ══════════════════════════════════════════════════════════════════════
// GPU batch execution
// ══════════════════════════════════════════════════════════════════════

#if NUKEX_HAS_OPENCL

static cl_mem create_buf(cl_context ctx, cl_mem_flags flags, size_t size, void* host) {
    cl_int err;
    cl_mem mem = clCreateBuffer(ctx, flags, size, host, &err);
    if (err != CL_SUCCESS) {
        std::cerr << "NukeX GPU: clCreateBuffer failed (size=" << size << " err=" << err << ")\n";
        return nullptr;
    }
    return mem;
}

void GPUExecutor::execute_batch_gpu(
    ShadowBuffers& buf, const FrameStats* fs,
    const WeightConfig& wc, int batch_size, int n_channels, int n_frames) {

    if (!kernels_.is_compiled()) {
        execute_batch_cpu(buf, fs, wc, batch_size, n_channels, n_frames);
        return;
    }

    cl_context ctx = context_.context();
    cl_command_queue queue = context_.queue();
    int B = batch_size, C = n_channels, N = n_frames;

    // ── Create GPU buffers ──
    // Input buffers
    cl_mem d_welford_mean = create_buf(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        C * B * sizeof(float), buf.welford_mean.data());
    cl_mem d_welford_M2 = create_buf(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        C * B * sizeof(float), buf.welford_M2.data());
    cl_mem d_welford_n = create_buf(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        C * B * sizeof(uint32_t), buf.welford_n.data());
    cl_mem d_pixel_values = create_buf(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        C * N * B * sizeof(float), buf.pixel_values.data());
    cl_mem d_n_frames = create_buf(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        B * sizeof(uint16_t), buf.n_frames.data());

    // Frame-level constants
    std::vector<float> frame_weight(N), psf_weight(N), cloud_score(N), frame_exposure(N);
    std::vector<float> frame_read_noise(N), frame_gain(N);
    std::vector<uint8_t> frame_has_noise(N);
    for (int i = 0; i < N; i++) {
        frame_weight[i] = fs[i].frame_weight;
        psf_weight[i] = fs[i].psf_weight;
        cloud_score[i] = fs[i].cloud_score;
        frame_exposure[i] = fs[i].exposure;
        frame_read_noise[i] = fs[i].read_noise;
        frame_gain[i] = fs[i].gain;
        frame_has_noise[i] = fs[i].has_noise_keywords ? 1 : 0;
    }
    cl_mem d_frame_weight = create_buf(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        N * sizeof(float), frame_weight.data());
    cl_mem d_psf_weight = create_buf(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        N * sizeof(float), psf_weight.data());
    cl_mem d_cloud_score = create_buf(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        N * sizeof(float), cloud_score.data());
    cl_mem d_frame_exposure = create_buf(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        N * sizeof(float), frame_exposure.data());

    // Output/intermediate buffers
    cl_mem d_pixel_weights = create_buf(ctx, CL_MEM_READ_WRITE,
        C * N * B * sizeof(float), nullptr);
    cl_mem d_cloud_count = create_buf(ctx, CL_MEM_WRITE_ONLY, B * sizeof(uint16_t), nullptr);
    cl_mem d_trail_count = create_buf(ctx, CL_MEM_WRITE_ONLY, B * sizeof(uint16_t), nullptr);
    cl_mem d_worst_sigma = create_buf(ctx, CL_MEM_WRITE_ONLY, B * sizeof(float), nullptr);
    cl_mem d_best_sigma = create_buf(ctx, CL_MEM_WRITE_ONLY, B * sizeof(float), nullptr);
    cl_mem d_mean_weight = create_buf(ctx, CL_MEM_WRITE_ONLY, B * sizeof(float), nullptr);
    cl_mem d_total_exp = create_buf(ctx, CL_MEM_WRITE_ONLY, B * sizeof(float), nullptr);

    cl_mem d_mad = create_buf(ctx, CL_MEM_WRITE_ONLY, C * B * sizeof(float), nullptr);
    cl_mem d_bwmv = create_buf(ctx, CL_MEM_WRITE_ONLY, C * B * sizeof(float), nullptr);
    cl_mem d_iqr = create_buf(ctx, CL_MEM_WRITE_ONLY, C * B * sizeof(float), nullptr);

    // ── Dispatch kernel 1: classify_weights ──
    {
        cl_kernel k = kernels_.classify_weights_kernel();
        int arg = 0;
        clSetKernelArg(k, arg++, sizeof(cl_mem), &d_welford_mean);
        clSetKernelArg(k, arg++, sizeof(cl_mem), &d_welford_M2);
        clSetKernelArg(k, arg++, sizeof(cl_mem), &d_welford_n);
        clSetKernelArg(k, arg++, sizeof(cl_mem), &d_pixel_values);
        clSetKernelArg(k, arg++, sizeof(cl_mem), &d_n_frames);
        clSetKernelArg(k, arg++, sizeof(cl_mem), &d_frame_weight);
        clSetKernelArg(k, arg++, sizeof(cl_mem), &d_psf_weight);
        clSetKernelArg(k, arg++, sizeof(cl_mem), &d_cloud_score);
        clSetKernelArg(k, arg++, sizeof(cl_mem), &d_frame_exposure);
        clSetKernelArg(k, arg++, sizeof(float), &wc.sigma_threshold);
        clSetKernelArg(k, arg++, sizeof(float), &wc.sigma_scale);
        clSetKernelArg(k, arg++, sizeof(float), &wc.weight_floor);
        clSetKernelArg(k, arg++, sizeof(int), &C);
        clSetKernelArg(k, arg++, sizeof(int), &N);
        clSetKernelArg(k, arg++, sizeof(int), &B);
        clSetKernelArg(k, arg++, sizeof(cl_mem), &d_pixel_weights);
        clSetKernelArg(k, arg++, sizeof(cl_mem), &d_cloud_count);
        clSetKernelArg(k, arg++, sizeof(cl_mem), &d_trail_count);
        clSetKernelArg(k, arg++, sizeof(cl_mem), &d_worst_sigma);
        clSetKernelArg(k, arg++, sizeof(cl_mem), &d_best_sigma);
        clSetKernelArg(k, arg++, sizeof(cl_mem), &d_mean_weight);
        clSetKernelArg(k, arg++, sizeof(cl_mem), &d_total_exp);

        size_t global_size = B;
        clEnqueueNDRangeKernel(queue, k, 1, nullptr, &global_size, nullptr, 0, nullptr, nullptr);
    }

    // ── Dispatch kernel 2: robust_stats ──
    {
        cl_kernel k = kernels_.robust_stats_kernel();
        int arg = 0;
        clSetKernelArg(k, arg++, sizeof(cl_mem), &d_pixel_values);
        clSetKernelArg(k, arg++, sizeof(cl_mem), &d_n_frames);
        clSetKernelArg(k, arg++, sizeof(int), &C);
        clSetKernelArg(k, arg++, sizeof(int), &N);
        clSetKernelArg(k, arg++, sizeof(int), &B);
        clSetKernelArg(k, arg++, sizeof(cl_mem), &d_mad);
        clSetKernelArg(k, arg++, sizeof(cl_mem), &d_bwmv);
        clSetKernelArg(k, arg++, sizeof(cl_mem), &d_iqr);

        size_t global_size = static_cast<size_t>(B) * C;
        clEnqueueNDRangeKernel(queue, k, 1, nullptr, &global_size, nullptr, 0, nullptr, nullptr);
    }

    // ── Read back results ──
    clFinish(queue);

    clEnqueueReadBuffer(queue, d_pixel_weights, CL_TRUE, 0,
        C * N * B * sizeof(float), buf.pixel_weights.data(), 0, nullptr, nullptr);
    clEnqueueReadBuffer(queue, d_cloud_count, CL_TRUE, 0,
        B * sizeof(uint16_t), buf.cloud_frame_count.data(), 0, nullptr, nullptr);
    clEnqueueReadBuffer(queue, d_trail_count, CL_TRUE, 0,
        B * sizeof(uint16_t), buf.trail_frame_count.data(), 0, nullptr, nullptr);
    clEnqueueReadBuffer(queue, d_worst_sigma, CL_TRUE, 0,
        B * sizeof(float), buf.worst_sigma_score.data(), 0, nullptr, nullptr);
    clEnqueueReadBuffer(queue, d_best_sigma, CL_TRUE, 0,
        B * sizeof(float), buf.best_sigma_score.data(), 0, nullptr, nullptr);
    clEnqueueReadBuffer(queue, d_mean_weight, CL_TRUE, 0,
        B * sizeof(float), buf.mean_weight_out.data(), 0, nullptr, nullptr);
    clEnqueueReadBuffer(queue, d_total_exp, CL_TRUE, 0,
        B * sizeof(float), buf.total_exposure_out.data(), 0, nullptr, nullptr);
    clEnqueueReadBuffer(queue, d_mad, CL_TRUE, 0,
        C * B * sizeof(float), buf.mad_out.data(), 0, nullptr, nullptr);
    clEnqueueReadBuffer(queue, d_bwmv, CL_TRUE, 0,
        C * B * sizeof(float), buf.biweight_midvar_out.data(), 0, nullptr, nullptr);
    clEnqueueReadBuffer(queue, d_iqr, CL_TRUE, 0,
        C * B * sizeof(float), buf.iqr_out.data(), 0, nullptr, nullptr);

    // ── Release GPU buffers ──
    clReleaseMemObject(d_welford_mean);
    clReleaseMemObject(d_welford_M2);
    clReleaseMemObject(d_welford_n);
    clReleaseMemObject(d_pixel_values);
    clReleaseMemObject(d_n_frames);
    clReleaseMemObject(d_frame_weight);
    clReleaseMemObject(d_psf_weight);
    clReleaseMemObject(d_cloud_score);
    clReleaseMemObject(d_frame_exposure);
    clReleaseMemObject(d_pixel_weights);
    clReleaseMemObject(d_cloud_count);
    clReleaseMemObject(d_trail_count);
    clReleaseMemObject(d_worst_sigma);
    clReleaseMemObject(d_best_sigma);
    clReleaseMemObject(d_mean_weight);
    clReleaseMemObject(d_total_exp);
    clReleaseMemObject(d_mad);
    clReleaseMemObject(d_bwmv);
    clReleaseMemObject(d_iqr);
}

// ── Kernel 3: select_pixels GPU dispatch ──

void GPUExecutor::execute_select_gpu(
    ShadowBuffers& buf, const FrameStats* fs,
    int batch_size, int n_channels, int n_frames) {

    if (!kernels_.is_compiled()) {
        GPUCPUFallback::select_pixels(buf, fs, batch_size, n_channels, n_frames);
        return;
    }

    cl_context ctx = context_.context();
    cl_command_queue queue = context_.queue();
    int B = batch_size, C = n_channels, N = n_frames;

    // Prepare frame-level noise model arrays
    std::vector<float> frame_read_noise(N), frame_gain(N);
    std::vector<uint8_t> frame_has_noise(N);
    for (int i = 0; i < N; i++) {
        frame_read_noise[i] = fs[i].read_noise;
        frame_gain[i] = fs[i].gain;
        frame_has_noise[i] = fs[i].has_noise_keywords ? 1 : 0;
    }

    // Create GPU buffers
    cl_mem d_dist_signal = create_buf(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        C * B * sizeof(float), buf.dist_true_signal.data());
    cl_mem d_pixel_values = create_buf(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        C * N * B * sizeof(float), buf.pixel_values.data());
    cl_mem d_pixel_weights = create_buf(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        C * N * B * sizeof(float), buf.pixel_weights.data());
    cl_mem d_n_frames = create_buf(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        B * sizeof(uint16_t), buf.n_frames.data());
    cl_mem d_read_noise = create_buf(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        N * sizeof(float), frame_read_noise.data());
    cl_mem d_gain = create_buf(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        N * sizeof(float), frame_gain.data());
    cl_mem d_has_noise = create_buf(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        N * sizeof(uint8_t), frame_has_noise.data());
    cl_mem d_welford_M2 = create_buf(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        C * B * sizeof(float), buf.welford_M2.data());
    cl_mem d_welford_n = create_buf(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        C * B * sizeof(uint32_t), buf.welford_n.data());

    cl_mem d_output = create_buf(ctx, CL_MEM_WRITE_ONLY, C * B * sizeof(float), nullptr);
    cl_mem d_noise = create_buf(ctx, CL_MEM_WRITE_ONLY, C * B * sizeof(float), nullptr);
    cl_mem d_snr = create_buf(ctx, CL_MEM_WRITE_ONLY, C * B * sizeof(float), nullptr);

    cl_kernel k = kernels_.select_pixels_kernel();
    int arg = 0;
    clSetKernelArg(k, arg++, sizeof(cl_mem), &d_dist_signal);
    clSetKernelArg(k, arg++, sizeof(cl_mem), &d_pixel_values);
    clSetKernelArg(k, arg++, sizeof(cl_mem), &d_pixel_weights);
    clSetKernelArg(k, arg++, sizeof(cl_mem), &d_n_frames);
    clSetKernelArg(k, arg++, sizeof(cl_mem), &d_read_noise);
    clSetKernelArg(k, arg++, sizeof(cl_mem), &d_gain);
    clSetKernelArg(k, arg++, sizeof(cl_mem), &d_has_noise);
    clSetKernelArg(k, arg++, sizeof(cl_mem), &d_welford_M2);
    clSetKernelArg(k, arg++, sizeof(cl_mem), &d_welford_n);
    clSetKernelArg(k, arg++, sizeof(int), &C);
    clSetKernelArg(k, arg++, sizeof(int), &N);
    clSetKernelArg(k, arg++, sizeof(int), &B);
    clSetKernelArg(k, arg++, sizeof(cl_mem), &d_output);
    clSetKernelArg(k, arg++, sizeof(cl_mem), &d_noise);
    clSetKernelArg(k, arg++, sizeof(cl_mem), &d_snr);

    size_t global_size = static_cast<size_t>(B) * C;
    clEnqueueNDRangeKernel(queue, k, 1, nullptr, &global_size, nullptr, 0, nullptr, nullptr);
    clFinish(queue);

    clEnqueueReadBuffer(queue, d_output, CL_TRUE, 0, C * B * sizeof(float), buf.output_value.data(), 0, nullptr, nullptr);
    clEnqueueReadBuffer(queue, d_noise, CL_TRUE, 0, C * B * sizeof(float), buf.noise_sigma.data(), 0, nullptr, nullptr);
    clEnqueueReadBuffer(queue, d_snr, CL_TRUE, 0, C * B * sizeof(float), buf.snr_out.data(), 0, nullptr, nullptr);

    clReleaseMemObject(d_dist_signal);
    clReleaseMemObject(d_pixel_values);
    clReleaseMemObject(d_pixel_weights);
    clReleaseMemObject(d_n_frames);
    clReleaseMemObject(d_read_noise);
    clReleaseMemObject(d_gain);
    clReleaseMemObject(d_has_noise);
    clReleaseMemObject(d_welford_M2);
    clReleaseMemObject(d_welford_n);
    clReleaseMemObject(d_output);
    clReleaseMemObject(d_noise);
    clReleaseMemObject(d_snr);
}

// ── Kernel 4: spatial_context GPU dispatch ──

void GPUExecutor::execute_spatial_gpu(
    const Image& stacked,
    float* gradient_mag, float* local_background, float* local_rms) {

    if (!kernels_.is_compiled()) {
        GPUCPUFallback::spatial_context(stacked.data(), stacked.width(),
            stacked.height(), stacked.n_channels(),
            gradient_mag, local_background, local_rms);
        return;
    }

    cl_context ctx = context_.context();
    cl_command_queue queue = context_.queue();
    int W = stacked.width(), H = stacked.height(), C = stacked.n_channels();
    int npix = W * H;

    cl_mem d_stacked = create_buf(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        C * npix * sizeof(float), const_cast<float*>(stacked.data()));
    cl_mem d_grad = create_buf(ctx, CL_MEM_WRITE_ONLY, npix * sizeof(float), nullptr);
    cl_mem d_bg = create_buf(ctx, CL_MEM_WRITE_ONLY, npix * sizeof(float), nullptr);
    cl_mem d_rms = create_buf(ctx, CL_MEM_WRITE_ONLY, npix * sizeof(float), nullptr);

    cl_kernel k = kernels_.spatial_context_kernel();
    int arg = 0;
    clSetKernelArg(k, arg++, sizeof(cl_mem), &d_stacked);
    clSetKernelArg(k, arg++, sizeof(int), &W);
    clSetKernelArg(k, arg++, sizeof(int), &H);
    clSetKernelArg(k, arg++, sizeof(int), &C);
    clSetKernelArg(k, arg++, sizeof(cl_mem), &d_grad);
    clSetKernelArg(k, arg++, sizeof(cl_mem), &d_bg);
    clSetKernelArg(k, arg++, sizeof(cl_mem), &d_rms);

    size_t global_size = static_cast<size_t>(npix);
    clEnqueueNDRangeKernel(queue, k, 1, nullptr, &global_size, nullptr, 0, nullptr, nullptr);
    clFinish(queue);

    clEnqueueReadBuffer(queue, d_grad, CL_TRUE, 0, npix * sizeof(float), gradient_mag, 0, nullptr, nullptr);
    clEnqueueReadBuffer(queue, d_bg, CL_TRUE, 0, npix * sizeof(float), local_background, 0, nullptr, nullptr);
    clEnqueueReadBuffer(queue, d_rms, CL_TRUE, 0, npix * sizeof(float), local_rms, 0, nullptr, nullptr);

    clReleaseMemObject(d_stacked);
    clReleaseMemObject(d_grad);
    clReleaseMemObject(d_bg);
    clReleaseMemObject(d_rms);
}

#else // !NUKEX_HAS_OPENCL

void GPUExecutor::execute_batch_gpu(
    ShadowBuffers& buf, const FrameStats* fs,
    const WeightConfig& wc, int batch_size, int n_channels, int n_frames) {
    execute_batch_cpu(buf, fs, wc, batch_size, n_channels, n_frames);
}

void GPUExecutor::execute_select_gpu(
    ShadowBuffers& buf, const FrameStats* fs,
    int batch_size, int n_channels, int n_frames) {
    GPUCPUFallback::select_pixels(buf, fs, batch_size, n_channels, n_frames);
}

void GPUExecutor::execute_spatial_gpu(
    const Image& stacked,
    float* gradient_mag, float* local_background, float* local_rms) {
    GPUCPUFallback::spatial_context(stacked.data(), stacked.width(),
        stacked.height(), stacked.n_channels(),
        gradient_mag, local_background, local_rms);
}

#endif

// ══════════════════════════════════════════════════════════════════════
// Phase B orchestration
// ══════════════════════════════════════════════════════════════════════

void GPUExecutor::execute_phase_b(
    Cube& cube,
    FrameCache& cache,
    const std::vector<FrameStats>& frame_stats,
    const WeightConfig& weight_config,
    FittingFn fitting_fn,
    Image& stacked_output,
    Image& noise_output,
    ProgressObserver* progress) {

    ProgressObserver& obs = progress ? *progress : null_progress_observer();

    int total_voxels = cube.total_pixels();
    int n_channels = cube.at(0, 0).n_channels;
    int n_frames = cache.n_frames_written();
    int N = std::min(n_frames, static_cast<int>(GPU_MAX_FRAMES));

    int batch_size = context_.estimate_batch_size(N, n_channels);
    batch_size = std::min(batch_size, total_voxels);

    ShadowBuffers buf;
    buf.allocate(batch_size, n_channels, N);

    bool use_gpu = context_.is_gpu_available() && kernels_.is_compiled();

    int total_batches = (total_voxels + batch_size - 1) / batch_size;
    obs.begin_phase("Phase B: Distribution fitting", total_batches);
    obs.advance(0, std::to_string(total_voxels) + " voxels, "
                   + std::to_string(total_batches) + " batches");

    std::string backend_tag = use_gpu ? " [GPU]" : " [CPU]";

    int processed = 0;
    int batch_idx = 0;
    while (processed < total_voxels) {
        int count = std::min(batch_size, total_voxels - processed);
        batch_idx++;

        obs.advance(0, "Batch " + std::to_string(batch_idx) + "/"
                       + std::to_string(total_batches) + " ("
                       + std::to_string(count) + " voxels)");

        // Step 1: Extract voxel data into SoA buffers
        buf.extract_from_cube(cube, cache, processed, count, n_channels);

        // Steps 2-3: Weight computation + robust stats (GPU or CPU)
        obs.advance(0, "  kernel 1: weight classification" + backend_tag);
        obs.advance(0, "  kernel 2: robust statistics" + backend_tag);
        if (use_gpu) {
            execute_batch_gpu(buf, frame_stats.data(), weight_config,
                              count, n_channels, N);
        } else {
            execute_batch_cpu(buf, frame_stats.data(), weight_config,
                              count, n_channels, N);
        }

        // Step 4: Writeback classification + robust stats
        buf.writeback_classification(cube, processed, count, n_channels);

        // Step 5: CPU fitting (Ceres — cannot run on GPU).
        //
        // The per-voxel loop is the hot section of Phase B (see
        // docs/superpowers/plans/2026-04-19-phase7-perf-findings.md).
        // Each iteration is independent:
        //   - cube.at(px, py) addresses a unique (x, y) voxel per vi.
        //   - The fitting_fn captured ModelSelector allocates its fitters
        //     (StudentT/Contamination/GMM/KDE) as stack locals, so there is
        //     no shared mutable state across threads.
        //   - buf.pixel_values / buf.pixel_weights / frame_stats are
        //     read-only for the duration of this loop.
        //   - vals / wts are per-iteration stack locals.
        // dynamic scheduling with a moderately large chunk keeps the
        // heterogeneous per-voxel work balanced without excessive
        // OpenMP scheduling overhead.
        obs.advance(0, "  fitting distributions (Ceres, parallel)");
        int w = cube.width;
        // Heartbeat: emits a "fitted K/N voxels (Ts)" line every 2 s from
        // thread 0 so PI's Process Console shows liveness during the 3-4 min
        // single-batch fit (otherwise silent between kernel 2 and kernel 3).
        FitHeartbeat hb(count, 2000);
        #pragma omp parallel for schedule(dynamic, 256)
        for (int vi = 0; vi < count; vi++) {
            int voxel_idx = processed + vi;
            int px = voxel_idx % w;
            int py = voxel_idx / w;
            auto& voxel = cube.at(px, py);

            std::vector<float> vals(n_channels * N);
            std::vector<float> wts(n_channels * N);
            for (int ch = 0; ch < n_channels; ch++) {
                for (int fi = 0; fi < N; fi++) {
                    vals[ch * N + fi] = buf.pixel_values[ch * N * count + fi * count + vi];
                    wts[ch * N + fi] = buf.pixel_weights[ch * N * count + fi * count + vi];
                }
            }

            fitting_fn(voxel, vals.data(), wts.data(), N,
                        n_channels, frame_stats.data());

            hb.tick(omp_get_thread_num(), obs);
        }

        // Step 6: Extract fitted distributions for select_pixels
        buf.extract_distributions(cube, processed, count, n_channels);

        // Step 7: Pixel selection (GPU or CPU)
        obs.advance(0, "  kernel 3: pixel selection" + backend_tag);
        if (use_gpu) {
            execute_select_gpu(buf, frame_stats.data(), count, n_channels, N);
        } else {
            GPUCPUFallback::select_pixels(buf, frame_stats.data(),
                                           count, n_channels, N);
        }

        // Step 8: Writeback output values
        buf.writeback_selection(cube, processed, count, n_channels,
                                 stacked_output.data(), noise_output.data());

        processed += count;
        obs.advance(1);  // batch complete, advance progress bar

        // Cancellation check
        if (obs.is_cancelled()) {
            obs.message("Cancelled during batch " + std::to_string(batch_idx)
                        + "/" + std::to_string(total_batches));
            break;
        }
    }

    obs.end_phase();
}

// ══════════════════════════════════════════════════════════════════════
// Spatial context
// ══════════════════════════════════════════════════════════════════════

void GPUExecutor::execute_spatial_context(
    const Image& stacked,
    Cube& cube,
    ProgressObserver* progress) {

    ProgressObserver& obs = progress ? *progress : null_progress_observer();

    int w = stacked.width();
    int h = stacked.height();
    int nc = stacked.n_channels();

    std::vector<float> grad(w * h), bg(w * h), rms(w * h);

    bool use_gpu = context_.is_gpu_available() && kernels_.is_compiled();
    if (use_gpu) {
        execute_spatial_gpu(stacked, grad.data(), bg.data(), rms.data());
    } else {
        GPUCPUFallback::spatial_context(stacked.data(), w, h, nc,
                                         grad.data(), bg.data(), rms.data());
    }

    // Write back to voxels
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            auto& voxel = cube.at(x, y);
            int pi = y * w + x;
            voxel.gradient_mag = grad[pi];
            voxel.local_background = bg[pi];
            voxel.local_rms = rms[pi];
        }
    }
}

} // namespace nukex
