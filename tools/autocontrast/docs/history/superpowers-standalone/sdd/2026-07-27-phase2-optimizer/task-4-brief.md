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

