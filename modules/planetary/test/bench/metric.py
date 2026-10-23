"""metric.py -- the PlanetaryStack benchmark's sharpness-at-equal-noise measure, END (judged), and
DAN (secondary).

END: EQUAL-NOISE DETAIL (Ruling P28; the spec's "one documented sharpness-at-equal-noise metric")
    The question: sharpened until its noise is the same, how much of the target's detail does an
    image give back? A linear filter scales signal and noise alike at every frequency, so what
    any sharpening can recover is set by the per-frequency SNR -- and the best it can do is a
    matched (Wiener) restoration. END is that restoration's quality on the box:
    1-4. Luminance, the target box, the normalisation x = (crop - b) / s, and the Hann-windowed
       periodogram P, exactly as DAN (below).
    5. Noise: ONE model for every image -- ours and every reference -- white noise of power
       N = sigma_frame^2 / k: sigma_frame^2 the capture's per-frame noise on the box (noise.py: photon
       transfer on the raw frames, independent of any stacker), k the image's effective frame count
       (ours: the noise-power k_eff over the box, from the engine's own plan -- stack-region-probe
       --keff-box; a reference: its stacking percentage x the capture's frames). Any smoothing, on
       either side, then counts as blur, as in DAN and Ruling 1. Not read from the image itself: the
       half-stack ground truth on our own stack showed the box's 0.40-0.50 power is 2.7-55 x its random
       noise -- structure the interleaved halves share (on the 2026-01-30 Jupiter: real horizontal
       structure, which ASIStudio shows too, and an isotropic part that is neither target-locked nor
       sensor-fixed; README, "What the interleaved halves share") -- and the sky's noise is
       600-54 000 x below the target's (README).
    6. Per END_EDGES bin (0.06-0.30 in steps of 0.03 cycles/px): SNR = max(mean P - N, 0) / N.
    7. END = sqrt( sum_f O(f) / sum_f O(f) / (1 + SNR(f)) ) over the band's frequency samples,
       O(f) = f^-3 (END_PRIOR): the band's object power over the error power of the matched Wiener
       restoration, as an amplitude ratio. With known blur and object, a Wiener restoration loses
       O / (1 + SNR) at each frequency; the object's spectrum is unknown but common to every image of
       a capture, and one fixed prior stands in for it. Higher is better; ratios between images are
       what Ruling 2's 0.97 bar judges.
    Validation (test_end.py; numbers in README.md): blur at equal noise ranks strictly lower; more
    frames at the same blur rank higher (x1.19 for 2x, x1.48 for 4x; DAN moves 0.3 %); sharper-but-
    noisier pairs and a capture's 20 % / 50 % stacks rank as the exact oracle Wiener restoration does
    (both outcomes occur in each set); a random scan of 157 clear pairs over object spectra
    f^-1.4..f^-4.4 agreed 157/157 (log-capacity: 96 %, so the restoration form was chosen); a tool's
    smoothing never raises it (drizzle x3 + area resample x0.81, Gaussian 0.7 x0.75, B-spline
    half-pixel shift x0.98, where reading the box's own white floor as its noise RAISES it -- the bias
    the photon budget removes).
    KNOWN LIMIT -- NOISE AND SHARPENING THE BUDGET DOES NOT HOLD (C1; pinned in test_end.py as strict
    xfails): END counts any power the photon budget does not explain as detail. Noise beyond the
    budget (uniform, shot-like, target-only) and an unsharp mask RAISE it (measured x1.18-1.88 on
    synthetic stacks, x1.02-1.94 on real AutoStakkert P20 stacks; README has the table). No
    single-image estimator can charge it without also charging genuine sharpness: a linear sharpening
    G of a stack whose own kernel is K is the same image, spectrum for spectrum, as an honest stack with
    kernel G K, which END must rank higher whenever G K <= 1; and noise added to a smoothed stack
    cannot be told from the stack's own (unknown) noise kernel. Measured headroom (the box's fitted
    white level over the budget): a B-spline-interpolated synthetic 0.67, a drizzled one 0.10; the
    real stacks 0.00-0.29 for ASIStudio and 0.03-0.29 for ours.
    HOW THE BENCH JUDGES DESPITE IT (Ruling P28b): ours is not a single image -- the split halves
    measure its noise -- so ours is charged N(f) = budget x max(1, R(f) / R_cal(f)) per END bin
    (noise_factor; halves.charge), the references stay on the budget. C1 then only inflates a
    reference's END, which only lowers ours' ratio: a numeric pass is sound, a numeric fail is sound
    only against a reference whose files record no processing (refs.provenance).
    END_INTERIOR: the same on the interior window's high-passed periodogram (DAN_INTERIOR below),
    the noise model times the high-pass's power gain, with sigma_frame^2 over the interior window.
    LIMITS: our k_eff comes from stack-region-probe's plan, which repeats RunStack's selection steps;
    the bench checks its per-region kept counts against stack-probe's (RunStack's own) and reports END
    unmeasured when they differ. The flip check is our k as the frames used x the unweighted mean kept
    fraction over regions (benchset.ours_frames_mean). AutoStakkert's percentage is global,
    approximate per alignment point. sigma_frame reads up to +7 % (+16 % on a 6 px disc) on a
    textured target moving between frames (noise.py), a factor common to every image's budget; row or
    column banding is invisible to it, and only ours is charged that (through the split halves).

DAN -- SECONDARY SINCE RULING P28: it removes each image's own noise, so it gives no credit for SNR
(a stack of 4x the frames at the same blur scores the same).

DEFINITION ("detail above noise", DAN)
    For an image (ours, AutoStakkert's, ASIStudio's), all registered to the same grid:
    1. Luminance: the mean of the colour channels (mono as is).
    2. Crop: one target box, chosen once on the anchor image (target_box) and applied to every
       image compared -- the planet's disc (largest component above 15 % of the low-passed
       maximum, so moons are left out; thin components on the frame edge are stacking
       artefacts and are skipped) plus a 15 % margin of sky, or for a frame-filling Moon the
       frame minus a 2 % inset. A box under MIN_BOX_SIDE (48 px; a tiny disc such as Mars) is
       grown about the target to that side with sky.
    3. Normalisation: x = (crop - b) / s, with b = the 1st percentile of the crop low-passed
       at sigma 4 px (the sky, or the darkest large-scale area of a lunar crop) and
       s = mean(crop) - b, the mean target signal above it. Gain and offset drop out exactly;
       a blur does not move the mean, so it cannot move s.
    4. Power spectrum: P = |FFT(w * (x - mean_w x))|**2 / sum(w**2) with a 2-D Hann window w
       (no edge leakage from a frame-filling crop). P is in variance-per-pixel units: white
       noise of variance v gives P = v at every frequency.
    5. Noise floor: N = median(P over 0.40 <= f <= 0.50 cycles/px) / ln 2 -- the outer annulus,
       where a seeing-limited stack has no signal left; the median of an exponential
       (periodogram) distribution is ln 2 times its mean, and the median ignores spikes.
    6. DAN = sqrt( sum over 0.06 <= f < 0.30 cycles/px of (P - N) / n_pixels ), clipped at 0:
       the RMS amplitude, as a fraction of the mean target signal, of the image's detail on the
       scales that separate a good stack from a soft one, with the noise power removed.

WHY THIS MEASURE
    * Sharpness: seeing and misalignment are low-pass filters; they take power out of the
      0.06-0.30 cycles/px band (features 3-17 px across: belts, festoons, ring edges,
      craterlets, the limb). Below 0.06 is the disc's shape, which every stack has.
    * At equal noise: a plain gradient or Laplacian energy rises with noise (noise is
      high-frequency power), so a stack of fewer frames could "win" by being noisier. Here the
      noise power is estimated in each image and subtracted, so DAN is what each image would
      score with its noise removed -- the same footing for a 20 % stack and a 50 % stack.
      Adding white noise leaves DAN unchanged in expectation (validated in test_metric.py).
    * It is translation invariant (a power spectrum), so references are cropped at the
      registered integer offset and never interpolated: no resampling blur is charged to a
      reference (the drizzle area-resampling of Ruling 1 is the only resampling).
    * One number per image, computed identically for all; band_snr says how far above the
      noise the band is (noise-floor mistakes matter only when it is near 1).

DAN_INTERIOR (review round 1): the same question asked of the surface alone
    On a small disc DAN_full is limb-weighted: on the 29 px 2_12_2023 AS_P20 Jupiter an
    interior-only blur sigma 1 costs 5.4 % of DAN, a whole-image blur 10 % (reviewer-measured),
    so a soft interior behind a crisp limb could pass. interior_window(anchor, box) takes the
    anchor's target mask (target_mask), erodes it by max(3 px, 2 x the limb's blur sigma,
    estimated from the limb's gradient), and tapers it over 2 px (cosine); measure(...,
    interior=window) then high-passes x inside the UNERODED mask (normalised convolution: x minus
    its sigma-3 Gaussian mean over target pixels only, so the limb step does not leak inward),
    and reports DAN_interior = sqrt(mean_w(hp^2) - N_int * g): the window-weighted RMS of that
    high-pass minus its noise, with N_int the noise floor of the windowed interior itself (median
    periodogram power at 0.40-0.50 / ln 2; photon noise on the disc exceeds the sky's) and g the
    high-pass's white-noise gain. It is judged (interior_judged) only when the eroded disc is at
    least INTERIOR_MIN_DIAMETER = 20 px across (equivalent diameter); below that -- Mars -- the
    result is limb-only and the side-by-side review decides. The erosion uses the sharpest limb
    of the capture (anchor and references, refs.compare), not the anchor's own.

LIMITS (stated, not hidden)
    * Valid only between UNSHARPENED LINEAR stacks. Sharpening buys DAN: a disc softened sigma
      1.6 and then unsharp-masked scores at least the 0.97 bar against the genuinely sharp disc
      (pinned in test_metric.py as a known limit). No automatic detector was found that
      separates them -- measured: the noise-floor/band-power ratio is unchanged by unsharp
      masking (3.05e-4 vs 3.00e-4 at R 15) while the references' own ratios span 1e-6..7e-3;
      limb undershoot moves 1.2-1.5 % against references at -0.4..-38 % (glow); sky-noise
      spectral tilt moves 0.74 -> 0.87-0.95 against references at 0.001..0.6. So: our stack is
      unsharpened by construction in this increment (cubic B-spline interpolation, no
      deconvolution; sharpening is increment 2's sliders); ASIStudio's .fit is its unsharpened
      stack (its .jpg carries the sharpening: verified side by side, ringing halos only in the
      .jpg; the .fit header's SHARPEN/PB/PC/PN/PS record the export settings); AutoStakkert's
      _conv files and every .jpg are refused.
    * The floor assumes the stack's noise is roughly white. Interpolated or resampled stacks
      have less noise power near Nyquist than mid-band, so part of their mid-band noise can
      count as detail; band_snr is reported so a low-SNR comparison is visible.
    * mosaic=True (flatten_cfa: the crop's four CFA sites scaled to one mean) is a FALLBACK
      only, never a pass/fail: a raw-CFA mono stack is a different representation from a
      debayered reference. The review re-mosaicked AS_P20 in all four phases and measured the
      bias of mosaic mode against RGB luminance: Jupiter x1.019-1.026, Moon x0.930-1.094 by
      phase (2x2 binning: Jupiter +-0.8 %, Moon 0.969-1.067). The benchmark stacks every
      capture debayered instead (stack-probe --bayer, pattern per capture in benchset.py).
"""
import math
from dataclasses import dataclass
from typing import NamedTuple, Optional

