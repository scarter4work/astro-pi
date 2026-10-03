### Task 10: The loop — orchestration, convergence, and the fail-safe

**Files:**
- Create: `src/autocontrast/optimize/loop.py`
- Test: `tests/test_optimize_loop.py`

**Interfaces:**
- Consumes: everything from Tasks 1–9; `extract`, `fingerprint_distance`, `FingerprintData`.
- Produces: `begin(...) -> Session`, `advance(session, executor, *, load, save) -> Session`, `run_to_convergence(session, executor, *, load, save) -> Session`, `outcome(session) -> dict`.

**This is the task where "fails safe" happens.** Best-so-far is seeded with the input; a candidate replaces it only on strict improvement beyond `epsilon_improve`.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_optimize_loop.py
import numpy as np

from autocontrast.eval.degrade import flatten
from autocontrast.fingerprint.extract import extract
from autocontrast.optimize.executors.numpy_exec import NumpyExecutor
from autocontrast.optimize.loop import begin, outcome, run_to_convergence

SCALE, PSF, NSCALES = 1.0, 2.0, 7


def _reference_image(h=160, w=160, seed=11):
    """A deep-looking render: structure across several scales plus color."""
    rng = np.random.default_rng(seed)
    field = rng.normal(0, 1, (h, w))
    layered = sum(
        np.roll(field, k, axis=0) / (k + 1) for k in (1, 2, 4, 8, 16)
    )
    layered = (layered - layered.min()) / (layered.ptp() + 1e-12)
    return np.clip(np.stack([layered, layered * 0.85, layered * 1.1], axis=-1), 0.02, 0.98)


def _memory_io():
    """In-memory stand-ins for the disk round-trip the PI executor performs."""
    store: dict[str, np.ndarray] = {}

    def save(path, rgb):
        store[path] = np.array(rgb, copy=True)

    def load(path):
        return np.array(store[path], copy=True)

    return store, load, save


def _session_for(image, reference, store, save):
    ref_fp = extract(reference, pixel_scale_arcsec=SCALE, n_scales=NSCALES,
                     psf_fwhm_arcsec=PSF, palette_class="HOO")
    save("/mem/proxy.png", image)
    return begin(
        source_path="/mem/src.png", proxy_path="/mem/proxy.png",
        reference_id="synthetic", reference_fp=ref_fp, work_dir="/mem",
        pixel_scale_arcsec=SCALE, psf_fwhm_arcsec=PSF, palette_class="HOO",
        n_scales=NSCALES, session_id="t1",
        load=lambda p: np.array(store[p], copy=True),
    )


def test_a_flattened_image_is_measurably_improved():
    ref = _reference_image()
    flat = flatten(ref, strength=0.6)
    store, load, save = _memory_io()
    session = _session_for(flat, ref, store, save)

    done = run_to_convergence(session, NumpyExecutor(), load=load, save=save)
    result = outcome(done)

    assert result["improved"] is True
    assert result["distance"] < result["baseline_distance"]
    assert len(result["recipe"]["actions"]) >= 1


def test_the_reference_itself_is_declined():
    ref = _reference_image()
    store, load, save = _memory_io()
    session = _session_for(ref, ref, store, save)

    done = run_to_convergence(session, NumpyExecutor(), load=load, save=save)
    result = outcome(done)

    # D is a valley: the image is already at the floor, every move climbs.
    assert result["improved"] is False
    assert result["recipe"]["actions"] == []
    assert result["result_path"] == "/mem/proxy.png"


def test_declining_returns_the_input_untouched():
    ref = _reference_image()
    store, load, save = _memory_io()
    session = _session_for(ref, ref, store, save)
    before = np.array(store["/mem/proxy.png"], copy=True)

    done = run_to_convergence(session, NumpyExecutor(), load=load, save=save)
    assert outcome(done)["improved"] is False
    assert np.array_equal(load(outcome(done)["result_path"]), before)


def test_iteration_cap_terminates_the_loop():
    ref = _reference_image()
    flat = flatten(ref, strength=0.6)
    store, load, save = _memory_io()
    session = _session_for(flat, ref, store, save)
    session.config = type(session.config)(iteration_cap=2)

    done = run_to_convergence(session, NumpyExecutor(), load=load, save=save)
    assert done.iteration <= 2
    assert done.converged


