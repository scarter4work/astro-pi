"""metric.py -- the PlanetaryStack benchmark's sharpness-at-equal-noise measure, END (judged), and
DAN (secondary).

END: EQUAL-NOISE DETAIL (Ruling P28; the spec's "one documented sharpness-at-equal-noise metric")
    The question: sharpened until its noise is the same, how much of the target's detail does an
    image give back? A linear filter scales signal and noise alike at every frequency, so what
    any sharpening can recover is set by the per-frequency SNR -- and the best it can do is a
    matched (Wiener) restoration. END is that restoration's quality on the box:
    1-4. Luminance, the target box, the normalisation x = (crop - b) / s, and the Hann-windowed
       periodogram P, exactly as DAN (below).
    5. Noise. NOT read from the image's own high frequencies: a stacker's interpolation, drizzle,
       resampling or denoising suppresses its noise toward Nyquist, and the half-stack ground truth
       on our own stack showed the box's 0.40-0.50 power is 2.7-55 x its random noise (structure
       common to disjoint halves) while sky noise is 600-54 000 x below the target's (README).
       Instead every image -- ours and every reference -- is judged against ONE model: white noise of
       power N = sigma_frame^2 / k, sigma_frame^2 the capture's per-frame noise on the box
       (noise.py: photon transfer on the raw frames, independent of any stacker) and k the image's
       effective frame count (ours: from stack-probe's region counts; a reference: its stacking
       percentage x the capture's frames). Any smoothing, on either side, then counts as blur, as
       in DAN and Ruling 1.
       Plus the image's EXCESS noise, read where no detail can hide it: the white power of its own
       signal-free sky at 0.40-0.50 cycles/px (sky_noise_floor) beyond the photon budget at the
       sky's level (noise.sky_noise / k). A stack noisier than its frame count implies is charged
       for it (so added noise can never raise END); detail at Nyquist on the target is never read
       as noise (that would penalise a genuinely sharp image -- measured: it reversed the 20 %/50 %
       synthetic ranking at low noise). N = budget + max(0, sky excess). Without a usable sky the
       judged END is None (end_note says so; end_pure, the budget alone, is still reported).
    6. Per END_EDGES bin (0.06-0.30 in steps of 0.03 cycles/px): SNR = max(mean P - N, 0) / N.
    7. END = sqrt( sum_f O(f) / sum_f O(f) / (1 + SNR(f)) ) over the band's frequency samples,
       O(f) = f^-3 (END_PRIOR): the band's object power over the error power of the matched Wiener
       restoration, as an amplitude ratio. With known blur and object, a Wiener restoration loses
       O / (1 + SNR) at each frequency; the object's spectrum is unknown but common to every image of
       a capture, and one fixed prior stands in for it. Higher is better; ratios between images are
       what Ruling 2's 0.97 bar judges.
    Validation (test_end.py; numbers in README.md): blur at equal noise ranks strictly lower; added
    noise never raises it; more frames at the same blur rank higher (x1.19 for 2x, x1.48 for 4x;
    DAN moves 0.3 %); sharper-but-noisier pairs and a capture's 20 % / 50 % stacks rank as the
    exact oracle Wiener restoration does (both directions occur); a random scan of 157 clear pairs
    over object spectra f^-1.4..f^-4.4 agreed 157/157 (log-capacity: 96 %, so the restoration form
    was chosen); a tool's smoothing never raises it (drizzle x3 + area resample x0.81, Gaussian 0.7
    x0.75, B-spline half-pixel shift x0.98, where reading the image's own floor as its noise would
    have RAISED it x2.06 / x3.08 / x1.62 -- the bias the photon budget removes).
    END_INTERIOR: the same on the interior window's high-passed periodogram (DAN_INTERIOR below),
    the noise model times the high-pass's power gain, with sigma_frame^2 over the interior window.
    LIMITS: our k is the frames used x the mean kept fraction over regions (the exact (sum w)^2 /
    sum w^2 needs per-frame region membership RunStack does not return); the bench also scores the
    harmonic-mean reading and sends a verdict that flips between them to review. AutoStakkert's
    percentage is global, approximate per alignment point. sigma_frame reads 4-11 % high on a
    textured target drifting 0.5 px per frame (noise.py), a factor common to every image. Detail
    and fixed-pattern structure in the band count as signal for every image alike.

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
SKY_TILE = 48                # px: the sky tiles of the excess-noise check
SKY_EDGE = 6                 # px kept clear of the frame edge
SKY_GUARD = 10               # px between a sky tile and any structure
SKY_STRUCTURE = 0.03         # structure = low-pass above the sky level + 3 % of (max - sky level)
SKY_MIN_TILES = 8


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
    end: Optional[float] = None               # judged: noise = max(budget, the box's white floor)
    end_pure: Optional[float] = None          # the same with the budget alone (the flip check)
    end_interior: Optional[float] = None      # judged, the interior (None when not judged)
    end_interior_pure: Optional[float] = None
    end_snr: Optional[tuple] = None           # judged per-bin SNR over END_EDGES
    end_noise: Optional[float] = None         # the judged noise power on the box
    end_noise_budget: Optional[float] = None  # the photon budget given (sigma_frame^2 / k_eff)
    end_excess: Optional[float] = None        # white noise the sky holds beyond its budget
    end_sky_tiles: int = 0
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


def _interior_scores(x, iw: InteriorWindow, budget: Optional[float], extra: Optional[float]):
    """(DAN_interior, END_interior judged, END_interior pure) of normalised crop x on the interior
    window; the END values are None without a noise budget (judged: also without the sky's excess
    `extra`)."""
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
        return dan, None, None
    # END of the interior: the periodogram of the windowed high-pass (without it the limb-darkening
    # slope times the window would leak into the low bins). The high-pass is linear, so it scales
    # signal and noise alike at every frequency: the noise model is multiplied by its power gain g.
    hw = hp[sl] * w
    ph = np.abs(np.fft.fft2(hw - hw.mean())) ** 2 / (w ** 2).sum()
    g = (1.0 - np.exp(-2.0 * (math.pi * INTERIOR_HP_SIGMA * f) ** 2)) ** 2
    judged = None if extra is None else _end_from_periodogram(ph, budget + extra, g)[0]
    pure, _ = _end_from_periodogram(ph, budget, g)
    return dan, judged, pure


def _end_from_periodogram(p, noise_power, gain=None):
    """(END, per-bin SNR) of periodogram `p` (variance-per-pixel units, on its own fftfreq grid)
    against white noise of power `noise_power` (times the per-frequency power gain `gain` of a
    linear pre-filter, when given): per END_EDGES bin, SNR = max(mean P - N, 0) / N, and
    END = sqrt( sum_f O(f) / sum_f O(f) / (1 + SNR(f)) ) over the band's frequency samples, with
    the object prior O(f) = f^-END_PRIOR (see END in the module doc)."""
    f = _radial_frequency(p.shape)
    num = den = 0.0
    snrs = []
    for a, b in zip(END_EDGES[:-1], END_EDGES[1:]):
        m = (f >= a) & (f < b)
        if not m.any():
            continue
        noise = noise_power * (float(gain[m].mean()) if gain is not None else 1.0)
        snr = max(float(p[m].mean()) - noise, 0.0) / noise
        snrs.append(snr)
        o = float((f[m] ** -END_PRIOR).sum())       # the bin's object power under the prior
        num += o
        den += o / (1.0 + snr)
    return math.sqrt(num / den), tuple(snrs)


def _radial_frequency(shape):
    fy = np.fft.fftfreq(shape[0])[:, None]
    fx = np.fft.fftfreq(shape[1])[None, :]
    return np.hypot(fy, fx)


def measure(img, box: Optional[Box] = None, *, mosaic=False,
            interior: Optional[InteriorWindow] = None, noise_power: Optional[float] = None,
            noise_power_interior: Optional[float] = None, sky_noise_power: Optional[float] = None,
            sky_floor=None) -> Result:
    """DAN and END of `img` (HxW or HxWxC array, any linear units) on `box` (default: its own
    target_box), and DAN_interior / END_interior when an anchor's interior window is given (judged
    only when the window is >= INTERIOR_MIN_DIAMETER across). `noise_power` is the image's photon
    budget on the box, sigma_frame^2 / k_eff in the metric's units (a fraction of the box's mean
    target signal squared; noise.box_noise / k), and `noise_power_interior` the same over the
    interior window (default: noise_power). Without it END is None and end_note says so. Raises
    ValueError when there is no target signal to measure."""
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
    x = (crop - bg) / signal
    win = np.outer(np.hanning(crop.shape[0]), np.hanning(crop.shape[1]))
    x = x - (win * x).sum() / win.sum()
    power = np.abs(np.fft.fft2(x * win)) ** 2 / (win ** 2).sum()
    f = _radial_frequency(crop.shape)
    band = (f >= BAND[0]) & (f < BAND[1])
    noise = (f >= NOISE_BAND[0]) & (f <= NOISE_BAND[1])
    floor = float(np.median(power[noise]) / math.log(2))
    band_power = float(power[band].mean())
    excess = (float(power[band].sum()) - floor * int(band.sum())) / power.size
    end = end_pure = end_snr = end_noise = end_note = extra = None
    sky_tiles = 0
    budget = noise_power
    if mosaic:
        end_note, budget = "END not computed in mosaic mode (a fallback, never a pass/fail)", None
    elif budget is None:
        end_note = "END not computed: no noise budget (per-frame noise and frame count) for this image"
    elif not budget > 0:
        raise ValueError(f"noise budget must be positive, got {budget}")
    if budget is not None:
        sky_budget = budget if sky_noise_power is None else sky_noise_power
        try:
            if sky_floor is None:
                sky_floor, sky_tiles = sky_noise_floor(lum)
            elif isinstance(sky_floor, str):          # the caller measured the sky and found none
                raise SkyUnmeasurable(sky_floor)
            else:
                sky_floor, sky_tiles = sky_floor
            extra = max(sky_floor / signal ** 2 - sky_budget, 0.0)
        except SkyUnmeasurable as e:
            end_note = f"judged END not computed: the excess-noise check needs sky ({e}); pure budget only"
        end_pure, pure_snr = _end_from_periodogram(power, budget)
        if extra is not None:
            end_noise = budget + extra
            end, end_snr = _end_from_periodogram(power, end_noise)
    interior_value = end_int = end_int_pure = None
    judged, diameter = False, None
    if interior is not None:
        if interior.window.shape != crop.shape:
            raise ValueError(f"interior window {interior.window.shape} does not match the crop {crop.shape}")
        diameter, judged = interior.diameter, interior.judged
        if judged:
            b_int = None if budget is None else (noise_power_interior if noise_power_interior is not None else budget)
            interior_value, end_int, end_int_pure = _interior_scores((crop - bg) / signal, interior, b_int, extra)
    return Result(value=math.sqrt(max(excess, 0.0)), band_power=band_power, noise_floor=floor,
                  band_snr=(band_power - floor) / floor if floor > 0 else math.inf,
                  background=bg, signal=signal, box=box, mosaic=mosaic,
                  interior=interior_value, interior_judged=judged, interior_diameter=diameter,
                  end=end, end_pure=end_pure, end_interior=end_int, end_interior_pure=end_int_pure,
                  end_snr=end_snr, end_noise=end_noise, end_noise_budget=budget, end_excess=extra,
                  end_sky_tiles=sky_tiles, end_note=end_note)


class SkyUnmeasurable(ValueError):
    """Too little signal-free, unclipped sky to read an image's own noise there."""