import numpy as np
from scipy import ndimage

BAND = (0.06, 0.30)          # cycles per measurement pixel
NOISE_BAND = (0.40, 0.50)    # cycles per measurement pixel
LOWPASS_SIGMA = 4.0          # px, for the background level b
BG_PERCENTILE = 1.0
BOX_THRESHOLD = 0.15         # target = low-passed luminance above b + 15 % of the range
BOX_MARGIN = 0.15            # of the target's larger side, at least 8 px
MIN_BOX_SIDE = 48            # px: a tiny disc's box grows (sky only) to this, so the reference overlap
                             # inset (refs.OVERLAP_INSET) still leaves refs.compare's 32 px floor
BOX_INSET = 0.02             # of each frame side, at least 4 px
EDGE_ARTEFACT_SHARE = 0.10   # an edge-touching component thinner than this share is no target
EDGE_NEAR = 0.03             # "touching" = within 3 % of the side (at least 4 px) of the edge
INTERIOR_HP_SIGMA = 3.0      # px, the interior high-pass
INTERIOR_MIN_EROSION = 3.0   # px
INTERIOR_TAPER = 2.0         # px, cosine edge of the interior window
INTERIOR_MIN_DIAMETER = 20.0 # px, equivalent diameter of the eroded disc for the interior to be judged
END_EDGES = (0.06, 0.09, 0.12, 0.15, 0.18, 0.21, 0.24, 0.27, 0.30)   # cycles/px: END's bins over BAND
END_PRIOR = 3.0              # END's object power spectrum ~ f^-3 (one prior for every image)


