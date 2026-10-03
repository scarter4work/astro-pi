### Task 14: The §10 exit criterion, on real data

**Files:**
- Create: `tests/test_optimize_exit_criterion_live.py`

**Interfaces:**
- Consumes: the full stack; real files under `/mnt/qnap/astro_data/`.

**This task decides whether Phase 2 is done.** All three assertions must hold with one set of thresholds.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_optimize_exit_criterion_live.py
"""The Phase 2 exit criterion (design §10), on real data, nothing stubbed.

    "measurably improves a flat image, and — more importantly — fails safe on an
     already-well-processed image by declining to make it worse."

All three cases must pass with ONE set of thresholds. If no single value
satisfies all three, that is a finding about the fingerprint — report it, do not
tune around it (spec §3.7).
"""

from pathlib import Path

import numpy as np
import pytest

from autocontrast.db.store import FingerprintStore
from autocontrast.eval.degrade import flatten
from autocontrast.io.loaders import load_raster
from autocontrast.optimize.executors.numpy_exec import NumpyExecutor
from autocontrast.optimize.loop import begin, outcome, run_to_convergence

PRINTS = Path("/mnt/qnap/astro_data/prints")
FINISHED_HOO = PRINTS / "M42_hoo_2_18_23.jpg"
STORE = "data/fingerprints.sqlite"


@pytest.fixture(scope="module")
def reference():
    store = FingerprintStore(STORE)
    try:
        for rid in ("esa_hubble:opo9545a1", "eso1103a"):
            rec = store.get(rid)
            if rec is not None:
                return rid, rec
    finally:
        store.close()
    pytest.fail(
        "no cached M42 reference. Run the Phase 1 path first:\n"
        "  python -m autocontrast.db.discover sync"
    )


def _memory_io():
    store: dict[str, np.ndarray] = {}
    return (lambda p: np.array(store[p], copy=True),
            lambda p, rgb: store.__setitem__(p, np.array(rgb, copy=True)))


def _optimize(image, rid, rec):
    load, save = _memory_io()
    save("/mem/proxy.png", image)
    session = begin(
        source_path="/mem/src.png", proxy_path="/mem/proxy.png", reference_id=rid,
        reference_fp=rec.fingerprint, work_dir="/mem",
        pixel_scale_arcsec=rec.fingerprint.pixel_scale_arcsec,
        psf_fwhm_arcsec=rec.fingerprint.psf_fwhm_arcsec,
        palette_class=rec.fingerprint.palette_class, n_scales=7,
        session_id="exit", load=load,
    )
    return outcome(run_to_convergence(session, NumpyExecutor(), load=load, save=save))


@pytest.fixture(scope="module")
def finished_print():
    if not FINISHED_HOO.exists():
        pytest.fail(f"missing the user's finished HOO M42: {FINISHED_HOO}")
    return load_raster(FINISHED_HOO, max_dim=1200)


def test_declines_on_the_users_own_finished_render(reference, finished_print):
    """§10, half two — the important half."""
    rid, rec = reference
    result = _optimize(finished_print, rid, rec)
    assert result["improved"] is False, (
        f"the optimizer tried to 'improve' a render the user considers finished. "
        f"baseline={result['baseline_distance']:.4f} best={result['distance']:.4f} "
        f"recipe={result['recipe']}"
    )


def test_improves_a_flattened_version_of_that_same_render(reference, finished_print):
    """§10, half one. Same image, same reference, same thresholds — only the
    input quality differs, which is what makes this a fair pair with the test
    above rather than two unrelated runs."""
    rid, rec = reference
    result = _optimize(flatten(finished_print, strength=0.6), rid, rec)
    assert result["improved"] is True, (
        f"a deliberately flattened render was not improved. "
        f"baseline={result['baseline_distance']:.4f} best={result['distance']:.4f} "
        f"stopped={result['convergence_reason']}"
    )
    assert result["distance"] < result["baseline_distance"]


def test_declines_on_a_cached_professional_render(reference):
    """§10, half two again, on data that needs no user files — the CI-runnable case."""
    rid, rec = reference
    cached = sorted(Path("data/discovery_cache").glob("*.jpg"))
    if not cached:
        pytest.fail("no cached professional render under data/discovery_cache/")
    result = _optimize(load_raster(cached[0], max_dim=1200), rid, rec)
    assert result["improved"] is False, (
        f"the optimizer tried to 'improve' {cached[0].name}, a professional render."
    )


def test_the_pair_is_decided_by_one_threshold_set(reference, finished_print):
    """The calibration hazard, made a test: the SAME config must decline the good
    image and improve the flat one. Passing these individually with different
    thresholds would defeat the exit criterion (spec §3.7)."""
    rid, rec = reference
    good = _optimize(finished_print, rid, rec)
    flat = _optimize(flatten(finished_print, strength=0.6), rid, rec)
    assert good["improved"] is False and flat["improved"] is True
