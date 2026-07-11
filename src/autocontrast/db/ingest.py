"""Reference ingest pipeline (design §5).

Loads a reference raster, fingerprints it, wraps it with its sky position, palette,
and provenance, and stores it. This is the ``fetch → fingerprint → store`` spine of
the Phase 1 exit criterion (§10). Provenance enforcement lives in the store, so a
``tool_output`` reference is rejected here too (§5.3).
"""

from __future__ import annotations

import uuid
from dataclasses import dataclass
from pathlib import Path

from autocontrast.fingerprint.extract import extract
from autocontrast.io.loaders import load_image

from .records import ReferenceRecord
from .store import FingerprintStore
from .wcs import BlindSolver, WcsResult, acquire_wcs


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
    rgb = load_image(image_path, max_dim=max_dim)
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


@dataclass
class IngestOutcome:
    """Result of an auto-WCS ingest. When ``ingested`` is False the reference was
    skipped because no tier produced a WCS — an expected branch, not an error
    (§5.2). ``wcs.detail`` explains why so the caller can prompt for annotation."""

    ingested: bool
    wcs: WcsResult
    record: ReferenceRecord | None
    detail: str


def ingest_reference_auto(
    store: FingerprintStore,
    image_path: str | Path,
    *,
    psf_fwhm_arcsec: float,
    palette_class: str,
    source_type: str,
    provenance: dict,
    blind_solver: BlindSolver | None = None,
    manual: dict | None = None,
    pixel_scale_arcsec: float | None = None,
    id: str | None = None,
    n_scales: int = 7,
    max_dim: int | None = 1600,
) -> IngestOutcome:
    """Acquire WCS (§5.2 three-tier), then fingerprint and store.

    The pixel scale comes from the solved WCS whenever available (§2.2); the
    ``pixel_scale_arcsec`` argument is only a fallback for references whose WCS
    came from a manual annotation that omitted it. If no tier solves, the reference
    is skipped and the reason is reported — never raised, never silently ingested
    with a bogus position.
    """
    wcs = acquire_wcs(image_path, blind_solver=blind_solver, manual=manual)

    if not wcs.solved:
        return IngestOutcome(ingested=False, wcs=wcs, record=None, detail=wcs.detail)

    scale = wcs.pixel_scale_arcsec if wcs.pixel_scale_arcsec is not None else pixel_scale_arcsec
    if scale is None:
        return IngestOutcome(
            ingested=False, wcs=wcs, record=None,
            detail=("WCS solved but no pixel scale available; §2.2 forbids comparing "
                    "scales in pixels. Supply pixel_scale_arcsec or annotate it."),
        )

    record = ingest_reference(
        store, image_path,
        ra_deg=wcs.ra_deg, dec_deg=wcs.dec_deg, fov_radius_arcmin=wcs.fov_radius_arcmin,
        pixel_scale_arcsec=scale, psf_fwhm_arcsec=psf_fwhm_arcsec,
        palette_class=palette_class, source_type=source_type,
        provenance={**provenance, "wcs_source": wcs.wcs_source},
        id=id, n_scales=n_scales, max_dim=max_dim,
    )
    return IngestOutcome(ingested=True, wcs=wcs, record=record, detail=wcs.detail)