class Box(NamedTuple):
    y0: int
    y1: int
    x0: int
    x1: int


@dataclass(frozen=True)
class Result:
    value: float          # DAN
    band_power: float     # mean P over BAND
    noise_floor: float    # N
    band_snr: float       # (band_power - N) / N
    background: float     # b (input units)
    signal: float         # s (input units)
    box: Box
    mosaic: bool
    interior: Optional[float] = None         # DAN_interior (None when not judged)
    interior_judged: bool = False
    interior_diameter: Optional[float] = None
    # END (Ruling P28; see END in the module doc). None when no noise budget was given or in mosaic
    # mode -- end_note says why, loudly; never a fallback to the image's own noise alone.
    end: Optional[float] = None               # END on the box, noise = the photon budget
    end_interior: Optional[float] = None      # END of the interior (None when not judged)
    end_snr: Optional[tuple] = None           # per-bin SNR over END_EDGES
    end_noise_budget: Optional[float] = None  # the photon budget given (sigma_frame^2 / k_eff)
    end_noise_factor: Optional[tuple] = None  # our measured-noise charge per bin (Ruling P28b), when given
    end_frames: Optional[float] = None        # the k the budget used (refs.compare)
    end_note: Optional[str] = None


@dataclass(frozen=True)
class InteriorWindow:
    """Chosen once on the anchor, in box coordinates, and applied to every image."""
    window: np.ndarray        # eroded target mask with a 2 px cosine edge, [0, 1]
    mask: np.ndarray          # uneroded target mask (bool), for the normalised high-pass
    diameter: float           # equivalent diameter of the eroded mask, px
    erosion: float            # px
    limb_sigma: float         # estimated blur sigma of the limb, px

    @property
    def judged(self) -> bool:
        return self.diameter >= INTERIOR_MIN_DIAMETER


