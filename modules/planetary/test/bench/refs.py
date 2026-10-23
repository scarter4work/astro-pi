#!/usr/bin/env python3
"""refs.py -- load, resample and register benchmark references (AutoStakkert, ASIStudio) and
our own stack, then measure them all with metric.py on one box.

    python3 -I refs.py compare --anchor ours.fits --ref AS.png --ref PATH::3 [--mosaic] [--json]
    python3 -I refs.py measure image.fit [--mosaic]

LOADING (load_image): FITS (.fit/.fits/.fts, astropy; CxHxW planes become HxWxC), TIFF
(tifffile), PNG (OpenCV, because AutoStakkert writes 16-bit RGB PNGs and Pillow reads those as
8-bit -- it keeps only the high byte, measured on the 2_12_2023 files), JPEG (Pillow). Values
are returned as float64 in the file's own units, HxW or HxWxC, rows in file order.

DRIZZLE (Ruling 1): AutoStakkert names drizzled output "..._Drizzle15" (x1.5) or "_Drizzle30"
(x3). load_reference area-resamples such a file to native scale: each native pixel is the
exact area-weighted mean of the drizzled pixels under its footprint (area_resample). The factor
comes from the name unless the caller passes the measured one; register() then MEASURES the
scale against the anchor and raises ScaleMismatch with the measured factor when it is not 1
(e.g. the 2_1_2024 AutoStakkert .fit named Drizzle15 is about x3 -- see README.md).

REGISTRATION (register): the anchor's target plus twice its size around it (target_patch) is
correlated with the whole moving image, using band-pass (difference of Gaussians, sigma
1.5 - 6 px) feature images, normalised -- so gain, offset and colour balance do not matter, and
frame-edge artefacts cannot outweigh a small planet. The orientation (identity / flipud /
fliplr / rot180: FITS writers disagree on row order; measured: ASIStudio's .fit is flipped
vertically against AutoStakkert's) and scale are chosen at a low resolution by normalised
cross-correlation (coarse scales x1/3..x3, then a parabola through a centred cubic rescale in
16 steps), the integer shift by full-resolution cross-correlation, the sub-pixel shift by
Hann-windowed upsampled-DFT refinement (Guizar-Sicairos et al. 2008, factor 20). The scale must
be 1 within max(1 %, 1 px / target extent) (scale_tolerance_for). Convention:
anchor[y, x] ~ oriented_moving[y - dy, x - dx].

MEASURING (compare): one target box chosen on the anchor (metric.target_box), shrunk to the
part every reference covers, applied to all; references are cropped at their rounded integer
offset -- never interpolated, since the metric is translation invariant (metric.py).
"""
import argparse
import json
import math
import os
import re
import sys
from dataclasses import dataclass, replace
from typing import List, Optional, Sequence, Tuple, Union

import numpy as np
from scipy import ndimage, sparse

import metric
from metric import Box

ORIENTATIONS = ("identity", "flipud", "fliplr", "rot180")
COARSE_SCALES = (1 / 3, 1 / 2, 1 / 1.5, 1.0, 1.5, 2.0, 3.0)
FINE_HALF_RANGE = 0.04       # fine scale search: at least +-4 % around the coarse winner...
FINE_STEPS = 8               # ...in 2 * 8 steps
SCALE_TOLERANCE = 0.01       # |scale - 1| above max(this, 1 px / target extent) is a ScaleMismatch
TARGET_PAD_MIN = 48          # px of anchor around the target that registration correlates
MIN_OVERLAP = 0.5            # a candidate shift must overlap >= half of the smaller image
LOW_RES_MAX_SIDE = 400
MIN_NCC = 0.3                # below this, registration has failed
UPSAMPLE = 20
OVERLAP_INSET = 4            # px kept clear of every reference's edge

DRIZZLE_TAGS = {"15": 1.5, "30": 3.0}
DRIZZLE_FACTORS = (1.0, 1.5, 3.0)
SIZE_TOLERANCE = 0.10        # a stacker's crop may shrink its output by up to 10 %
NOISE_REGIME_FACTOR = 30.0   # ours' floor/band outside [min/30, max*30] of the references' -> review;
                             # legitimate tools differ by up to 17.7x (8_23_2023 ASIStudio vs AutoStakkert)
MOSAIC_WARNING = ("mosaic mode: a raw-CFA mono image compared with debayered references; bias measured "
                  "x0.93-1.09 on the 2_12_2023 Moon by CFA phase (review round 1) -- not a pass/fail")
LIMB_ONLY = "limb-only: human review decides"
PASS_BAR = 0.97              # Ruling 2


class ScaleMismatch(ValueError):
    def __init__(self, measured, orientation, ncc):
        self.measured, self.orientation, self.ncc = measured, orientation, ncc
        super().__init__(f"reference is at x{measured:.4f} the anchor's scale (orientation "
                         f"{orientation}, ncc {ncc:.3f}); pass the measured factor to load_reference")


class RegistrationFailed(ValueError):
    pass


# --- names -----------------------------------------------------------------------------------

@dataclass(frozen=True)
class RefInfo:
    tool: str                 # "AS", "ASI" or "ours"
    percent: Optional[int]    # frames stacked, % (AS_P20 -> 20, _Planet_74 -> 74)
    drizzle: float            # from the name (1.0 = none)
    processed: bool           # sharpened / stretched output, not a stack
    label: str


