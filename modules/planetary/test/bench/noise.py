#!/usr/bin/env python3
"""noise.py -- a capture's per-frame noise, measured on its raw frames (Ruling P28, rule 1).

    python3 -I noise.py <video.avi> WxH [--frames N]      # prints the photon-transfer fit

END (metric.py) judges every stack -- ours, AutoStakkert's, ASIStudio's -- against ONE noise
model: white noise of power sigma_frame^2 / k_eff, where sigma_frame^2 is the noise of a single raw
frame on the target box and k_eff the stack's effective frame count. sigma_frame is a property of
the capture, independent of any stacker, so no tool's own smoothing (interpolation, drizzle,
denoising) can lower its modelled noise: smoothing counts as blur on every side, as in DAN and
Ruling 1.

THE MEASUREMENT (photon_transfer)
    Raw CFA frames, as ffmpeg decodes them (= the reader's layout, bayer.read_frames), consecutive
    pairs. Per CFA site plane (each of the four 2x2 sites: one colour, sampled every 2 px):
      d = frame[t+1] - frame[t]          -- fixed-pattern noise (hot pixels, bias pattern, PRNU)
                                            cancels, as it is the same in both frames;
      r = d convolved with [1 -2 1] x [1 -2 1] on the site plane -- removes the target's change
                                            between the frames (seeing shift x gradient) wherever
                                            the scene is locally smooth: linear trends vanish
                                            exactly. For white per-pixel noise of variance v,
                                            Var(r) = 2 x 36 v.
      m = the pair's mean, low-passed (sigma 1 site px) -- the local level.
    Samples with any of the 3 x 3 site neighbours at 250 or more (saturation) are left out.
    The samples are binned by m: PTC_BINS bins spaced geometrically from the lowest level to the
    level of the PTC_MIN_SAMPLES-th brightest sample (not a percentile: a small disc in a large frame
    is under 0.1 % of the samples, and rounds 0-1 fitted Mars and Saturn on sky and limb alone), and
    adjacent bins too sparse alone are merged rather than dropped. Per bin: only the samples under its
    GRADIENT_KEEP (80th) percentile of local gradient (edges, where the scene moved between the
    frames), then those within CLIP x the robust sigma of r (IQR / 1.349; none dropped when the IQR is
    0, as on an 8-bit sky that rounds to one value), and v = mean(r^2) / (2 x 36) -- an untrimmed mean,
    so a discrete low-level bin is not biased the way a trimmed chi-square constant biased it.
    v(m) = a m + c is fitted symmetrically (iteratively reweighted, Cauchy, on relative residuals; a
    bin far above or far below the line counts alike), a >= 0, c >= 0: a sky that rounds to one
    8-bit value has no variance at all, so c is not floored at 1/12 DN^2. Refused (NoiseUnmeasurable)
    when fewer than PTC_MIN_KEPT (half) of the bins lie on the line, or the slope is not > 0.
    Measured (test_end.py): a textured disc drifting ~0.5 px per frame +7 % on a; a 6 px disc +16 %
    (mostly limb); a lunar texture under 0-1.5 px of jitter within 0.4 %.
    Blind spot: noise constant along a row or a column of one frame (sensor banding) passes the
    separable [1 -2 1] x [1 -2 1] high-pass unseen. The split halves see it (the 2026-01-30 Jupiter,
    README); ours is charged it (Ruling P28b), a reference is not.

ON A BOX (raw_under, box_noise)
    The stack's target box is mapped onto the raw frames by registering the frames' mean (the CFA
    flattened) to the stack -- orientation and integer shift (refs.register); the frames cover at
    least MIN_COVER of the box's window weight or it is NoiseUnmeasurable. sigma_frame^2 there is
    sum w^2 v(m) / s_site^2 / sum w^2 over the box's raw pixels: w the metric's Hann window (or an
    interior window), m the frames' mean level at the pixel, and s_site the mean signal of that
    pixel's site over the box minus its background -- the metric's own normalisation (metric.measure
    divides by the mean target signal above the background), applied per site so the colour
    sites' different gains drop out (as metric.flatten_cfa does). The result is in the metric's
    units: the power of white noise per pixel, as a fraction of the mean target signal squared.
    This is the noise of an UNSMOOTHED frame -- one raw sample per pixel. A debayered frame holds
    less (the demosaic interpolates); that, like every other smoothing, counts as blur. A box whose
    95th-percentile level exceeds EXTRAPOLATE (1.5) x the highest kept bin is refused: no line is
    extrapolated over the target from the sky.

LIMITS
    * Frames: the first `frames` of the capture (default 128), read from the start of the file:
      the noise of a capture does not change over its minutes, but its level is that of the
      target's brightness at the start (haze later in a capture is not followed).
    * The line is a 2-parameter fit; gain changes or non-linear sensor response are not modelled.
    * Fixed-pattern noise is excluded by construction (it does not average down with frames and
      is not part of sigma_frame^2 / k); whatever of it a stack keeps counts as detail for every image
      alike (metric.py, KNOWN LIMIT).
"""
import argparse
import math
import os
import sys
from dataclasses import dataclass
from typing import Optional, Tuple

