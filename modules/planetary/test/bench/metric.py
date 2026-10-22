"""metric.py -- the PlanetaryStack benchmark's one sharpness-at-equal-noise measure.

DEFINITION ("detail above noise", DAN)
    For an image (ours, AutoStakkert's, ASIStudio's), all registered to the same grid:
    1. Luminance: the mean of the colour channels (mono as is).
    2. Crop: one target box, chosen once on the anchor image (target_box) and applied to every
       image compared -- the planet's disc (largest component above 15 % of the low-passed
       maximum, so moons are left out; thin components on the frame edge are stacking
       artefacts and are skipped) plus a 15 % margin of sky, or for a frame-filling Moon the
       frame minus a 2 % inset.
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
BOX_INSET = 0.02             # of each frame side, at least 4 px
EDGE_ARTEFACT_SHARE = 0.10   # an edge-touching component thinner than this share is no target
EDGE_NEAR = 0.03             # "touching" = within 3 % of the side (at least 4 px) of the edge
INTERIOR_HP_SIGMA = 3.0      # px, the interior high-pass
INTERIOR_MIN_EROSION = 3.0   # px
INTERIOR_TAPER = 2.0         # px, cosine edge of the interior window
INTERIOR_MIN_DIAMETER = 20.0 # px, equivalent diameter of the eroded disc for the interior to be judged


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


def target_box(img):
    """The target crop on this image's grid (see DEFINITION step 2)."""
    lum = luminance(img)
    h, w = lum.shape
    ty0, ty1, tx0, tx1 = target_extent(lum)
    margin = max(8, int(round(BOX_MARGIN * max(ty1 - ty0, tx1 - tx0))))
    iy, ix = max(4, int(round(BOX_INSET * h))), max(4, int(round(BOX_INSET * w)))
    y0, y1 = max(iy, ty0 - margin), min(h - iy, ty1 + margin)
    x0, x1 = max(ix, tx0 - margin), min(w - ix, tx1 + margin)
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


def _interior_score(x, iw: InteriorWindow) -> float:
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
    n_int = float(np.median(p[(f >= NOISE_BAND[0]) & (f <= NOISE_BAND[1])]) / math.log(2))
    fc = _radial_frequency(x.shape)
    gain = float(np.mean((1.0 - np.exp(-2.0 * (math.pi * INTERIOR_HP_SIGMA * fc) ** 2)) ** 2))
    return math.sqrt(max(ms - n_int * gain, 0.0))


def _radial_frequency(shape):
    fy = np.fft.fftfreq(shape[0])[:, None]
    fx = np.fft.fftfreq(shape[1])[None, :]
    return np.hypot(fy, fx)


def measure(img, box: Optional[Box] = None, *, mosaic=False,
            interior: Optional[InteriorWindow] = None) -> Result:
    """DAN of `img` (HxW or HxWxC array, any linear units) on `box` (default: its own
    target_box), and DAN_interior when an anchor's interior window is given (judged only when
    the window is >= INTERIOR_MIN_DIAMETER across). Raises ValueError when there is no target
    signal to measure."""
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
    interior_value, judged, diameter = None, False, None
    if interior is not None:
        if interior.window.shape != crop.shape:
            raise ValueError(f"interior window {interior.window.shape} does not match the crop {crop.shape}")
        diameter, judged = interior.diameter, interior.judged
        if judged:
            interior_value = _interior_score((crop - bg) / signal, interior)
    return Result(value=math.sqrt(max(excess, 0.0)), band_power=band_power, noise_floor=floor,
                  band_snr=(band_power - floor) / floor if floor > 0 else math.inf,
                  background=bg, signal=signal, box=box, mosaic=mosaic,
                  interior=interior_value, interior_judged=judged, interior_diameter=diameter)


def measure_path(path, box: Optional[Box] = None, *, mosaic=False) -> Result:
    """DAN of an image file (FITS / TIFF / PNG / JPEG, via refs.load_image), no resampling."""
    import refs  # local import: refs imports this module
    return measure(refs.load_image(path), box, mosaic=mosaic)
