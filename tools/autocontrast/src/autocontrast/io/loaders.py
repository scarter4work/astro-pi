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
        try:
            # naxis=2: a debayered stack is a 3-axis cube, and astropy raises on a
            # 3-axis header carrying SIP distortion (what PixInsight writes).
            wcs = WCS(header, naxis=2)
        except Exception:
            return None
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

# TIFF is decoded by `tifffile`, NOT by Pillow, and the reason is measured:
# Pillow opens a 48-bit RGB TIFF without complaint and hands back uint8. A
# source pixel of (65535, 40000, 257) comes back as (255, 156, 1) — the low
# byte is gone and nothing says so. That is the one failure mode §12 forbids
# outright, and it is exactly what the optimizer's candidate round trip would
# have hit: `.tif` is already in `supported_suffixes()` (Pillow registers it),
# so it passes `optimize_begin`'s validation and then silently quantizes every
# candidate to 8 bits. Production must not optimize against an approximation
# (§2.2), and an 8-bit intermediate is one.
_TIFF_SUFFIXES = {".tif", ".tiff"}

# What "full scale" means for the integer dtypes a raster can carry, used in
# place of INFERRING the range from the pixel values. Inference is wrong the
# moment an image is genuinely dark: a uint16 candidate whose brightest pixel
# is 200 has a true value of 200/65535 = 0.00305, but a data-derived rule sees
# `max <= 255`, divides by 255, and returns 0.78431 — 257x too bright, with no
# log and no error (§12). That was harmless while every raster came back from
# Pillow as uint8 by construction; making 16-bit TIFF the optimizer's candidate
# format turned it into a live path.
#
# uint8 and uint16 only, deliberately. For wider integer types the container
# width is not the data range — Pillow stores 16-bit grayscale TIFF as mode
# "I" (int32), where full scale is 65535 and not 2**31-1 — so there is no
# defensible constant, and those fall back to the range inference in
# `load_raster`. The formats this project writes and reads back are covered.
_INTEGER_FULL_SCALE = {np.dtype(np.uint8): 255.0, np.dtype(np.uint16): 65535.0}


def supported_suffixes() -> frozenset[str]:
    """Every file suffix :func:`load_image` can actually read back.

    ``load_image`` dispatches on suffix: FITS goes through astropy, TIFF through
    ``tifffile``, everything else through Pillow's ``Image.open``. Each of those
    three dispatch conditions contributes its own suffixes here, and each is the
    single place that condition lives — the Pillow half is read off its own
    registered plugins rather than hand-copied, so this can never drift from what
    Pillow can actually open. Callers that hand a path to something outside this
    set (a PixInsight-native ``.xisf``, say) need to fail immediately and loudly,
    not discover it as a read error deep inside a run (§12).
    """
    Image.init()  # populate Image.registered_extensions(); idempotent
    return (frozenset(_FITS_SUFFIXES) | frozenset(_TIFF_SUFFIXES)
            | frozenset(Image.registered_extensions()))


def _resize_native(arr: np.ndarray, max_dim: int | None) -> np.ndarray:
    """Downsize ``arr``'s longest side to ``max_dim`` WITHOUT quantizing it.

    The Pillow path resizes the decoded image object, which for 8-bit data is
    lossless in the sense that matters. Here the array may be uint16 or float32
    and Pillow has no RGB mode that holds either, so each channel is resized on
    its own as a 32-bit float image ("F" mode) and restacked. Same resampling
    kernel as the Pillow path (``Image.resize`` defaults to bicubic), so the two
    paths do not disagree about what "downsized to 1600" means.
    """
    height, width = arr.shape[:2]
    if max_dim is None or max(width, height) <= max_dim:
        return arr
    scale = max_dim / max(width, height)
    new_size = (max(1, round(width * scale)), max(1, round(height * scale)))
    planes = arr if arr.ndim == 3 else arr[..., None]
    resized = [
        np.asarray(Image.fromarray(planes[..., c].astype(np.float32), mode="F")
                   .resize(new_size))
        for c in range(planes.shape[-1])
    ]
    stacked = np.stack(resized, axis=-1)
    return stacked if arr.ndim == 3 else stacked[..., 0]