def drizzle_factor(name: str) -> float:
    m = re.search(r"_Drizzle(\d+)", os.path.basename(name))
    if not m:
        return 1.0
    if m.group(1) not in DRIZZLE_TAGS:
        raise ValueError(f"unknown AutoStakkert drizzle tag Drizzle{m.group(1)} in {name}")
    return DRIZZLE_TAGS[m.group(1)]


def _factor_text(f: float) -> str:
    return f"x{f:g}"


def parse_reference_name(path: str, factor: Optional[float] = None, source: str = "measured") -> RefInfo:
    """Tool, percentage, name drizzle factor and label. `factor` is the factor actually used
    when it was not taken from the name; `source` says where it came from ("measured" by
    register, or "size" from the reference's size against the capture's frame)."""
    base = os.path.basename(path)
    stem, ext = os.path.splitext(base)
    parts = path.replace("\\", "/").split("/")
    as_dir = next((p for p in parts if re.fullmatch(r"AS_P\d+", p)), None)
    asi = re.search(r"_(Planet|Surface)_(\d+)", base)
    jpeg = ext.lower() in (".jpg", ".jpeg")
    if as_dir or "_lapl" in base:
        tool, percent = "AS", int(as_dir[4:]) if as_dir else None
        processed = stem.endswith("_conv") or jpeg
        label = f"AS_P{percent}" if percent is not None else "AS"
    elif asi or "ASIVideoStack_Output" in parts:
        tool, percent = "ASI", int(asi.group(2)) if asi else None
        processed = "_Sharp" in base or jpeg
        label = f"ASI_{percent}" if percent is not None else "ASI"
    else:
        tool, percent, processed, label = "ours", None, jpeg, "ours"
    drz = drizzle_factor(base) if tool == "AS" else 1.0
    used = drz if factor is None else factor
    if drz != 1.0 or used != 1.0:
        if used == drz:
            label += f" (drizzled {_factor_text(drz)}, resampled)"
        else:
            label += f" (named {_factor_text(drz)}, {source} {_factor_text(used)}, resampled)"
    return RefInfo(tool, percent, drz, processed, label)


# --- loading ---------------------------------------------------------------------------------

def _hwc(a):
    a = np.asarray(a)
    if a.ndim == 3 and a.shape[0] in (1, 3, 4) and a.shape[2] not in (1, 3, 4):
        a = np.moveaxis(a, 0, -1)
    if a.ndim == 3 and a.shape[2] == 1:
        a = a[..., 0]
    if a.ndim not in (2, 3):
        raise ValueError(f"unsupported image shape {a.shape}")
    return a.astype(np.float64)


def load_image(path: str) -> np.ndarray:
    ext = os.path.splitext(path)[1].lower()
    if ext in (".fit", ".fits", ".fts"):
        from astropy.io import fits
        with fits.open(path, memmap=False) as hdul:
            data = next((h.data for h in hdul if h.data is not None), None)
            if data is None:
                raise ValueError(f"no image data in {path}")
            return _hwc(data)
    if ext in (".tif", ".tiff"):
        import tifffile
        return _hwc(tifffile.imread(path))
    if ext == ".png":
        import cv2
        a = cv2.imread(path, cv2.IMREAD_UNCHANGED)
        if a is None:
            raise ValueError(f"cannot read {path}")
        if a.ndim == 3:
            a = a[..., :3][..., ::-1]           # BGR(A) -> RGB
        return _hwc(a)
    if ext in (".jpg", ".jpeg"):
        from PIL import Image
        with Image.open(path) as im:
            return _hwc(np.asarray(im.convert("RGB") if im.mode not in ("L", "RGB") else im))
    raise ValueError(f"unsupported image type: {path}")


# --- area resampling -------------------------------------------------------------------------

def _area_matrix(n_in: int, factor: float):
    n_out = int(math.floor(n_in / factor + 1e-9))
    rows, cols, vals = [], [], []
    for j in range(n_out):
        a, b = j * factor, (j + 1) * factor
        for i in range(int(math.floor(a)), min(n_in, int(math.ceil(b - 1e-12)))):
            w = min(b, i + 1) - max(a, i)
            if w > 1e-12:
                rows.append(j), cols.append(i), vals.append(w / factor)
    return sparse.csr_matrix((vals, (rows, cols)), shape=(n_out, n_in))


def area_resample(img: np.ndarray, factor: float) -> np.ndarray:
    """Downsample by `factor` (> 1): each output pixel is the area-weighted mean of the input
    pixels under its footprint [j*factor, (j+1)*factor). A partial last pixel is dropped."""
    if not factor > 1.0:
        raise ValueError(f"area_resample factor must be > 1, got {factor}")
    a = np.asarray(img, dtype=np.float64)
    ry, rx = _area_matrix(a.shape[0], factor), _area_matrix(a.shape[1], factor)
    planes = [a] if a.ndim == 2 else [a[..., c] for c in range(a.shape[2])]
    out = [np.asarray((rx @ (ry @ p).T).T) for p in planes]
    return out[0] if a.ndim == 2 else np.stack(out, axis=-1)


@dataclass
class Reference:
    path: str
    info: RefInfo
    factor: float
    image: np.ndarray          # native scale, HxW or HxWxC, file units