def luminance(img):
    """2-D float64 luminance of a HxW or HxWxC (C = 1..4) image; colour = channel mean."""
    a = np.asarray(img, dtype=np.float64)
    if a.ndim == 2:
        return a.copy()
    if a.ndim == 3 and 1 <= a.shape[2] <= 4:
        return a.mean(axis=2)
    raise ValueError(f"expected HxW or HxWxC image, got shape {a.shape}")


def flatten_cfa(lum):
    """Scale each of the four 2x2 sites (by parity) to the crop's overall mean: removes a raw
    CFA's per-site gain checkerboard; any crop parity works (all four sites are equalised)."""
    out = np.array(lum, dtype=np.float64)
    mean = out.mean()
    for dy in (0, 1):
        for dx in (0, 1):
            site = out[dy::2, dx::2]
            m = site.mean()
            if not m > 0:
                raise ValueError("a CFA site has no signal; cannot flatten the mosaic")
            site *= mean / m
    return out


def target_extent(img) -> Box:
    """Bounding box of the target itself: the largest connected component of the low-passed
    luminance above b + 15 % of (its maximum - b); moons and specks are smaller components.
    A 3x3 median first, so a hot pixel cannot set the maximum. (The maximum, not a high
    percentile: a 30 px Saturn fills 0.06 % of a 1280x960 frame, below any percentile cut.)"""
    return _target_component(img)[0]


