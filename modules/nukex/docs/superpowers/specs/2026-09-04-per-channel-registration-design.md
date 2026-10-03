# Per-channel registration — design

**Status:** design, 2026-09-04. Supersedes nothing; new capability.

## The problem, measured

A user stacked 53 frames of M3 (ASI2400MC Pro, RGGB, Optolong L-Quad Enhance,
300 s, 647 mm f.l., 5.94 µm pixels) on v5.0.1.0 and reported coloured rings
around stars in his processed result.

The rings were not in NukeX's output. What is in NukeX's output is a
**positional disagreement between the colour channels**, measured on 245 stars
in `NukeX_stacked`:

| pair | median separation |
|---|---|
| blue vs green | 0.058 px |
| red vs green | **0.435 px** |

(245 stars, selected at the 99.9th percentile in green. A narrower cut of the
83 brightest gives 0.552 px; the wider sample is the one used throughout this
document, so the figures are comparable.)

The red offset is radial and grows with distance from the field centre:

| field radius | radial shift |
|---|---|
| 0–1000 px | +0.19 px |
| 1000–2000 px | +0.47 px |
| 2000–3000 px | +0.48 px |
| 3000–4000 px | +0.48 px |

That is lateral chromatic aberration. It reads as red-specific rather than a
smooth function of wavelength because of the filter: through an L-Quad Enhance
the red photosites record Ha at 656 nm while green and blue both record Hb and
OIII at 486 and 501 nm. Red is imaging 150 nm away from the other two; green
and blue are 15 nm apart and therefore agree.

Fitting a single scale-plus-shift over the whole stack gives **+238 ppm** on
the red plane and takes the median separation from 0.435 px to 0.173 px — a
2.5× improvement from four parameters.

There is a second, independent effect. Measuring five subs spaced across the
five-hour session shows the offset **drifting monotonically by ~0.2 px**:

```
04:58   R-G dx +0.158   dy +0.149
09:48   R-G dx +0.372   dy +0.314
```

(Absolute values there are contaminated by a crude per-lattice debayer used
for the probe; the *change* over time is not, since that artefact is
constant.) A fixed optical term cannot drift, so this is atmospheric
dispersion as the target's altitude changed.

**NukeX registers frames to each other and never registers the channels to
each other.** Nothing in the pipeline can correct either effect today.

## Scope

Correct both effects, per frame, automatically, for every multi-channel stack.
Decided with the user 2026-09-04.

Not in scope: correcting colour *balance* — red is genuinely weak in this data
because M3 has no H-alpha, and no registration fixes that. Not in scope:
higher-order distortion models (see Rejected alternatives).

## Design

### Where it sits

Phase A per frame, unchanged except for one new step:

```
load -> debayer -> flat -> [measure channel transforms] -> align -> warp -> cache -> accumulate
```

`FrameAligner::align` already detects stars and computes a homography before
warping. Channel registration slots between those two: it reuses the stars the
aligner has already found, centroids each channel at those positions, and fits
one affine per channel against green.

The aligner then warps each channel with `H · A_c` rather than `H`. One
resample per channel, exactly as today — the correction is folded into a warp
that was already happening, so there is no compounded interpolation and no
extra pass over the pixels. This is why "always automatic" is affordable.

### Green is the reference

Two changes that are really one:

1. `StarDetector::detect` currently uses **channel 0** of a multi-channel
   image, which for an OSC frame is red. It takes green instead.
2. Green is the fixed channel; red and blue are registered to it.

Green has two of every four photosites on an RGGB sensor, so its centroids are
the best available, and on a multi-band filter it is not the starved channel.
Registering frames on red — the channel with both the colour error and, on a
quad-band filter, the weakest signal — is a defect in its own right.

Mono is a no-op: one channel, nothing to register.

### The model

Per channel, four parameters: uniform scale and translation.

```
x' = s·x + tx
y' = s·y + ty
```

Scale absorbs the optical term (+238 ppm on this data). Translation absorbs
the dispersion drift. Fitted by least squares over matched star centroids.

Rotation is deliberately absent. Lateral colour does not rotate, and neither
does dispersion; a rotation term would only fit noise.

### Interface

New component in `lib/alignment`, no dependency on the engine:

```cpp
struct ChannelTransform {   // identity for the reference channel
    double s = 1.0, tx = 0.0, ty = 0.0;
    int    n_stars   = 0;       // stars that survived to the fit
    double residual  = 0.0;     // median |measured - modelled|, px
    enum class Fit { Affine, TranslationOnly, Identity } fit = Fit::Identity;
};

struct ChannelTransforms { std::vector<ChannelTransform> per_channel; };

ChannelTransforms measure(const Image&, const StarCatalog&, int reference_channel);
```

`HomographyComputer::warp` gains an overload taking `ChannelTransforms` and
applying `H · A_c` per channel.

## Failure handling

Every fallback is a rung on a ladder, and the console names the rung.

| condition | behaviour |
|---|---|
| too few stars for 4 parameters | translation only |
| too few for translation | identity, frame named in the console |
| a channel's per-star SNR too low to centroid | those stars dropped before fitting |
| fitted \|s−1\| > 0.01, or \|tx\|/\|ty\| > 5 px | rejected as a bad solve, identity used |
| star with a neighbour within 13 px | excluded from every channel's fit |
| composed transform within 0.01 px of identity everywhere | warp with `H` alone, skipping the per-channel path |