def factor_from_size(ref_size, frame_size) -> float:
    """The drizzle factor a reference's size implies: the nearest of DRIZZLE_FACTORS to its
    width and height over the capture's frame (AutoStakkert crops its output a little, e.g.
    5688x3168 for 1920x1080 -> 2.96 / 2.93 -> x3). Both ratios must agree with the chosen
    factor within SIZE_TOLERANCE, or the size implies no drizzle factor at all (ValueError)."""
    rw, rh = ref_size[0] / frame_size[0], ref_size[1] / frame_size[1]
    f = min(DRIZZLE_FACTORS, key=lambda k: abs(math.log(rw / k)) + abs(math.log(rh / k)))
    if abs(rw / f - 1) > SIZE_TOLERANCE or abs(rh / f - 1) > SIZE_TOLERANCE:
        raise ValueError(f"reference {ref_size[0]}x{ref_size[1]} for a {frame_size[0]}x{frame_size[1]} "
                         f"capture is x{rw:.3f} / x{rh:.3f}: no drizzle factor of {DRIZZLE_FACTORS} fits")
    return f


def load_reference(path: str, factor: Optional[float] = None, frame_size=None) -> Reference:
    """Load a reference and bring it to native scale. The factor is, in order: `factor` if given
    (a measured one); the size-derived factor when the capture's `frame_size` (w, h) is given
    (factor_from_size; a disagreement with the name is recorded in the label); else the
    drizzle factor in the name."""
    img = load_image(path)
    if factor is not None:
        used, info = float(factor), parse_reference_name(path, factor, "measured")
    elif frame_size is not None:
        used = factor_from_size((img.shape[1], img.shape[0]), frame_size)
        info = parse_reference_name(path, used, "size")
    else:
        info = parse_reference_name(path)
        used = info.drizzle
    if not used >= 1.0:
        raise ValueError(f"resampling factor must be >= 1, got {used}")
    return Reference(path, info, used, area_resample(img, used) if used > 1.0 else img)


# --- registration ----------------------------------------------------------------------------

def apply_orientation(img, orientation: str):
    if orientation == "identity":
        return img
    if orientation == "flipud":
        return img[::-1]
    if orientation == "fliplr":
        return img[:, ::-1]
    if orientation == "rot180":
        return img[::-1, ::-1]
    raise ValueError(orientation)


unapply_orientation = apply_orientation    # every candidate is its own inverse


@dataclass(frozen=True)
class Registration:
    orientation: str
    shift: Tuple[float, float]   # (dy, dx): anchor[y, x] ~ oriented_moving[y - dy, x - dx]
    scale: float                 # measured moving/anchor scale (1 within scale_tolerance)
    ncc: float                   # NCC of the band-pass features once registered
    scale_tolerance: float = SCALE_TOLERANCE   # what the scale was checked against

    @property
    def int_shift(self) -> Tuple[int, int]:
        return int(round(self.shift[0])), int(round(self.shift[1]))

    def __str__(self):
        return (f"{self.orientation}, shift ({self.shift[0]:+.2f}, {self.shift[1]:+.2f}) px, "
                f"scale {self.scale:.4f} (tol {self.scale_tolerance:.3f}), ncc {self.ncc:.3f}")


def _features(lum, s1=1.5, s2=6.0):
    f = ndimage.gaussian_filter(lum, s1) - ndimage.gaussian_filter(lum, s2)
    sd = f.std()
    if not sd > 0:
        raise RegistrationFailed("image has no structure to register")
    return f / sd


def _xcorr_peak(a, m, min_overlap=MIN_OVERLAP):
    """Integer shift t (a[x] ~ m[x - t]) maximising the normalised cross-correlation over the
    overlap, among shifts whose overlap is at least `min_overlap` of the smaller image, and that
    NCC. Every shift is normalised by its own overlap (masked NCC, Padfield 2012, via FFT): the
    argmax of a raw correlation would be pulled toward high-energy regions such as frame edges."""
    return _ncc_peaks([a], m, min_overlap)[0]


def _ncc_peaks(anchors, m, min_overlap=MIN_OVERLAP):
    """_xcorr_peak for several same-shape anchors against one m, sharing m's transforms."""
    ha, wa = anchors[0].shape
    hm, wm = m.shape
    sh = (ha + hm - 1, wa + wm - 1)
    fwd = lambda p: np.fft.rfft2(p, sh)                                  # noqa: E731
    xc = lambda P, Q: np.fft.irfft2(P * np.conj(Q), sh)                  # noqa: E731  sum_x p[x] q[x-t]
    F1a, F1m, Fm, Fm2 = fwd(np.ones((ha, wa))), fwd(np.ones((hm, wm))), fwd(m), fwd(m * m)
    n = np.maximum(np.round(xc(F1a, F1m)), 1)
    sm = xc(F1a, Fm)
    vm = np.clip(xc(F1a, Fm2) - sm * sm / n, 0, None)
    enough = n >= min_overlap * min(ha * wa, hm * wm)
    out = []
    for a in anchors:
        Fa = fwd(a)
        sa = xc(Fa, F1m)
        va = np.clip(xc(fwd(a * a), F1m) - sa * sa / n, 0, None)
        num = xc(Fa, Fm) - sa * sm / n
        den = np.sqrt(va * vm)
        valid = enough & (den > 1e-9 * max(float(den.max()), 1e-300))
        if not valid.any():
            out.append(((0, 0), -1.0))
            continue
        ncc = np.where(valid, num / np.where(valid, den, 1.0), -np.inf)
        ty, tx = np.unravel_index(int(np.argmax(ncc)), sh)
        best = float(ncc[ty, tx])
        ty = ty - sh[0] if ty >= ha else ty
        tx = tx - sh[1] if tx >= wa else tx
        out.append(((int(ty), int(tx)), best))
    return out