def target_mask(img) -> np.ndarray:
    """The target component of target_extent as a boolean image, holes filled."""
    return ndimage.binary_fill_holes(_target_component(img)[1])


def _target_component(img):
    lum = luminance(img)
    lp = ndimage.gaussian_filter(ndimage.median_filter(lum, size=3), 2.0)
    bg, top = np.percentile(lp, BG_PERCENTILE), float(lp.max())
    if not top > bg:
        raise ValueError("no target: the image is flat")
    labels, n = ndimage.label(lp > bg + BOX_THRESHOLD * (top - bg))
    if n == 0:
        raise ValueError("no target above the threshold")
    sizes = ndimage.sum_labels(np.ones_like(lum), labels, index=np.arange(1, n + 1))
    h, w = lum.shape
    best = None
    for k, sl in enumerate(ndimage.find_objects(labels)):
        y0, y1, x0, x1 = sl[0].start, sl[0].stop, sl[1].start, sl[1].stop
        my, mx = max(4, EDGE_NEAR * h), max(4, EDGE_NEAR * w)     # stackers leave black borders
        on_edge = y0 <= my or x0 <= mx or y1 >= h - my or x1 >= w - mx
        # A thin component on the frame edge is a stacking artefact (AutoStakkert's drizzle
        # edge strip, a padding seam), not a target: a real target on the edge is a planet
        # leaving the frame or the Moon, both wide in both directions.
        if on_edge and min((y1 - y0) / h, (x1 - x0) / w) < EDGE_ARTEFACT_SHARE:
            continue
        if best is None or sizes[k] > sizes[best[0]]:
            best = (k, Box(y0, y1, x0, x1))
    if best is None:
        raise ValueError("only thin structures on the frame edge; no target")
    return best[1], labels == best[0] + 1


def _min_side(a, b, lo, hi):
    """[a, b) grown about its centre to at least MIN_BOX_SIDE (when the frame allows), then
    clipped to [lo, hi) and shifted back inside rather than shrunk."""
    if b - a < MIN_BOX_SIDE:
        c = (a + b) / 2
        a, b = int(math.floor(c - MIN_BOX_SIDE / 2)), int(math.floor(c - MIN_BOX_SIDE / 2)) + MIN_BOX_SIDE
        if a < lo:
            a, b = lo, lo + MIN_BOX_SIDE
        if b > hi:
            a, b = hi - MIN_BOX_SIDE, hi
    return max(lo, a), min(hi, b)


def target_box(img):
    """The target crop on this image's grid (see DEFINITION step 2)."""
    lum = luminance(img)
    h, w = lum.shape
    ty0, ty1, tx0, tx1 = target_extent(lum)
    margin = max(8, int(round(BOX_MARGIN * max(ty1 - ty0, tx1 - tx0))))
    iy, ix = max(4, int(round(BOX_INSET * h))), max(4, int(round(BOX_INSET * w)))
    y0, y1 = _min_side(ty0 - margin, ty1 + margin, iy, h - iy)
    x0, x1 = _min_side(tx0 - margin, tx1 + margin, ix, w - ix)
    if y1 - y0 < 16 or x1 - x0 < 16:
        raise ValueError(f"target box too small: {(y0, y1, x0, x1)}")
    return Box(int(y0), int(y1), int(x0), int(x1))


