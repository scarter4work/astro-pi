# Phase A performance: storage layout and threading

**Status:** design, for review. Three of the changes are already implemented and
measured; the rest are proposals.

**Measured on:** a 156-frame M63 batch (114 x 120 s + 42 x 300 s, 3840x2160
OSC, 4 cube slots), Ryzen 32-thread / 30 GB / RTX 5070 Ti, cache on NVMe.

---

## 1. Where the time actually goes

Baseline wall time, v5.0.3.1:

| phase | time | share |
|---|---|---|
| Measuring frames | 41 s | 2% |
| **Phase A** | **1133 s** | **67%** |
| Phase B | 514 s | 30% |
| Phase C | 3 s | <1% |

Phase A is the problem, not Phase B. Earlier notes put Phase A at 28-34% of
wall time; on this batch it is 67%, and the difference is degradation over the
run rather than a different mix of work.

One frame, broken down from the console log (frame 61, 3.51 s total):

| stage | time | share |
|---|---|---|
| read + debayer | 0.033 s | 1% |
| align (detect, match, homography, warp) | 0.681 s | 19% |
| **cache write** | **2.500 s** | **71%** |
| accumulate | 0.295 s | 8% |

And the per-frame cost is not constant: **3.85 s at frame 1, 13.4 s at frame
140.** That degradation is most of the phase.

## 2. Root cause: 26x write amplification

`FrameCache::write_frame` wrote through an mmap whose layout was **pixel-major**:

```
offset(x, y, ch, f) = ((y*W + x)*n_ch + ch) * max_frames + f
```

Consecutive writes for one frame were `max_frames` elements apart — 312 bytes
at 156 frames — so **writing one frame touched every page of the mapping.**
The mapping was `W*H*n_ch*max_frames*2` = 10.4 GB here, while the frame's own
data was 66 MB.

It then called `msync(mapped_, mapped_size_, MS_ASYNC)` over the **whole
mapping, after every frame**, forcing the entire file back to disk per frame.

Measured directly from `/proc/diskstats` during Phase A:

```
20,450 MB written in 45 s over 12 frames  ->  1,704 MB per frame
```

for 66 MB of actual data. **26x write amplification**, saturating the NVMe.
That was the degradation: as the cache filled, more of the mapping was
resident and dirty, so each forced writeback cost more.

### What was NOT the cause

I initially blamed swap, from a `vmstat` snapshot showing 3.9 GB `swpd` and
57% iowait. That was wrong and the correction is worth recording:

- System swap was **3 GB before the run started** and 3.9 GB during it. Under
  1 GB was swapped by NukeX.
- `smaps_rollup` on the PixInsight process: Rss 13.11 GB but **Anonymous only
  5.38 GB, Swap 0.00**. Most of the RSS is file-backed, reclaimable page cache
  from the mmap — not memory pressure.
- The iowait and `bo` spikes were the write amplification above, which is a
  bandwidth problem, not a memory one.

Lesson: `RSS` on a process with a large mmap says almost nothing. Split it
with `smaps_rollup` before drawing a conclusion.

## 3. Done, and measured

### 3a. Periodic writeback (implemented, superseded by 3c)

On the pixel-major layout, `write_frame` scheduled writeback every 8th frame
instead of every frame, with an explicit `flush()` at the end of Phase A so
the dirty-page bound that prevented the 2026-09-05 OOM was kept. It was the
right fix for that layout — batching the `msync` calls was the only lever
available without changing the layout itself — and the result is what made
the layout's cost visible: batching 8x only bought 2x, which is what showed
that most of the 26x amplification was coming from the layout, not from
`msync` frequency.

```
write volume   1,704 MB/frame -> 870 MB/frame     (2.0x)
first 40 frames   145.7 s     -> 100.5 s          (1.45x)
```

Only 2x rather than 8x, because the kernel's own `dirty_background_ratio`
forces writeback once ~3 GB is dirty regardless of `msync`. The rest of the
amplification is structural — see 4a.

Superseded by 3c: once the layout became frame-major, batching writeback
every 8th frame over the whole mapping stopped being the right shape.
Writeback there is per-frame, over only that frame's own bytes — see 3c for
what replaced this and its measurements.

### 3b. Bounded GPU staging budget (implemented)

`estimate_batch_size` took `MemAvailable / 2` as its host budget, which
returned a **13.2 GB staging batch** on this machine. `MemAvailable` counts
reclaimable page cache, and the frame cache fills page cache by design, so it
overstates what an anonymous allocation can get. Now `min(MemAvailable/2,
2 GiB)`.

