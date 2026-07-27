"""The sidecar file-protocol service (design §3.1).

PixInsight (PJSR) drives the Python sidecar without any network dependency: it
writes a request JSON, runs

    <venv>/bin/python -m autocontrast.sidecar request.json response.json

and reads the response JSON. This is the default, reliable transport (§3.1); it
avoids blocking the PI event loop and needs no NetworkTransfer POST support.

The sidecar stays headless-importable so the offline harness (§10, Phase 0) can
use the same code with no PixInsight present.

Response contract:
    {"ok": true,  "op": <op>, "result": {...}}
    {"ok": false, "op": <op>, "error": "<message>"}
Errors are always surfaced, never silently swallowed (§12).
"""

from __future__ import annotations

import json
import sys
import traceback
from pathlib import Path

from autocontrast import __version__
from autocontrast.db.discover.crawl import PoliteFetcher
from autocontrast.db.discover.discover import acquire_reference
from autocontrast.db.discover.index import GalleryIndex
from autocontrast.db.store import FingerprintStore
from autocontrast.db.wcs import acquire_wcs
from autocontrast.fingerprint.distance import fingerprint_distance
from autocontrast.fingerprint.extract import FingerprintData, extract
from autocontrast.fingerprint.palette import palette_class_from_filters
from autocontrast.io.loaders import downsample_factor, load_image, load_raster


def _op_ping(_req: dict) -> dict:
    return {"pong": True, "version": __version__}


def _op_fingerprint(req: dict) -> dict:
    rgb = load_raster(req["image"], max_dim=req.get("max_dim", 1600))
    fp = extract(
        rgb,
        pixel_scale_arcsec=req["pixel_scale_arcsec"],
        n_scales=req.get("n_scales", 7),
        psf_fwhm_arcsec=req["psf_fwhm_arcsec"],
        palette_class=req["palette_class"],
    )
    return fp.to_dict()


def _op_distance(req: dict) -> dict:
    reference = FingerprintData.from_dict(req["reference"])
    target = FingerprintData.from_dict(req["target"])
    return {"distance": fingerprint_distance(reference, target)}


def _wcs_to_dict(wcs) -> dict:
    return {
        "solved": wcs.solved,
        "ra_deg": wcs.ra_deg,
        "dec_deg": wcs.dec_deg,
        "fov_radius_arcmin": wcs.fov_radius_arcmin,
        "pixel_scale_arcsec": wcs.pixel_scale_arcsec,
        "wcs_source": wcs.wcs_source,
        "detail": wcs.detail,
    }


def _op_solve(req: dict) -> dict:
    """§5.2 three-tier WCS acquisition for one file. Never raises on 'no WCS' —
    that is a reported outcome, not an error."""
    return _wcs_to_dict(acquire_wcs(req["image"], manual=req.get("manual")))


def _derive_palette(image: str | Path, declared: str | None) -> str:
    """Declared palette wins; otherwise derive from the FITS FILTER keyword (§4.3).

    Never guesses: a raster with no header yields 'unknown', which forfeits chroma
    via the §2.3 gate rather than inventing a match.
    """
    if declared:
        return declared
    path = Path(image)
    if path.suffix.lower() not in {".fit", ".fits", ".fts"}:
        return "unknown"
    try:
        from astropy.io import fits

        with fits.open(path) as hdul:
            filters = [hdul[0].header.get("FILTER")]
    except Exception:
        return "unknown"
    return palette_class_from_filters([f for f in filters if f])


