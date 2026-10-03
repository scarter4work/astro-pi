#include "catch_amalgamated.hpp"
#include "nukex/core/voxel.hpp"

using namespace nukex;

TEST_CASE("SubcubeVoxel: default initialization", "[voxel]") {
    StandaloneVoxel<MAX_CHANNELS> v;
    REQUIRE(v->n_frames == 0);
    REQUIRE(v->n_channels == MAX_CHANNELS);
    REQUIRE(v->flags == 0);
    REQUIRE(v->confidence == 0.0f);
    REQUIRE(v->dominant_shape == DistributionShape::UNKNOWN);
    REQUIRE(v->cloud_frame_count == 0);
    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
        REQUIRE(v->channel(ch).welford.count() == 0);
        REQUIRE(v->channel(ch).histogram.total_count() == 0);
        REQUIRE(v->channel(ch).distribution.shape == DistributionShape::UNKNOWN);
    }
}

TEST_CASE("SubcubeVoxel: per-channel Welford accumulation", "[voxel]") {
    StandaloneVoxel<3> v;
    REQUIRE(v->n_channels == 3);
    v->channel(0).welford.update(0.5f);
    v->channel(0).welford.update(0.6f);
    v->channel(0).welford.update(0.55f);
    REQUIRE(v->channel(0).welford.count() == 3);
    REQUIRE(v->channel(0).welford.mean == Catch::Approx(0.55f));
    REQUIRE(v->channel(1).welford.count() == 0);
    REQUIRE(v->channel(2).welford.count() == 0);
}

TEST_CASE("SubcubeVoxel: classification summary", "[voxel]") {
    StandaloneVoxel<1> sv; SubcubeVoxel& v = *sv;
    v.n_frames = 5;
    v.cloud_frame_count = 2;
    v.trail_frame_count = 1;
    v.worst_sigma_score = 4.5f;
    v.total_exposure = 1500.0f;
    REQUIRE(v.cloud_frame_count == 2);
    REQUIRE(v.total_exposure == Catch::Approx(1500.0f));
}

TEST_CASE("SubcubeVoxel: flag operations", "[voxel]") {
    StandaloneVoxel<1> sv; SubcubeVoxel& v = *sv;
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

TEST_CASE("SubcubeVoxel: the per-channel record stays small", "[voxel]") {
    // The reservoir used to live per channel and made a voxel ~20 KB. It was
    // removed in favour of the disk frame cache; nothing that grows with the
    // frame count may come back into the record, because the record is
    // multiplied by width x height x n_channels.
    INFO("sizeof(VoxelChannel) = " << sizeof(VoxelChannel));
    INFO("sizeof(SubcubeVoxel) = " << sizeof(SubcubeVoxel));
    REQUIRE(sizeof(VoxelChannel) < 256);
    REQUIRE(sizeof(SubcubeVoxel) < 128);
}

// ── Runtime-sized channel storage ────────────────────────────────────
//
// The per-channel payload used to be provisioned at MAX_CHANNELS on every
// voxel, so an L-only stack paid for eight channels and used one. These
// cases pin the record layout that replaced it: fixed fields, then exactly
// n_channels trailing VoxelChannel records.

TEST_CASE("SubcubeVoxel: channels are independent records", "[voxel]") {
    StandaloneVoxel<3> sv;
    REQUIRE(sv->n_channels == 3);

    sv->channel(0).welford.update(0.5f);
    sv->channel(0).welford.update(0.6f);
    sv->channel(2).welford.update(1.0f);

    REQUIRE(sv->channel(0).welford.count() == 2);
    REQUIRE(sv->channel(0).welford.mean == Catch::Approx(0.55f));
    REQUIRE(sv->channel(1).welford.count() == 0);
    REQUIRE(sv->channel(2).welford.count() == 1);
    REQUIRE(sv->channel(2).welford.mean == Catch::Approx(1.0f));
}

TEST_CASE("SubcubeVoxel: a fresh channel starts at the identity for min/max",
          "[voxel]") {
    // Zero-filling the buffer would leave min_val/max_val at 0, which silently
    // clamps every subsequent sample. Each channel must be constructed, not
    // just zeroed.
    StandaloneVoxel<2> sv;
    sv->channel(1).welford.update(-3.0f);
    REQUIRE(sv->channel(1).welford.min_val == Catch::Approx(-3.0f));
    REQUIRE(sv->channel(1).welford.max_val == Catch::Approx(-3.0f));
}

TEST_CASE("ZDistribution: fields are packed with no wasted interior padding",
          "[voxel]") {
    // shape and used_nonparametric are both one byte and share a single
    // 4-byte slot; the union and the seven floats are all 4-aligned.
    constexpr size_t expected = 4                       // shape + flag + pad
                              + sizeof(BimodalParams)   // widest union member
                              + 7 * sizeof(float);
    INFO("sizeof(ZDistribution) = " << sizeof(ZDistribution));
    REQUIRE(sizeof(ZDistribution) == expected);
}

TEST_CASE("PixelHistogram: bins are sized to the frame-count bound", "[voxel]") {
    // One bin is incremented per sample, and a channel receives at most one
    // sample per frame, so no bin can exceed SubcubeVoxel::n_frames — which is
    // itself uint16_t. 16-bit bins are therefore exactly lossless.
    STATIC_REQUIRE(sizeof(decltype(PixelHistogram::bins[0]))
                   == sizeof(decltype(SubcubeVoxel::n_frames)));
    INFO("sizeof(PixelHistogram) = " << sizeof(PixelHistogram));
    REQUIRE(sizeof(PixelHistogram)
            == PixelHistogram::N_BINS * sizeof(uint16_t) + 2 * sizeof(float));
}

TEST_CASE("PixelHistogram: bin counts stay exact up to the frame-count limit",
          "[voxel]") {
    PixelHistogram h;
    h.initialize_range(0.0f, 1.0f);
    constexpr uint32_t n = 65535;   // max value of SubcubeVoxel::n_frames
    for (uint32_t i = 0; i < n; i++) {
        h.update(0.5f);
    }
    REQUIRE(h.total_count() == n);
    REQUIRE(h.bins[h.peak_bin()] == n);
}
