#pragma once

#include "nukex/core/types.hpp"
#include "nukex/core/welford.hpp"
#include "nukex/core/histogram.hpp"
#include "nukex/core/distribution.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace nukex {

namespace VoxelFlags {
    constexpr uint8_t BORDER     = 0x01;
    constexpr uint8_t SATURATED  = 0x02;
    constexpr uint8_t LOW_N      = 0x04;
    constexpr uint8_t FIT_FAILED = 0x08;
}

/// Everything a voxel records about ONE channel.
///
/// These fields used to be seven parallel arrays inside SubcubeVoxel, each
/// dimensioned MAX_CHANNELS. That provisioned eight channels on every voxel of
/// every stack: an L-only stack used one of them and paid for eight, and the
/// per-channel arrays were 96% of the voxel. Grouping them into one record lets
/// the voxel carry exactly as many as the stack actually has (see
/// SubcubeVoxel::channel), and puts a channel's Welford state next to its own
/// histogram — the two the Phase A hot loop touches together.
struct VoxelChannel {
    // ── Streaming accumulators (Phase A) ─────────────────────────────
    WelfordAccumulator  welford;
    PixelHistogram      histogram;

    // ── Fitted distribution (Phase B) ────────────────────────────────
    ZDistribution       distribution;

    // ── Output and robust statistics (Phase B) ───────────────────────
    float               snr                  = 0.0f;
    float               mad                  = 0.0f;
    float               biweight_midvariance = 0.0f;
    float               iqr                  = 0.0f;
};

/// One record per (x, y). The fixed fields below are followed in memory by
/// exactly `n_channels` VoxelChannel records — reach them with channel().
///
/// A voxel is therefore NOT a self-contained value: its size is only known at
/// run time, and copying one by value would silently drop every channel. The
/// copy operations are deleted so that mistake is a compile error. Allocate
/// voxels through Cube, or through StandaloneVoxel for a single one.
struct SubcubeVoxel {
    // ── Classification summaries (Phase B) ───────────────────────────
    uint16_t            cloud_frame_count  = 0;
    uint16_t            trail_frame_count  = 0;
    float               worst_sigma_score  = 0.0f;
    float               best_sigma_score   = 0.0f;
    float               mean_weight        = 0.0f;
    float               total_exposure     = 0.0f;

    // ── Cross-channel quality (Phase B) ──────────────────────────────
    float               confidence         = 0.0f;
    float               quality_score      = 0.0f;
    DistributionShape   dominant_shape     = DistributionShape::UNKNOWN;

    // ── Spatial context (Phase B, post-selection) ────────────────────
    float               gradient_mag       = 0.0f;
    float               local_background   = 0.0f;
    float               local_rms          = 0.0f;

    // ── PSF quality at this position ─────────────────────────────────
    float               mean_fwhm          = 0.0f;
    float               mean_eccentricity  = 0.0f;
    float               best_fwhm          = 0.0f;

    // ── Bookkeeping ──────────────────────────────────────────────────
    uint16_t            n_frames           = 0;
    uint8_t             n_channels         = 0;
    uint8_t             flags              = 0;

    SubcubeVoxel()                               = default;
    SubcubeVoxel(const SubcubeVoxel&)            = delete;
    SubcubeVoxel& operator=(const SubcubeVoxel&) = delete;

    /// The channel record at index `c`. Caller must keep 0 <= c < n_channels;
    /// every call site indexes with a slot index that ChannelConfig already
    /// bounds-checked, so this stays branch-free in the Phase A hot loop.
    VoxelChannel&       channel(int c)       { return channels_()[c]; }
    const VoxelChannel& channel(int c) const { return channels_()[c]; }

    bool has_flag(uint8_t flag) const { return (flags & flag) != 0; }
    void set_flag(uint8_t flag)       { flags |= flag; }
    void clear_flag(uint8_t flag)     { flags &= ~flag; }

private:
    // std::launder is required, not decorative: the bytes after this object
    // were given their type by a placement-new elsewhere, and without it the
    // compiler is entitled to assume this reinterpret_cast does not name
    // those objects. No compiler exploits that today -- the E2E hashes are
    // unchanged either way -- but the strict reading is worth closing, and
    // construct_voxel now creates the channels as an ARRAY so that indexing
    // the returned pointer is arithmetic within one complete object rather
    // than across several.
    VoxelChannel* channels_() {
        return std::launder(reinterpret_cast<VoxelChannel*>(
            reinterpret_cast<std::byte*>(this) + sizeof(SubcubeVoxel)));
    }
    const VoxelChannel* channels_() const {
        return std::launder(reinterpret_cast<const VoxelChannel*>(
            reinterpret_cast<const std::byte*>(this) + sizeof(SubcubeVoxel)));
    }
};