def test_convergence_reason_is_always_reported():
    ref = _reference_image()
    store, load, save = _memory_io()
    session = _session_for(ref, ref, store, save)
    done = run_to_convergence(session, NumpyExecutor(), load=load, save=save)
    assert done.convergence_reason
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_optimize_loop.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'autocontrast.optimize.loop'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/autocontrast/optimize/loop.py
"""The optimization loop (SS6).

The fail-safe is structural, not a special case. Best-so-far is seeded with the
INPUT and its own baseline distance; a candidate replaces it only on a strict
improvement beyond epsilon_improve. Because the fingerprint distance is a VALLEY
-- D=0 at the reference and rising in BOTH directions, including the
over-processed one -- an already-good image starts near the floor, every move
climbs, and the loop declines on its own. A monotone "more contrast is better"
objective could not do that at any amount of guardrailing.
"""

from __future__ import annotations

import numpy as np

from autocontrast.fingerprint.distance import fingerprint_distance
from autocontrast.fingerprint.extract import FingerprintData, extract

from .beam import BeamConfig, Branch, prune
from .executor import Executor
from .guardrails import GuardrailLimits, evaluate_guardrails
from .propose import propose_actions
from .recipe import Recipe
from .session import Session


def _measure(rgb, session: Session) -> FingerprintData:
    return extract(
        rgb, pixel_scale_arcsec=session.pixel_scale_arcsec,
        n_scales=session.n_scales, psf_fwhm_arcsec=session.psf_fwhm_arcsec,
        palette_class=session.palette_class,
    )


def begin(
    *, source_path: str, proxy_path: str, reference_id: str,
    reference_fp: FingerprintData, work_dir: str, pixel_scale_arcsec: float,
    psf_fwhm_arcsec: float, palette_class: str, n_scales: int, session_id: str,
    load, config: BeamConfig | None = None,
) -> Session:
    """Open a session, measuring the input as the baseline AND as best-so-far."""
    config = config or BeamConfig()
    proxy = load(proxy_path)
    baseline_fp = _measure(proxy, _Stub(pixel_scale_arcsec, psf_fwhm_arcsec,
                                        palette_class, n_scales))
    baseline = fingerprint_distance(reference_fp, baseline_fp)

    root = Branch(recipe=Recipe.empty(), image_path=proxy_path, distance=baseline)
    return Session(
        session_id=session_id, work_dir=work_dir, source_path=source_path,
        proxy_path=proxy_path, reference_id=reference_id,
        reference_fp=reference_fp.to_dict(), pixel_scale_arcsec=pixel_scale_arcsec,
        psf_fwhm_arcsec=psf_fwhm_arcsec, palette_class=palette_class,
        n_scales=n_scales, config=config, iteration=0, branches=[root], best=root,
        baseline_distance=baseline, distance_history=[baseline], converged=False,
        convergence_reason="", guardrail_log=[],
    )


class _Stub:
    """Carries just the measurement metadata `_measure` needs before a Session exists."""

    def __init__(self, pixel_scale_arcsec, psf_fwhm_arcsec, palette_class, n_scales):
        self.pixel_scale_arcsec = pixel_scale_arcsec
        self.psf_fwhm_arcsec = psf_fwhm_arcsec
        self.palette_class = palette_class
        self.n_scales = n_scales