def _overlap(a, m, ty, tx):
    y0, y1 = max(0, ty), min(a.shape[0], ty + m.shape[0])
    x0, x1 = max(0, tx), min(a.shape[1], tx + m.shape[1])
    if y1 <= y0 or x1 <= x0:
        return np.zeros((0, 0)), np.zeros((0, 0))
    return a[y0:y1, x0:x1], m[y0 - ty:y1 - ty, x0 - tx:x1 - tx]


def _upsampled_dft(data, region, factor, offsets):
    for n_items, off in zip(data.shape[::-1], offsets[::-1]):
        kernel = np.exp(-2j * np.pi * (np.arange(region) - off)[:, None]
                        * np.fft.fftfreq(n_items, factor)[None, :])
        data = np.tensordot(kernel, data, axes=(1, -1))
    return data


def _subpixel(a, m):
    """Residual shift (dy, dx) with a[x] ~ m[x - d], for equal-size overlap crops. Both crops
    are Hann-windowed (same window): the FFT treats them as periodic, and unwindowed crop edges
    would pull the peak toward zero shift."""
    win = np.outer(np.hanning(a.shape[0]), np.hanning(a.shape[1]))
    prod = np.fft.fft2(a * win) * np.conj(np.fft.fft2(m * win))
    cc = np.fft.ifft2(prod)
    peak = np.array(np.unravel_index(int(np.argmax(np.abs(cc))), cc.shape), dtype=float)
    mid = np.array(cc.shape) // 2
    peak[peak > mid] -= np.array(cc.shape)[peak > mid]
    peak = np.round(peak * UPSAMPLE) / UPSAMPLE
    region = int(math.ceil(UPSAMPLE * 1.5))
    dftshift = math.floor(region / 2.0)
    up = _upsampled_dft(np.conj(prod), region, UPSAMPLE, dftshift - peak * UPSAMPLE).conj()
    best = np.array(np.unravel_index(int(np.argmax(np.abs(up))), up.shape), dtype=float)
    return tuple(peak + (best - dftshift) / UPSAMPLE)


def _rescale(lum, s):
    """Bring an image that is at x s the anchor's scale to the anchor's scale."""
    if abs(s - 1.0) < 1e-12:
        return lum
    if s > 1.0 + 0.2:
        return area_resample(lum, s)
    # Cubic: a smoothing (linear) interpolation would favour every scale but exactly 1 whenever
    # the two images are also misaligned by a fraction of a pixel.
    return ndimage.zoom(lum, 1.0 / s, order=3)


def _low(lum, d):
    return area_resample(lum, d) if d > 1.0 else lum


def target_patch(anchor_lum) -> Tuple[Box, int]:
    """The anchor region registration correlates against -- the target plus twice its size on
    every side (at least TARGET_PAD_MIN px), clamped to the frame -- and the target's larger
    side in px. Whole frames would let frame-edge artefacts (ASIStudio's padding border,
    AutoStakkert's drizzle edge strip) outweigh a 12-30 px planet."""
    t = metric.target_extent(anchor_lum)
    extent = max(t.y1 - t.y0, t.x1 - t.x0)
    pad = max(TARGET_PAD_MIN, 2 * extent)
    h, w = anchor_lum.shape
    return Box(max(0, t.y0 - pad), min(h, t.y1 + pad), max(0, t.x0 - pad), min(w, t.x1 + pad)), extent


def scale_tolerance_for(extent: int, floor: float = SCALE_TOLERANCE) -> float:
    """A scale error e moves the target's edge by e * extent / 2 px; below ~0.5 px it cannot be
    measured, so the tolerance is max(floor, 1 px / extent). The true factors are discrete
    (x1, x1.5, x3), so this check only has to tell them apart and catch a wrong one."""
    return max(floor, 1.0 / max(extent, 1))