```

- [ ] **Step 2: Run and observe honestly**

Run: `.venv/bin/python -m pytest tests/test_optimize_exit_criterion_live.py -v`

Record the actual numbers. If a test fails:

- **Do not** lower `epsilon_improve` to rescue the improve case, or raise it to rescue a decline case, without re-running all four.
- If no single threshold set satisfies all four, **stop and report it**. That is a finding about the fingerprint or the proposer, exactly like the Phase 0 exit finding — which was reframed on evidence rather than tuned around.

- [ ] **Step 3: Run the whole suite**

Run: `.venv/bin/python -m pytest -q`
Expected: all prior tests still pass, nothing deselected.

- [ ] **Step 4: Commit**

```bash
git add tests/test_optimize_exit_criterion_live.py
git commit -m "Phase 2 exit criterion on real data

Declines on the user's own finished HOO M42 and on a cached professional
render; improves a deliberately flattened copy of that same finished
render. One threshold set decides all of them -- the pairing is itself a
test, because passing the halves under different thresholds would defeat
the criterion (spec SS3.7)."
```

- [ ] **Step 5: Update the project memory**

Record: Phase 2 Stage 1 status, the measured baseline/best distances, whichever thresholds were settled on, and any finding from Step 2. Note explicitly that Stage 0 remains uncommitted work and that `autocontrast_optimize.js` inherits the open release-compliance defect.

---

## Self-Review

**Spec coverage:**

| Spec section | Task |
|---|---|
| §1 scope, stretched-only, linear = loud error | Task 10 (`begin`), Task 13 |
| §2.2 sidecar-not-a-server, resumable state | Task 9, Task 11 |
| §2.3 Executor seam | Task 6 |
| §2.4 guardrails in Python from pixels | Tasks 2, 3, 4 |
| §2.5 batched instructions | Task 11 (`optimize_step` returns whole iteration) |
| §3.1 scoring, violations not scored | Task 10 |
| §3.2 fail-safe structural, decline returns original | Task 10 |
| §3.3 beam width, distinctness | Task 8 |
| §3.4 convergence conditions 1/3/4 | Task 10 |
| §3.5 proposer off component gaps | Task 7 |
| §3.6 proxy validation | **gap — see below** |
| §3.7 tunables + calibration discipline | Task 8 (`BeamConfig`), Task 14 |
| §4 action space, band-limited | Task 1 |
| §5 guardrail table | Tasks 2, 3, 4 |
| §6 error handling | Tasks 10, 11 |
| §7.1/7.2/7.3/7.4 three test tiers | Tasks 1–11, 12, 14 |
| §8 deliverable, recipe, STATUS_FILE | Tasks 5, 13 |

**Identified gap — §3.6 full-resolution validation.** Tasks 1–14 run the search entirely on the proxy and replay once; the *periodic full-res checkpoint* is not implemented. This is deliberate sequencing, not an omission: it requires the `PixInsightExecutor` to exist, and it cannot be meaningfully tested until the exit criterion is passing. **It must be added before Phase 2 is called complete.** Add as Task 15 once Task 14 is green:

> **Task 15:** every `config.validation_interval` iterations, `advance()` replays `session.best.recipe` against `session.source_path` at full resolution, re-measures, and appends `{"iteration", "proxy_distance", "full_distance", "diverged"}` to a `validation_log`. Divergence beyond `config.divergence_tolerance` sets a `diverged` flag surfaced in `outcome()` and printed loudly by the PJSR script (§12). Test: a deliberately scale-sensitive action must show divergence between an 800px proxy and the 1600px original.

**Placeholder scan:** no TBD/TODO. Task 12 Step 3 intentionally instructs adjusting a test to the real provenance shape after inspecting it — that is a real investigative step with a stated command, not a placeholder.

**Type consistency check:** `Action.key` used consistently in Tasks 1/5/7/8/10. `Recipe.key`/`applied_kinds` consistent in Tasks 5/7/8/10. `GuardrailVerdict.name`/`ok`/`reason` consistent in Tasks 2/3/4/10. `Branch(recipe, image_path, distance, alive)` consistent in Tasks 8/9/10. `begin(...)` keyword signature identical in Tasks 10/11/12/14. `outcome()` keys (`improved`, `baseline_distance`, `distance`, `result_path`, `recipe`, `pixinsight_steps`, `convergence_reason`, `guardrail_log`) consistent in Tasks 10/11/12/13/14.

One known wart, resolved: Task 10's `begin()` uses a `_Stub` to carry measurement metadata before a `Session` exists. If the implementer prefers, refactor `_measure` to take the four fields directly — behavior must not change.
