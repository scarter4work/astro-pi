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

LIMITS (stated, not hidden)
    * The floor assumes the stack's noise is roughly white. Interpolated or resampled stacks
      have less noise power near Nyquist than mid-band, so part of their mid-band noise can
      count as detail; band_snr is reported so a low-SNR comparison is visible.
    * Linear images only: a gamma-stretched or sharpened file (ASIStudio's .jpg, AutoStakkert's
      _conv) is not a stack and is refused by refs.compare.
    * Undeclared Bayer (raw CFA stacked as mono, e.g. the 2023 Moons): measure with mosaic=True
      for EVERY image of that capture. The crop's four CFA sites are then scaled to a common
      mean (flatten_cfa) before step 3, which removes the per-site gain checkerboard at native
      resolution (same bands). Measured on raw NAS frames, the checkerboard moves DAN by
      0.2-1.7 % (its power sits at Nyquist, outside the band) but inflates the noise floor;
      on a debayered reference the flattening is a no-op to < 0.2 %.
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
    return best[1]


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


def _radial_frequency(shape):
    fy = np.fft.fftfreq(shape[0])[:, None]
    fx = np.fft.fftfreq(shape[1])[None, :]
    return np.hypot(fy, fx)


def measure(img, box: Optional[Box] = None, *, mosaic=False) -> Result:
    """DAN of `img` (HxW or HxWxC array, any linear units) on `box` (default: its own
    target_box). Raises ValueError when there is no target signal to measure."""
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
    return Result(value=math.sqrt(max(excess, 0.0)), band_power=band_power, noise_floor=floor,
                  band_snr=(band_power - floor) / floor if floor > 0 else math.inf,
                  background=bg, signal=signal, box=box, mosaic=mosaic)


def measure_path(path, box: Optional[Box] = None, *, mosaic=False) -> Result:
    """DAN of an image file (FITS / TIFF / PNG / JPEG, via refs.load_image), no resampling."""
    import refs  # local import: refs imports this module
    return measure(refs.load_image(path), box, mosaic=mosaic)