def register(moving, anchor, *, scale_tolerance: float = SCALE_TOLERANCE) -> Registration:
    """Register `moving` (HxW or HxWxC) onto `anchor`; see the module docstring.

    Raises ScaleMismatch (with .measured) if the scale is not 1 within
    scale_tolerance_for(target extent, scale_tolerance), and RegistrationFailed if no
    orientation/shift correlates (ncc < MIN_NCC)."""
    m, a_full = metric.luminance(moving), metric.luminance(anchor)
    try:
        patch, extent = target_patch(a_full)
    except ValueError as e:
        raise RegistrationFailed(f"anchor has no target to register on: {e}") from e
    a = a_full[patch.y0:patch.y1, patch.x0:patch.x1]
    tol = scale_tolerance_for(extent, scale_tolerance)

    # Coarse: the scale family. The resolution follows the anchor PATCH (a 17 px Venus in a
    # 1920 px frame would be 3 px at frame-driven resolution), the reference capped at
    # 4 * LOW_RES_MAX_SIDE px for cost. A candidate scale below 1 shrinks the anchor patch
    # instead of enlarging the reference (same test, a fraction of the cost).
    d_c = max(1.0, max(a.shape) / LOW_RES_MAX_SIDE, max(m.shape) / (4 * LOW_RES_MAX_SIDE))
    a_c, m_c = _low(a, d_c), _low(m, d_c)

    def coarse_score(s):
        """Best NCC over the four orientations at candidate scale s. Flipping the anchor patch
        instead of the reference gives the same NCC values and shares the reference's FFTs.
        Intensity (lightly smoothed), not band-pass features: a small featureless disc's
        band-pass response is mostly the filter kernel at any size, so it cannot tell x1 from
        x1.5 (measured on the 17 px Venus: ncc 0.92-1.00 over x1/3..x3)."""
        aa, mm = (a_c, _rescale(m_c, s)) if s >= 1.0 else (_rescale(a_c, 1.0 / s), m_c)
        if min(aa.shape) < 16 or min(mm.shape) < 16:
            return -1.0
        aa, mm = ndimage.gaussian_filter(aa, 1.0), ndimage.gaussian_filter(mm, 1.0)
        return max(r[1] for r in _ncc_peaks([apply_orientation(aa, o) for o in ORIENTATIONS], mm))

    best_ncc, best_s = max((coarse_score(s), s) for s in COARSE_SCALES)
    if best_ncc < MIN_NCC:
        raise RegistrationFailed(f"no orientation/scale correlates (best coarse ncc {best_ncc:.3f})")

    # Fine, at the patch's resolution: the orientation (a small disc is too few pixels at the
    # coarse resolution to break its near-symmetry), then the scale to a fraction of a percent
    # on a crop of the reference around where the target was found.
    d_f = max(1.0, max(a.shape) / LOW_RES_MAX_SIDE)
    a_f = _features(_low(a, d_f), 2.0, 8.0)
    m_f = _rescale(_low(m, d_f), best_s)
    if min(m_f.shape) < 16:
        raise RegistrationFailed(f"reference too small at x{best_s:g}: {m_f.shape}")
    m_f_feat = _features(m_f, 2.0, 8.0)
    located = max((_xcorr_peak(a_f, apply_orientation(m_f_feat, o))[::-1] + (o,)) for o in ORIENTATIONS)
    _, (ly, lx), orientation = located
    full = apply_orientation(m_f, orientation)
    half_range = max(FINE_HALF_RANGE, 1.5 * tol)
    step = half_range / FINE_STEPS
    # The rim that rescaling can push in from outside the crop (plus the features' reach) is
    # left out of the correlation; otherwise padded edges penalise one side of the search.
    rim = int(math.ceil(half_range * max(a_f.shape))) + 12
    y0, x0 = max(0, -ly - rim), max(0, -lx - rim)
    base = full[y0:min(full.shape[0], -ly + a_f.shape[0] + rim), x0:min(full.shape[1], -lx + a_f.shape[1] + rim)]
    centre = (np.array(base.shape) - 1) / 2.0

    def fine_score(r):
        # Scale by 1/r about the crop centre, same output shape: zooming about the origin
        # would also move the content by (r - 1) * distance, changing the sub-pixel
        # misalignment from step to step and biasing the peak.
        mm = ndimage.affine_transform(base, np.diag([r, r]), offset=centre - r * centre,
                                      order=3, mode="nearest")
        feat = _features(mm, 2.0, 8.0)
        if min(feat.shape) > 2 * rim + 16:
            feat = feat[rim:-rim, rim:-rim]
        return _xcorr_peak(a_f, feat)[1]

    ratios = 1.0 + step * np.arange(-FINE_STEPS, FINE_STEPS + 1)
    fine = np.array([fine_score(r) for r in ratios])
    k = int(np.argmax(fine))
    ratio = float(ratios[k])
    if 0 < k < len(fine) - 1:
        c0, c1, c2 = fine[k - 1], fine[k], fine[k + 1]
        den = c0 - 2 * c1 + c2
        if den < 0:
            ratio += 0.5 * (c0 - c2) / den * step
    scale = best_s * ratio
    if fine[k] < MIN_NCC:
        raise RegistrationFailed(f"no orientation/scale correlates (best ncc {fine[k]:.3f})")
    if abs(scale - 1.0) > tol:
        raise ScaleMismatch(scale, orientation, float(fine[k]))

    af = _features(a)
    mf = _features(apply_orientation(m, orientation))
    (ty, tx), ncc0 = _xcorr_peak(af, mf)
    if ncc0 < MIN_NCC:
        raise RegistrationFailed(f"full-resolution ncc {ncc0:.3f} below {MIN_NCC}")
    ao, mo = _overlap(af, mf, ty, tx)
    ry, rx = _subpixel(ao, mo)
    return Registration(orientation, (patch.y0 + ty + ry, patch.x0 + tx + rx), scale,
                        _ncc_after(ao, mo, ry, rx), tol)


def _ncc_after(ao, mo, ry, rx, border=4):
    """NCC of the overlap features once the residual sub-pixel shift is applied (cubic spline),
    ignoring a border the shift pulls in from outside."""
    ms = ndimage.shift(mo, (ry, rx), order=3, mode="nearest")
    a_, m_ = ao[border:-border, border:-border], ms[border:-border, border:-border]
    den = math.sqrt(float((a_ * a_).sum()) * float((m_ * m_).sum()))
    return float((a_ * m_).sum()) / den if den > 0 else 0.0


def overlap_box(reg: Registration, moving_shape, anchor_shape) -> Box:
    """The part of the anchor's grid that the (oriented, integer-shifted) moving image covers."""
    iy, ix = reg.int_shift
    h, w = moving_shape[0], moving_shape[1]
    return Box(max(0, iy), min(anchor_shape[0], iy + h), max(0, ix), min(anchor_shape[1], ix + w))


