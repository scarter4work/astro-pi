"""Reference ingest pipeline (design §5).

Loads a reference raster, fingerprints it, wraps it with its sky position, palette,
and provenance, and stores it. This is the ``fetch → fingerprint → store`` spine of
the Phase 1 exit criterion (§10). Provenance enforcement lives in the store, so a
``tool_output`` reference is rejected here too (§5.3).
"""

from __future__ import annotations

import uuid
from pathlib import Path

from autocontrast.fingerprint.extract import extract
from autocontrast.io.loaders import load_raster

from .records import ReferenceRecord
from .store import FingerprintStore


def ingest_reference(
    store: FingerprintStore,
    image_path: str | Path,
    *,
    ra_deg: float,
    dec_deg: float,
    fov_radius_arcmin: float,
    pixel_scale_arcsec: float,
    psf_fwhm_arcsec: float,
    palette_class: str,
    source_type: str,
    provenance: dict,
    id: str | None = None,
    n_scales: int = 7,
    max_dim: int | None = 1600,
) -> ReferenceRecord:
    """Fingerprint ``image_path`` and store it as a reference record.

    ``palette_class`` must be declared for references (§4.3 — it cannot be derived
    from a rendered raster). ``id`` is auto-generated if omitted.
    """
    rgb = load_raster(image_path, max_dim=max_dim)
    fingerprint = extract(
        rgb, pixel_scale_arcsec=pixel_scale_arcsec, n_scales=n_scales,
        psf_fwhm_arcsec=psf_fwhm_arcsec, palette_class=palette_class,
    )
    record = ReferenceRecord(
        id=id or str(uuid.uuid4()),
        ra_deg=ra_deg,
        dec_deg=dec_deg,
        fov_radius_arcmin=fov_radius_arcmin,
        palette_class=palette_class,
        source_type=source_type,
        fingerprint=fingerprint,
        provenance=provenance,
    )
    store.store(record)  # provenance enforcement (§5.3) happens here
    return record
