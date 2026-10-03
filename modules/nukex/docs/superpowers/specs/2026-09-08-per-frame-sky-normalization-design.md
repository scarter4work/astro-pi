# Per-frame sky normalisation

2026-09-08. Wires `solve_frame_normalization` (landed unused in `74fcf09`) into
the measuring pass and Phase A, and fixes what measuring it on real data
exposed.

## The problem

Nothing in NukeX normalised for per-frame level. `EXPTIME` was read into
`frame_metadata`, copied into `frame_stats`, summed into
`voxel.total_exposure` for bookkeeping, and never used to scale anything.
There was no level normalisation either.

The consequence is measured, not theorised. Across five real corpora the AICc
model race produces pixel-scale noise 1.00x to 5.19x that of a plain Huber
M-estimator on identical inputs -- four of five between 1.28x and 5.19x, which
is 1.6x to 27x of effective exposure thrown away. The mechanism: `GMM` wins
about 31% of voxels and its `true_signal_estimate` is a mixture COMPONENT,
selected by whether `pi1` crosses 0.5. That is a per-pixel coin flip between
two levels about 1.33 noise floors apart.

The bimodality it is flipping on is real, and it is a per-FRAME effect being
modelled per-PIXEL: per-frame sky offsets run -0.8 to +3.2 sigma across a
session, and upper-component membership is 60x more frame-structured than
chance.

## Grouping: per cube slot, never per batch

Measured on the user's M27 2025 LRGB set:

| filter | frames | median sky (ADU) | per-frame range |
|--------|-------:|-----------------:|-----------------|
| L      | 24     | 2934             | 2829 - 3084     |
| R      | 12     | 3447             | 3425 - 3512     |
| G      | 12     | 4050             | 3874 - 4314     |
| B      | 24     | 5222             | 4383 - 12962    |

A single batch-wide reference would scale L up 1.28x and B down 0.72x and
flatten the colour. Grouping is therefore by the cube slot a frame's channel
feeds -- which is also, uniformly, the right grouping for OSC, where every
frame feeds R, G and B and each gets its own reference.

The within-filter spread is what this exists to remove. B is the case: 4383 to
12962 ADU in one session.

## Where it is applied

- **Measured** in the existing measuring pass, on the debayered,
  flat-corrected image, before any warp mixes in the zeros alignment leaves
  outside the source. That pass already opens every frame, so this is free.
- **Solved** once the pass completes and the slot union is final. Keyed by
  slot NAME during measurement because the union is not final before then.
- **Applied** in Phase A after alignment and before caching. After alignment
  so saturation rejection and star detection still see raw values and behave
  exactly as before; before caching so the cache and the cube agree.

A frame the measuring pass rejected has no measurement and keeps the identity.

## Three consequences that had to be handled

1. **The noise model.** `sample_variance` converts the sample to ADU and
   applies a Poisson term. That term is only meaningful on raw ADU, so a frame
   scaled by `a` would have its shot noise misread by exactly `a`. The
   coefficients are therefore carried per slot in `FrameStats` and undone
   before the Poisson term, then carried back through `Var(a*x + b) = a^2
   Var(x)`. This is duplicated in three places -- `pixel_selector.cpp`,
   `select_pixels.cl`, `gpu_cpu_fallback.cpp` -- held together by
   `test_gpu_agreement`.

   At the identity the arithmetic reduces bit-for-bit, so an uncorrected batch
   cannot move. That is asserted with `==`, not `Approx`.

2. **Cloud detection.** It compares each frame's `median_luminance` to the
   median of all frames' medians. Normalisation exists to make those equal, so
   measuring afterwards would leave every frame looking clear and silently
   retire the cloud penalty. The median is now taken before normalisation.

3. **The synthesized L slot.** OSC's L is `0.299R + 0.587G + 0.114B`, built
   from planes that have already been corrected, so it must not be corrected
   again -- it inherits. The noise model still needs the map that was
   effectively applied, which `mix_coefficients` supplies. That function
   returns its input exactly when all inputs agree, because rec709's weights
   sum to `0.9999999999999999` and mixing three identities the long way would
   move every OSC stack that had nothing to correct.

