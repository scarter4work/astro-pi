#pragma once

#include <cstdint>
#include <string>
#include <tuple>

namespace nukex {

class FrameCache; // forward declare — defined in frame_cache.hpp

// ─────────────────────────────────────────────────────────────────────────────
// CacheSig: key for the engine's per-geometry cache map.
//
// Two frames share a FrameCache iff their post-debayer Image has the same
// (width, height, n_ch) AND they route into the same slot group.
//
// The routing key is what makes multi-filter mono batches work. Every mono
// frame is (W, H, 1) whatever its filter, so a geometry-only key collapsed
// L, R, G and B into one cache file and there was no way for a slot to read
// only its own frames -- which is why such batches produced one populated
// channel of four and three exactly-zero ones.
//
// It is the slot name for a single-channel frame ("L", "Ha", "R"), and empty
// for multi-channel frames, which serve several slots from one cache by
// channel index and were always correct.
// ─────────────────────────────────────────────────────────────────────────────
using CacheSig = std::tuple<int, int, int, std::string>; // (w, h, n_ch, routing key)

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