def sky_mask(lum) -> np.ndarray:
    """Signal-free sky of a whole image: further than SKY_GUARD px from any structure, where
    structure = the 2 px low-pass above the sky level + SKY_STRUCTURE x (max - sky level). The sky
    level is the 5th percentile of the low-pass over pixels above the image's minimum, so a zero
    border or a clipped sky does not set it (ASIStudio's 2023-09-29 Saturn: zero borders under a
    1043 DN sky pedestal)."""
    lum = luminance(lum)
    lp = ndimage.gaussian_filter(ndimage.median_filter(lum, size=3), 2.0)
    above = lp[lum > lum.min()]
    if above.size == 0:
        return np.zeros(lum.shape, dtype=bool)
    level, top = float(np.percentile(above, 5.0)), float(lp.max())
    return ~ndimage.binary_dilation(lp > level + SKY_STRUCTURE * (top - level), iterations=SKY_GUARD)


def _detrend(t):
    n0, n1 = t.shape
    yy, xx = np.meshgrid(np.linspace(-0.5, 0.5, n0), np.linspace(-0.5, 0.5, n1), indexing="ij")
    a = np.stack([np.ones(t.size), yy.ravel(), xx.ravel(), (yy * yy).ravel(), (xx * xx).ravel(),
                  (xx * yy).ravel()], 1)
    coef, *_ = np.linalg.lstsq(a, t.ravel(), rcond=None)
    return t - (a @ coef).reshape(t.shape)


