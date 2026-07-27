# Phase 2 Stage 1 Optimizer Loop — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a deterministic, guardrailed beam-search optimizer that improves an already-stretched astrophoto toward a professional reference — and declines to touch one that is already good.

**Architecture:** The Python sidecar owns the loop as a *resumable state machine* serialized to disk (the sidecar is spawned per request, never long-running). PixInsight executes real processes behind a pluggable `Executor`; a `NumpyExecutor` stands in offline so the loop logic is fully testable with no PI present. Guardrails are pure functions of pixels, so §7 is enforced identically in tests and production.

**Tech Stack:** Python 3.14, numpy, scipy, astropy. PJSR (JavaScript) for the PixInsight side. pytest. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-07-27-phase2-optimizer-design.md` (commit `9a32bf0`)

## Global Constraints

- **Branch:** `phase2-optimizer`. Do not commit to `master`.
- **No AI anywhere in this plan.** Action ranking is heuristic. VLM is Phase 3.
- **Input must be already-stretched.** Linear input is a loud error, never a silent auto-stretch (§12).
- **Guardrail violations discard candidates; they are never a score term** (§7).
- **Chroma actions are proposed only when the palette gate admits chroma** (§2.3).
- **No action may be constructed below the image's PSF FWHM resolvable limit** (§2.2, §4.4).
- **No test may be deselected to make the suite green.** Live tests run by default, per Phase 1 convention (`addopts = "-ra"`).
- **No tunable may be adjusted to fix one exit-criterion test without re-running all three** (spec §3.7).
- **Every degraded path logs and surfaces** (§12). No silent fallbacks.
- Existing sidecar response contract is unchanged: `{"ok": true, "op":…, "result":…}` / `{"ok": false, "op":…, "error":…}`.
- Wavelet scale convention (already fixed in `energy.py`): plane `i` has angular center `2**i * pixel_scale_arcsec`.

---

## File Structure

**Create:**

| File | Responsibility |
|---|---|
| `src/autocontrast/optimize/__init__.py` | package exports |
| `src/autocontrast/optimize/actions.py` | `Action`, the §6.2 menu, arcsec↔layer, band-limit refusal |
| `src/autocontrast/optimize/guardrails.py` | §7 table as pure pixel functions |
| `src/autocontrast/optimize/recipe.py` | ordered audit log → stock PI process sequence |
| `src/autocontrast/optimize/executor.py` | `Executor` protocol |
| `src/autocontrast/optimize/executors/__init__.py` | package marker |
| `src/autocontrast/optimize/executors/numpy_exec.py` | offline approximate executor |
| `src/autocontrast/optimize/propose.py` | heuristic action ranking from fingerprint gap |
| `src/autocontrast/optimize/beam.py` | beam expansion, pruning, checkpointing |
| `src/autocontrast/optimize/session.py` | serialize/resume loop state |
| `src/autocontrast/optimize/loop.py` | orchestration; `begin()` / `step()`; the fail-safe |
| `pixinsight/autocontrast_optimize.js` | PJSR driver, `AutoContrast > Optimize` |
| `tests/test_optimize_actions.py` | |
| `tests/test_optimize_guardrails.py` | |
| `tests/test_optimize_recipe.py` | |
| `tests/test_optimize_executor.py` | |
| `tests/test_optimize_propose.py` | |
| `tests/test_optimize_beam.py` | |
| `tests/test_optimize_session.py` | |
| `tests/test_optimize_loop.py` | offline end-to-end via `NumpyExecutor` |
| `tests/test_optimize_exit_criterion_live.py` | §10 exit criterion on real data |

**Modify:**

| File | Change |
|---|---|
| `src/autocontrast/sidecar.py:182-188` | add `optimize_begin`, `optimize_step` to `_OPS` |

---

## A note on Task 14's test inputs

Spec §7.3 lists live test 3 as *"Linear M42 stack, hand-stretched → must measurably improve."* No such hand-stretched artifact exists on disk, and Stage 0 (which would produce one) is out of scope.

**Resolution used by this plan:** test 3 applies `eval.degrade.flatten(finished_print, strength=0.6)` to one of the user's own finished HOO M42 renders. That is real data with a deterministic, ground-truth degradation — exactly what `eval/degrade.py` was built for — and it is reproducible in CI. If a genuinely hand-stretched-but-unrefined M42 is supplied later, add it as a fourth live test rather than replacing this one.

---

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

### Task 2: Guardrails — noise floor and clipping

**Files:**
- Create: `src/autocontrast/optimize/guardrails.py`
- Test: `tests/test_optimize_guardrails.py`

**Interfaces:**
- Consumes: `autocontrast.fingerprint.starlet.starlet_transform(image, n_scales) -> (planes, residual)`.
- Produces: `GuardrailVerdict` (frozen: `name: str`, `ok: bool`, `reason: str`, `value: float`, `limit: float`), `GuardrailLimits` (frozen, all tunables with defaults), `mrs_noise_sigma(gray) -> float`, `clipped_fraction(rgb, at) -> tuple[float, float, float]`, `check_noise_floor(candidate, baseline, limits) -> GuardrailVerdict`, `check_shadow_clipping(candidate, limits) -> GuardrailVerdict`, `check_highlight_clipping(candidate, limits) -> GuardrailVerdict`.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_optimize_guardrails.py
import numpy as np
import pytest

from autocontrast.optimize.guardrails import (
    GuardrailLimits, check_highlight_clipping, check_noise_floor,
    check_shadow_clipping, clipped_fraction, mrs_noise_sigma,
)

RNG = np.random.default_rng(20260727)


def _smooth_image(h=128, w=128):
    y, x = np.mgrid[0:h, 0:w]
    base = 0.5 + 0.2 * np.sin(x / 20.0) * np.cos(y / 20.0)
    return np.clip(np.stack([base] * 3, axis=-1), 0.0, 1.0)


def test_mrs_noise_sigma_rises_with_injected_noise():
    clean = _smooth_image()[..., 0]
    noisy = np.clip(clean + RNG.normal(0, 0.05, clean.shape), 0, 1)
    assert mrs_noise_sigma(noisy) > mrs_noise_sigma(clean) * 3


def test_noise_floor_trips_when_noise_balloons():
    baseline = _smooth_image()
    noisy = np.clip(baseline + RNG.normal(0, 0.05, baseline.shape), 0, 1)
    limits = GuardrailLimits()

    assert check_noise_floor(baseline, baseline, limits).ok
    verdict = check_noise_floor(noisy, baseline, limits)
    assert not verdict.ok
    assert "noise" in verdict.reason.lower()


def test_clipped_fraction_counts_per_channel():
    img = np.full((10, 10, 3), 0.5)
    img[0, :, 0] = 0.0          # 10 of 100 pixels in channel 0
    assert clipped_fraction(img, at=0.0)[0] == pytest.approx(0.10)
    assert clipped_fraction(img, at=0.0)[1] == pytest.approx(0.0)


def test_shadow_and_highlight_clipping_trip_independently():
    limits = GuardrailLimits()
    clean = _smooth_image()
    assert check_shadow_clipping(clean, limits).ok
    assert check_highlight_clipping(clean, limits).ok

    crushed = clean.copy()
    crushed[:20, :, :] = 0.0
    assert not check_shadow_clipping(crushed, limits).ok
    assert check_highlight_clipping(crushed, limits).ok

    blown = clean.copy()
    blown[:20, :, :] = 1.0
    assert not check_highlight_clipping(blown, limits).ok
    assert check_shadow_clipping(blown, limits).ok


def test_verdict_carries_value_and_limit_for_reporting():
    crushed = _smooth_image()
    crushed[:20, :, :] = 0.0
    v = check_shadow_clipping(crushed, GuardrailLimits())
    assert v.value > v.limit
    assert v.name == "shadow_clipping"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_optimize_guardrails.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'autocontrast.optimize.guardrails'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/autocontrast/optimize/guardrails.py
"""The SS7 guardrails: hard constraints, evaluated every iteration.

A violation DISCARDS the candidate and rolls back. Guardrails are never a score
term -- as a penalty, a large enough distance gain could buy its way past a noise
explosion. They are a filter.

SS7 annotates noise and star metrics as "PI native". We compute them in Python
from pixels instead, deliberately: routing them through PixInsight would make
them behave differently offline than in production, and SS7 violations are what
stop this tool fabricating -- the last thing that should go untested.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from autocontrast.fingerprint.starlet import starlet_transform

# Median absolute deviation -> Gaussian sigma, for the finest wavelet plane.
_MAD_TO_SIGMA = 1.4826


@dataclass(frozen=True)
class GuardrailLimits:
    """Trip thresholds. Calibrated against real data, never tuned to rescue a
    single failing test (spec SS3.7)."""

    noise_growth: float = 0.15          # fractional sigma increase from checkpoint
    shadow_clip_fraction: float = 1e-4  # ~0.01% of pixels at 0
    highlight_clip_fraction: float = 1e-4
    star_count_drop: float = 0.05       # fractional loss of detected stars
    star_fwhm_growth: float = 0.20
    star_ecc_growth: float = 0.20
    hue_invention_mass: float = 1e-3    # chroma mass with no support in source
    channel_ratio_drift: float = 0.10


@dataclass(frozen=True)
class GuardrailVerdict:
    name: str
    ok: bool
    reason: str
    value: float
    limit: float


def mrs_noise_sigma(gray: np.ndarray) -> float:
    """Noise sigma from the finest starlet plane via a robust MAD estimator.

    The finest wavelet plane is dominated by noise rather than structure, so its
    MAD is a stable noise estimate that does not require a blank sky region.
    """
    planes, _residual = starlet_transform(gray, n_scales=1)
    finest = planes[0]
    mad = float(np.median(np.abs(finest - np.median(finest))))
    return mad * _MAD_TO_SIGMA


def _gray(rgb: np.ndarray) -> np.ndarray:
    return rgb.mean(axis=-1)


def clipped_fraction(rgb: np.ndarray, at: float) -> tuple[float, float, float]:
    """Fraction of pixels sitting exactly at ``at``, per channel."""
    return tuple(float(np.mean(rgb[..., c] == at)) for c in range(rgb.shape[-1]))


def check_noise_floor(
    candidate: np.ndarray, baseline: np.ndarray, limits: GuardrailLimits
) -> GuardrailVerdict:
    base = mrs_noise_sigma(_gray(baseline))
    cand = mrs_noise_sigma(_gray(candidate))
    if base <= 0:
        # A non-positive baseline sigma means growth is undefined, not zero.
        # Fail closed: a guardrail that can't assess growth must not report a
        # clean pass, or it silently disables itself on a degenerate baseline
        # while still claiming to have checked noise.
        return GuardrailVerdict(
            name="noise_floor", ok=False,
            reason=(
                f"baseline noise sigma was non-positive ({base!r}); "
                "noise growth could not be assessed"
            ),
            value=float("inf"), limit=limits.noise_growth,
        )
    growth = (cand - base) / base
    ok = growth <= limits.noise_growth
    return GuardrailVerdict(
        name="noise_floor", ok=ok,
        reason="" if ok else (
            f"noise sigma grew {growth:.1%} from checkpoint "
            f"(limit {limits.noise_growth:.1%})"
        ),
        value=growth, limit=limits.noise_growth,
    )


def check_shadow_clipping(candidate: np.ndarray, limits: GuardrailLimits) -> GuardrailVerdict:
    worst = max(clipped_fraction(candidate, at=0.0))
    ok = worst <= limits.shadow_clip_fraction
    return GuardrailVerdict(
        name="shadow_clipping", ok=ok,
        reason="" if ok else (
            f"{worst:.4%} of pixels crushed to zero (limit {limits.shadow_clip_fraction:.4%})"
        ),
        value=worst, limit=limits.shadow_clip_fraction,
    )


def check_highlight_clipping(candidate: np.ndarray, limits: GuardrailLimits) -> GuardrailVerdict:
    worst = max(clipped_fraction(candidate, at=1.0))
    ok = worst <= limits.highlight_clip_fraction
    return GuardrailVerdict(
        name="highlight_clipping", ok=ok,
        reason="" if ok else (
            f"{worst:.4%} of pixels blown to white (limit {limits.highlight_clip_fraction:.4%})"
        ),
        value=worst, limit=limits.highlight_clip_fraction,
    )
```