def crop_registered(img, reg: Registration, box: Box) -> np.ndarray:
    """The moving image's pixels under `box` (anchor coordinates), integer offset, no resampling."""
    o = apply_orientation(np.asarray(img), reg.orientation)
    iy, ix = reg.int_shift
    y0, y1, x0, x1 = box.y0 - iy, box.y1 - iy, box.x0 - ix, box.x1 - ix
    if y0 < 0 or x0 < 0 or y1 > o.shape[0] or x1 > o.shape[1]:
        raise ValueError(f"box {tuple(box)} is not inside the registered reference")
    return o[y0:y1, x0:x1]


# --- comparison ------------------------------------------------------------------------------

@dataclass
class Row:
    label: str
    path: Optional[str]
    factor: float
    registration: Optional[Registration]    # None for the anchor
    result: metric.Result
    # The anchor measured with its ALTERNATIVE frame count (compare's anchor_frames_alt): the flip
    # check between the two readings of our effective count (bench.status_of).
    alt: Optional[metric.Result] = None


RefSpec = Union[str, Tuple[str, Optional[float]]]


def compare(anchor, references: Sequence[RefSpec], *, mosaic: bool = False,
            anchor_label: Optional[str] = None, frame_size=None, frame_noise=None,
            anchor_frames: Optional[float] = None, capture_frames: Optional[int] = None,
            anchor_frames_alt: Optional[float] = None) -> List[Row]:
    """Measure the anchor (normally OUR stack: a path, (path, factor) or an array) and each
    reference (path, or (path, measured_factor)) with metric.measure on one common box, with
    DAN_interior on the anchor's interior window. Row 0 is the anchor. `frame_size` (w, h) of
    the capture makes every reference's resampling factor size-derived (load_reference).
    mosaic=True is a fallback that rows_to_dicts marks as not a pass/fail.

    END (Ruling P28) needs `frame_noise` -- (noise.PhotonTransfer, mean raw frame) of the capture
    (noise.measure_video) -- and every image's effective frame count: `anchor_frames` for the
    anchor, and for a reference its stacking percentage (RefInfo.percent: AutoStakkert's global
    percentage, approximate per alignment point; ASIStudio's) x `capture_frames`. One per-frame
    noise sigma_frame^2 on the common box (noise.box_noise of the frames under it) then gives every
    image its budget sigma_frame^2 / k. Without these, or for an image whose count is unknown, END
    is None and Row.result.end_note says why (`frame_noise` may also be a str: why the capture's
    per-frame noise could not be measured).

    Refuses processed references (sharpened/stretched: ASIStudio .jpg, AutoStakkert _conv)."""
    if isinstance(anchor, (str, tuple)):
        a_path, a_factor = (anchor, None) if isinstance(anchor, str) else anchor
        a_ref = load_reference(a_path, a_factor, frame_size)   # a drizzled anchor comes to native too
        a_img, a_label, a_used = a_ref.image, anchor_label or a_ref.info.label, a_ref.factor
    else:
        a_img, a_path, a_used = np.asarray(anchor, dtype=np.float64), None, 1.0
        a_label = anchor_label or "ours"
    a_lum = metric.luminance(a_img)
    loaded = []
    for spec in references:
        path, factor = (spec, None) if isinstance(spec, str) else spec
        if parse_reference_name(path, factor).processed:
            raise ValueError(f"{path} is processed output (sharpened or stretched), not a stack;"
                             " measure the linear stack (.fit/.tif/.png) instead")
        ref = load_reference(path, factor, frame_size)
        loaded.append((ref, register(ref.image, a_lum)))

    box = metric.target_box(a_lum)
    y0, y1, x0, x1 = box
    for ref, reg in loaded:
        ob = overlap_box(reg, ref.image.shape, a_lum.shape)
        y0, y1 = max(y0, ob.y0 + OVERLAP_INSET), min(y1, ob.y1 - OVERLAP_INSET)
        x0, x1 = max(x0, ob.x0 + OVERLAP_INSET), min(x1, ob.x1 - OVERLAP_INSET)
    if y1 - y0 < 32 or x1 - x0 < 32:
        raise ValueError(f"the references' common area with the target is too small: {(y0, y1, x0, x1)}")
    box = Box(y0, y1, x0, x1)
    # The interior window's erosion comes from the sharpest limb of the capture (ours and every
    # reference, each on its registered crop), so a softer anchor cannot erode itself out of
    # the interior bar.
    limbs = [metric.limb_sigma(a_lum[box.y0:box.y1, box.x0:box.x1])]
    limbs += [metric.limb_sigma(metric.luminance(crop_registered(ref.image, reg, box))) for ref, reg in loaded]
    window = metric.interior_window(a_lum, box, limb=min(limbs))

    budget = _noise_budget(frame_noise, a_lum, box, window, mosaic)   # (box, interior, sky) or a reason
    a_kw = _end_kwargs(budget, anchor_frames, "our stack's effective frame count")
    a_res = metric.measure(a_lum, box, mosaic=mosaic, interior=window, **a_kw.pop("measure"))
    alt = None
    if anchor_frames_alt is not None:
        alt_kw = _end_kwargs(budget, anchor_frames_alt, "our stack's alternative effective frame count")
        alt = _with_note(metric.measure(a_lum, box, mosaic=mosaic, interior=window, **alt_kw.pop("measure")), alt_kw)
    rows = [Row(a_label, a_path, a_used, None, _with_note(a_res, a_kw), alt)]
    for ref, reg in loaded:
        crop = metric.luminance(crop_registered(ref.image, reg, box))
        k = (ref.info.percent / 100.0 * capture_frames) if (ref.info.percent and capture_frames) else None
        kw = _end_kwargs(budget, k, f"{ref.info.label}'s stacking percentage x the capture's frames")
        m = kw.pop("measure")
        if "noise_power" in m and not mosaic:
            # The reference's own sky, for the excess-noise check: its WHOLE image (the crop is mostly
            # target). An unmeasurable sky leaves the judged END None (no fallback to the crop).
            try:
                m["sky_floor"] = metric.sky_noise_floor(ref.image)
            except metric.SkyUnmeasurable as e:
                m["sky_floor"] = f"{e}"
        r = metric.measure(crop, Box(0, crop.shape[0], 0, crop.shape[1]), mosaic=mosaic, interior=window, **m)
        rows.append(Row(ref.info.label, ref.path, ref.factor, reg, replace(_with_note(r, kw), box=box)))
    return rows