def limb_sigma(img, mask=None) -> float:
    """Blur sigma (px) of the target's edge: a Gaussian-blurred step of height H has a peak
    gradient H / (sigma sqrt(2 pi)). H = (median just inside the edge) - (median of the sky
    just outside), G = median over edge pixels of the local maximum gradient; iterated, since
    "just inside" scales with sigma. Edge pixels on the frame border are not limb. Limb
    darkening makes the edge read softer than it is (synthetic disc, true sigma 0.7 / 1 / 2 / 3:
    reads 1.04 / 1.23 / 2.20 / 3.09), which only erodes the interior window a little more."""
    lum = luminance(img)
    if mask is None:
        mask = target_mask(lum)
    pre = 0.7                                       # px; removed again in quadrature
    lp = ndimage.gaussian_filter(lum, pre)
    gy, gx = np.gradient(lp)
    grad = np.hypot(gy, gx)
    d_in = ndimage.distance_transform_edt(mask)
    d_out = ndimage.distance_transform_edt(~mask)
    edge = mask & ~ndimage.binary_erosion(mask)
    edge[:3, :] = edge[-3:, :] = False
    edge[:, :3] = edge[:, -3:] = False
    if not edge.any():
        raise ValueError("the target has no limb inside the frame")
    sigma = 1.5
    for _ in range(4):
        reach = int(math.ceil(2 * sigma)) + 1
        inner = lp[(d_in >= reach) & (d_in < reach + 3)]
        sky = lp[(d_out >= reach + 2) & (d_out < reach + 8)]
        if inner.size == 0 or sky.size == 0:
            break
        height = float(np.median(inner) - np.median(sky))
        g = float(np.median(ndimage.maximum_filter(grad, size=2 * reach + 1)[edge]))
        if not (height > 0 and g > 0):
            break
        sigma = float(np.clip(height / (math.sqrt(2 * math.pi) * g), 0.3, 50.0))
    return math.sqrt(max(sigma ** 2 - pre ** 2, 0.09))


def interior_window(img, box: Box, limb: Optional[float] = None) -> InteriorWindow:
    """The interior window for DAN_interior on `box` (see DAN_INTERIOR): the target mask of
    `img` (the anchor) eroded by max(3 px, 2 x `limb`). `limb` defaults to img's own limb sigma;
    refs.compare passes the SHARPEST limb among the anchor and the references, so the window --
    and whether the interior is judged -- is a property of the capture, and a softer stack of
    ours cannot erode itself under 20 px and escape the interior bar."""
    lum = luminance(img)
    full = target_mask(lum)
    sig = limb_sigma(lum, full) if limb is None else float(limb)
    erosion = max(INTERIOR_MIN_EROSION, 2.0 * sig)
    mask = full[box.y0:box.y1, box.x0:box.x1]
    # distance to the limb or to the crop's edge (a lunar disc runs past the box)
    d = ndimage.distance_transform_edt(np.pad(mask, 1))[1:-1, 1:-1]
    inner = d > erosion
    d2 = ndimage.distance_transform_edt(np.pad(inner, 1))[1:-1, 1:-1]
    w = np.where(inner, 0.5 - 0.5 * np.cos(np.pi * np.clip(d2 / INTERIOR_TAPER, 0, 1)), 0.0)
    diameter = 2.0 * math.sqrt(float(inner.sum()) / math.pi)
    return InteriorWindow(w, mask, diameter, erosion, sig)