Batch size is a staging choice, not a numerical one — proven bit-exact across
batch splits in `test_gpu_cpu_fallback` — so a smaller batch buys more kernel
launches and costs nothing else. This did not move the Phase A numbers above
(staging is allocated in Phase B); it removes a second way to exhaust the box.

### 3c. Frame-major layout (implemented)

`offset(x, y, ch, f) = f*(W*H*n_ch) + (y*W+x)*n_ch + ch`. Phase B reads by
frame-row (`FrameCache::read_frame_range`) instead of gathering one pixel
across all frames; `read_pixel` is gone. Writeback narrowed to the frame just
written, replacing the 8-frame batching from 3a that the old layout forced.

Measured on the four-corpus E2E manifest (BEFORE = main 338d6ab, pixel-major;
AFTER = this branch, frame-major; same machine, back to back, nothing else
changed):

Phase A wall time:

| corpus | BEFORE | AFTER | speedup |
|---|---|---|---|
| NGC7635 65f mono (4 runs) | 37.4-38.7 s | 36.3-36.6 s | ~1.03x |
| M27-2023 33f OSC | 122.4 s | 69.8 s | 1.75x |
| M16 12f dual-NB | 59.2 s | 53.9 s | 1.10x |
| M27-2025 72f mono | 68.3 s | 50.4 s | 1.36x |
| **total** | **401.3 s** | **320.0 s** | **1.25x** |

Bytes written to the NVMe during Phase A (`/proc/diskstats`, 5 s sampling),
for the three corpora with enough volume to read:

| corpus | BEFORE MB/frame | AFTER MB/frame | reduction |
|---|---|---|---|
| M27-2023 33f OSC | 2165.6 | 78.0 | 27.76x |
| M16 12f dual-NB | 492.3 | 65.9 | 7.47x |
| M27-2025 72f mono | 142.0 | 8.8 | 16.17x |

(MB/frame columns above are rounded for display; each reduction ratio is
computed from the unrounded per-sample deltas, so dividing the displayed
columns can disagree with the stated ratio in the last digit — e.g.
142.0 / 8.8 reads as 16.14x against the stated 16.17x. The ratios as stated
are correct.)

The four NGC7635 runs moved between 5.6 and 26.6 MB/frame in both directions
(0.55x-4.21x). `/proc/diskstats` is system-wide, and at that size the reading
is at or below the noise floor of a machine also running a browser and a
desktop, so those four rows are reported unedited but no conclusion is drawn
from them. The three corpora above all move the same way and are the basis
for the reduction claim.

Per-frame degradation (first 3 frames vs. last 3 of Phase A):

| corpus | BEFORE | AFTER |
|---|---|---|
| NGC7635 65f mono | 1.23-1.30x | 1.35-1.40x |
| M27-2023 33f OSC | 1.72x | 1.03x -- degradation eliminated |
| M16 12f dual-NB | 1.75x | 1.83x (12 frames, too few to read) |
| M27-2025 72f mono | 1.67x | 1.33x |

This spec's own baseline figures in sections 1 and 2 -- Phase A 1133 s of a
1689 s run, per-frame 3.85 s -> 13.4 s, 1704 MB/frame -- come from a
156-frame M63 batch that is not in the E2E manifest, so they are not
directly comparable to the numbers above. That mismatch is exactly why a
matched BEFORE run was made on `main` rather than dividing one figure by the
other.

E2E: verify mode, run twice, all four corpora (`lrgb_mono_ngc7635`,
`bayer_rgb_m27_2023`, `bayer_nb_hao3_m16`, `mono_lrgb_m27_2025`) came back
`regen: False, checked: True, match: True`. 19/19 pixel hashes identical
between the two independent runs and identical to the committed v5.0.4.1
goldens. No golden regenerated.

## 4. Proposed

### 4a. Frame-major cache layout — the real fix (recommended first)

*Implemented — see 3c above for the measured result. Left here as the
original design rationale.*

Store each frame's pixels **contiguously**:

```
offset(x, y, ch, f) = f * (W*H*n_ch) + (y*W + x)*n_ch + ch
```

- **Phase A** becomes a 66 MB sequential write per frame instead of a strided
  scan that dirties 10.4 GB. Expected: the 2.5 s cache step drops to well
  under 0.5 s, and stops degrading as the cache fills.
