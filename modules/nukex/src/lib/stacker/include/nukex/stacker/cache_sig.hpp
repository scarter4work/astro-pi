#pragma once

#include <cstdint>
#include <tuple>

namespace nukex {

class FrameCache; // forward declare — defined in frame_cache.hpp

// ─────────────────────────────────────────────────────────────────────────────
// CacheSig: key for the engine's per-geometry cache map.
//
// Two frames share a FrameCache iff their post-debayer Image has the same
// (width, height, n_ch). BayerPattern and FilterClass are NOT part of the
// key — the cache only cares about what DebayerEngine::debayer() outputs.
// ─────────────────────────────────────────────────────────────────────────────
using CacheSig = std::tuple<int, int, int>; // (width, height, n_ch)

// ─────────────────────────────────────────────────────────────────────────────
// SlotSynthesis: how a cube slot's per-frame value is derived during Phase B.
// ─────────────────────────────────────────────────────────────────────────────
enum class SlotSynthesis : uint8_t {
    DIRECT,       // Per-frame value comes directly from cache[cache_ch].
    REC709_LUMA,  // Synthesised from cache ch 0/1/2 as 0.299R + 0.587G + 0.114B.
                  // Used for BROADBAND_OSC's synthesised L slot — saves 33% disk
                  // vs caching L separately; Phase B is GPU-bound not disk-bound,
                  // so the extra reads are free in practice.
};

// ─────────────────────────────────────────────────────────────────────────────
// ChannelCacheRef: per-cube-slot descriptor for Phase B reads.
//
// Built by the engine once after Phase A completes; consumed by
// ShadowBuffers::extract_from_cube during Phase B. One entry per cube slot.
//
// Design note: this routing table is verbose by design. Encoding
// (FilterClass × cache geometry) rules in a flat struct is clearer than
// smearing them across ChannelConfig string conventions or slot-name parsing.
// ─────────────────────────────────────────────────────────────────────────────
struct ChannelCacheRef {
    const FrameCache* cache    = nullptr;               // null ⇒ no per-frame data; Phase B
                                                         // falls back to welford-only for this slot
    int               cache_ch = -1;                    // channel index within `cache`; -1 for
                                                         // non-DIRECT synthesis
    SlotSynthesis     kind     = SlotSynthesis::DIRECT;
};

} // namespace nukex