def _interior_scores(x, iw: InteriorWindow, budget: Optional[float], factor=None):
    """(DAN_interior, END_interior) of normalised crop x on the interior window; END_interior is
    None without a noise budget."""
    m = iw.mask.astype(np.float64)
    num = ndimage.gaussian_filter(x * m, INTERIOR_HP_SIGMA)
    den = ndimage.gaussian_filter(m, INTERIOR_HP_SIGMA)
    hp = x - num / np.maximum(den, 1e-6)
    w2 = iw.window ** 2
    ms = float((w2 * hp ** 2).sum() / w2.sum())
    ys, xs = np.nonzero(iw.window > 0)
    sl = (slice(ys.min(), ys.max() + 1), slice(xs.min(), xs.max() + 1))
    w, xx = iw.window[sl], x[sl]
    xw = (xx - (w * xx).sum() / w.sum()) * w
    p = np.abs(np.fft.fft2(xw)) ** 2 / (w ** 2).sum()
    f = _radial_frequency(xw.shape)
    nb = (f >= NOISE_BAND[0]) & (f <= NOISE_BAND[1])
    n_int = float(np.median(p[nb]) / math.log(2))
    fc = _radial_frequency(x.shape)
    gain = float(np.mean((1.0 - np.exp(-2.0 * (math.pi * INTERIOR_HP_SIGMA * fc) ** 2)) ** 2))
    dan = math.sqrt(max(ms - n_int * gain, 0.0))
    if budget is None:
        return dan, None
    # END of the interior: the periodogram of the windowed high-pass (without it the limb-darkening
    # slope times the window would leak into the low bins). The high-pass is linear, so it scales
    # signal and noise alike at every frequency: the noise model is multiplied by its power gain g.
    hw = hp[sl] * w
    ph = np.abs(np.fft.fft2(hw - hw.mean())) ** 2 / (w ** 2).sum()
    g = (1.0 - np.exp(-2.0 * (math.pi * INTERIOR_HP_SIGMA * f) ** 2)) ** 2
    return dan, _end_from_periodogram(ph, budget, g, factor)[0]


def _end_from_periodogram(p, noise_power, gain=None, factor=None):
    """(END, per-bin SNR) of periodogram `p` (variance-per-pixel units, on its own fftfreq grid)
    against white noise of power `noise_power` (times the per-frequency power gain `gain` of a
    linear pre-filter, when given; times the per-bin `factor`, our measured-noise charge, when given):
    per END_EDGES bin, SNR = max(mean P - N, 0) / N, and
    END = sqrt( sum_f O(f) / sum_f O(f) / (1 + SNR(f)) ) over the band's frequency samples, with
    the object prior O(f) = f^-END_PRIOR (see END in the module doc)."""
    f = _radial_frequency(p.shape)
    num = den = 0.0
    snrs = []
    for i, (a, b) in enumerate(zip(END_EDGES[:-1], END_EDGES[1:])):
        m = (f >= a) & (f < b)
        if not m.any():
            continue
        noise = noise_power * (float(gain[m].mean()) if gain is not None else 1.0) * (factor[i] if factor else 1.0)
        snr = max(float(p[m].mean()) - noise, 0.0) / noise
        snrs.append(snr)
        o = float((f[m] ** -END_PRIOR).sum())       # the bin's object power under the prior
        num += o
        den += o / (1.0 + snr)
    return math.sqrt(num / den), tuple(snrs)


def periodogram(x) -> np.ndarray:
    """The metric's periodogram of a crop: the 2-D Hann window, the window-weighted mean removed,
    |FFT|^2 / sum(w^2) -- variance-per-pixel units (white noise of variance v gives v everywhere)."""
    x = np.asarray(x, dtype=np.float64)
    w = np.outer(np.hanning(x.shape[0]), np.hanning(x.shape[1]))
    x = x - (w * x).sum() / w.sum()
    return np.abs(np.fft.fft2(x * w)) ** 2 / (w ** 2).sum()


def band_means(p, edges) -> list:
    """Mean of periodogram `p` over each annulus [edges[i], edges[i + 1]) of radial frequency."""
    f = _radial_frequency(p.shape)
    return [float(p[(f >= a) & (f < b)].mean()) for a, b in zip(edges[:-1], edges[1:])]


def _radial_frequency(shape):
    fy = np.fft.fftfreq(shape[0])[:, None]
    fx = np.fft.fftfreq(shape[1])[None, :]
    return np.hypot(fy, fx)


