#pragma once

#include "nukex/core/cube.hpp"
#include "nukex/io/image.hpp"
#include <vector>

namespace nukex {

/// The most common fitted distribution shape across a voxel's `n_ch`
/// channels (lowest enumerator on a tie), into voxel.dominant_shape.
void compute_dominant_shape(SubcubeVoxel& voxel, int n_ch);

/// Phase C's per-voxel work, in ONE row-parallel pass over the record.
///
/// For every voxel: its dominant shape, its quality score and confidence,
/// the spatial context computed beforehand from the stacked image
/// (`gradient_mag`, `local_background`, `local_rms`, raster order, one value
/// per pixel), and its pixel of the quality map and the measured-noise map.
/// These were five sweeps of the record; file-backed, each one re-read and
/// re-wrote the whole file. None reads another voxel, so one pass does all
/// of it with identical results.
///
/// `quality_map` and `measured_noise` are (re)allocated to the cube's size
/// with 4 and 1 channels.
void post_process_record(Cube& cube, int n_ch,
                         const std::vector<float>& gradient_mag,
                         const std::vector<float>& local_background,
                         const std::vector<float>& local_rms,
                         Image& quality_map, Image& measured_noise);

} // namespace nukex