def advance(session: Session, executor: Executor, *, load, save,
            limits: GuardrailLimits = GuardrailLimits()) -> Session:
    """One iteration: expand every live branch, guardrail, score, prune."""
    if session.converged:
        return session

    reference_fp = FingerprintData.from_dict(session.reference_fp)
    source = load(session.proxy_path)
    candidates: list[Branch] = []

    for branch in session.branches:
        parent = load(branch.image_path)
        parent_fp = _measure(parent, session)
        actions = propose_actions(
            reference_fp, parent_fp, applied_kinds=branch.recipe.applied_kinds,
            n_scales=session.n_scales, top_k=session.config.top_k,
        )
        for action in actions:
            try:
                produced = executor.apply(
                    parent, action, pixel_scale_arcsec=session.pixel_scale_arcsec
                )
            except Exception as exc:  # a dead candidate, not a dead run (SS12)
                session.guardrail_log.append({
                    "iteration": session.iteration + 1, "action": action.key,
                    "failed": ["executor"], "reason": f"{type(exc).__name__}: {exc}",
                })
                continue

            verdicts = evaluate_guardrails(produced, parent, source, limits)
            failed = [v for v in verdicts if not v.ok]
            if failed:
                # A violation DISCARDS the candidate; it is never a score term (SS7).
                session.guardrail_log.append({
                    "iteration": session.iteration + 1, "action": action.key,
                    "failed": [v.name for v in failed],
                    "reason": "; ".join(v.reason for v in failed),
                })
                continue

            recipe = branch.recipe.extend(action)
            path = f"{session.work_dir}/cand-{session.iteration + 1}-{len(candidates)}.png"
            save(path, produced)
            distance = fingerprint_distance(reference_fp, _measure(produced, session))
            candidates.append(Branch(recipe=recipe, image_path=path, distance=distance))

    session.iteration += 1
    survivors = prune(candidates, session.config.width)

    if not survivors:
        # SS6.3 condition 3: guardrails exhausted the branch set. Reported honestly.
        session.branches = []
        session.converged = True
        session.convergence_reason = (
            "every branch was discarded by guardrails or executor failure"
        )
        return session

    session.branches = survivors
    best_candidate = survivors[0]
    if best_candidate.distance < session.best.distance - session.config.epsilon_improve:
        session.best = best_candidate  # checkpoint (SS6.1)

    session.distance_history.append(session.best.distance)
    _check_convergence(session)
    return session


def _check_convergence(session: Session) -> None:
    """SS6.3, OR of the conditions. Condition 2 (VLM) is absent in Phase 2."""
    cfg = session.config
    if session.iteration >= cfg.iteration_cap:
        session.converged = True
        session.convergence_reason = f"iteration cap ({cfg.iteration_cap}) reached"
        return

    window = session.distance_history[-(cfg.convergence_window + 1):]
    if len(window) > cfg.convergence_window:
        if abs(window[0] - window[-1]) < cfg.epsilon:
            session.converged = True
            session.convergence_reason = (
                f"deltaD below {cfg.epsilon} across the last "
                f"{cfg.convergence_window} iterations"
            )


def run_to_convergence(session: Session, executor: Executor, *, load, save,
                       limits: GuardrailLimits = GuardrailLimits()) -> Session:
    while not session.converged:
        session = advance(session, executor, load=load, save=save, limits=limits)
    return session


def outcome(session: Session) -> dict:
    """The reportable result. ``improved`` False means we DECLINED and the user
    gets their original file back untouched (spec SS3.2)."""
    improved = session.best.distance < (
        session.baseline_distance - session.config.epsilon_improve
    )
    return {
        "improved": improved,
        "baseline_distance": session.baseline_distance,
        "distance": session.best.distance if improved else session.baseline_distance,
        "result_path": session.best.image_path if improved else session.proxy_path,
        "recipe": (session.best.recipe if improved else Recipe.empty()).to_dict(),
        "pixinsight_steps": (
            session.best.recipe if improved else Recipe.empty()
        ).to_pixinsight_steps(),
        "iterations": session.iteration,
        "convergence_reason": session.convergence_reason,
        "reference_id": session.reference_id,
        "guardrail_log": session.guardrail_log,
    }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_optimize_loop.py -v`
Expected: PASS (5 tests)

If `test_a_flattened_image_is_measurably_improved` fails, do **not** lower `epsilon_improve` to force it — re-read spec §3.7. Diagnose whether the proposer is offering actions that address the actual gap first.

- [ ] **Step 5: Commit**

```bash
git add src/autocontrast/optimize/loop.py tests/test_optimize_loop.py
git commit -m "optimize: the loop, and the fail-safe that falls out of the valley

Best-so-far is seeded with the input, so an already-good image sees every
candidate score worse and the loop declines by itself. Declining returns
the original untouched. Guardrail trips discard candidates and are logged
with reasons, so a decline can explain itself (SS12)."
```

---