def sky_noise_floor(img, mask=None):
    """(white noise power of the image's own sky at 0.40-0.50 cycles/px, in the image's units;
    tiles used). Tiles of SKY_TILE px, half overlapping, wholly in sky_mask and SKY_EDGE px clear
    of the frame edge; a tile holding the image's minimum value (a clipped sky, a black border) or
    no variation is refused. Each tile: quadratic detrend (glow), 2-D Hann window, periodogram in
    variance-per-pixel units, mean over the 0.40-0.50 annulus; the median over tiles (a star or a
    moon cannot pull it). `mask` replaces sky_mask(img) (another image's sky on the same grid).
    Raises SkyUnmeasurable below SKY_MIN_TILES usable tiles."""
    lum = luminance(img)
    sky = sky_mask(lum) if mask is None else np.asarray(mask, dtype=bool)
    lo, t = lum.min(), SKY_TILE
    win = np.outer(np.hanning(t), np.hanning(t))
    f = _radial_frequency((t, t))
    nb = (f >= NOISE_BAND[0]) & (f <= NOISE_BAND[1])
    h, w = lum.shape
    vals, cand, clipped = [], 0, 0
    for y in range(SKY_EDGE, h - t - SKY_EDGE + 1, t // 2):
        for x in range(SKY_EDGE, w - t - SKY_EDGE + 1, t // 2):
            if not sky[y:y + t, x:x + t].all():
                continue
            cand += 1
            tile = lum[y:y + t, x:x + t]
            if (tile <= lo).any() or not np.ptp(tile) > 0:
                clipped += 1
                continue
            p = np.abs(np.fft.fft2(_detrend(tile) * win)) ** 2 / (win ** 2).sum()
            vals.append(float(p[nb].mean()))
    if len(vals) < SKY_MIN_TILES:
        raise SkyUnmeasurable(f"{len(vals)} usable sky tiles of {t} px, need {SKY_MIN_TILES} "
                              f"({cand} clear of structure, {clipped} of them clipped or constant)")
    return float(np.median(vals)), len(vals)


def measure_path(path, box: Optional[Box] = None, *, mosaic=False) -> Result:
    """DAN of an image file (FITS / TIFF / PNG / JPEG, via refs.load_image), no resampling."""
    import refs  # local import: refs imports this module
    return measure(refs.load_image(path), box, mosaic=mosaic)