## The estimator defect this uncovered

The first wiring made a real stack **worse**: on the 24 B frames, pixel noise
rose 8% (0.00026157 -> 0.00028243).

`scale` is what the solver divides by, so it has to be noise. MAD about the
median cannot separate noise from structure. On the seven clouded B exposures
it reported 375-1159 where the actual pixel noise was 120-169, so the solver
crushed them by up to 8x. The tell is the exponent:

    d log(MAD) / d log(sky)       = 2.31    -- no photon noise can do this
    d log(nn-diff) / d log(sky)   = 0.53    -- photon noise is 0.5

`measure_channel_sky` now takes the MAD of horizontal nearest-neighbour
DIFFERENCES about their own median, over every adjacent pair, divided by
sqrt(2). A difference cancels anything smooth, so cloud, moon gradient and
vignetting drop out.

Deliberately **not** restricted to background pixels: screening pairs on both
members falling below the median conditions the estimate on the noise it is
measuring and truncates it -- 0.00056 for a true sigma of 0.00100 -- and the
bias vanishes when a gradient is present, so the estimator disagreed with
itself by 1.7x between two frames of identical noise. Stars need no screening:
they are a small minority of pairs, and ignoring minorities is what a MAD is
for.

## What the two halves do

The two halves of `v' = scale*v + offset` are not the same kind of correction
and are separately switchable (`Config::normalize_frames`,
`Config::normalize_scale`).

- **offset** re-levels. It removes the sky-level differences the per-voxel fit
  was mistaking for per-pixel bimodality. It cannot change a frame's signal
  amplitude.
- **scale** equalises noise. On this corpus the sky rise is added skyglow
  (exponent 0.53) while the signal loss is separate cloud extinction: star
  flux falls 10x while sky rises 2.5x. Matching noise scale therefore scales
  already-dim frames down further, and widens the star-flux spread from
  0.090-1.119x of the median to 0.056-1.399x.

## Measured

M27 2025, the 24 B frames -- the corpus with the worst within-filter spread
(4383 to 12962 ADU) and therefore the most to gain. Pixel-scale noise from
horizontal nearest-neighbour differences over background pixels, which is
blind to the smooth gradients that would otherwise dominate:

| corpus | arm | pixel noise | vs off |
|--------|-----|------------:|--------|
| M27 2025 B, 24f mono | off            | 0.00026157 | -- |
|                      | offset + scale | 0.00026165 | +0.03%, neutral |
|                      | offset only    | 0.00024208 | **-7.5%** |
| NGC7635, 65f mono L  | off            | 0.00099188 | -- |
|                      | offset + scale | 0.00023512 | **-76.3%** |
|                      | offset only    | 0.00023288 | **-76.5%** |

Noise goes as 1/sqrt(n), so -7.5% is worth about 17% more frames and -76.5%
is worth about 18x.

NGC7635 is the corpus previously measured at 5.19x the pixel-scale noise of a
plain Huber estimator on identical voxels, and 26.9x of effective exposure
thrown away. Normalisation recovers 4.26x of that by fixing the fit's INPUTS,
which is direct evidence that the bimodality the GMM was coin-flipping on is
per-frame level variation rather than anything per-pixel. The model race does
not have to be abandoned to get most of that back.

The multiplicative half never helps and sometimes hurts.

Before the estimator was fixed, the same corpus measured 0.00028243 with
offset + scale -- 8% WORSE than doing nothing. That regression is entirely
attributable to the scale term being taken from MAD about the median.

### How this was measured

`StackingEngine` is a plain C++ library and needs no PixInsight, so the
instrument is a headless driver that links the static libraries directly and
runs the same corpus through each arm in one process. Corpora must be run ONE
AT A TIME: three concurrent stacks exhaust a 30 GB box and the kernel kills
them.