- **Phase B** must change with it. `read_pixel(x, y, ch)` gathers all frames
  for one pixel, which is exactly the access frame-major is bad at. But Phase
  B already works in **batches of voxels**, so the batched form is
  `n_frames` contiguous runs of `batch_size` elements — at 400k voxels per
  batch that is 156 reads of 800 KB each, which is excellent sequential I/O.

The work is therefore: change `offset()`, and replace the per-pixel read path
with a per-batch one. The per-pixel `read_pixel` should go, not be kept as a
slow fallback — leaving it invites an accidental O(n^2) caller.

**Risk:** this changes the on-disk format and the Phase B read path. It must
be bit-identical — the E2E goldens are the gate, and they should not move at
all.

### 4b. Parallel Phase A

Phase A is one thread of thirty-two. There is exactly one parallel construct
in the codebase (`gpu_executor.cpp:492`) and no `std::thread` in `src/lib`.

With the cache write fixed, the per-frame profile is roughly align 70% /
accumulate 25% / read+debayer 5%. Accumulation into the cube is the only
shared-state step.

**Shape:** a bounded producer/consumer. A thread pool runs read -> debayer ->
flat -> align -> cache write per frame; a single consumer accumulates **in
frame order**. Accumulating in a fixed order is what keeps the result
bit-identical, which is the property that makes this verifiable at all.

Amdahl with ~25% serial gives ~2.7x at 8 threads; if accumulation is also
parallelised over disjoint row bands, more.

**The ordering hazard:** `FrameAligner` chaining rescues a frame by matching
it against an already-aligned neighbour, so its anchor set depends on
completion order. Parallelising naively makes the output depend on thread
scheduling — non-deterministic, which is unacceptable (a variable batch count
already produced the coverage-stride bug once).

**Fix:** two passes. Pass 1 in parallel, direct-to-reference alignment only.
Pass 2 serial, chaining for whatever failed, with anchors drawn from the
complete set of pass-1 successes. The anchor set is then order-independent by
construction. Most frames align directly (156/156 on this batch), so pass 2 is
usually empty.

**Prerequisite:** confirm `StarDetector`, `HomographyComputer`,
`DebayerEngine` and `FlatCalibration` are reentrant. The project rule says
statistical functions must be, but it needs verifying rather than assuming.

### 4c. Memory-mapped cube (the "treat it like a database" question)

`Cube` is already a flat, fixed-stride, pointer-addressable record store:

```cpp
offset_of(x, y) = (y*width + x) * stride_;
std::unique_ptr<std::byte[]> storage_;
```

Making it file-backed is **changing the allocator, not the architecture** —
everything above `Cube` is untouched. Sizes measured: 196 B/voxel mono,
604 B/voxel at 4 channels, so **4.67 GiB** for a 3840x2160x4 cube.

Advantages, in order of how much they matter:

1. **File-backed pages never go to swap.** An anonymous page must be *written*
   to swap before it can be evicted, then read back — two I/Os. A clean
   file-backed page is dropped and re-read — one, often zero. The same access
   pattern is strictly cheaper through a mapping.
2. **Determinism.** Batch count currently varies with free memory, and that is
   what exposed the coverage-stride bug. A fixed on-disk layout takes free
   memory out of the output path.
3. **Larger-than-RAM runs.** 156 x 24 MP is at the edge today; a 61 MP camera
   or 500 frames cannot run at all.
4. **Restartability.** Phase A is ~19 minutes currently lost on any failure.
5. **Inspectability.** During the ghost-hole investigation there was no way to
   ask "what did the 156 frames at this voxel contain?" — it had to be
   inferred from the raw frames.

**It should be a flat binary file, not a database.** `(y*width+x)*stride` is
already a perfect O(1) index; SQLite or a KV store would add serialisation and
lookup cost for nothing.

**Caveats:** Phase A writes scattered across all 8.3M records per frame, which
is the pattern that could thrash a mapping — measure before committing.
`/tmp` is a 16 GB tmpfs here, so the backing file must go somewhere real.
And it does not help the GPU staging buffers, which need host memory.

## 5. Order of work

1. ~~Periodic writeback~~ (done, 1.45x)
2. ~~Bounded staging budget~~ (done)
3. ~~4a frame-major layout~~ (done, 1.25x Phase A wall time, up to 27.76x
   fewer bytes/frame written -- see 3c)
4. **4b parallel Phase A** — bigger win but needs the determinism design above
5. **4c mmap'd cube** — architectural; do after 4a proves the access pattern

Each step is gated on the E2E corpus coming back bit-identical. None of these
should change a single output pixel.