The last row is what keeps "always automatic" honest: a well-corrected rig
never pays for interpolation it does not need, without a user-facing threshold
to argue about.

A frame that cannot be measured is stacked as it is. Nothing is ever guessed.

### The centroid, which is where the accuracy actually comes from

The fit is four parameters over hundreds of stars, so it is not the fit that
limits accuracy -- it is the per-star centroid. Three choices there were
settled by simulating a known transform and measuring what came back, and each
one moved the result by more than the fit ever could:

| choice | wrong answer | right answer | worst error |
|---|---|---|---|
| box half-width | 4 px | **6 px** | 0.052 -> 0.005 |
| background | minimum of the border | **median of the border** | 0.217 -> 0.057 |
| passes | 1 | **2** | 0.057 -> 0.019 |

The background row is the important one and the least obvious. The minimum of
a noisy ring is biased low, subtracting too little leaves a pedestal under the
star, and a pedestal pulls an intensity-weighted centroid toward the centre of
its own box -- so the error depends on the star's sub-pixel phase and does not
cancel between channels. The figures above are from the case that matters
most, a star five times fainter in red than in green, which is ordinary
through a dual-narrowband filter.

Neighbour exclusion is a separate filter rather than something the star
detector already provides. Its `exclusion_radius` is 5 px and the centroid box
is 13 px wide, so it permits exactly the neighbours that hurt. On a field
where 30% of stars had a companion 7.6 px away the fit erred by 0.029 px using
every star and 0.006 px using only the isolated ones.

## Testing

**Synthetic and exact.** Build a star field, apply a known scale and shift to
the red plane, assert the fit recovers it to within 0.02 px. This is the core
test; everything else is degradation behaviour.

**The ladder.** Few stars gives translation-only; no stars gives identity; an
absurd injected transform is rejected; green returns identity by construction;
a one-channel image takes the no-op path.

**Integration.** Synthetic multi-channel frames through the engine, asserting
the output channels are co-registered.

**Acceptance, on real data.** Re-run the M3 set and measure red-to-green
separation in the stack.

The bar is a RATIO, not an absolute pixel count, and that is a correction to
an earlier draft of this document. Re-measuring the same stack with the final
centroid estimator and a larger star sample gave different absolute numbers
than the figures this document was first written with -- 0.354 px and 0.081 px
rather than 0.435 px and 0.058 px -- because the estimator and the sample both
changed while the spec was being written. Nothing about the data changed.

An absolute threshold inherits that fragility: it silently encodes whichever
estimator happened to measure it. A ratio against the blue-green floor,
measured on the same image with the same code in the same run, does not.

| measured on the shipped v5.0.1.0 stack, 1047 stars | |
|---|---|
| red vs green | 0.354 px |
| blue vs green, the centroid-noise floor | 0.081 px |
| ratio today | **4.4x** |
| **done at** | **ratio at or below 1.5x** |

Blue is the control. It is subject to identical centroid noise and has almost
no colour error to correct, so the ratio isolates what the feature is for.

Green's star FWHM must be unchanged, since green is never resampled. A change
there means something is broken.

M3 becomes a diagnostic case rather than a fourth E2E golden: 53 frames at
24 MP is another long run, and what it proves is a measured number, not a hash.

**Human judgement required before ship:** whether the corner stars look right
after correction, since that is where the correction is largest.

## Goldens

`bayer_rgb_m27_2023` and `bayer_nb_hao3_m16` move. This is a deliberate,
explained re-baseline.

`lrgb_mono_ngc7635` does **not** move — a one-channel stack skips this path
entirely — and stays the frozen anchor proving nothing else drifted. Its
`golden_frozen` flag is the mechanism that makes that claim checkable.

## Rejected alternatives

**Per-frame radial polynomial.** More faithful to how lateral colour behaves,
and would chase the residual below 0.17 px. Rejected: it needs many more stars
per frame to fit safely, so sparse and cloudy frames would fall back
constantly, and the extra tenth of a pixel does not pay for that fragility.

**Tile-wise cross-correlation of the channel planes.** Needs no star detection
and works on nebulosity. Rejected, and this one would have been actively
harmful: in dual-narrowband the red channel is H-alpha while green and blue
are OIII, and those show **genuinely different nebulosity**. Correlating them
would register real astrophysical structure differences as if they were
misalignment. Stars are the correct feature precisely because a point source
is a point source at every wavelength.

**Measuring once on the stacked result.** One fit, one resample, negligible
cost, and it delivers the measured 2.5×. Rejected because it cannot see the
0.2 px session drift — that is baked into the stack by the time it could be
measured.

**A threshold or an opt-in checkbox.** Rejected with the user: almost nobody
knows they have lateral colour, so the people who need it would not find it.
The near-identity skip above gives the same saving without a knob.

## Decided, with a revisit trigger

**Each channel is fitted independently.** The alternative was a joint fit
across red and blue sharing one scale with separate translations. Lateral
colour is a smooth function of wavelength, so the two scales are physically
related and a joint fit would use every star twice — but through a multi-band
filter the relationship depends on which lines land on which photosites, and
this data is the case where it breaks down: red is 150 nm from green, blue is
15 nm. Independent fits make no assumption that has to hold.

Revisit only on evidence: if blue's fitted scale is noisy across frames on real
data, that is the signal that blue has too few usable stars to fit alone, and a
shared scale becomes worth its assumption.
