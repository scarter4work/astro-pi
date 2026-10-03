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
import uuid
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
from autocontrast.io.loaders import (
    downsample_factor,
    load_image,
    load_raster,
    supported_suffixes,
)
from autocontrast.optimize import loop
from autocontrast.optimize.session import session_path


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
        # The reference's own fingerprint, verbatim. `optimize_begin` requires it
        # and PJSR has no other way to get it: the store is the sidecar's, and a
        # second `analyze` call to re-derive it would re-run discovery. Returned
        # from the call that already chose the reference, so the fingerprint the
        # optimizer descends toward is provably the one belonging to the
        # reference this response names.
        result["reference_fingerprint"] = best.record.fingerprint.to_dict()

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


# ---------------------------------------------------------------------------
# §2.5: the batched optimizer protocol.
#
# The sidecar NEVER applies pixels in production -- PixInsight does. These two
# ops measure, guardrail, score and prune candidates PJSR produced and saved,
# and hand back the next batch of instructions. A "PixInsightExecutor" behind
# `executor.py`'s one-candidate-at-a-time `apply` was rejected on cost: it would
# launch a headless PixInsight per candidate, ~540 launches for a 20-iteration
# run at the measured ~27 candidates/iteration.
# ---------------------------------------------------------------------------


def _optimize_loader(max_dim: int | None):
    """The single way an optimizer session reads pixels.

    Every spawned sidecar must read the proxy, the parents and the candidates
    the same way the baseline was measured, which is why ``max_dim`` lives on
    the session rather than in each request. ``load_image`` rather than
    ``load_raster`` so a FITS proxy goes through astropy: Pillow's FITS plugin
    hands back NaNs and would silently poison the fingerprint.
    """
    def load(path):
        return load_image(path, max_dim=max_dim)

    return load


def _batch_result(session, instructions: list[dict]) -> dict:
    """Fields common to both ops.

    ``iteration`` is the iteration the returned ``instructions`` belong to; with
    no instructions there is no further work, so it is the last one completed.
    (``outcome``'s ``iterations`` is the count of COMPLETED iterations, which is
    a different number while a batch is in flight.)
    """
    return {
        "session_id": session.session_id,
        "converged": session.converged,
        "instructions": instructions,
        "iteration": session.iteration + 1 if instructions else session.iteration,
    }


def _op_optimize_begin(req: dict) -> dict:
    """Open a session, measure the baseline, return instruction batch 1."""
    work_dir = req["work_dir"]
    Path(work_dir).mkdir(parents=True, exist_ok=True)
    session_id = req.get("session_id") or uuid.uuid4().hex[:12]
    max_dim = req.get("max_dim", 1600)
    image = req["image"]
    load = _optimize_loader(max_dim)

    # The request carries the image's NATIVE on-sky scale; the fingerprint is
    # taken on a downsampled array, whose effective scale is coarser by exactly
    # that factor (§2.2). Same correction `_op_analyze` applies.
    effective_scale = req["pixel_scale_arcsec"] * downsample_factor(image, load(image))

    session = loop.begin(
        source_path=req.get("source_path", image), proxy_path=image,
        reference_id=req["reference_id"],
        reference_fp=FingerprintData.from_dict(req["reference_fingerprint"]),
        work_dir=work_dir, pixel_scale_arcsec=effective_scale,
        psf_fwhm_arcsec=req["psf_fwhm_arcsec"], palette_class=req["palette_class"],
        n_scales=req.get("n_scales", 7), session_id=session_id, load=load,
    )
    candidate_suffix = req.get("candidate_suffix", ".png")
    supported = supported_suffixes()
    if candidate_suffix.lower() not in supported:
        # Caught here, this is one bad config field. Left to the loader, it
        # becomes every candidate raising inside `_ingest_batch`, every branch
        # discarded, and a run that reports "guardrails or executor failure"
        # for what was actually a suffix PixInsight can save but the sidecar
        # can never read back (§12) -- e.g. its own native `.xisf`.
        raise ValueError(
            f"candidate_suffix {candidate_suffix!r} is not a format the sidecar "
            "can load back (it reads every candidate through the same loader as "
            f"everything else); supported suffixes are {sorted(supported)}"
        )

    session.max_dim = max_dim
    session.candidate_suffix = candidate_suffix

    instructions = loop.begin_batch(session, load=load)
    session.save(session_path(work_dir, session_id))
    return {
        "baseline_distance": session.baseline_distance,
        "reference_id": session.reference_id,
        **_batch_result(session, instructions),
    }


def _op_optimize_step(req: dict) -> dict:
    """Score the candidates PixInsight saved; return the next batch."""
    work_dir, session_id = req["work_dir"], req["session_id"]
    path = session_path(work_dir, session_id)
    if not path.exists():
        # Never a silent fresh start: that would discard a run's progress while
        # PixInsight believed it was stepping the session it began (§12).
        raise FileNotFoundError(
            f"no session {session_id!r} under {work_dir!r} (expected {path}). "
            "Refusing to start a fresh run -- that would silently discard the "
            "progress of the session PixInsight thinks it is stepping."
        )

    # `resume`, never `Session.load`: the exact-path guard against resuming
    # against a different image lives there, and this is the one seam it exists
    # for. Bypassing it would make it dead code.
    session = loop.resume(path, proxy_path=req["image"])
    load = _optimize_loader(session.max_dim)

    instructions = loop.step_batch(session, req.get("produced", []), load=load)
    session.save(path)
    return {**loop.outcome(session), **_batch_result(session, instructions)}


_OPS = {
    "ping": _op_ping,
    "fingerprint": _op_fingerprint,
    "distance": _op_distance,
    "solve": _op_solve,
    "analyze": _op_analyze,
    "optimize_begin": _op_optimize_begin,
    "optimize_step": _op_optimize_step,
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