import numpy as np
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import bayer   # noqa: E402
import metric  # noqa: E402

FRAMES = 128
SATURATED = 250
PTC_BINS = 40               # level bins, geometric from the lowest level to the 99.9th percentile
PTC_MIN_SAMPLES = 2000
CLIP = 5.0                  # per bin: samples beyond 5 robust sigma of r are dropped (a scene change)
GRADIENT_KEEP = 80.0        # per bin: only the samples under its 80th percentile of local gradient (edges move)
PTC_ITERATIONS = 20          # the line: iteratively reweighted (Cauchy) on relative residuals
PTC_INLIER = 3.0             # a bin is kept (an inlier) within 3 robust scales of the line
PTC_MIN_KEPT = 0.5           # fewer than half the bins kept: the line is refused (NoiseUnmeasurable)
EXTRAPOLATE = 1.5            # box_noise refuses a box whose bright levels exceed 1.5 x the highest kept bin
QUANTISATION = 1.0 / 12.0    # DN^2: the rounding variance of an 8-bit sample
MIN_COVER = 0.5           # box_noise: the raw frames must cover half the box window's weight
RAW_SCALE_TOLERANCE = 0.08  # raw_under: the survey's planets read 0.2-4.7 % against blurred raw means
HP = np.outer([1.0, -2.0, 1.0], [1.0, -2.0, 1.0])
HP_GAIN = float((HP ** 2).sum())        # 36
SITES = ((0, 0), (0, 1), (1, 0), (1, 1))


class NoiseUnmeasurable(ValueError):
    pass


@dataclass(frozen=True)
class PhotonTransfer:
    a: float                  # variance per DN of level (shot noise)
    c: float                  # variance at level 0 (read noise, quantisation), DN^2
    bins: Tuple[Tuple[float, float, int], ...]   # (level, variance, samples) per bin
    used_bins: int
    pairs: int
    kept_levels: Tuple[float, float] = (0.0, math.inf)   # lowest and highest kept bin level, DN

    def variance(self, level):
        return self.a * np.maximum(level, 0.0) + self.c

    def __str__(self):
        return (f"v(m) = {self.a:.4g} m + {self.c:.4g} DN^2 ({self.used_bins} of {len(self.bins)} bins, "
                f"{self.pairs} frame pairs)")


def _fit(levels, var):
    """v = a m + c through the bins: least squares weighted by 1 / v^2 (relative residuals), then
    iteratively reweighted with Cauchy weights on the relative residual, symmetric -- a bin far above
    the line (a limb, where the scene moved) and one far below count alike. Returns (a, c, kept), kept
    = the bins within PTC_INLIER robust scales (1.4826 MAD of the relative residuals) of the line."""
    A = np.stack([levels, np.ones_like(levels)], 1)
    w = 1.0 / np.maximum(var, 1e-12) ** 2
    a = c = 0.0
    for _ in range(PTC_ITERATIONS):
        sw = np.sqrt(w)
        (a, c), *_ = np.linalg.lstsq(A * sw[:, None], var * sw, rcond=None)
        c = max(c, 0.0)
        line = np.maximum(a * levels + c, 1e-12)
        rel = (var - line) / line
        scale = max(1.4826 * float(np.median(np.abs(rel))), 0.02)
        w = 1.0 / (1.0 + (rel / (2.385 * scale)) ** 2) / line ** 2
    kept = np.abs(rel) <= PTC_INLIER * scale
    return float(a), float(c), kept