def _op_analyze(req: dict) -> dict:
    """The §6 miss path as one call, for PJSR: solve -> look up -> discover -> measure.

    This is what PixInsight actually drives. It reports which reference was chosen and
    why, its licensing obligation (§5.5), and the fingerprint distance to it — the
    number Phase 2's optimizer will descend.
    """
    image = req["image"]
    wcs = acquire_wcs(image, manual=req.get("manual"))
    if not wcs.solved:
        # No position means no cone search. Refusing beats inventing one (§5.2).
        raise ValueError(
            f"cannot solve a WCS for {image}: {wcs.detail} "
            "Supply a manual annotation or plate-solve it first."
        )

    palette_class = _derive_palette(image, req.get("palette_class"))
    radius = req.get("search_radius_arcmin") or wcs.fov_radius_arcmin
    psf = req.get("psf_fwhm_arcsec", 2.0)
    n_scales = req.get("n_scales", 7)
    max_dim = req.get("max_dim", 1600)

    store = FingerprintStore(req["store"])
    index = GalleryIndex(req["index"])
    try:
        if req.get("allow_discovery", True):
            outcome = acquire_reference(
                store, index,
                ra_deg=wcs.ra_deg, dec_deg=wcs.dec_deg, search_radius_arcmin=radius,
                palette_class=palette_class,
                fetcher=PoliteFetcher(), cache_dir=req["cache_dir"],
                psf_fwhm_arcsec=psf, n_scales=n_scales, max_dim=max_dim,
            )
            matches, discovery = outcome.matches, outcome.discovered
        else:
            matches, discovery = store.cone_search(
                wcs.ra_deg, wcs.dec_deg, radius, palette_class
            ), None

        result: dict = {
            "wcs": _wcs_to_dict(wcs),
            "palette_class": palette_class,
            "search_radius_arcmin": radius,
            "discovered": bool(discovery and discovery.ingested),
            "reference": None,
            "distance": None,
            "considered": [
                {"id": r.id, "accepted": r.accepted, "reason": r.reason}
                for r in (discovery.considered if discovery else [])
            ],
            "detail": discovery.detail if discovery else f"{len(matches)} stored reference(s).",
        }
        if not matches:
            return result

        best = min(matches, key=lambda m: m.separation_arcmin)
        provenance = best.record.provenance or {}
        result["reference"] = {
            "id": best.record.id,
            "palette_class": best.record.palette_class,
            "source_type": best.record.source_type,
            "separation_arcmin": best.separation_arcmin,
            "palette_compatible": best.palette_compatible,
            "license": provenance.get("license"),
            "attribution": provenance.get("attribution"),
            "source_url": provenance.get("source_url"),
            "gallery": provenance.get("gallery"),
        }

        rgb = load_image(image, max_dim=max_dim)
        effective_scale = wcs.pixel_scale_arcsec * downsample_factor(image, rgb)
        target = extract(
            rgb, pixel_scale_arcsec=effective_scale, n_scales=n_scales,
            psf_fwhm_arcsec=psf, palette_class=palette_class,
        )
        result["distance"] = fingerprint_distance(best.record.fingerprint, target)
        return result
    finally:
        store.close()
        index.close()


_OPS = {
    "ping": _op_ping,
    "fingerprint": _op_fingerprint,
    "distance": _op_distance,
    "solve": _op_solve,
    "analyze": _op_analyze,
}


def handle_request(request: dict) -> dict:
    """Dispatch a single request dict to a response dict. Never raises: failures
    are returned as ``{"ok": false, "error": ...}`` so the bridge always gets a
    well-formed response."""
    op = request.get("op")
    handler = _OPS.get(op)
    if handler is None:
        return {"ok": False, "op": op, "error": f"Unknown op {op!r}; expected {sorted(_OPS)}"}
    try:
        return {"ok": True, "op": op, "result": handler(request)}
    except Exception as exc:  # surface the real failure to PixInsight (§12)
        return {"ok": False, "op": op, "error": f"{type(exc).__name__}: {exc}",
                "traceback": traceback.format_exc()}


def main(argv: list[str] | None = None) -> int:
    """CLI entry: ``sidecar <request.json> <response.json>``. Returns 0 iff ok."""
    argv = argv if argv is not None else sys.argv[1:]
    if len(argv) != 2:
        sys.stderr.write("usage: python -m autocontrast.sidecar <request.json> <response.json>\n")
        return 2
    request_path, response_path = Path(argv[0]), Path(argv[1])
    request = json.loads(request_path.read_text())
    response = handle_request(request)
    response_path.write_text(json.dumps(response))
    return 0 if response.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