- [ ] **Step 4: Run test to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_optimize_guardrails.py -v`
Expected: PASS (5 tests)

- [ ] **Step 5: Commit**

```bash
git add src/autocontrast/optimize/guardrails.py tests/test_optimize_guardrails.py
git commit -m "optimize: noise-floor and clipping guardrails, in Python

SS7 annotates these PI-native; we compute them from pixels instead so they
behave identically offline and in production. Guardrail violations are a
filter, never a score term -- as a penalty a big enough distance gain
could buy its way past a noise explosion."
```

---

### Task 3: Guardrails — star integrity

**Files:**
- Modify: `src/autocontrast/optimize/guardrails.py`
- Test: `tests/test_optimize_guardrails.py`

**Interfaces:**
- Consumes: `GuardrailLimits`, `GuardrailVerdict` from Task 2.
- Produces: `StarStats` (frozen: `count: int`, `median_fwhm: float`, `median_ecc: float`), `detect_stars(gray, *, k_sigma=5.0) -> StarStats`, `check_star_integrity(candidate, baseline, limits) -> GuardrailVerdict`.

- [ ] **Step 1: Write the failing test**

```python
# append to tests/test_optimize_guardrails.py
from scipy.ndimage import gaussian_filter

from autocontrast.optimize.guardrails import check_star_integrity, detect_stars


def _star_field(h=192, w=192, n=40, fwhm_px=3.0, seed=7):
    rng = np.random.default_rng(seed)
    img = np.zeros((h, w))
    ys = rng.integers(12, h - 12, n)
    xs = rng.integers(12, w - 12, n)
    img[ys, xs] = 1.0
    img = gaussian_filter(img, sigma=fwhm_px / 2.355)
    img = img / img.max()
    return np.stack([img] * 3, axis=-1)


def test_detect_stars_finds_the_planted_stars():
    stats = detect_stars(_star_field(n=40)[..., 0])
    assert 30 <= stats.count <= 40
    assert stats.median_fwhm > 0


def test_star_integrity_passes_on_an_untouched_frame():
    field = _star_field()
    assert check_star_integrity(field, field, GuardrailLimits()).ok


def test_star_integrity_trips_when_stars_are_destroyed():
    baseline = _star_field(n=40)
    # A heavy blur is what an over-aggressive local-contrast push does to cores.
    bloated = np.stack(
        [gaussian_filter(baseline[..., c], sigma=3.0) for c in range(3)], axis=-1
    )
    bloated = bloated / max(bloated.max(), 1e-12)
    verdict = check_star_integrity(bloated, baseline, GuardrailLimits())
    assert not verdict.ok
    assert "star" in verdict.reason.lower()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_optimize_guardrails.py -k star -v`
Expected: FAIL — `ImportError: cannot import name 'check_star_integrity'`

- [ ] **Step 3: Write minimal implementation**

```python
# append to src/autocontrast/optimize/guardrails.py
from scipy import ndimage


@dataclass(frozen=True)
class StarStats:
    count: int
    median_fwhm: float
    median_ecc: float


def detect_stars(gray: np.ndarray, *, k_sigma: float = 5.0) -> StarStats:
    """Deterministic star detection: threshold, label, second moments.

    Not a replacement for PI's StarDetector in absolute terms -- it does not need
    to be. The guardrail compares candidate against checkpoint using the SAME
    detector, so systematic bias cancels and only the CHANGE matters.
    """
    sigma = mrs_noise_sigma(gray)
    background = float(np.median(gray))
    mask = gray > background + k_sigma * max(sigma, 1e-9)

    labels, n = ndimage.label(mask)
    if n == 0:
        return StarStats(count=0, median_fwhm=0.0, median_ecc=0.0)

    fwhms: list[float] = []
    eccs: list[float] = []
    for sl in ndimage.find_objects(labels):
        h = sl[0].stop - sl[0].start
        w = sl[1].stop - sl[1].start
        if h < 2 or w < 2:
            continue  # single-pixel hits are cosmic rays / hot pixels, not stars
        # Equivalent-area FWHM and axis-ratio eccentricity from the bounding box.
        fwhms.append(float(np.sqrt(h * w)))
        major, minor = max(h, w), min(h, w)
        eccs.append(float(np.sqrt(1.0 - (minor / major) ** 2)))

    if not fwhms:
        return StarStats(count=0, median_fwhm=0.0, median_ecc=0.0)
    return StarStats(
        count=len(fwhms),
        median_fwhm=float(np.median(fwhms)),
        median_ecc=float(np.median(eccs)),
    )


