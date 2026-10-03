# Per-Channel Frame Sets Through Phase B — Design

**Status:** design, 2026-09-05. Implements Task 5 of
`docs/superpowers/plans/2026-09-05-v5-outstanding-program.md`.

**Problem it solves:** a batch with more than one mono filter is refused,
because stacking it produced one populated channel of four and three
exactly-zero ones. That blocks the first capability the product advertises
(LRGB mono).

## What is actually wrong today, read from the code

### 1. One cache serves every mono filter

`CacheSig` is `(width, height, n_channels)`. Every mono frame is `(W, H, 1)`
whatever its filter, so L, R, G and B frames all land in **one** cache file.
`stacking_engine.cpp` then guards against routing three slots at channel 0 of
one cache and leaves R/G/B with `cache == nullptr`. The comment there says
Phase B will "use welford-only stats" for those slots. **It does not.**
`ShadowBuffers::extract_from_cube` skips the fill, `pixel_values` stays zero
from `allocate()`, and the fitter fits zeros.

### 2. The cache writes at a global index and reads a local one

`FrameCache::write_frame(frame_index, image)` stores at slot `frame_index` —
the **global** batch index — and sets
`n_frames_written_ = max(current, frame_index + 1)`.
`read_pixel` then returns slots `0 .. n_frames_written_-1` **contiguously**.

So a cache that receives frames 5, 9 and 12 of a twenty-frame batch reports
thirteen frames and hands Phase B ten unwritten slots as though they were
measurements. Any batch producing more than one cache hits this — it is not
specific to mono. The mono-L + Bayer-HaO3 integration case is affected today.

### 3. Every channel is assumed to share one frame set in one order

`GPUCPUFallback::classify_weights` and `select_pixels` index
`frame_stats[fi]` with the same `fi` they use for
`pixel_values[ch * N * B + fi * B + vi]`. `ShadowBuffers::n_frames` is
`[batch]` — one count per voxel, not per channel. So the frame set is
structurally global: there is nowhere to say that channel 0 has 24 frames and
channel 1 has 12.

This is the reason the fix is not a cache-key change, and it is what the
pre-existing `TASK-11-MONO-RGB` marker refers to.

## Design

### Cache identity gains a routing key

`CacheSig` becomes `(width, height, n_channels, routing_key)`, where
`routing_key` is the slot group a frame's filter routes into — the filter's
canonical name for a mono frame, and empty for the geometry-only cases that
behave correctly today. Each mono filter therefore gets its own cache file,
and every frame in a cache belongs to the same slot.

Rejected: one cache with per-channel frame maps inside it. The cache is a
flat mmap addressed by `offset(x, y, ch, f)`; making `f` mean different
frames for different `ch` would put the mapping in the hottest indexing path
in Phase B for no gain, since separate files cost nothing here.

### The cache owns its local-to-global frame map

`FrameCache` gains `std::vector<int> frame_map_`, and `write_frame` becomes:

```cpp
/// Append `aligned` at the next local slot. Returns that slot.
/// `global_index` is recorded so Phase B can recover this frame's
/// FrameStats, which are indexed by the batch's global frame numbering.
int write_frame(const Image& aligned, int global_index);
```

Local slots are dense by construction, so `read_pixel`'s contiguous read
becomes correct rather than accidentally correct. `global_frame(local)`
exposes the map.

This alone fixes defect 2, and it is independently testable: a cache written
with a sparse set of global indices must report exactly as many frames as it
received.

### Frame stats become per channel

`ShadowBuffers` gains a staged, per-channel copy:

```cpp
std::vector<FrameStats> frame_stats_soa;   // [n_channels * max_frames]
std::vector<uint16_t>   n_frames;          // [n_channels * batch]  (was [batch])
```

`extract_from_cube` fills `frame_stats_soa[ch * max_frames + local]` from
`global_stats[cache->global_frame(local)]` for the cache backing channel
`ch`. The kernels then index `frame_stats[ch * N + fi]` with the same `fi`
they use for `pixel_values`, and the two cannot drift apart.

Both `GPUCPUFallback` and the OpenCL kernels change signature together; the
GPU/CPU agreement suite is what proves they still match.

### Per-voxel frame count becomes per channel

`n_frames[vi]` becomes `n_frames[ch * batch + vi]`. Kernels that loop
`for (fi = 0; fi < n_frames[vi]; ++fi)` take the channel's own count. A
channel with no cache keeps count 0, which is now representable and means
"welford-only for this slot" — the behaviour the existing comment claims.

### What this does NOT change

- The voxel record. Slots already exist per channel; only their *sources*
  differ.
- Alignment. Every frame still aligns to one reference.
- The refusal for mixed Bayer/mono batches, which is a per-frame debayer
  question and lands on this same machinery. It is sequenced after.

## Order of work

1. `FrameCache` local slots + frame map, with its own test. Independently
   wrong today, so it lands first and alone.
2. `CacheSig` routing key, so each mono filter gets its own cache.
3. `ShadowBuffers` per-channel `frame_stats_soa` and `n_frames`.
4. CPU fallback kernels take the per-channel indexing.
5. OpenCL kernels take the same, and the agreement suite re-proves the match.
6. Remove the refusal in `StackingEngine::execute`; un-skip
   `mono_lrgb_m27_2025` in `test/fixtures/e2e_manifest.json`.
7. Fold in the two formal-only voxel notes deferred to whichever task next
   touches this code: `SubcubeVoxel::channels_()` omits `std::launder` on the
   pointer it reinterpret_casts out of the record's trailing bytes, and
   `construct_voxel` placement-news the channels individually rather than as
   an array.

## How it is proven

- The cache map: a sparse write set reads back exactly what was written.
- Per-channel frame sets: a synthetic batch with two mono filters of
  *different frame counts* stacks with both slots populated and neither
  reading the other's frames.
- GPU/CPU agreement: `ctest -R "gpu_agreement|gpu_cpu_fallback"`.
- Batch invariance is already pinned and must stay green.
- The E2E case `mono_lrgb_m27_2025` moves from `skip: true` to a real golden.