def _decode_tiff(path: Path, max_dim: int | None) -> tuple[np.ndarray, float | None]:
    """Decode a TIFF at its NATIVE bit depth (see ``_TIFF_SUFFIXES``).

    Returns the array and its full-scale value — the number that means 1.0 for
    the dtype the file was written at, or ``None`` when that dtype has no
    defensible one (see ``_INTEGER_FULL_SCALE``). The decode is the only place
    that knows the true depth: ``_resize_native`` hands back float32, so by the
    time ``load_raster`` sees the array the dtype is gone and all that is left
    is a guess from the data.
    """
    import tifffile

    arr = np.asarray(tifffile.imread(path))
    if arr.ndim > 3:
        # A multi-page or multi-sample TIFF: the optimizer's candidates are
        # single-page RGB, so anything else means we are reading a file we did
        # not write and cannot interpret. Refusing beats fingerprinting page 0
        # of something and calling it the candidate (§12).
        raise ValueError(
            f"{path.name}: expected a single 2-D or 3-D TIFF page, got shape "
            f"{arr.shape}; the sidecar has no rule for choosing among planes"
        )
    return _resize_native(arr, max_dim), _INTEGER_FULL_SCALE.get(arr.dtype)


def image_dimensions(path: str | Path) -> tuple[int, int]:
    """Native ``(width, height)`` of an image on disk, without decoding its pixels.

    Needed to compute how much :func:`load_image` downsampled a file, so a
    WCS-derived pixel scale can be corrected to the scale of the array we actually
    fingerprint (§2.2).
    """
    path = Path(path)
    if path.suffix.lower() in _FITS_SUFFIXES:
        with fits.open(path) as hdul:
            header = hdul[0].header
            return int(header["NAXIS1"]), int(header["NAXIS2"])
    if path.suffix.lower() in _TIFF_SUFFIXES:
        # Read through the same decoder `load_raster` uses, so the factor this
        # feeds `downsample_factor` is measured against the file the loader will
        # actually open. Only the page header is touched; no pixels are decoded.
        import tifffile

        with tifffile.TiffFile(path) as tif:
            page = tif.pages[0]
            return int(page.imagewidth), int(page.imagelength)
    Image.MAX_IMAGE_PIXELS = None  # professional mosaics exceed the default guard
    with Image.open(path) as im:
        return im.size  # (width, height)


def downsample_factor(path: str | Path, loaded: np.ndarray) -> float:
    """How many native pixels each pixel of ``loaded`` represents (>= 1.0).

    A WCS gives the pixel scale of the ORIGINAL file. If we fingerprint a
    downsampled copy, the effective scale is coarser by exactly this factor —
    multiply, or the energy spectrum's angular mapping (and therefore band-limiting,
    §4.4) is wrong by that factor.
    """
    native_w, native_h = image_dimensions(path)
    loaded_h, loaded_w = loaded.shape[:2]
    return max(native_w, native_h) / max(loaded_w, loaded_h)


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

    TIFF is decoded by ``tifffile`` rather than Pillow so 16-bit data survives
    (see ``_TIFF_SUFFIXES``); everything downstream of the decode — channel
    promotion, alpha, normalization, clipping — is shared, deliberately, so the
    two decoders cannot come to disagree about what a loaded image is.
    """
    path = Path(path)
    if path.suffix.lower() in _TIFF_SUFFIXES:
        arr, full_scale = _decode_tiff(path, max_dim)
    else:
        full_scale = None
        Image.MAX_IMAGE_PIXELS = None  # professional mosaics exceed Pillow's default guard
        with Image.open(path) as im:
            if max_dim is not None and max(im.size) > max_dim:
                scale = max_dim / max(im.size)
                new_size = (max(1, round(im.size[0] * scale)),
                            max(1, round(im.size[1] * scale)))
                im = im.resize(new_size)
            arr = np.asarray(im)
            full_scale = _INTEGER_FULL_SCALE.get(arr.dtype)

    if arr.ndim == 2:
        arr = np.stack([arr] * 3, axis=-1)
    if arr.shape[-1] == 4:  # drop alpha
        arr = arr[..., :3]

    arr = arr.astype(np.float64)
    # Normalize integer ranges to [0, 1]; float rasters are assumed display-scaled.
    if full_scale is not None:
        # The decoder knew the true depth, so use it. A dark 16-bit candidate
        # must not be rescaled as if it were 8-bit data (see _INTEGER_FULL_SCALE).
        arr = arr / full_scale
    else:
        max_value = float(arr.max()) if arr.size else 1.0
        if max_value > 1.0:
            # No known full scale (float data, or an integer width with no
            # standard range): infer the smallest standard range that fits.
            divisor = 65535.0 if max_value > 255.0 else 255.0
            arr = arr / divisor

    return np.clip(arr, 0.0, 1.0)