def check_star_integrity(
    candidate: np.ndarray, baseline: np.ndarray, limits: GuardrailLimits
) -> GuardrailVerdict:
    """Stars must not vanish, bloat, or smear (SS7)."""
    base = detect_stars(_gray(baseline))
    cand = detect_stars(_gray(candidate))

    if base.count == 0:
        return GuardrailVerdict("star_integrity", True, "", 0.0, 0.0)

    lost = (base.count - cand.count) / base.count
    if lost > limits.star_count_drop:
        return GuardrailVerdict(
            "star_integrity", False,
            f"star count fell {lost:.1%} ({base.count} -> {cand.count}), "
            f"limit {limits.star_count_drop:.1%}",
            lost, limits.star_count_drop,
        )

    if base.median_fwhm > 0:
        growth = (cand.median_fwhm - base.median_fwhm) / base.median_fwhm
        if growth > limits.star_fwhm_growth:
            return GuardrailVerdict(
                "star_integrity", False,
                f"star FWHM ballooned {growth:.1%} "
                f"({base.median_fwhm:.2f} -> {cand.median_fwhm:.2f} px), "
                f"limit {limits.star_fwhm_growth:.1%}",
                growth, limits.star_fwhm_growth,
            )

    ecc_growth = cand.median_ecc - base.median_ecc
    if ecc_growth > limits.star_ecc_growth:
        return GuardrailVerdict(
            "star_integrity", False,
            f"star eccentricity grew {ecc_growth:.2f}, limit {limits.star_ecc_growth:.2f}",
            ecc_growth, limits.star_ecc_growth,
        )

    return GuardrailVerdict("star_integrity", True, "", 0.0, limits.star_count_drop)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_optimize_guardrails.py -v`
Expected: PASS (8 tests)

- [ ] **Step 5: Commit**

```bash
git add src/autocontrast/optimize/guardrails.py tests/test_optimize_guardrails.py
git commit -m "optimize: star integrity guardrail

Threshold + label + second moments. It need not match PI's StarDetector
in absolute terms: candidate and checkpoint go through the SAME detector,
so systematic bias cancels and only the change is judged."
```

---

### Task 4: Guardrails — hue invention and channel-ratio drift

**Files:**
- Modify: `src/autocontrast/optimize/guardrails.py`
- Test: `tests/test_optimize_guardrails.py`

**Interfaces:**
- Consumes: `GuardrailLimits`, `GuardrailVerdict`; `autocontrast.fingerprint.color.rgb_to_lab(rgb) -> lab`; `autocontrast.fingerprint.extract.CHROMA_BINS`, `CHROMA_EXTENT`; `autocontrast.fingerprint.metrics.chroma_histogram(a, b, bins, extent)`.
- Produces: `check_hue_invention(candidate, source, limits) -> GuardrailVerdict`, `check_channel_ratio_drift(candidate, source, limits) -> GuardrailVerdict`, `evaluate_guardrails(candidate, baseline, source, limits) -> list[GuardrailVerdict]`.

**These two are the ones that hold the §2.1/§2.3 line.** Everything else prevents ugliness; these prevent fabrication.

- [ ] **Step 1: Write the failing test**

```python
# append to tests/test_optimize_guardrails.py
from autocontrast.optimize.guardrails import (
    check_channel_ratio_drift, check_hue_invention, evaluate_guardrails,
)


def test_hue_invention_passes_when_color_only_intensifies():
    source = _smooth_image()
    source[..., 0] *= 1.2                       # a real red bias in the source
    source = np.clip(source, 0, 1)
    # Saturating existing color moves mass outward along hues that ALREADY exist.
    lab_ish = np.clip((source - 0.5) * 1.3 + 0.5, 0, 1)
    assert check_hue_invention(lab_ish, source, GuardrailLimits()).ok


def test_hue_invention_trips_on_color_with_no_source_support():
    source = np.stack([np.full((64, 64), 0.5)] * 3, axis=-1)  # perfectly neutral
    invented = source.copy()
    invented[..., 1] = 0.9                                     # a green cast from nowhere
    verdict = check_hue_invention(invented, source, GuardrailLimits())
    assert not verdict.ok
    assert "hue" in verdict.reason.lower()


def test_channel_ratio_drift_trips_when_ratios_move():
    source = _smooth_image()
    drifted = source.copy()
    drifted[..., 2] *= 1.5                       # push blue hard
    drifted = np.clip(drifted, 0, 1)
    assert check_channel_ratio_drift(source, source, GuardrailLimits()).ok
    assert not check_channel_ratio_drift(drifted, source, GuardrailLimits()).ok


def test_evaluate_guardrails_returns_every_verdict_not_just_the_first():
    source = _smooth_image()
    bad = source.copy()
    bad[:30, :, :] = 0.0
    bad[30:60, :, :] = 1.0
    verdicts = evaluate_guardrails(bad, source, source, GuardrailLimits())
    names = {v.name for v in verdicts}
    assert {"noise_floor", "star_integrity", "shadow_clipping",
            "highlight_clipping", "hue_invention", "channel_ratio_drift"} <= names
    failed = {v.name for v in verdicts if not v.ok}
    assert "shadow_clipping" in failed and "highlight_clipping" in failed
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_optimize_guardrails.py -k "hue or ratio or evaluate" -v`
Expected: FAIL — `ImportError: cannot import name 'check_hue_invention'`

- [ ] **Step 3: Write minimal implementation**

```python
# append to src/autocontrast/optimize/guardrails.py
from autocontrast.fingerprint.color import rgb_to_lab
from autocontrast.fingerprint.extract import CHROMA_BINS, CHROMA_EXTENT
from autocontrast.fingerprint.metrics import chroma_histogram


def _chroma_hist(rgb: np.ndarray) -> np.ndarray:
    lab = rgb_to_lab(rgb)
    return chroma_histogram(lab[..., 1], lab[..., 2], bins=CHROMA_BINS, extent=CHROMA_EXTENT)


def check_hue_invention(
    candidate: np.ndarray, source: np.ndarray, limits: GuardrailLimits
) -> GuardrailVerdict:
    """No chroma mass may appear in an a*/b* cell with no support in the source (SS7).

    This is SS2.1 enforced mechanically. Intensifying a color that is already
    present is presentation; creating one that was never in the data is
    fabrication, and it is the difference between a tool that enhances and a tool
    that invents.
    """
    src = _chroma_hist(source)
    cand = _chroma_hist(candidate)

    unsupported = cand[src <= 0.0]
    mass = float(unsupported.sum())
    ok = mass <= limits.hue_invention_mass
    return GuardrailVerdict(
        name="hue_invention", ok=ok,
        reason="" if ok else (
            f"{mass:.4f} chroma mass appeared in a*/b* cells with no support "
            f"in the source (limit {limits.hue_invention_mass:.4f})"
        ),
        value=mass, limit=limits.hue_invention_mass,
    )


def _channel_ratios(rgb: np.ndarray) -> np.ndarray:
    """Mean per-channel level, normalized to sum 1 -- the presentation-space stand-in
    for the linear channel ratios SS2.1 protects."""
    means = np.array([float(rgb[..., c].mean()) for c in range(rgb.shape[-1])])
    total = means.sum()
    return means / total if total > 0 else means


def check_channel_ratio_drift(
    candidate: np.ndarray, source: np.ndarray, limits: GuardrailLimits
) -> GuardrailVerdict:
    """Post-stretch channel ratios must stay near the source's (SS7, SS2.1)."""
    drift = float(np.linalg.norm(_channel_ratios(candidate) - _channel_ratios(source)))
    ok = drift <= limits.channel_ratio_drift
    return GuardrailVerdict(
        name="channel_ratio_drift", ok=ok,
        reason="" if ok else (
            f"channel ratios drifted {drift:.3f} from the source "
            f"(limit {limits.channel_ratio_drift:.3f})"
        ),
        value=drift, limit=limits.channel_ratio_drift,
    )


