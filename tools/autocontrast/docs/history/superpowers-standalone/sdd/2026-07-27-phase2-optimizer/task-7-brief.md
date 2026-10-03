### Task 7: Proposer

**Files:**
- Create: `src/autocontrast/optimize/propose.py`
- Test: `tests/test_optimize_propose.py`

**Interfaces:**
- Consumes: `Action`, `available_actions`; `FingerprintData`; `autocontrast.fingerprint.distance` internals `_spectrum_distance`, `_tonal_distance`, `_chroma_distance`, `_background_distance`; `autocontrast.fingerprint.palette.palette_chroma_compatible`.
- Produces: `component_gaps(ref, target) -> dict[str, float]`, `propose_actions(ref, target, *, applied_kinds, n_scales, top_k) -> list[Action]`.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_optimize_propose.py
import numpy as np

from autocontrast.fingerprint.extract import extract
from autocontrast.optimize.propose import component_gaps, propose_actions


def _fp(rgb, palette="HOO", psf=2.0, scale=1.0):
    return extract(rgb, pixel_scale_arcsec=scale, n_scales=7,
                   psf_fwhm_arcsec=psf, palette_class=palette)


def _textured(h=128, w=128, amp=0.2, seed=3):
    rng = np.random.default_rng(seed)
    base = 0.4 + amp * rng.normal(0, 1, (h, w)).cumsum(axis=0) / h
    base = np.clip(base, 0.05, 0.95)
    return np.stack([base, base * 0.95, base * 1.05], axis=-1)


def _flat(h=128, w=128):
    return np.full((h, w, 3), 0.45)


def test_component_gaps_names_every_distance_component():
    ref, target = _fp(_textured()), _fp(_flat())
    gaps = component_gaps(ref, target)
    assert set(gaps) == {"spectrum", "tonal", "chroma", "background"}
    assert all(v >= 0 for v in gaps.values())


def test_proposals_are_deterministic():
    ref, target = _fp(_textured()), _fp(_flat())
    kwargs = dict(applied_kinds=frozenset(), n_scales=7, top_k=3)
    first = [a.key for a in propose_actions(ref, target, **kwargs)]
    second = [a.key for a in propose_actions(ref, target, **kwargs)]
    assert first == second


def test_top_k_is_respected():
    ref, target = _fp(_textured()), _fp(_flat())
    assert len(propose_actions(ref, target, applied_kinds=frozenset(),
                               n_scales=7, top_k=3)) == 3


def test_chroma_is_never_proposed_on_a_palette_mismatch():
    # L-only matches nothing, not even itself -- the hardest gate case.
    ref = _fp(_textured(), palette="L-only")
    target = _fp(_flat(), palette="HOO")
    proposed = propose_actions(ref, target, applied_kinds=frozenset(),
                               n_scales=7, top_k=8)
    assert not any(a.kind == "chroma" for a in proposed)


def test_a_flat_target_is_offered_structural_help_first():
    ref, target = _fp(_textured()), _fp(_flat())
    proposed = propose_actions(ref, target, applied_kinds=frozenset(),
                               n_scales=7, top_k=3)
    assert any(a.kind in ("local_contrast", "local_equalize") for a in proposed)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_optimize_propose.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'autocontrast.optimize.propose'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/autocontrast/optimize/propose.py
"""Deterministic action ranking (SS6.2).

No AI. The VLM director is Phase 3 and must earn its place there. Ranking works
off the distance function's own decomposition: whichever component carries the
largest gap gets its remedies proposed first.
"""

from __future__ import annotations

from autocontrast.fingerprint.distance import (
    _background_distance, _chroma_distance, _spectrum_distance, _tonal_distance,
)
from autocontrast.fingerprint.palette import palette_chroma_compatible

from .actions import Action, available_actions

# Which action kinds address which fingerprint component.
_REMEDIES = {
    "spectrum": ("local_contrast", "local_equalize", "core_hdr"),
    "tonal": ("tonal_reshape", "black_point"),
    "chroma": ("chroma",),
    "background": ("background_neutralize", "black_point"),
}


def component_gaps(ref, target) -> dict[str, float]:
    """Per-component distance between reference and target."""
    spectrum, usable = _spectrum_distance(ref, target)
    return {
        "spectrum": spectrum if usable else 0.0,
        "tonal": _tonal_distance(ref, target),
        "chroma": _chroma_distance(ref, target),
        "background": _background_distance(ref, target),
    }


def propose_actions(
    ref, target, *, applied_kinds: frozenset[str], n_scales: int, top_k: int
) -> list[Action]:
    """The top_k actions most likely to close the largest gap.

    Chroma actions are withheld entirely when the palette gate is closed (SS2.3);
    the gate is consulted here as well as in the distance function so a
    mismatched reference can never even suggest a color move.
    """
    compatible = palette_chroma_compatible(ref.palette_class, target.palette_class)
    gaps = component_gaps(ref, target)
    if not compatible:
        gaps["chroma"] = 0.0

    menu = available_actions(
        pixel_scale_arcsec=target.pixel_scale_arcsec,
        psf_fwhm_arcsec=max(ref.psf_fwhm_arcsec, target.psf_fwhm_arcsec),
        n_scales=n_scales,
        palette_compatible=compatible,
        applied_kinds=applied_kinds,
    )

    # Rank by the gap the action addresses; ties broken on the action key so the
    # ordering is fully reproducible for a given (ref, target).
    def rank(action: Action) -> tuple[float, str]:
        best = 0.0
        for component, kinds in _REMEDIES.items():
            if action.kind in kinds:
                best = max(best, gaps[component])
        return (-best, action.key)

    return sorted(menu, key=rank)[:top_k]
```

- [ ] **Step 4: Run test to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_optimize_propose.py -v`
Expected: PASS (5 tests)

- [ ] **Step 5: Commit**

```bash
git add src/autocontrast/optimize/propose.py tests/test_optimize_propose.py
git commit -m "optimize: deterministic proposer off the distance decomposition

Ranks actions by which fingerprint component carries the largest gap.
The palette gate is consulted here as well as in the distance function,
so a mismatched reference cannot even suggest a color move (SS2.3)."
```

---

