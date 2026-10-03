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