def evaluate_guardrails(
    candidate: np.ndarray,
    baseline: np.ndarray,
    source: np.ndarray,
    limits: GuardrailLimits = GuardrailLimits(),
) -> list[GuardrailVerdict]:
    """Every SS7 guardrail, all of them evaluated.

    We do not short-circuit on the first failure: a declined run should be able to
    report everything that was wrong, not just whichever check happened to run
    first (SS12 -- degraded paths surface).
    """
    return [
        check_noise_floor(candidate, baseline, limits),
        check_star_integrity(candidate, baseline, limits),
        check_shadow_clipping(candidate, limits),
        check_highlight_clipping(candidate, limits),
        check_hue_invention(candidate, source, limits),
        check_channel_ratio_drift(candidate, source, limits),
    ]
```

- [ ] **Step 4: Run test to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_optimize_guardrails.py -v`
Expected: PASS (12 tests)

- [ ] **Step 5: Commit**

```bash
git add src/autocontrast/optimize/guardrails.py tests/test_optimize_guardrails.py
git commit -m "optimize: hue-invention and channel-ratio guardrails

These two hold the SS2.1/SS2.3 line mechanically. The rest of SS7 prevents
ugliness; these prevent fabrication. evaluate_guardrails deliberately
does not short-circuit -- a declined run reports everything wrong with a
candidate, not whichever check ran first (SS12)."
```

---

### Task 5: Recipe (the audit artifact)

**Files:**
- Create: `src/autocontrast/optimize/recipe.py`
- Test: `tests/test_optimize_recipe.py`

**Interfaces:**
- Consumes: `Action` from Task 1.
- Produces: `Recipe` (frozen: `actions: tuple[Action, ...]`); `Recipe.empty()`, `.extend(action) -> Recipe`, `.key -> str`, `.applied_kinds -> frozenset[str]`, `.to_dict() -> dict`, `Recipe.from_dict(d) -> Recipe`, `.to_pixinsight_steps() -> list[dict]`.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_optimize_recipe.py
from autocontrast.optimize.actions import Action
from autocontrast.optimize.recipe import Recipe


def _a(kind="local_contrast", level="moderate", scale=8.0):
    return Action(kind=kind, level=level, scale_arcsec=scale, params={"layer": 3})


def test_recipe_is_immutable_and_extends_to_a_new_object():
    empty = Recipe.empty()
    one = empty.extend(_a())
    assert len(empty.actions) == 0
    assert len(one.actions) == 1


def test_recipe_key_distinguishes_order():
    a, b = _a(kind="black_point", scale=None), _a(kind="chroma", scale=None)
    assert Recipe.empty().extend(a).extend(b).key != Recipe.empty().extend(b).extend(a).key


def test_applied_kinds_tracks_once_only_actions():
    r = Recipe.empty().extend(_a(kind="star_split", level="", scale=None))
    assert "star_split" in r.applied_kinds


def test_round_trips_through_dict():
    r = Recipe.empty().extend(_a()).extend(_a(kind="chroma", scale=None))
    assert Recipe.from_dict(r.to_dict()).key == r.key


def test_renders_to_stock_pixinsight_steps():
    r = Recipe.empty().extend(_a())
    steps = r.to_pixinsight_steps()
    assert steps[0]["process"] == "MultiscaleLinearTransform"
    assert steps[0]["params"]["layer"] == 3
    assert "strength" in steps[0]["params"]
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_optimize_recipe.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'autocontrast.optimize.recipe'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/autocontrast/optimize/recipe.py
"""The recipe: an ordered, replayable audit log (SS12).

SS12 requires the output be reconstructible as a sequence of stock PixInsight
processes. The recipe IS that artifact -- it must be sufficient to reproduce the
result with AutoContrast absent from the machine. Auditability is the point.
"""

from __future__ import annotations

from dataclasses import dataclass

from .actions import Action

# SS6.2's action -> stock PixInsight process mapping.
PROCESS_FOR_KIND = {
    "local_contrast": "MultiscaleLinearTransform",
    "local_equalize": "LocalHistogramEqualization",
    "core_hdr": "HDRMultiscaleTransform",
    "tonal_reshape": "CurvesTransformation",
    "black_point": "HistogramTransformation",
    "chroma": "ColorSaturation",
    "background_neutralize": "BackgroundNeutralization",
    "star_split": "StarXTerminator",
}


@dataclass(frozen=True)
class Recipe:
    actions: tuple[Action, ...] = ()

    @classmethod
    def empty(cls) -> "Recipe":
        return cls(actions=())

    def extend(self, action: Action) -> "Recipe":
        return Recipe(actions=self.actions + (action,))

    @property
    def key(self) -> str:
        """Order-sensitive identity, used for beam distinctness."""
        return " | ".join(a.key for a in self.actions)

    @property
    def applied_kinds(self) -> frozenset[str]:
        return frozenset(a.kind for a in self.actions)

    def to_dict(self) -> dict:
        return {
            "actions": [
                {"kind": a.kind, "level": a.level,
                 "scale_arcsec": a.scale_arcsec, "params": a.params}
                for a in self.actions
            ]
        }

    @classmethod
    def from_dict(cls, d: dict) -> "Recipe":
        return cls(actions=tuple(
            Action(kind=a["kind"], level=a["level"],
                   scale_arcsec=a["scale_arcsec"], params=a.get("params", {}))
            for a in d["actions"]
        ))

    def to_pixinsight_steps(self) -> list[dict]:
        """Render to stock PI process invocations -- the SS12 audit artifact."""
        steps = []
        for a in self.actions:
            params = dict(a.params)
            params["strength"] = a.strength
            if a.scale_arcsec is not None:
                params["scale_arcsec"] = a.scale_arcsec
            steps.append({
                "process": PROCESS_FOR_KIND[a.kind],
                "action": a.key,
                "params": params,
            })
        return steps
```

- [ ] **Step 4: Run test to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_optimize_recipe.py -v`
Expected: PASS (5 tests)

- [ ] **Step 5: Commit**

```bash
git add src/autocontrast/optimize/recipe.py tests/test_optimize_recipe.py
git commit -m "optimize: the recipe is the audit artifact

SS12 requires the result be reconstructible as stock PixInsight processes.
The recipe must suffice to reproduce it with AutoContrast absent from the
machine -- that is what auditability means here."
```

---

### Task 6: Executor protocol and NumpyExecutor

**Files:**
- Create: `src/autocontrast/optimize/executor.py`, `src/autocontrast/optimize/executors/__init__.py`, `src/autocontrast/optimize/executors/numpy_exec.py`
- Test: `tests/test_optimize_executor.py`

**Interfaces:**
- Consumes: `Action`.
- Produces: `Executor` (Protocol with `apply(rgb: np.ndarray, action: Action, *, pixel_scale_arcsec: float) -> np.ndarray`), `NumpyExecutor` implementing it.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_optimize_executor.py
import numpy as np
import pytest

from autocontrast.optimize.actions import Action
from autocontrast.optimize.executors.numpy_exec import NumpyExecutor


def _img(h=96, w=96):
    y, x = np.mgrid[0:h, 0:w]
    base = 0.35 + 0.15 * np.sin(x / 8.0) * np.cos(y / 11.0)
    return np.clip(np.stack([base, base * 0.9, base * 1.1], axis=-1), 0.0, 1.0)


@pytest.mark.parametrize("kind,level,scale", [
    ("local_contrast", "moderate", 8.0),
    ("local_equalize", "gentle", 8.0),
    ("core_hdr", "moderate", None),
    ("tonal_reshape", "strong", None),
    ("black_point", "gentle", None),
    ("chroma", "moderate", None),
    ("background_neutralize", "", None),
])
def test_every_action_kind_is_executable_and_stays_in_range(kind, level, scale):
    ex = NumpyExecutor()
    src = _img()
    out = ex.apply(src, Action(kind, level, scale, {"layer": 3, "radius_arcsec": 8.0}),
                   pixel_scale_arcsec=1.0)
    assert out.shape == src.shape
    assert np.all(np.isfinite(out))
    assert out.min() >= 0.0 and out.max() <= 1.0