/// Alignment a voxel record must start on.
constexpr std::size_t kVoxelAlign =
    std::max(alignof(SubcubeVoxel), alignof(VoxelChannel));

/// Bytes one voxel occupies when it carries `n_channels` channels.
constexpr std::size_t voxel_record_size(int n_channels) {
    return sizeof(SubcubeVoxel)
         + static_cast<std::size_t>(n_channels) * sizeof(VoxelChannel);
}

// The trailing channels start at exactly sizeof(SubcubeVoxel), so that offset
// must already satisfy VoxelChannel's alignment, and consecutive records must
// stay aligned for the next SubcubeVoxel.
static_assert(sizeof(SubcubeVoxel) % alignof(VoxelChannel) == 0,
              "voxel header size must be a multiple of the channel alignment");
static_assert(sizeof(VoxelChannel) % alignof(SubcubeVoxel) == 0,
              "channel size must be a multiple of the voxel alignment");
static_assert(std::is_trivially_destructible_v<SubcubeVoxel>,
              "voxel storage is released as raw bytes, so it must not need a dtor");
static_assert(std::is_trivially_destructible_v<VoxelChannel>,
              "channel storage is released as raw bytes, so it must not need a dtor");

/// Construct a voxel and its `n_channels` channel records into raw storage.
///
/// The bytes cannot simply be zeroed: WelfordAccumulator seeds min_val to
/// +FLT_MAX and max_val to -FLT_MAX, and a zeroed accumulator would clamp
/// every sample that followed. Each record is constructed, never memset.
inline SubcubeVoxel* construct_voxel(void* storage, int n_channels) {
    auto* bytes = static_cast<std::byte*>(storage);
    auto* voxel = new (bytes) SubcubeVoxel();
    voxel->n_channels = static_cast<uint8_t>(n_channels);
    // Array placement-new writes an ABI "array cookie" before the first
    // element for any type whose destructor is non-trivial, which would
    // shift every channel and silently corrupt the record. VoxelChannel is
    // trivially destructible, so no cookie is emitted -- asserted here so
    // that adding a destructor to VoxelChannel fails the build instead of
    // the stack.
    static_assert(std::is_trivially_destructible<VoxelChannel>::value,
                  "VoxelChannel must stay trivially destructible: array "
                  "placement-new would otherwise emit a cookie and shift "
                  "the channel records.");
    // Created as an array, not one at a time: `channels_()[c]` is then
    // indexing within a single complete object. Constructing them
    // individually made every access for c >= 1 pointer arithmetic across
    // separate complete objects, which is formally undefined even though it
    // is the standard flexible-array-member idiom and works everywhere.
    new (bytes + sizeof(SubcubeVoxel)) VoxelChannel[n_channels]();
    return voxel;
}

/// A single self-contained voxel with N channels, for tests and for the few
/// call sites that need one outside a Cube.
template <int N>
class StandaloneVoxel {
public:
    static_assert(N >= 1 && N <= MAX_CHANNELS, "channel count out of range");

    StandaloneVoxel() { construct_voxel(storage_, N); }

    SubcubeVoxel*       operator->()       { return get(); }
    const SubcubeVoxel* operator->() const { return get(); }
    SubcubeVoxel&       operator*()        { return *get(); }
    const SubcubeVoxel& operator*()  const { return *get(); }

private:
    SubcubeVoxel* get() {
        return std::launder(reinterpret_cast<SubcubeVoxel*>(storage_));
    }
    const SubcubeVoxel* get() const {
        return std::launder(reinterpret_cast<const SubcubeVoxel*>(storage_));
    }

    alignas(kVoxelAlign) std::byte storage_[voxel_record_size(N)];
};

} // namespace nukex
