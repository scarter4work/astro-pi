### Task 1: Action space

**Files:**
- Create: `src/autocontrast/optimize/__init__.py`, `src/autocontrast/optimize/actions.py`
- Test: `tests/test_optimize_actions.py`

**Interfaces:**
- Consumes: nothing.
- Produces: `Action` (frozen dataclass: `kind: str`, `level: str`, `scale_arcsec: float | None`, `params: dict`; property `key: str`), `layer_for_scale(scale_arcsec, pixel_scale_arcsec) -> int`, `scale_for_layer(layer, pixel_scale_arcsec) -> float`, `available_actions(*, pixel_scale_arcsec, psf_fwhm_arcsec, n_scales, palette_compatible, applied_kinds) -> list[Action]`, `LEVELS = ("gentle", "moderate", "strong")`.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_optimize_actions.py
import math
import pytest

from autocontrast.optimize.actions import (
    Action, available_actions, layer_for_scale, scale_for_layer,
)


def test_layer_and_scale_are_inverse():
    # energy.py fixes plane i at 2**i * pixel_scale arcsec.
    assert scale_for_layer(3, 1.0) == 8.0
    assert layer_for_scale(8.0, 1.0) == 3


def test_action_key_is_stable_and_distinguishing():
    a = Action(kind="local_contrast", level="moderate", scale_arcsec=8.0, params={})
    b = Action(kind="local_contrast", level="strong", scale_arcsec=8.0, params={})
    assert a.key == a.key
    assert a.key != b.key


def test_no_action_is_offered_below_the_psf_limit():
    # 1.0"/px with a 4.0" PSF: bands at 1" and 2" are unresolvable and must
    # never be proposed (SS2.2 -- chasing them sharpens noise into artifacts).
    actions = available_actions(
        pixel_scale_arcsec=1.0, psf_fwhm_arcsec=4.0, n_scales=7,
        palette_compatible=True, applied_kinds=frozenset(),
    )
    scaled = [a for a in actions if a.scale_arcsec is not None]
    assert scaled, "expected some scale-denominated actions"
    assert all(a.scale_arcsec >= 4.0 for a in scaled)


def test_chroma_actions_are_withheld_when_palette_is_incompatible():
    gated = available_actions(
        pixel_scale_arcsec=1.0, psf_fwhm_arcsec=2.0, n_scales=7,
        palette_compatible=False, applied_kinds=frozenset(),
    )
    assert not any(a.kind == "chroma" for a in gated)

    admitted = available_actions(
        pixel_scale_arcsec=1.0, psf_fwhm_arcsec=2.0, n_scales=7,
        palette_compatible=True, applied_kinds=frozenset(),
    )
    assert any(a.kind == "chroma" for a in admitted)


def test_once_only_actions_are_not_reoffered():
    once = available_actions(
        pixel_scale_arcsec=1.0, psf_fwhm_arcsec=2.0, n_scales=7,
        palette_compatible=True, applied_kinds=frozenset({"star_split", "background_neutralize"}),
    )
    assert not any(a.kind in ("star_split", "background_neutralize") for a in once)


def test_scale_below_pixel_scale_is_rejected_outright():
    with pytest.raises(ValueError):
        layer_for_scale(0.1, 1.0)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_optimize_actions.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'autocontrast.optimize'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/autocontrast/optimize/__init__.py