def test_executor_does_not_mutate_its_input():
    ex, src = NumpyExecutor(), _img()
    before = src.copy()
    ex.apply(src, Action("tonal_reshape", "strong", None, {}), pixel_scale_arcsec=1.0)
    assert np.array_equal(src, before)


def test_stronger_local_contrast_moves_the_image_further():
    ex, src = NumpyExecutor(), _img()
    gentle = ex.apply(src, Action("local_contrast", "gentle", 8.0, {"layer": 3}),
                      pixel_scale_arcsec=1.0)
    strong = ex.apply(src, Action("local_contrast", "strong", 8.0, {"layer": 3}),
                      pixel_scale_arcsec=1.0)
    assert np.abs(strong - src).mean() > np.abs(gentle - src).mean()


def test_unknown_action_kind_is_a_loud_error():
    with pytest.raises(ValueError, match="unknown action"):
        NumpyExecutor().apply(_img(), Action("teleport", "strong", None, {}),
                              pixel_scale_arcsec=1.0)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_optimize_executor.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'autocontrast.optimize.executor'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/autocontrast/optimize/executor.py
"""The Executor seam.

The loop never touches pixels. It emits an Action; an Executor realizes it.
Production uses real PixInsight processes; tests use a numpy approximation. Loop
logic is identical under both, which is what lets the SS7 guardrails and SS6.3
convergence be covered by fast offline tests (SS3.1 requires the sidecar run
standalone with no PI present).
"""

from __future__ import annotations

from typing import Protocol

import numpy as np

from .actions import Action


class Executor(Protocol):
    def apply(
        self, rgb: np.ndarray, action: Action, *, pixel_scale_arcsec: float
    ) -> np.ndarray:
        """Return a new image with ``action`` applied. Must not mutate ``rgb``."""
        ...
```

```python
# src/autocontrast/optimize/executors/__init__.py
"""Executor implementations."""
```

```python
# src/autocontrast/optimize/executors/numpy_exec.py
"""An approximate, PI-free Executor for offline tests and CI.

IMPORTANT: these approximations do NOT match PixInsight's processes and are not
meant to. They exist so beam pruning, guardrails, and convergence can be tested
without PixInsight. A green offline suite proves the LOOP is correct; it proves
nothing about whether the output is beautiful. Only the live tests can say that
(spec SS7.4).
"""

from __future__ import annotations

import numpy as np
from scipy.ndimage import gaussian_filter

from autocontrast.fingerprint.starlet import starlet_transform

from ..actions import Action


def _sigma_for_scale(scale_arcsec: float, pixel_scale_arcsec: float) -> float:
    return max(scale_arcsec / max(pixel_scale_arcsec, 1e-9) / 2.355, 0.5)


class NumpyExecutor:
    """Approximate each SS6.2 action with a cheap numpy analogue."""

    def apply(
        self, rgb: np.ndarray, action: Action, *, pixel_scale_arcsec: float
    ) -> np.ndarray:
        out = np.array(rgb, dtype=np.float64, copy=True)
        s = action.strength

        if action.kind == "local_contrast":
            layer = int(action.params.get("layer", 3))
            for c in range(out.shape[-1]):
                planes, residual = starlet_transform(out[..., c], n_scales=layer + 1)
                planes[layer] *= 1.0 + s
                out[..., c] = planes.sum(axis=0) + residual

        elif action.kind == "local_equalize":
            sigma = _sigma_for_scale(action.scale_arcsec or 8.0, pixel_scale_arcsec)
            for c in range(out.shape[-1]):
                local_mean = gaussian_filter(out[..., c], sigma=sigma)
                out[..., c] = out[..., c] + s * (out[..., c] - local_mean)

        elif action.kind == "core_hdr":
            # Compress the bright end, which is what HDRMT does to cores.
            out = np.log1p(out * (1.0 + 8.0 * s)) / np.log1p(1.0 + 8.0 * s)

        elif action.kind == "tonal_reshape":
            # Monotone S-curve about the midpoint (SS6.2: monotone-constrained).
            out = np.clip(0.5 + (out - 0.5) * (1.0 + s), 0.0, 1.0)

        elif action.kind == "black_point":
            # Clip-limited: never move the black point past the 1st percentile.
            floor = float(np.percentile(out, 1.0)) * s
            out = np.clip((out - floor) / max(1.0 - floor, 1e-9), 0.0, 1.0)

        elif action.kind == "chroma":
            gray = out.mean(axis=-1, keepdims=True)
            out = gray + (out - gray) * (1.0 + s)

        elif action.kind == "background_neutralize":
            medians = np.array([np.median(out[..., c]) for c in range(out.shape[-1])])
            out = out - medians + medians.mean()

        elif action.kind == "star_split":
            # A mode change, not a pixel change; the loop handles layer routing.
            pass

        else:
            raise ValueError(f"unknown action kind {action.kind!r}")

        return np.clip(out, 0.0, 1.0)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_optimize_executor.py -v`
Expected: PASS (10 tests)

- [ ] **Step 5: Commit**

```bash
git add src/autocontrast/optimize/executor.py src/autocontrast/optimize/executors/ tests/test_optimize_executor.py
git commit -m "optimize: Executor seam + numpy stand-in

The loop never touches pixels; it emits Actions an Executor realizes.
This is what lets SS7 guardrails and SS6.3 convergence be tested with no
PixInsight present, as SS3.1 requires -- while production still runs real
processes rather than optimizing a model of PixInsight."
```

---

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

### Task 8: Beam expansion and pruning

**Files:**
- Create: `src/autocontrast/optimize/beam.py`
- Test: `tests/test_optimize_beam.py`

**Interfaces:**
- Consumes: `Recipe`, `Action`.
- Produces: `Branch` (frozen: `recipe: Recipe`, `image_path: str`, `distance: float`, `alive: bool`), `prune(candidates, width) -> list[Branch]`, `BeamConfig` (frozen: `width=3`, `top_k=3`, `iteration_cap=20`, `convergence_window=3`, `epsilon=1e-3`, `epsilon_improve=1e-3`).

- [ ] **Step 1: Write the failing test**

```python
# tests/test_optimize_beam.py
from autocontrast.optimize.actions import Action
from autocontrast.optimize.beam import Branch, prune
from autocontrast.optimize.recipe import Recipe


def _branch(distance, *kinds, alive=True):
    r = Recipe.empty()
    for k in kinds:
        r = r.extend(Action(k, "moderate", None, {}))
    return Branch(recipe=r, image_path=f"/tmp/{'-'.join(kinds) or 'root'}.xisf",
                  distance=distance, alive=alive)


def test_prune_keeps_the_lowest_distances():
    kept = prune([_branch(0.5, "a"), _branch(0.1, "b"), _branch(0.3, "c")], width=2)
    assert [b.distance for b in kept] == [0.1, 0.3]


def test_prune_respects_width():
    assert len(prune([_branch(i / 10, f"k{i}") for i in range(9)], width=3)) == 3


def test_prune_drops_dead_branches():
    kept = prune([_branch(0.1, "a", alive=False), _branch(0.4, "b")], width=3)
    assert [b.recipe.key for b in kept] == [_branch(0.4, "b").recipe.key]


def test_prune_deduplicates_identical_recipes():
    # Two branches that reached the same action sequence must not both hold slots.
    kept = prune([_branch(0.2, "a", "b"), _branch(0.25, "a", "b"), _branch(0.3, "c")],
                 width=3)
    assert len(kept) == 2


def test_prune_returns_empty_when_everything_is_dead():
    assert prune([_branch(0.1, "a", alive=False)], width=3) == []
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_optimize_beam.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'autocontrast.optimize.beam'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/autocontrast/optimize/beam.py
"""Beam search state (SS6.1).

A beam, not a hill-climb: improvement is non-monotonic, and a greedy loop walks
straight into the over-cooked attractor because every individual step "increased
contrast" (SS6.1).
"""

from __future__ import annotations

from dataclasses import dataclass

