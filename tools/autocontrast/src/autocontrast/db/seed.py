"""Curated seed catalog + seeder (design §5.2, baseline reference sourcing).

There is no cone-search API over professional press-release renders (verified: MAST
and ESASky cone-search *science observations*, not renders; the ESA/Hubble and ESO
public galleries expose no JSON API). So the reliable baseline is a curated catalog
of known-good renders, ingested through the normal pipeline. Auto-discovery for
arbitrary positions is layered on top (see :mod:`.discover`).

Critically, a linear science FITS must NEVER be seeded as a ``professional_render``:
it has no presentation layer (§2.1), would fingerprint as maximally flat, and would
poison the reference set with an anti-target. Catalog entries are rendered images.

Each entry declares a fallback position, but WCS embedded in the image itself (AVM)
always wins — the declared position is passed only as a manual annotation (§2.2).
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

from .ingest import ingest_reference_auto
from .store import FingerprintStore


@dataclass
class SeedOutcome:
    id: str
    ingested: bool
    detail: str
    wcs_source: str | None = None


def load_catalog(path: str | Path) -> list[dict]:
    """Load a JSON seed catalog (a list of entry dicts)."""
    return json.loads(Path(path).read_text())


def _resolve_image(entry: dict, image_dir: Path) -> Path | None:
    filename = entry.get("filename") or f"{entry['id']}.jpg"
    candidate = image_dir / filename
    return candidate if candidate.exists() else None


def seed_reference(
    store: FingerprintStore, entry: dict, image_dir: Path, *, n_scales: int = 7,
    max_dim: int | None = 1600,
) -> SeedOutcome:
    """Ingest a single catalog entry from an already-downloaded image."""
    path = _resolve_image(entry, Path(image_dir))
    if path is None:
        return SeedOutcome(
            id=entry["id"], ingested=False,
            detail=(f"Image not found for {entry['id']} in {image_dir}. "
                    f"Fetch it from {entry.get('url')} first."),
        )

    provenance = {
        "source_url": entry.get("source_url", ""),
        "license": entry.get("license", ""),
        "attribution": entry.get("attribution", ""),
        "ingested_utc": entry.get("ingested_utc", ""),
    }
    # The image's own WCS (AVM) wins; the catalog position is only a fallback.
    manual = None
    if entry.get("ra_deg") is not None and entry.get("dec_deg") is not None:
        manual = {
            "ra_deg": entry["ra_deg"],
            "dec_deg": entry["dec_deg"],
            "fov_radius_arcmin": entry.get("fov_radius_arcmin", 30.0),
            "pixel_scale_arcsec": entry.get("pixel_scale_arcsec"),
        }

    try:
        outcome = ingest_reference_auto(
            store, path,
            psf_fwhm_arcsec=entry.get("psf_fwhm_arcsec", 2.0),
            palette_class=entry["palette_class"],
            source_type=entry["source_type"],
            provenance=provenance,
            manual=manual,
            pixel_scale_arcsec=entry.get("pixel_scale_arcsec"),
            id=entry["id"], n_scales=n_scales, max_dim=max_dim,
        )
    except ValueError as exc:  # provenance rejection (§5.3) — report, do not crash the run
        return SeedOutcome(id=entry["id"], ingested=False, detail=str(exc))

    return SeedOutcome(
        id=entry["id"], ingested=outcome.ingested, detail=outcome.detail,
        wcs_source=outcome.wcs.wcs_source if outcome.ingested else None,
    )


def seed_catalog(
    store: FingerprintStore, entries: list[dict], image_dir: str | Path, *,
    n_scales: int = 7, max_dim: int | None = 1600,
) -> list[SeedOutcome]:
    """Ingest every catalog entry whose image is present in ``image_dir``.

    Failures (missing image, rejected provenance, unsolved WCS) are reported per
    entry rather than aborting the run — every degraded path surfaces (§12).
    """
    image_dir = Path(image_dir)
    return [
        seed_reference(store, entry, image_dir, n_scales=n_scales, max_dim=max_dim)
        for entry in entries
    ]
