#include "catch_amalgamated.hpp"
#include "nukex/core/voxel.hpp"

using namespace nukex;

TEST_CASE("SubcubeVoxel: default initialization", "[voxel]") {
    SubcubeVoxel v{};
    REQUIRE(v.n_frames == 0);
    REQUIRE(v.n_channels == 0);
    REQUIRE(v.flags == 0);
    REQUIRE(v.confidence == 0.0f);
    REQUIRE(v.dominant_shape == DistributionShape::UNKNOWN);
    REQUIRE(v.cloud_frame_count == 0);
    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
        REQUIRE(v.welford[ch].count() == 0);
        REQUIRE(v.histogram[ch].total_count() == 0);
        REQUIRE(v.distribution[ch].shape == DistributionShape::UNKNOWN);
    }
}

TEST_CASE("SubcubeVoxel: per-channel Welford accumulation", "[voxel]") {
    SubcubeVoxel v{};
    v.n_channels = 3;
    v.welford[0].update(0.5f);
    v.welford[0].update(0.6f);
    v.welford[0].update(0.55f);
    REQUIRE(v.welford[0].count() == 3);
    REQUIRE(v.welford[0].mean == Catch::Approx(0.55f));
    REQUIRE(v.welford[1].count() == 0);
    REQUIRE(v.welford[2].count() == 0);
}

TEST_CASE("SubcubeVoxel: classification summary", "[voxel]") {
    SubcubeVoxel v{};
    v.n_frames = 5;
    v.cloud_frame_count = 2;
    v.trail_frame_count = 1;
    v.worst_sigma_score = 4.5f;
    v.total_exposure = 1500.0f;
    REQUIRE(v.cloud_frame_count == 2);
    REQUIRE(v.total_exposure == Catch::Approx(1500.0f));
}

TEST_CASE("SubcubeVoxel: flag operations", "[voxel]") {
    SubcubeVoxel v{};
    REQUIRE(v.has_flag(VoxelFlags::BORDER) == false);
    v.set_flag(VoxelFlags::BORDER);
    REQUIRE(v.has_flag(VoxelFlags::BORDER) == true);
    REQUIRE(v.has_flag(VoxelFlags::SATURATED) == false);
    v.set_flag(VoxelFlags::SATURATED);
    REQUIRE(v.has_flag(VoxelFlags::SATURATED) == true);
    v.clear_flag(VoxelFlags::BORDER);
    REQUIRE(v.has_flag(VoxelFlags::BORDER) == false);
    REQUIRE(v.has_flag(VoxelFlags::SATURATED) == true);
}

TEST_CASE("SubcubeVoxel: sizeof reduced after reservoir removal", "[voxel]") {
    INFO("sizeof(SubcubeVoxel) = " << sizeof(SubcubeVoxel));
    // Without reservoir (was ~20KB+), should be well under 10KB
    REQUIRE(sizeof(SubcubeVoxel) > 500);
    REQUIRE(sizeof(SubcubeVoxel) < 10000);
}