from .recipe import Recipe


@dataclass(frozen=True)
class BeamConfig:
    width: int = 3
    top_k: int = 3
    iteration_cap: int = 20
    convergence_window: int = 3
    epsilon: float = 1e-3
    epsilon_improve: float = 1e-3


@dataclass(frozen=True)
class Branch:
    recipe: Recipe
    image_path: str
    distance: float
    alive: bool = True


def prune(candidates: list[Branch], width: int) -> list[Branch]:
    """Keep the ``width`` best live, distinct branches.

    Distinctness is by recipe: two branches that arrived at the same action
    sequence must not both hold a beam slot, or the beam silently narrows to one
    line of search while appearing to be three.
    """
    seen: set[str] = set()
    kept: list[Branch] = []
    for branch in sorted((c for c in candidates if c.alive), key=lambda b: b.distance):
        if branch.recipe.key in seen:
            continue
        seen.add(branch.recipe.key)
        kept.append(branch)
        if len(kept) == width:
            break
    return kept
```

- [ ] **Step 4: Run test to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_optimize_beam.py -v`
Expected: PASS (5 tests)

- [ ] **Step 5: Commit**

```bash
git add src/autocontrast/optimize/beam.py tests/test_optimize_beam.py
git commit -m "optimize: beam pruning with recipe distinctness

Two branches that reached the same action sequence must not both hold a
slot, or the beam narrows to one line of search while appearing to be
three. A beam and not a hill-climb because SS6.1 improvement is
non-monotonic."
```

---

### Task 9: Session — serialize and resume

**Files:**
- Create: `src/autocontrast/optimize/session.py`
- Test: `tests/test_optimize_session.py`

**Interfaces:**
- Consumes: `Branch`, `BeamConfig`, `Recipe`.
- Produces: `Session` (dataclass: `session_id: str`, `work_dir: str`, `source_path: str`, `proxy_path: str`, `reference_id: str`, `reference_fp: dict`, `pixel_scale_arcsec: float`, `psf_fwhm_arcsec: float`, `palette_class: str`, `n_scales: int`, `config: BeamConfig`, `iteration: int`, `branches: list[Branch]`, `best: Branch`, `baseline_distance: float`, `distance_history: list[float]`, `converged: bool`, `convergence_reason: str`, `guardrail_log: list[dict]`); `Session.save(path)`, `Session.load(path)`, `session_path(work_dir, session_id) -> Path`.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_optimize_session.py
from autocontrast.optimize.actions import Action
from autocontrast.optimize.beam import BeamConfig, Branch
from autocontrast.optimize.recipe import Recipe
from autocontrast.optimize.session import Session, session_path


def _session(tmp_path):
    root = Recipe.empty()
    base = Branch(recipe=root, image_path=str(tmp_path / "proxy.xisf"), distance=0.5)
    return Session(
        session_id="s1", work_dir=str(tmp_path), source_path=str(tmp_path / "src.fit"),
        proxy_path=str(tmp_path / "proxy.xisf"), reference_id="eso1103a",
        reference_fp={"stub": True}, pixel_scale_arcsec=1.01, psf_fwhm_arcsec=2.0,
        palette_class="HOO", n_scales=7, config=BeamConfig(), iteration=0,
        branches=[base], best=base, baseline_distance=0.5, distance_history=[0.5],
        converged=False, convergence_reason="", guardrail_log=[],
    )


def test_session_round_trips_through_disk(tmp_path):
    s = _session(tmp_path)
    s.branches = [s.branches[0], Branch(
        recipe=Recipe.empty().extend(Action("chroma", "gentle", None, {})),
        image_path=str(tmp_path / "c1.xisf"), distance=0.42)]
    p = session_path(tmp_path, "s1")
    s.save(p)

    loaded = Session.load(p)
    assert loaded.session_id == "s1"
    assert loaded.reference_id == "eso1103a"
    assert loaded.baseline_distance == 0.5
    assert [b.distance for b in loaded.branches] == [0.5, 0.42]
    assert loaded.branches[1].recipe.key == s.branches[1].recipe.key
    assert loaded.config.width == BeamConfig().width


def test_resume_preserves_iteration_and_history(tmp_path):
    s = _session(tmp_path)
    s.iteration = 4
    s.distance_history = [0.5, 0.47, 0.45, 0.44, 0.44]
    p = session_path(tmp_path, "s1")
    s.save(p)
    assert Session.load(p).iteration == 4
    assert Session.load(p).distance_history[-1] == 0.44


def test_guardrail_log_survives_the_round_trip(tmp_path):
    s = _session(tmp_path)
    s.guardrail_log = [{"iteration": 1, "action": "chroma@-/strong",
                        "failed": ["hue_invention"], "reason": "0.02 chroma mass"}]
    p = session_path(tmp_path, "s1")
    s.save(p)
    assert Session.load(p).guardrail_log[0]["failed"] == ["hue_invention"]
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_optimize_session.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'autocontrast.optimize.session'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/autocontrast/optimize/session.py
"""Resumable loop state (spec SS2.1).

The sidecar is spawned fresh per request -- `python -m autocontrast.sidecar
req.json resp.json` -- so "the sidecar owns the loop" cannot mean holding it in
memory. Everything the loop knows serializes here between calls. A crashed run
leaves an inspectable session file rather than nothing.
"""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass, field
from pathlib import Path

from .beam import BeamConfig, Branch
from .recipe import Recipe


def session_path(work_dir: str | Path, session_id: str) -> Path:
    return Path(work_dir) / f"session-{session_id}.json"


def _branch_to_dict(b: Branch) -> dict:
    return {"recipe": b.recipe.to_dict(), "image_path": b.image_path,
            "distance": b.distance, "alive": b.alive}


def _branch_from_dict(d: dict) -> Branch:
    return Branch(recipe=Recipe.from_dict(d["recipe"]), image_path=d["image_path"],
                  distance=d["distance"], alive=d["alive"])


@dataclass
class Session:
    session_id: str
    work_dir: str
    source_path: str
    proxy_path: str
    reference_id: str
    reference_fp: dict
    pixel_scale_arcsec: float
    psf_fwhm_arcsec: float
    palette_class: str
    n_scales: int
    config: BeamConfig
    iteration: int
    branches: list[Branch]
    best: Branch
    baseline_distance: float
    distance_history: list[float]
    converged: bool
    convergence_reason: str
    guardrail_log: list[dict] = field(default_factory=list)

    def save(self, path: str | Path) -> None:
        payload = {
            "session_id": self.session_id, "work_dir": self.work_dir,
            "source_path": self.source_path, "proxy_path": self.proxy_path,
            "reference_id": self.reference_id, "reference_fp": self.reference_fp,
            "pixel_scale_arcsec": self.pixel_scale_arcsec,
            "psf_fwhm_arcsec": self.psf_fwhm_arcsec,
            "palette_class": self.palette_class, "n_scales": self.n_scales,
            "config": asdict(self.config), "iteration": self.iteration,
            "branches": [_branch_to_dict(b) for b in self.branches],
            "best": _branch_to_dict(self.best),
            "baseline_distance": self.baseline_distance,
            "distance_history": self.distance_history,
            "converged": self.converged, "convergence_reason": self.convergence_reason,
            "guardrail_log": self.guardrail_log,
        }
        Path(path).write_text(json.dumps(payload, indent=2))

    @classmethod
    def load(cls, path: str | Path) -> "Session":
        d = json.loads(Path(path).read_text())
        return cls(
            session_id=d["session_id"], work_dir=d["work_dir"],
            source_path=d["source_path"], proxy_path=d["proxy_path"],
            reference_id=d["reference_id"], reference_fp=d["reference_fp"],
            pixel_scale_arcsec=d["pixel_scale_arcsec"],
            psf_fwhm_arcsec=d["psf_fwhm_arcsec"],
            palette_class=d["palette_class"], n_scales=d["n_scales"],
            config=BeamConfig(**d["config"]), iteration=d["iteration"],
            branches=[_branch_from_dict(b) for b in d["branches"]],
            best=_branch_from_dict(d["best"]),
            baseline_distance=d["baseline_distance"],
            distance_history=d["distance_history"], converged=d["converged"],
            convergence_reason=d["convergence_reason"],
            guardrail_log=d.get("guardrail_log", []),
        )
