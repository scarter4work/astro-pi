"""WCS acquisition — the three-tier fallback (design §5.2).

    1. FITS header WCS      (astropy)      — sidecar, offline
    2. embedded AVM         (pyavm)        — sidecar, offline
    3. blind plate solve    (injected)     — PixInsight ImageSolver in deployment
    4. manual annotation    (caller)       — first-class fallback, not an error

Tier 3 is a ``blind_solver`` callback rather than a hard dependency. The tool runs
inside PixInsight, which already owns plate-solving via **ImageSolver** — so the
PJSR orchestrator supplies the solver through the bridge. Standalone (the offline
harness, §3.1) passes ``None`` and simply falls through to manual/unsolved.

> "Do not treat a failed solve as an error state. It is the expected outcome for a
> nontrivial share of the prettiest targets. Handle it as a first-class branch."
> — §5.2

Accordingly this module never raises on an unsolvable image: it returns a result
with ``solved=False`` and a ``detail`` string the caller surfaces to the user (§12).
"""

from __future__ import annotations

import warnings
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

import numpy as np
from astropy.io import fits
from astropy.wcs import WCS
from PIL import Image

_FITS_SUFFIXES = {".fits", ".fit", ".fts"}


@dataclass
class WcsResult:
    """Outcome of WCS acquisition. ``wcs_source`` mirrors the §4.1 provenance field:
    ``header`` | ``avm`` | ``blind`` | ``manual`` | ``unsolved``.

    ``pixel_scale_arcsec`` is derived from the solved WCS wherever possible — §2.2
    requires each fingerprint to carry its *own* on-sky scale, so a real solve must
    never be overridden by a caller's nominal guess.
    """

    ra_deg: float | None
    dec_deg: float | None
    fov_radius_arcmin: float | None
    wcs_source: str
    solved: bool
    detail: str
    pixel_scale_arcsec: float | None = None


BlindSolver = Callable[[Path], WcsResult | None]


def _geometry(wcs: WCS, nx: int, ny: int) -> tuple[float, float, float, float]:
    """Field center (RA/Dec), corner radius (arcmin), and pixel scale (arcsec/px).

    Image dimensions are taken from the file, not from the WCS metadata — an AVM
    tag often omits Spatial.ReferenceDimension, leaving ``pixel_shape`` unset.
    Pixel scale comes from the full projection matrix, so rotation/skew are handled.
    """
    center = wcs.pixel_to_world(nx / 2.0, ny / 2.0)
    corner = wcs.pixel_to_world(0.0, 0.0)
    radius_arcmin = float(center.separation(corner).arcminute)
    scales = wcs.proj_plane_pixel_scales()  # Quantity per axis
    pixel_scale = float(np.mean([s.to("arcsec").value for s in scales]))
    return float(center.ra.deg), float(center.dec.deg), radius_arcmin, pixel_scale


def _from_fits_header(path: Path, notes: list[str]) -> WcsResult | None:
    with fits.open(path) as hdul:
        header = hdul[0].header
        data = hdul[0].data
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        wcs = WCS(header)
    if not wcs.has_celestial or data is None:
        notes.append("FITS header carries no celestial WCS")
        return None
    ny, nx = np.asarray(data).shape[:2]
    ra, dec, radius, scale = _geometry(wcs.celestial, nx, ny)
    return WcsResult(ra, dec, radius, "header", True, "WCS from FITS header",
                     pixel_scale_arcsec=scale)


def _from_avm(path: Path, notes: list[str]) -> WcsResult | None:
    """Tier 2. A missing AVM tag is normal and falls through quietly; a *malformed*
    one is a real error and is recorded in ``notes`` so it surfaces (§12) rather
    than being silently indistinguishable from "no tag"."""
    try:
        from pyavm import AVM, NoAVMPresent
    except ImportError:  # pragma: no cover - pyavm is an optional extra
        notes.append("pyavm not installed; AVM tier skipped")
        return None

    try:
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            avm = AVM.from_image(str(path))
            wcs = avm.to_wcs()
    except NoAVMPresent:
        return None  # expected: most renders carry no AVM tag
    except Exception as exc:  # malformed tag — a real failure, do not bury it
        notes.append(f"AVM tag present but unreadable: {type(exc).__name__}: {exc}")
        return None

    if wcs is None or not wcs.has_celestial:
        notes.append("AVM tag present but carries no celestial WCS")
        return None

    with Image.open(path) as im:
        nx, ny = im.size
    ra, dec, radius, scale = _geometry(wcs.celestial, nx, ny)
    return WcsResult(ra, dec, radius, "avm", True, "WCS from embedded AVM tag",
                     pixel_scale_arcsec=scale)


def acquire_wcs(
    path: str | Path,
    *,
    blind_solver: BlindSolver | None = None,
    manual: dict | None = None,
) -> WcsResult:
    """Resolve a sky position for ``path`` using the §5.2 tier order.

    ``blind_solver`` receives the path and returns a :class:`WcsResult` or ``None``
    if it cannot solve (PixInsight ImageSolver in deployment). ``manual`` is the
    annotation dict ``{ra_deg, dec_deg, fov_radius_arcmin}``.
    """
    path = Path(path)
    tried: list[str] = []
    notes: list[str] = []  # diagnostics from failed tiers; surfaced in `detail`

    if path.suffix.lower() in _FITS_SUFFIXES:
        result = _from_fits_header(path, notes)
        if result is not None:
            return result
        tried.append("header")
    else:
        result = _from_avm(path, notes)
        if result is not None:
            return result
        tried.append("avm")

    if blind_solver is not None:
        result = blind_solver(path)
        if result is not None and result.solved:
            return result
        tried.append("blind")
        notes.append("blind solver could not solve this field")

    if manual is not None:
        return WcsResult(
            ra_deg=float(manual["ra_deg"]),
            dec_deg=float(manual["dec_deg"]),
            fov_radius_arcmin=float(manual["fov_radius_arcmin"]),
            wcs_source="manual",
            solved=True,
            detail="WCS from manual annotation",
            pixel_scale_arcsec=(float(manual["pixel_scale_arcsec"])
                                if manual.get("pixel_scale_arcsec") is not None else None),
        )

    reasons = ("; ".join(notes)) if notes else "no tier produced a WCS"
    return WcsResult(
        ra_deg=None, dec_deg=None, fov_radius_arcmin=None,
        wcs_source="unsolved", solved=False,
        detail=(f"No WCS for {path.name} after tiers {tried or ['none']}: {reasons}. "
                "Expected for starless/composite renders (§5.2) — "
                "prompt for manual annotation or skip this reference."),
    )
