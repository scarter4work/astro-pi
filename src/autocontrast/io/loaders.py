"""Standalone FITS loader.

Loads pixel data plus the two pieces of metadata the fingerprint depends on:
the on-sky pixel scale (§2.2) and the acquisition filter(s) used to derive the
palette class (§4.3). Runs with no PixInsight present (§3.1).

XISF support is deferred; FITS is the Phase 0 baseline.
"""

from __future__ import annotations

import warnings
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
from astropy.io import fits
from astropy.wcs import WCS
from PIL import Image


@dataclass
class LoadedImage:
    """An image plus the metadata needed to fingerprint it.

    Pixel data is float64 in its native value range — normalization is a metric
    concern (CIELAB needs [0, 1]), deliberately kept out of the loader.
    """

    data: np.ndarray
    pixel_scale_arcsec: float | None
    filters: list[str] = field(default_factory=list)


def _pixel_scale_from_header(header: fits.Header) -> float | None:
    """On-sky pixel scale in arcsec, from the full WCS matrix, or None if absent.

    Uses ``proj_plane_pixel_scales`` so rotation/skew in a CD matrix are handled
    correctly rather than reading CDELT naively.
    """
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")  # a header with no WCS warns; we handle it below
        wcs = WCS(header)
    if not wcs.has_celestial:
        return None
    scales_deg = wcs.proj_plane_pixel_scales()  # list of Quantity, one per axis
    arcsec = [s.to("arcsec").value for s in scales_deg]
    return float(np.mean(arcsec))


def load_fits(path: str | Path) -> LoadedImage:
    """Load a FITS file into a :class:`LoadedImage`."""
    with fits.open(path) as hdul:
        hdu = hdul[0]
        data = np.asarray(hdu.data, dtype=np.float64)
        header = hdu.header

    pixel_scale = _pixel_scale_from_header(header)

    filters: list[str] = []
    if "FILTER" in header:
        filters = [str(header["FILTER"]).strip()]

    return LoadedImage(data=data, pixel_scale_arcsec=pixel_scale, filters=filters)


_FITS_SUFFIXES = {".fits", ".fit", ".fts"}


def load_image(path: str | Path, max_dim: int | None = None) -> np.ndarray:
    """Load any supported image as an ``(H, W, 3)`` float64 array in [0, 1].

    Dispatches on file type: FITS goes through **astropy**, everything else through
    Pillow. This matters — Pillow has a FITS plugin that will happily open a float
    FITS and hand back NaNs, silently poisoning the fingerprint. FITS data is
    normalized by its own finite min/max and NaNs are zeroed.

    Note a linear FITS master will (correctly) fingerprint as *flat*: the
    fingerprint measures the nonlinear presentation layer (§2.1), and a linear
    master has none. References are expected to be rendered images.
    """
    path = Path(path)
    if path.suffix.lower() not in _FITS_SUFFIXES:
        return load_raster(path, max_dim=max_dim)

    data = np.asarray(load_fits(path).data, dtype=np.float64)
    if data.ndim == 3 and data.shape[0] in (1, 3):  # channel-first FITS cube
        data = np.moveaxis(data, 0, -1)
    if data.ndim == 2:
        data = np.stack([data] * 3, axis=-1)
    if data.shape[-1] == 1:
        data = np.repeat(data, 3, axis=-1)

    finite = np.isfinite(data)
    if not finite.any():
        raise ValueError(f"{path.name}: FITS contains no finite pixels")
    lo = float(data[finite].min())
    hi = float(data[finite].max())
    data = np.where(finite, data, lo)  # NaN/inf -> floor, never propagated
    if hi > lo:
        data = (data - lo) / (hi - lo)
    else:
        data = np.zeros_like(data)
    return np.clip(data, 0.0, 1.0)


def load_raster(path: str | Path, max_dim: int | None = None) -> np.ndarray:
    """Load a display-referred raster (PNG/JPEG/TIFF) as an ``(H, W, 3)`` float64
    sRGB image in [0, 1].

    Professional references are published as raster, not FITS (§2.1). Grayscale is
    promoted to three channels; any alpha channel is dropped; integer data is
    normalized by its full dynamic range so 8- and 16-bit renders land on the same
    [0, 1] scale.

    ``max_dim`` downsizes the longest side to at most ``max_dim`` pixels (aspect
    preserved, never upscaled). Fingerprint comparisons must normalize resolution
    across the whole set, or the energy spectrum (§4.2) confuses pixel count with
    presentation depth — a 18000px mosaic carries octaves of fine structure a
    1600px render simply cannot.
    """
    Image.MAX_IMAGE_PIXELS = None  # professional mosaics exceed Pillow's default guard
    with Image.open(path) as im:
        if max_dim is not None and max(im.size) > max_dim:
            scale = max_dim / max(im.size)
            new_size = (max(1, round(im.size[0] * scale)), max(1, round(im.size[1] * scale)))
            im = im.resize(new_size)
        arr = np.asarray(im)

    if arr.ndim == 2:
        arr = np.stack([arr] * 3, axis=-1)
    if arr.shape[-1] == 4:  # drop alpha
        arr = arr[..., :3]

    arr = arr.astype(np.float64)
    # Normalize integer ranges to [0, 1]; float rasters are assumed display-scaled.
    max_value = float(arr.max()) if arr.size else 1.0
    if max_value > 1.0:
        # 8-bit -> 255, 16-bit -> 65535; infer the smallest standard range that fits.
        divisor = 65535.0 if max_value > 255.0 else 255.0
        arr = arr / divisor

    return np.clip(arr, 0.0, 1.0)