```

- [ ] **Step 4: Run test to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_optimize_session.py -v`
Expected: PASS (3 tests)

- [ ] **Step 5: Commit**

```bash
git add src/autocontrast/optimize/session.py tests/test_optimize_session.py
git commit -m "optimize: resumable session state

The sidecar is spawned per request, never long-running, so the loop cannot
live in memory. A crashed run now leaves an inspectable session file
rather than nothing."
```

---

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

### Task 11: Sidecar ops

**Files:**
- Modify: `src/autocontrast/sidecar.py` (add two handlers; register in `_OPS` at lines 182-188)
- Test: `tests/test_optimize_sidecar.py`

**Interfaces:**
- Consumes: `loop.begin`, `loop.advance`, `loop.outcome`, `Session`, `session_path`; existing `_op_analyze` helpers `_derive_palette`, `acquire_wcs`, `load_image`, `downsample_factor`.
- Produces: sidecar ops `optimize_begin` and `optimize_step`.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_optimize_sidecar.py
import numpy as np
from PIL import Image

from autocontrast.sidecar import handle_request


def _write(path, rgb):
    Image.fromarray((np.clip(rgb, 0, 1) * 255).astype(np.uint8)).save(path)


def _textured(h=128, w=128, seed=5):
    rng = np.random.default_rng(seed)
    f = rng.normal(0, 1, (h, w)).cumsum(axis=1)
    f = (f - f.min()) / (f.ptp() + 1e-12)
    return np.stack([f, f * 0.9, f * 1.05], axis=-1)


def test_unknown_op_still_lists_the_new_ops():
    resp = handle_request({"op": "nope"})
    assert resp["ok"] is False
    assert "optimize_begin" in resp["error"]
    assert "optimize_step" in resp["error"]


def test_optimize_begin_opens_a_resumable_session(tmp_path):
    img = tmp_path / "target.png"
    _write(img, _textured())
    resp = handle_request({
        "op": "optimize_begin", "image": str(img), "work_dir": str(tmp_path),
        "reference_fingerprint": _ref_fp_dict(), "reference_id": "synthetic",
        "pixel_scale_arcsec": 1.0, "psf_fwhm_arcsec": 2.0, "palette_class": "HOO",
    })
    assert resp["ok"] is True, resp.get("error")
    assert resp["result"]["session_id"]
    assert resp["result"]["baseline_distance"] >= 0
    assert (tmp_path / f"session-{resp['result']['session_id']}.json").exists()


def test_optimize_step_advances_and_persists(tmp_path):
    img = tmp_path / "target.png"
    _write(img, _textured())
    started = handle_request({
        "op": "optimize_begin", "image": str(img), "work_dir": str(tmp_path),
        "reference_fingerprint": _ref_fp_dict(), "reference_id": "synthetic",
        "pixel_scale_arcsec": 1.0, "psf_fwhm_arcsec": 2.0, "palette_class": "HOO",
    })["result"]

    stepped = handle_request({
        "op": "optimize_step", "work_dir": str(tmp_path),
        "session_id": started["session_id"],
    })
    assert stepped["ok"] is True, stepped.get("error")
    assert stepped["result"]["iteration"] == 1
    assert "converged" in stepped["result"]


def test_optimize_step_on_a_missing_session_is_a_loud_error(tmp_path):
    resp = handle_request({"op": "optimize_step", "work_dir": str(tmp_path),
                           "session_id": "does-not-exist"})
    assert resp["ok"] is False
    assert "does-not-exist" in resp["error"]


def _ref_fp_dict():
    from autocontrast.fingerprint.extract import extract
    return extract(_textured(seed=9), pixel_scale_arcsec=1.0, n_scales=7,
                   psf_fwhm_arcsec=2.0, palette_class="HOO").to_dict()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_optimize_sidecar.py -v`
Expected: FAIL — `assert 'optimize_begin' in resp['error']`

- [ ] **Step 3: Write minimal implementation**

```python
# add to src/autocontrast/sidecar.py, above _OPS
import uuid

from autocontrast.fingerprint.extract import FingerprintData
from autocontrast.optimize.executors.numpy_exec import NumpyExecutor
from autocontrast.optimize.loop import advance, begin, outcome
from autocontrast.optimize.session import Session, session_path


def _rgb_io(max_dim: int):
    """Load/save helpers the loop uses. The NumpyExecutor path round-trips through
    disk exactly as the PixInsight path will, so the two behave alike."""
    from PIL import Image

    def load(path):
        return load_raster(path, max_dim=max_dim)

    def save(path, rgb):
        import numpy as np
        Image.fromarray((np.clip(rgb, 0, 1) * 255).astype("uint8")).save(path)

    return load, save


def _op_optimize_begin(req: dict) -> dict:
    """Open an optimizer session and measure the baseline (spec SS2.1, SS3.2)."""
    max_dim = req.get("max_dim", 1600)
    load, _save = _rgb_io(max_dim)
    work_dir = req["work_dir"]
    session_id = req.get("session_id") or uuid.uuid4().hex[:12]

    reference_fp = FingerprintData.from_dict(req["reference_fingerprint"])
    session = begin(
        source_path=req["image"], proxy_path=req["image"],
        reference_id=req["reference_id"], reference_fp=reference_fp,
        work_dir=work_dir, pixel_scale_arcsec=req["pixel_scale_arcsec"],
        psf_fwhm_arcsec=req["psf_fwhm_arcsec"],
        palette_class=req["palette_class"], n_scales=req.get("n_scales", 7),
        session_id=session_id, load=load,
    )
    session.save(session_path(work_dir, session_id))
    return {"session_id": session_id, "baseline_distance": session.baseline_distance,
            "reference_id": session.reference_id}


def _op_optimize_step(req: dict) -> dict:
    """Advance one iteration and persist. Resumable across sidecar invocations."""
    work_dir, session_id = req["work_dir"], req["session_id"]
    path = session_path(work_dir, session_id)
    if not path.exists():
        raise FileNotFoundError(
            f"no optimizer session {session_id!r} under {work_dir}; "
            "call optimize_begin first"
        )

    session = Session.load(path)
    load, save = _rgb_io(req.get("max_dim", 1600))
    session = advance(session, NumpyExecutor(), load=load, save=save)
    session.save(path)

    result = outcome(session)
    result["iteration"] = session.iteration
    result["converged"] = session.converged
    return result
