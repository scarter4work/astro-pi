"""End-to-end loop over REAL cached reference fingerprints, with no PixInsight.

Bridges the synthetic unit tests (which use manufactured fingerprints) and the
live PI run: this module uses a real `FingerprintStore` record and a real
cached professional render, degraded with the real `flatten()` pixel
transform. It proves the LOOP is correct on real fingerprint geometry -- beam
search, guardrails and convergence all operate on real energy spectra, real
color-moment statistics, real star fields.

It does NOT prove the output is beautiful -- `NumpyExecutor` is an
approximation of the SS6.2 actions (spec SS7.4), not PixInsight's own
processes. Only the live PixInsight tests can speak to output quality.

Reference resolution and palette note: this store currently holds exactly one
professional-render fingerprint, `eso1103a` (a broadband RGB ESO image);
`esa_hubble:opo9545a1` is not present, so resolution always falls through to
`eso1103a`. That render is broadband, not narrowband/HOO, so these tests do
NOT exercise the SS2.3 palette gate's chroma-drop path -- the reference and
the degraded/undegraded proxy share one palette class throughout. That is a
real gap in offline coverage, not a bug in the test.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest

from autocontrast.db.store import FingerprintStore
from autocontrast.eval.degrade import flatten
from autocontrast.io.loaders import load_raster
from autocontrast.optimize.executors.numpy_exec import NumpyExecutor
from autocontrast.optimize.loop import begin, outcome, run_to_convergence

STORE = "data/fingerprints.sqlite"
CACHE_DIR = Path("data/discovery_cache")


def _reference(store_path=STORE):
    store = FingerprintStore(store_path)
    try:
        for rid in ("eso1103a", "esa_hubble:opo9545a1"):
            rec = store.get(rid)
            if rec is not None:
                return rid, rec
    finally:
        store.close()
    return None, None


@pytest.fixture
def reference():
    rid, rec = _reference()
    if rec is None:
        pytest.fail(
            "no cached reference in data/fingerprints.sqlite. Run the Phase 1 "
            "analyze path first: python -m autocontrast.db.discover sync"
        )
    return rid, rec


def _cached_raster_path(rid: str) -> Path:
    """The raster the Phase 1 discovery crawl cached for this reference.

    The store's provenance dict carries no `cache_file` key (it holds
    `source_url`/`license`/`attribution`/`ingested_utc`/`wcs_source` instead --
    see `discover.py`'s `_cached_image`, which names the file
    `{entry_id}{suffix}` where `entry_id` IS the record's own `id`). So the
    cache filename is derived from `rid` directly, not read out of provenance.
    """
    matches = sorted(CACHE_DIR.glob(f"{rid}.*"))
    if not matches:
        pytest.fail(
            f"reference {rid!r} has no cached raster under {CACHE_DIR}/. Run "
            "the Phase 1 discovery path first: "
            "python -m autocontrast.db.discover sync"
        )
    return matches[0]


def _memory_io():
    store: dict[str, np.ndarray] = {}
    return (store,
            lambda p: np.array(store[p], copy=True),
            lambda p, rgb: store.__setitem__(p, np.array(rgb, copy=True)))


def _run(image, rid, rec):
    store, load, save = _memory_io()
    save("/mem/proxy.png", image)
    session = begin(
        source_path="/mem/src.png", proxy_path="/mem/proxy.png", reference_id=rid,
        reference_fp=rec.fingerprint, work_dir="/mem",
        pixel_scale_arcsec=rec.fingerprint.pixel_scale_arcsec,
        psf_fwhm_arcsec=rec.fingerprint.psf_fwhm_arcsec,
        palette_class=rec.fingerprint.palette_class, n_scales=7,
        session_id="offline", load=load,
    )
    return outcome(run_to_convergence(session, NumpyExecutor(), load=load, save=save))


def test_flattening_a_real_render_is_recovered(reference):
    """A real professional render, flattened, is recoverable toward its OWN
    fingerprint -- the render and the reference describe the same object, so
    a genuine improvement is the only correct outcome (spec SS3.2's valley)."""
    rid, rec = reference
    source = load_raster(_cached_raster_path(rid), max_dim=800)

    result = _run(flatten(source, strength=0.6), rid, rec)
    assert result["improved"] is True
    assert result["distance"] < result["baseline_distance"]


def test_a_real_professional_render_is_declined(reference):
    """The reference's OWN cached render, measured against its OWN
    fingerprint, is already at the valley floor. Using any other cached
    render would compare two unrelated targets, where finding "improvements"
    would be correct rather than a fail-safe failure -- so this deliberately
    uses `_cached_raster_path(rid)` for the SAME `rid` the fixture resolved,
    never an arbitrary file from the cache directory."""
    rid, rec = reference
    source = load_raster(_cached_raster_path(rid), max_dim=800)

    result = _run(source, rid, rec)
    assert result["improved"] is False, (
        f"the optimizer tried to 'improve' {rid}, a professional render measured "
        f"against its own fingerprint. baseline={result['baseline_distance']:.4f} "
        f"best={result['distance']:.4f}"
    )
    assert result["recipe"]["actions"] == []


def test_a_declined_run_explains_itself(reference):
    """§6.3: convergence always reports a reason, even (especially) on decline."""
    rid, rec = reference
    source = load_raster(_cached_raster_path(rid), max_dim=800)

    result = _run(source, rid, rec)
    assert result["convergence_reason"]
