### Task 12: Offline end-to-end on real reference data

**Files:**
- Create: `tests/test_optimize_offline_real.py`

**Interfaces:**
- Consumes: `loop`, `NumpyExecutor`, `eval.degrade.flatten`, `FingerprintStore`.

This is the bridge between synthetic unit tests and the live PI run: real cached reference fingerprints, real degradation, no PixInsight.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_optimize_offline_real.py
"""End-to-end loop over REAL cached reference fingerprints, with no PixInsight.

Proves the loop is correct on real fingerprint geometry. It does NOT prove the
output is beautiful -- NumpyExecutor is an approximation (spec SS7.4). Only the
live tests can speak to output quality.
"""

import numpy as np
import pytest

from autocontrast.db.store import FingerprintStore
from autocontrast.eval.degrade import flatten
from autocontrast.io.loaders import load_raster
from autocontrast.optimize.executors.numpy_exec import NumpyExecutor
from autocontrast.optimize.loop import begin, outcome, run_to_convergence

STORE = "data/fingerprints.sqlite"


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


def test_flattening_a_real_render_is_recovered(reference, tmp_path):
    rid, rec = reference
    # Round-trip a real render through flatten() and require measurable recovery.
    source = load_raster("data/discovery_cache/" + rec.provenance["cache_file"],
                         max_dim=800) if rec.provenance.get("cache_file") else None
    if source is None:
        pytest.fail(f"reference {rid} has no cached raster to degrade")

    result = _run(flatten(source, strength=0.6), rid, rec)
    assert result["improved"] is True
    assert result["distance"] < result["baseline_distance"]


def test_a_real_professional_render_is_declined(reference):
    rid, rec = reference
    source = load_raster("data/discovery_cache/" + rec.provenance["cache_file"],
                         max_dim=800)
    result = _run(source, rid, rec)
    assert result["improved"] is False, (
        f"the optimizer tried to 'improve' {rid}, a professional render measured "
        f"against its own fingerprint. baseline={result['baseline_distance']:.4f} "
        f"best={result['distance']:.4f}"
    )
    assert result["recipe"]["actions"] == []


def test_a_declined_run_explains_itself(reference):
    rid, rec = reference
    source = load_raster("data/discovery_cache/" + rec.provenance["cache_file"],
                         max_dim=800)
    result = _run(source, rid, rec)
    assert result["convergence_reason"]
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_optimize_offline_real.py -v`
Expected: FAIL — likely on the `cache_file` provenance key. Inspect an actual record first:

```bash
.venv/bin/python -c "
from autocontrast.db.store import FingerprintStore
s = FingerprintStore('data/fingerprints.sqlite')
for rid in ('eso1103a', 'esa_hubble:opo9545a1'):
    r = s.get(rid)
    print(rid, '->', None if r is None else r.provenance)
s.close()"
```

- [ ] **Step 3: Adjust the test to the real provenance shape**

Replace the `cache_file` lookups with whatever key the inspection above reports (likely `source_url`'s cached filename under `data/discovery_cache/`). Do **not** stub the reference — if no cached raster exists, the test must fail loudly telling the user to run the Phase 1 discovery path, exactly as written.

- [ ] **Step 4: Run test to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_optimize_offline_real.py -v`
Expected: PASS (3 tests)

- [ ] **Step 5: Commit**

```bash
git add tests/test_optimize_offline_real.py
git commit -m "optimize: offline end-to-end over real reference fingerprints

Bridges synthetic unit tests and the live PI run: real cached fingerprint
geometry, real degradation, no PixInsight. Proves the loop is correct --
NOT that the output is beautiful, which only the live tests can say."
```

---

