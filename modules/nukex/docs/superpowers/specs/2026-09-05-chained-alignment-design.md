# Chained Alignment for Session Drift — Design

**Status:** design, 2026-09-05. Implements Task 6 of
`docs/superpowers/plans/2026-09-05-v5-outstanding-program.md`.

## The measurement this is built from

`mono_lrgb_m27_2025` (`/mnt/qnap/astro_data/9_1_2025/M27`, ATR585M,
L24 R12 G12 B24) aligns **51 of 72** frames. The 21 failures are the two
temporal ends of a continuous seven-hour session — chronological frames 0-19
and 66-71. Nothing in the middle fails.

Ruled out by measurement, not assumption:

- **Not a meridian flip.** `PIERSIDE` is 0 on every frame.
- **Not filter mismatch.** Failures span L and B; R and G next to the
  reference all pass, and L frames near the reference pass too.
- **Not reference choice.** The selector already picks index 36, the temporal
  midpoint. No single reference reaches both ends.
- **Not matcher tolerance.** Sweeping `descriptor_tolerance` 0.003-0.05 and
  `scale_tolerance_log` 0.02-0.1 changes nothing.

Header `RA`/`DEC` drift 0.028 deg over the session; at 2.9 um pixels and
490 mm focal length (1.221 arcsec/px) that is **~110 px** of cumulative
motion.

**The mechanism, from `test_alignment_diag.cpp`:**

| distance from reference | stars | n_matches | n_inliers |
|---|---|---|---|
| 4 | 200 | 87 | 83 |
| 6 | 200 | 76 | 68 |
| 16 | 200 | 59 | 29 |
| 26 | 200 | 62 | 12 |
| 36 | 200 | 55 | **0** |

The far frames are **not short of correspondences** — they get 55 and the
homography rejects every one. Drift reshuffles which stars land in the
top-K, so triangle descriptors increasingly match the wrong stars.

## Design: anchor chaining, as a fallback only

A frame is first matched against the reference exactly as today. **Only if
that fails** does it try to reach the reference through an anchor — a frame
already aligned successfully, near it in time:

```
H_ref<-i  =  H_ref<-a  *  H_a<-i
```

where `a` is the nearest anchor by frame index. `H_a<-i` is fitted from
frame `i`'s catalog against anchor `a`'s catalog, both of which the aligner
already computes.

### Why fallback-only, and not chain everything

Because it makes the change **provably inert for every corpus that already
aligns fully**. `lrgb_mono_ngc7635` (65/65), `bayer_rgb_m27_2023` (33/33) and
`bayer_nb_hao3_m16` (12/12) never enter the new path, so their goldens cannot
move. Only frames that fail today — which currently contribute nothing —
change anything. It is the same minimal-intervention principle the reference
selector already follows, and for the same reason: the alternative was
measured to make things worse (always-pick-best took NGC7635 from 65/65 to
59/65).

### Why this composes transforms rather than resampling twice

The chain composes **homographies**, then warps once. `H_ref<-a * H_a<-i` is a
single 3x3 product and the frame is resampled exactly once, as now. Chained
*resampling* would blur every chained frame and is not what this does.

### Anchor selection

Anchors are frames whose alignment succeeded, holding `(frame_index,
catalog, H_ref<-anchor)`. On failure the aligner tries anchors in order of
increasing `|frame_index - anchor_index|`, and stops at the first that
yields a plausible homography. A cap on attempts keeps the cost bounded; a
star catalog is ~200 entries, so retained anchors cost almost nothing.

A frame aligned *through* an anchor becomes an anchor itself. That is what
lets the chain walk outward to the ends of a session, which is precisely
where the failures are.

### What stays unchanged

- The reference selection rule (keep the first usable frame unless it falls
  below 75% of the best star count). Measured; do not "improve" it.
- One resample per frame.
- The failed-alignment path: a frame that no anchor rescues is still stacked
  with a penalised weight, exactly as today.

## How it is proven

- A synthetic drifting session: frames whose star field translates
  cumulatively far enough that the ends cannot match the middle directly.
  Direct-only alignment fails at the ends; anchor chaining aligns them, and
  the recovered homography matches the known cumulative transform.
- The three fully-aligning corpora stay bit-identical, because they never
  enter the new path.
- `min_frames_ok_alignment` for `mono_lrgb_m27_2025` ratchets from 51 to
  whatever the implementation achieves.