def _noise_budget(frame_noise, a_lum, box, window, mosaic):
    """(sigma_frame^2 on the box, on the interior window or None, at the sky's level), or the reason
    there is none (a str)."""
    if mosaic:
        return "mosaic fallback"
    if frame_noise is None:
        return "no per-frame noise for the capture (noise.measure_video not run)"
    if isinstance(frame_noise, str):
        return f"per-frame noise: {frame_noise}"
    import noise   # local: noise imports this module
    ptc, raw_mean = frame_noise
    try:
        raw = noise.raw_under(raw_mean, a_lum, box)
        s2 = noise.box_noise(ptc, raw)
        s2_int = noise.box_noise(ptc, raw, window.window) if window.judged else None
        return s2, s2_int, noise.sky_noise(ptc, raw)
    except noise.NoiseUnmeasurable as e:
        return f"per-frame noise unmeasurable on the box: {e}"


def _end_kwargs(budget, frames, what):
    """metric.measure keyword arguments for END, plus 'k' and a 'note' when END cannot be had."""
    if isinstance(budget, str):
        return {"measure": {}, "k": frames, "note": f"END not computed: {budget}"}
    if not frames or not frames > 0:
        return {"measure": {}, "k": frames, "note": f"END not computed: {what} is unknown"}
    s2, s2_int, s2_sky = budget
    m = {"noise_power": s2 / frames, "sky_noise_power": s2_sky / frames}
    if s2_int is not None:
        m["noise_power_interior"] = s2_int / frames
    return {"measure": m, "k": frames, "note": None}


def _with_note(res: metric.Result, kw) -> metric.Result:
    res = replace(res, end_frames=kw["k"])
    return replace(res, end_note=kw["note"]) if kw["note"] else res


def review_notes(rows: Sequence[Row]) -> List[str]:
    """What sends the comparison to human review rather than the numbers alone."""
    notes = []
    if any(r.result.mosaic for r in rows):
        notes.append(MOSAIC_WARNING)
    if not rows[0].result.interior_judged:
        d = rows[0].result.interior_diameter
        notes.append(LIMB_ONLY + (f" (eroded disc {d:.1f} px < {metric.INTERIOR_MIN_DIAMETER:g})" if d is not None else ""))
    ratios = [r.result.noise_floor / r.result.band_power for r in rows[1:] if r.result.band_power > 0]
    if ratios and rows[0].result.band_power > 0:
        ours = rows[0].result.noise_floor / rows[0].result.band_power
        lo, hi = min(ratios) / NOISE_REGIME_FACTOR, max(ratios) * NOISE_REGIME_FACTOR
        if not lo <= ours <= hi:
            notes.append(f"noise regime unlike every reference (floor/band {ours:.2e}, references "
                         f"{min(ratios):.2e}..{max(ratios):.2e}): human review (this does not detect sharpening)")
    return notes


def _ratios(rows: Sequence[Row], key: str):
    """Each row's `key` over the best REFERENCE's (rows 1..), or None; and whether every reference
    had a value (a reference without one makes the best unknowable -- the caller says so)."""
    vals = [getattr(r.result, key) for r in rows[1:]]
    known = [v for v in vals if v is not None]
    best = max(known) if known else None
    out = [(getattr(r.result, key) / best) if (best and getattr(r.result, key) is not None) else None for r in rows]
    return out, (len(known) == len(vals))


def _over(value, rows: Sequence[Row], key: str):
    known = [getattr(r.result, key) for r in rows[1:] if getattr(r.result, key) is not None]
    return (value / max(known)) if (value is not None and known) else None