"""The Phase 2 deterministic optimizer (design SS6, SS7)."""
```

```python
# src/autocontrast/optimize/actions.py
"""The discretized action space (design SS6.2).

No model -- and no heuristic in this package -- may set free-form process
parameters. The search space is a bounded menu; each entry carries 2-3 magnitude
levels. Actions are also constructed BAND-LIMITED: an action whose angular scale
sits below the image's own resolvable limit is never emitted at all (SS2.2, SS4.4).
That makes it structurally impossible to propose sharpening 0.05"/px HST detail
into 1.01"/px backyard data -- the failure SS2.2 exists to prevent.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

LEVELS = ("gentle", "moderate", "strong")

# Actions that change mode rather than degree, and may be applied at most once.
ONCE_ONLY = frozenset({"star_split", "background_neutralize"})

# Magnitude -> a unitless strength the executors interpret per process.
_STRENGTH = {"gentle": 0.25, "moderate": 0.5, "strong": 0.85}


@dataclass(frozen=True)
class Action:
    """One discrete move: what to do, how hard, and at what angular scale."""

    kind: str
    level: str
    scale_arcsec: float | None
    params: dict = field(default_factory=dict, compare=False)

    @property
    def key(self) -> str:
        """Stable identity, used for recipe distinctness in the beam."""
        scale = "-" if self.scale_arcsec is None else f"{self.scale_arcsec:.3f}"
        return f"{self.kind}@{scale}/{self.level or '-'}"

    @property
    def strength(self) -> float:
        return _STRENGTH.get(self.level, 1.0)


def scale_for_layer(layer: int, pixel_scale_arcsec: float) -> float:
    """Angular center of wavelet plane ``layer`` (matches energy.py's convention)."""
    return (2.0**layer) * pixel_scale_arcsec


def layer_for_scale(scale_arcsec: float, pixel_scale_arcsec: float) -> int:
    """Inverse of :func:`scale_for_layer`, rounded to the nearest plane."""
    if scale_arcsec < pixel_scale_arcsec:
        raise ValueError(
            f"scale {scale_arcsec}\" is below the pixel scale {pixel_scale_arcsec}\"/px; "
            "there is no wavelet plane there"
        )
    return int(round(math.log2(scale_arcsec / pixel_scale_arcsec)))


def available_actions(
    *,
    pixel_scale_arcsec: float,
    psf_fwhm_arcsec: float,
    n_scales: int,
    palette_compatible: bool,
    applied_kinds: frozenset[str],
) -> list[Action]:
    """The full menu legal for this image right now.

    ``palette_compatible`` False withholds every chroma action (SS2.3) -- the
    reference's color cloud must never push a palette-mismatched image.
    ``applied_kinds`` withholds once-only actions already spent.
    """
    actions: list[Action] = []

    bands = [
        scale_for_layer(i, pixel_scale_arcsec)
        for i in range(n_scales)
        if scale_for_layer(i, pixel_scale_arcsec) >= psf_fwhm_arcsec
    ]

    for band in bands:
        for level in LEVELS:
            actions.append(Action("local_contrast", level, band,
                                  {"layer": layer_for_scale(band, pixel_scale_arcsec)}))
            actions.append(Action("local_equalize", level, band,
                                  {"radius_arcsec": band}))

    for level in LEVELS:
        actions.append(Action("tonal_reshape", level, None, {"monotone": True}))
        actions.append(Action("black_point", level, None, {"clip_limited": True}))
        actions.append(Action("core_hdr", level, None,
                              {"layers": {"gentle": 2, "moderate": 3, "strong": 4}[level]}))
        if palette_compatible:
            actions.append(Action("chroma", level, None, {}))

    for kind in ("background_neutralize", "star_split"):
        if kind not in applied_kinds:
            actions.append(Action(kind, "", None, {}))

    return [a for a in actions if a.kind not in applied_kinds or a.kind not in ONCE_ONLY]
```

- [ ] **Step 4: Run test to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_optimize_actions.py -v`
Expected: PASS (6 tests)

- [ ] **Step 5: Commit**

```bash
git add src/autocontrast/optimize/__init__.py src/autocontrast/optimize/actions.py tests/test_optimize_actions.py
git commit -m "optimize: band-limited action space refuses unresolvable scales

The SS6.2 menu, with the SS2.2 band limit enforced at construction rather
than caught downstream: an action below the image's PSF limit is never
emitted, so the optimizer cannot propose sharpening detail that is not in
the photons. Chroma actions are withheld entirely when the palette gate
is closed (SS2.3)."
```

---