def measure(img, box: Optional[Box] = None, *, mosaic=False,
            interior: Optional[InteriorWindow] = None, noise_power: Optional[float] = None,
            noise_power_interior: Optional[float] = None, noise_factor=None) -> Result:
    """DAN and END of `img` (HxW or HxWxC array, any linear units) on `box` (default: its own
    target_box), and DAN_interior / END_interior when an anchor's interior window is given (judged
    only when the window is >= INTERIOR_MIN_DIAMETER across). `noise_power` is the image's photon
    budget on the box, sigma_frame^2 / k_eff in the metric's units (a fraction of the box's mean
    target signal squared; noise.box_noise / k), and `noise_power_interior` the same over the
    interior window (default: noise_power). `noise_factor` (one per END_EDGES bin, >= 1) multiplies
    the noise bin by bin: our stack's measured-noise charge (Ruling P28b, halves.charge). Without a
    budget END is None and end_note says so. Raises ValueError when there is no target signal."""
    if noise_factor is not None and (len(noise_factor) != len(END_EDGES) - 1 or min(noise_factor) < 1.0):
        raise ValueError(f"noise_factor needs {len(END_EDGES) - 1} values >= 1, got {noise_factor}")
    lum = luminance(img)
    if box is None:
        box = target_box(lum)
    box = Box(*map(int, box))
    crop = lum[box.y0:box.y1, box.x0:box.x1]
    if mosaic:
        crop = flatten_cfa(crop)
    if min(crop.shape) < 16:
        raise ValueError(f"crop too small to measure: {crop.shape}")
    bg = float(np.percentile(ndimage.gaussian_filter(crop, LOWPASS_SIGMA), BG_PERCENTILE))
    signal = float(crop.mean()) - bg
    if not signal > 0 or not np.ptp(crop) > 0:
        raise ValueError("no target signal above the background in the crop")
    power = periodogram((crop - bg) / signal)
    f = _radial_frequency(crop.shape)
    band = (f >= BAND[0]) & (f < BAND[1])
    noise = (f >= NOISE_BAND[0]) & (f <= NOISE_BAND[1])
    floor = float(np.median(power[noise]) / math.log(2))
    band_power = float(power[band].mean())
    excess = (float(power[band].sum()) - floor * int(band.sum())) / power.size
    end = end_snr = end_note = None
    budget = noise_power
    if mosaic:
        end_note, budget = "END not computed in mosaic mode (a fallback, never a pass/fail)", None
    elif budget is None:
        end_note = "END not computed: no noise budget (per-frame noise and frame count) for this image"
    elif not budget > 0:
        raise ValueError(f"noise budget must be positive, got {budget}")
    if budget is not None:
        end, end_snr = _end_from_periodogram(power, budget, factor=noise_factor)
    interior_value = end_int = None
    judged, diameter = False, None
    if interior is not None:
        if interior.window.shape != crop.shape:
            raise ValueError(f"interior window {interior.window.shape} does not match the crop {crop.shape}")
        diameter, judged = interior.diameter, interior.judged
        if judged:
            b_int = None if budget is None else (noise_power_interior if noise_power_interior is not None else budget)
            interior_value, end_int = _interior_scores((crop - bg) / signal, interior, b_int, noise_factor)
    return Result(value=math.sqrt(max(excess, 0.0)), band_power=band_power, noise_floor=floor,
                  band_snr=(band_power - floor) / floor if floor > 0 else math.inf,
                  background=bg, signal=signal, box=box, mosaic=mosaic,
                  interior=interior_value, interior_judged=judged, interior_diameter=diameter,
                  end=end, end_interior=end_int, end_snr=end_snr, end_noise_budget=budget,
                  end_noise_factor=tuple(noise_factor) if noise_factor else None, end_note=end_note)


def measure_path(path, box: Optional[Box] = None, *, mosaic=False) -> Result:
    """DAN of an image file (FITS / TIFF / PNG / JPEG, via refs.load_image), no resampling."""
    import refs  # local import: refs imports this module
    return measure(refs.load_image(path), box, mosaic=mosaic)