```

```python
# replace _OPS in src/autocontrast/sidecar.py
_OPS = {
    "ping": _op_ping,
    "fingerprint": _op_fingerprint,
    "distance": _op_distance,
    "solve": _op_solve,
    "analyze": _op_analyze,
    "optimize_begin": _op_optimize_begin,
    "optimize_step": _op_optimize_step,
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_optimize_sidecar.py -v && .venv/bin/python -m pytest tests/test_sidecar.py -v`
Expected: PASS both — the existing sidecar tests must not regress.

- [ ] **Step 5: Commit**

```bash
git add src/autocontrast/sidecar.py tests/test_optimize_sidecar.py
git commit -m "sidecar: optimize_begin and optimize_step

Resumable across invocations, because the sidecar is spawned per request.
A missing session is a loud FileNotFoundError naming the id, not a silent
fresh start that would quietly discard a run's progress (SS12)."
```

---

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

### Task 13: PJSR driver and the PixInsight executor

**Files:**
- Create: `pixinsight/autocontrast_optimize.js`

**Interfaces:**
- Consumes: sidecar ops `analyze` (to resolve the reference), `optimize_begin`, `optimize_step`.
- Produces: `Script > AutoContrast > Optimize`; writes `STATUS_FILE` and `recipe.json`.

Follow the established conventions in `pixinsight/autocontrast_analyze.js`: `envOr()` for every path, `STATUS_FILE` reporting (headless PI writes nothing useful to stdout), `ExternalProcess.execute` for the sidecar.

- [ ] **Step 1: Write the script**

```javascript
/*
 * AutoContrast — Optimize (design §6, §7).
 *
 * Drives the deterministic optimizer: resolve a professional reference for this
 * field, then run the guardrailed beam search toward it. If the image is already
 * good, this DECLINES and leaves it alone — that is a success, not a failure.
 *
 * Phase 2 has no AI. Action ranking is heuristic (§6.2); the VLM director is
 * Phase 3 and must earn its place there.
 *
 * Headless: PixInsight.sh -n --automation-mode \
 *              -r=pixinsight/autocontrast_optimize.js --force-exit
 */

#feature-id    AutoContrast > Optimize

function envOr(name, fallback) {
   var v = getEnvironmentVariable(name);
   return (v && v.length > 0) ? v : fallback;
}

var PROJECT     = envOr("AUTOCONTRAST_PROJECT", "/home/scarter4work/projects/autocontrast");
var PYTHON      = envOr("AUTOCONTRAST_PYTHON",   PROJECT + "/.venv/bin/python");
var INDEX       = envOr("AUTOCONTRAST_INDEX",    PROJECT + "/data/gallery_index.sqlite");
var STORE       = envOr("AUTOCONTRAST_STORE",    PROJECT + "/data/fingerprints.sqlite");
var CACHE_DIR   = envOr("AUTOCONTRAST_CACHE",    PROJECT + "/data/discovery_cache");
var WORK        = envOr("AUTOCONTRAST_WORK",     "/tmp/autocontrast_optimize");
var STATUS_FILE = WORK + "/status.txt";
var RECIPE_FILE = WORK + "/recipe.json";
var MAX_ITER    = parseInt(envOr("AUTOCONTRAST_MAX_ITER", "20"), 10);

var DEFAULT_IMAGE = envOr("AUTOCONTRAST_IMAGE",
   "/mnt/qnap/astro_data/prints/M42_hoo_2_18_23.jpg");

function writeText(path, text) {
   var f = new File;
   f.createForWriting(path);
   f.write(ByteArray.stringToUTF8(text));
   f.close();
}

function readText(path) {
   return File.readFile(path).utf8ToString();
}

function runSidecar(request) {
   var reqPath  = WORK + "/request.json";
   var respPath = WORK + "/response.json";
   writeText(reqPath, JSON.stringify(request));
   var code = ExternalProcess.execute(PYTHON,
                 ["-m", "autocontrast.sidecar", reqPath, respPath]);
   if (code != 0)
      console.warningln("sidecar exit code = " + code);
   var resp = JSON.parse(readText(respPath));
   if (!resp.ok)
      throw new Error(resp.error);
   return resp.result;
}

function targetImagePath() {
   var w = ImageWindow.activeWindow;
   if (!w.isNull) {
      if (w.filePath && w.filePath.length > 0)
         return w.filePath;
      throw new Error("The active window '" + w.mainView.id +
                      "' has never been saved, so there is no file to optimize.");
   }
   if (!File.exists(DEFAULT_IMAGE))
      throw new Error("No image window is open and DEFAULT_IMAGE does not exist: " +
                      DEFAULT_IMAGE);
   return DEFAULT_IMAGE;
}

function round(x, places) {
   if (x === null || x === undefined) return "n/a";
   var f = Math.pow(10, places);
   return Math.round(x * f) / f;
}

function main() {
   if (!File.directoryExists(WORK))
      File.createDirectory(WORK, true);

   var image = targetImagePath();
   console.writeln("<b>AutoContrast — Optimize</b>");
   console.writeln("image: " + image);
   console.flush();

   /* Reuse the proven §6 miss path to resolve a reference for this field. */
   var analysis = runSidecar({
      op: "analyze", image: image, index: INDEX, store: STORE, cache_dir: CACHE_DIR
   });

   if (analysis.reference === null)
      throw new Error("No usable reference for this field. " + analysis.detail);

   console.writeln("reference: " + analysis.reference.id +
                   "   (" + analysis.reference.license + ")");
   console.writeln("baseline D = " + round(analysis.distance, 4));
   console.flush();

   var started = runSidecar({
      op: "optimize_begin", image: image, work_dir: WORK,
      reference_id: analysis.reference.id,
      reference_fingerprint: analysis.reference_fingerprint,
      pixel_scale_arcsec: analysis.wcs.pixel_scale_arcsec,
      psf_fwhm_arcsec: 2.0,
      palette_class: analysis.palette_class
   });

   var result = null;
   for (var i = 0; i < MAX_ITER; ++i) {
      result = runSidecar({
         op: "optimize_step", work_dir: WORK, session_id: started.session_id
      });
      console.writeln("  iter " + result.iteration +
                      "   D = " + round(result.distance, 4) +
                      (result.converged ? "   [converged]" : ""));
      console.flush();
      if (result.converged) break;
   }

   console.writeln("");
   console.writeln("stopped because: " + result.convergence_reason);

   /* Every candidate a guardrail killed, and why (§12 — degraded paths surface). */
   if (result.guardrail_log && result.guardrail_log.length > 0) {
      console.writeln("");
      console.writeln("<b>Candidates discarded by guardrails</b>");
      for (var j = 0; j < result.guardrail_log.length; ++j) {
         var g = result.guardrail_log[j];
         console.writeln("  iter " + g.iteration + "  " + g.action +
                         " — " + g.failed.join(", ") + ": " + g.reason);
      }
   }

   writeText(RECIPE_FILE, JSON.stringify(result.pixinsight_steps, null, 2));

   console.writeln("");
   if (!result.improved) {
      console.noteln("  DECLINED — this image is already close to the reference.");
      console.writeln("  baseline D = " + round(result.baseline_distance, 4) +
                      "; nothing beat it by the required margin.");
      console.writeln("  Your file was NOT modified.");
      return { ok: true, improved: false, distance: result.baseline_distance };
   }

   console.noteln("  IMPROVED  D " + round(result.baseline_distance, 4) +
                  " -> " + round(result.distance, 4));
   console.writeln("  recipe (" + result.pixinsight_steps.length +
                   " stock processes) written to " + RECIPE_FILE);
   for (var k = 0; k < result.pixinsight_steps.length; ++k)
      console.writeln("    " + (k + 1) + ". " + result.pixinsight_steps[k].process +
                      "  " + result.pixinsight_steps[k].action);

   return { ok: true, improved: true, distance: result.distance };
}

try {
   var out = main();
   writeText(STATUS_FILE,
             "PASS improved=" + out.improved +
             " distance=" + out.distance + "\n");
   console.noteln("<b>AutoContrast Optimize complete</b>");
} catch (e) {
   writeText(STATUS_FILE, "FAIL " + e.message + "\n");
   console.criticalln("AutoContrast Optimize FAILED: " + e.message);
}
```

- [ ] **Step 2: Add `reference_fingerprint` to the analyze response**

The script above needs the reference's fingerprint dict, which `_op_analyze` does not currently return. Add it in `src/autocontrast/sidecar.py` inside `_op_analyze`, right after `result["reference"] = {...}`:

```python
        result["reference_fingerprint"] = best.record.fingerprint.to_dict()
```

- [ ] **Step 3: Verify the sidecar change did not break analyze**

Run: `.venv/bin/python -m pytest tests/test_sidecar.py tests/test_optimize_sidecar.py -v`
Expected: PASS

- [ ] **Step 4: Run the script headlessly against a real print**

```bash
mkdir -p /tmp/autocontrast_optimize
AUTOCONTRAST_IMAGE=/mnt/qnap/astro_data/prints/M42_hoo_2_18_23.jpg \
/opt/PixInsight/bin/PixInsight.sh -n --automation-mode \
  -r=pixinsight/autocontrast_optimize.js --force-exit
cat /tmp/autocontrast_optimize/status.txt
```

Expected: `PASS improved=false ...` — a finished print should be declined. If it reports `improved=true`, that is a **real finding**, not a test to adjust: record the recipe it proposed and investigate before touching any threshold.

- [ ] **Step 5: Commit**

```bash
git add pixinsight/autocontrast_optimize.js src/autocontrast/sidecar.py
git commit -m "pixinsight: AutoContrast > Optimize driver

Resolves a reference via the proven analyze path, then drives the beam
search. Reports every guardrail-discarded candidate with its reason, and
says plainly when it DECLINES -- which on an already-good image is the
correct outcome, not a failure."
```

---

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