def photon_transfer(frames) -> PhotonTransfer:
    """The photon-transfer line v(m) = a m + c of raw CFA frames (n x h x w, 8-bit values, n >= 2).
    NoiseUnmeasurable when too few bins have data, when fewer than PTC_MIN_KEPT of them lie on the
    line, or when the slope collapses to <= 0 -- never a line extrapolated from the sky alone."""
    f = np.asarray(frames, dtype=np.float64)
    if f.ndim != 3 or f.shape[0] < 2:
        raise NoiseUnmeasurable("photon transfer needs at least two frames")
    f = f[:, :f.shape[1] // 2 * 2, :f.shape[2] // 2 * 2]
    lv, rr, gg = [], [], []
    for t in range(f.shape[0] - 1):
        for dy, dx in SITES:
            a, b = f[t, dy::2, dx::2], f[t + 1, dy::2, dx::2]
            r = ndimage.convolve(b - a, HP, mode="constant")[1:-1, 1:-1]
            mm = ndimage.gaussian_filter((a + b) / 2, 1.0)
            gy, gx = np.gradient(mm)
            m, g = mm[1:-1, 1:-1], np.hypot(gy, gx)[1:-1, 1:-1]
            sat = ndimage.maximum_filter(np.maximum(a, b), size=3)[1:-1, 1:-1] >= SATURATED
            lv.append(m[~sat])
            rr.append(r[~sat])
            gg.append(g[~sat])
    lv, rr, gg = np.concatenate(lv), np.concatenate(rr), np.concatenate(gg)
    # The top of the binning is the level of the PTC_MIN_SAMPLES-th brightest sample, not a percentile:
    # a small disc in a large frame (Saturn, Mars) is far under 0.1 % of the samples, and a percentile
    # cut would leave its levels out of the fit (rounds 0-1 did: their lines came from sky and limb).
    top = float(np.partition(lv, lv.size - PTC_MIN_SAMPLES)[lv.size - PTC_MIN_SAMPLES]) if lv.size > PTC_MIN_SAMPLES else float(lv.max())
    edges = np.geomspace(max(float(lv.min()), 0.5), max(top, 1.0) + 1e-9, PTC_BINS + 1)
    idx = np.clip(np.digitize(lv, edges) - 1, 0, PTC_BINS - 1)
    order = np.argsort(idx, kind="stable")
    counts = np.bincount(idx, minlength=PTC_BINS)
    starts = np.concatenate([[0], np.cumsum(counts)])
    bins, acc = [], []
    for i in range(PTC_BINS):          # adjacent bins too sparse alone are merged, not dropped
        acc.append(order[starts[i]:starts[i + 1]])
        if sum(len(x) for x in acc) < PTC_MIN_SAMPLES and i < PTC_BINS - 1:
            continue
        sel = np.concatenate(acc)
        acc = []
        if sel.size < PTC_MIN_SAMPLES:
            continue
        r, g = rr[sel], gg[sel]
        r = r[g <= np.percentile(g, GRADIENT_KEEP)]     # drop the bin's edge samples: the scene moved there
        q1, q3 = np.percentile(r, [25, 75])
        sig = (q3 - q1) / 1.349
        if sig > 0:               # a discrete sky that rounds to one value has IQR 0: keep every sample
            r = r[np.abs(r - np.median(r)) <= CLIP * sig]
        bins.append((float(lv[sel].mean()), float((r * r).mean() / (2 * HP_GAIN)), int(sel.size)))
    if len(bins) < 3:
        raise NoiseUnmeasurable(f"only {len(bins)} level bins with {PTC_MIN_SAMPLES} or more unsaturated samples")
    L = np.array([b[0] for b in bins])
    V = np.array([b[1] for b in bins])
    a, c, keep = _fit(L, V)
    if keep.sum() < PTC_MIN_KEPT * L.size:
        raise NoiseUnmeasurable(f"photon transfer: only {int(keep.sum())} of {L.size} level bins lie on the line "
                                f"(need {PTC_MIN_KEPT:.0%}); fit v = {a:.4g} m + {c:.4g}")
    if not a > 0:
        raise NoiseUnmeasurable(f"photon transfer: the slope collapsed to {a:.4g} ({int(keep.sum())} of {L.size} bins)")
    return PhotonTransfer(a, c, tuple(bins), int(keep.sum()), f.shape[0] - 1,
                          (float(L[keep].min()), float(L[keep].max())))


def measure_video(path, size, frames: int = FRAMES):
    """(PhotonTransfer, mean raw frame) of the first `frames` frames of an 8-bit AVI."""
    raw = bayer.read_frames(path, size, frames)
    return photon_transfer(raw), raw.astype(np.float64).mean(axis=0)


@dataclass(frozen=True)
class RawBox:
    """The frames' mean under a stack's box. `crop`: the raw pixels under the part of the box the
    frames cover, at box[sub]; `full`: the whole oriented mean; `origin`: crop's top-left in full."""
    crop: np.ndarray
    sub: Tuple[slice, slice]
    box_shape: Tuple[int, int]
    full: np.ndarray
    orientation: str
    origin: Tuple[int, int]


def raw_under(raw_mean, stack_lum, box: metric.Box) -> RawBox:
    """The raw frames' mean under `box` of the stack's grid: the CFA flattened to one luminance
    (metric.flatten_cfa) is registered to the stack (refs.register: orientation, then the integer
    shift; scale tolerance RAW_SCALE_TOLERANCE, since a drift-smeared mean of raw frames against
    a sharp stack reads the scale of a small disc a few % off, README "References"). A frame-filling
    box the drift has moved partly off the frames keeps the part they cover (box_noise weighs it
    with the box's own window and refuses under MIN_COVER of it). NoiseUnmeasurable on a failed
    registration."""
    import refs   # local: refs is the heavier module and does not need this one
    raw_mean = np.asarray(raw_mean, dtype=np.float64)
    raw_mean = raw_mean[:raw_mean.shape[0] // 2 * 2, :raw_mean.shape[1] // 2 * 2]
    try:
        reg = refs.register(metric.flatten_cfa(raw_mean), stack_lum, scale_tolerance=RAW_SCALE_TOLERANCE)
    except (refs.RegistrationFailed, refs.ScaleMismatch) as e:
        raise NoiseUnmeasurable(f"the raw frames do not register to the stack: {e}") from e
    full = refs.apply_orientation(raw_mean, reg.orientation)
    iy, ix = reg.int_shift
    ob = refs.overlap_box(reg, full.shape, (box.y1, box.x1))
    y0, y1 = max(box.y0, ob.y0), min(box.y1, ob.y1)
    x0, x1 = max(box.x0, ob.x0), min(box.x1, ob.x1)
    if y1 - y0 < 16 or x1 - x0 < 16:
        raise NoiseUnmeasurable(f"the raw frames cover too little of the box {tuple(box)}")
    crop = full[y0 - iy:y1 - iy, x0 - ix:x1 - ix]
    sub = (slice(y0 - box.y0, y1 - box.y0), slice(x0 - box.x0, x1 - box.x0))
    return RawBox(np.asarray(crop, dtype=np.float64), sub, (box.y1 - box.y0, box.x1 - box.x0), full,
                  reg.orientation, (y0 - iy, x0 - ix))


def _site_terms(crop, w2, variance_of):
    num = 0.0
    for sy, sx in SITES:
        site = crop[sy::2, sx::2]
        bg = float(np.percentile(ndimage.gaussian_filter(site, metric.LOWPASS_SIGMA / 2), metric.BG_PERCENTILE))
        s = float(site.mean()) - bg
        if not s > 0:
            raise NoiseUnmeasurable("a CFA site has no signal above its background on the box")
        num += float((w2[sy::2, sx::2] * variance_of(site, sy, sx)).sum()) / s ** 2
    return num / float(w2.sum())


def _as_rawbox(raw) -> RawBox:
    if isinstance(raw, RawBox):
        return raw
    crop = np.asarray(raw, dtype=np.float64)       # a whole box of raw pixels
    return RawBox(crop, (slice(0, crop.shape[0]), slice(0, crop.shape[1])), crop.shape, crop, "identity", (0, 0))


def _weights(raw: RawBox, window):
    wts = np.outer(np.hanning(raw.box_shape[0]), np.hanning(raw.box_shape[1])) if window is None \
        else np.asarray(window, dtype=np.float64)
    if wts.shape != tuple(raw.box_shape):
        raise ValueError(f"window {wts.shape} does not match the box {raw.box_shape}")
    w2 = wts ** 2
    part = w2[raw.sub]
    if part.sum() < MIN_COVER * w2.sum():
        raise NoiseUnmeasurable(f"the raw frames cover {part.sum() / w2.sum():.0%} of the box's window "
                                f"(need {MIN_COVER:.0%})")
    return part


def box_noise(ptc: PhotonTransfer, raw, window: Optional[np.ndarray] = None) -> float:
    """sigma_frame^2 on a box (see ON A BOX), from the frames' mean under it (a RawBox, or a
    box-sized array of raw pixels): white noise power per pixel of one unsmoothed raw frame, as a
    fraction of the mean target signal squared. `window` (box-sized weights, e.g. an interior
    window) replaces the Hann window."""
    raw = _as_rawbox(raw)
    top = float(np.percentile(raw.crop, 95))
    if top > EXTRAPOLATE * ptc.kept_levels[1]:
        raise NoiseUnmeasurable(f"the box reaches {top:.1f} DN, beyond {EXTRAPOLATE} x the highest level the "
                                f"photon-transfer line was fitted on ({ptc.kept_levels[1]:.1f} DN)")
    return _site_terms(raw.crop, _weights(raw, window), lambda site, sy, sx: ptc.variance(site))


def _main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("video")
    ap.add_argument("size", help="WxH")
    ap.add_argument("--frames", type=int, default=FRAMES)
    a = ap.parse_args(argv)
    w, h = (int(v) for v in a.size.lower().split("x"))
    ptc, _ = measure_video(a.video, (w, h), a.frames)
    print(ptc)
    for lvl, v, n in ptc.bins:
        print(f"  level {lvl:7.2f}  variance {v:8.4f}  line {ptc.variance(lvl):8.4f}  ({n} samples)")
    return 0


if __name__ == "__main__":
    sys.exit(_main())