def rows_to_dicts(rows: Sequence[Row]) -> List[dict]:
    """One dict per row (row 0 = the anchor). Ratios are to the best REFERENCE (rows 1..).
    `bar` applies Ruling 2's 0.97 to END (Ruling P28: END_full and, when judged, END_interior);
    `dan_bar` the same to DAN, now a secondary figure. `end_complete` is False when some
    reference has no END (then no END ratio is a verdict: bench.status_of says so loudly).
    `review` lists what needs a human (the same list on every row)."""
    best = max((r.result.value for r in rows[1:]), default=None)
    ints = [r.result.interior for r in rows[1:] if r.result.interior is not None]
    best_int = max(ints) if ints else None
    notes = review_notes(rows)
    e_full, ok_full = _ratios(rows, "end")
    e_int, ok_int = _ratios(rows, "end_interior")
    p_full, _ = _ratios(rows, "end_pure")
    p_int, _ = _ratios(rows, "end_interior_pure")
    out = []
    for i, r in enumerate(rows):
        g, res = r.registration, r.result
        full = (res.value / best) if best else None
        interior = (res.interior / best_int) if (best_int and res.interior is not None) else None
        out.append({
            "label": r.label, "path": r.path, "factor": r.factor,
            "end": res.end, "end_ratio": e_full[i],
            "end_interior": res.end_interior, "end_interior_ratio": e_int[i],
            "end_pure": res.end_pure, "end_pure_ratio": p_full[i],
            "end_interior_pure": res.end_interior_pure, "end_interior_pure_ratio": p_int[i],
            "end_frames": res.end_frames, "end_noise": res.end_noise, "end_noise_budget": res.end_noise_budget,
            "end_excess": res.end_excess, "end_sky_tiles": res.end_sky_tiles,
            "end_snr": list(res.end_snr) if res.end_snr else None, "end_note": res.end_note,
            "end_complete": ok_full and (ok_int or not res.interior_judged),
            "end_alt": r.alt.end if r.alt else None, "end_alt_frames": r.alt.end_frames if r.alt else None,
            "end_alt_ratio": _over(r.alt.end if r.alt else None, rows, "end"),
            "end_interior_alt": r.alt.end_interior if r.alt else None,
            "end_interior_alt_ratio": _over(r.alt.end_interior if r.alt else None, rows, "end_interior"),
            "dan": res.value, "band_snr": res.band_snr,
            "noise_floor": res.noise_floor, "box": list(res.box), "mosaic": res.mosaic,
            "ratio_to_best_reference": full,
            "dan_interior": res.interior, "interior_judged": res.interior_judged,
            "interior_diameter": res.interior_diameter,
            "interior_ratio_to_best_reference": interior,
            "bar": {"full": None if e_full[i] is None else e_full[i] >= PASS_BAR,
                    "interior": None if e_int[i] is None else e_int[i] >= PASS_BAR},
            "dan_bar": {"full": None if full is None else full >= PASS_BAR,
                        "interior": None if interior is None else interior >= PASS_BAR},
            "review": list(notes),
            "orientation": g.orientation if g else None,
            "shift": list(g.shift) if g else None, "scale": g.scale if g else None,
            "ncc": g.ncc if g else None,
        })
    return out


def print_rows(d: Sequence[dict]) -> None:
    for r in d:
        reg = (f"  {r['orientation']} shift ({r['shift'][0]:+.2f},{r['shift'][1]:+.2f}) "
               f"scale {r['scale']:.4f} ncc {r['ncc']:.3f}") if r["orientation"] else ""
        full = f"  {r['ratio_to_best_reference']:.3f}" if r["ratio_to_best_reference"] is not None else "  -"
        inner = (f"  interior {r['dan_interior']:.5f} ({r['interior_ratio_to_best_reference']:.3f})"
                 if r["interior_ratio_to_best_reference"] is not None else
                 (f"  interior {r['dan_interior']:.5f}" if r["dan_interior"] is not None else "  interior -"))
        end = (f"END {r['end']:.3f}" + (f" ({r['end_ratio']:.3f})" if r["end_ratio"] is not None else "")
               if r["end"] is not None else "END -")
        print(f"{r['label']:<48} {end}  DAN {r['dan']:.6f}{full}{inner}  SNR {r['band_snr']:9.1f}{reg}")
        if r["end_note"]:
            print(f"    {r['end_note']}")
    for note in (d[0]["review"] if d else []):
        print(f"REVIEW: {note}")


def _main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("compare", help="measure an anchor and references on one box")
    c.add_argument("--anchor", required=True)
    c.add_argument("--anchor-label")
    c.add_argument("--ref", action="append", default=[],
                   help="reference path, optionally PATH::FACTOR with a measured resampling factor")
    c.add_argument("--frame", help="capture frame size WxH: reference factors from their size")
    c.add_argument("--mosaic", action="store_true",
                   help="FALLBACK, not a pass/fail: raw-CFA mono anchor, four CFA sites flattened")
    c.add_argument("--json", action="store_true")
    m = sub.add_parser("measure", help="DAN of one image on its own target box")
    m.add_argument("path")
    m.add_argument("--mosaic", action="store_true", help="FALLBACK: four CFA sites flattened")
    args = ap.parse_args(argv)
    if args.cmd == "measure":
        r = metric.measure_path(args.path, mosaic=args.mosaic)
        print(f"DAN {r.value:.6f}  band SNR {r.band_snr:.2f}  box {tuple(r.box)}")
        if args.mosaic:
            print(f"REVIEW: {MOSAIC_WARNING}")
        return 0
    specs = []
    for s in args.ref:
        path, sep, f = s.rpartition("::")
        specs.append((path, float(f)) if sep else s)
    frame = tuple(int(v) for v in args.frame.lower().split("x")) if args.frame else None
    d = rows_to_dicts(compare(args.anchor, specs, mosaic=args.mosaic, anchor_label=args.anchor_label,
                              frame_size=frame))
    if args.json:
        print(json.dumps(d, indent=2))
    else:
        print_rows(d)
    return 0


if __name__ == "__main__":
    sys.exit(_main())
