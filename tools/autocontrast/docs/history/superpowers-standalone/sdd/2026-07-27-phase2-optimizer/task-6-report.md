# Task 6 report: Executor protocol + NumpyExecutor

## What I implemented

Exactly the three files and one test file specified in `task-6-brief.md`, verbatim (no
deviations from the given code):

- `src/autocontrast/optimize/executor.py` — `Executor` Protocol with
  `apply(rgb, action, *, pixel_scale_arcsec) -> np.ndarray`.
- `src/autocontrast/optimize/executors/__init__.py` — package marker.
- `src/autocontrast/optimize/executors/numpy_exec.py` — `NumpyExecutor`, a cheap numpy
  analogue for each SS6.2 action kind (`local_contrast`, `local_equalize`, `core_hdr`,
  `tonal_reshape`, `black_point`, `chroma`, `background_neutralize`, `star_split` as a
  mode-change no-op), with an unrecognized `kind` raising `ValueError(f"unknown action
  kind {action.kind!r}")`. Every branch funnels through a single `np.clip(out, 0.0, 1.0)`
  on the return path, and `apply()` copies its input (`np.array(rgb, dtype=np.float64,
  copy=True)`) before any mutation.
- `tests/test_optimize_executor.py` — the 10 parametrized/standalone tests from the brief,
  unmodified.

I confirmed before writing that `Action` (`src/autocontrast/optimize/actions.py`) and
`starlet_transform` (`src/autocontrast/fingerprint/starlet.py`) already match the
signatures assumed by the brief, so no adaptation was needed.

## Step 2: failing-test run (before implementation existed)

```
$ .venv/bin/python -m pytest tests/test_optimize_executor.py -v
============================= test session starts ==============================
...
ERROR collecting tests/test_optimize_executor.py
ImportError while importing test module '.../tests/test_optimize_executor.py'.
tests/test_optimize_executor.py:5: in <module>
    from autocontrast.optimize.executors.numpy_exec import NumpyExecutor
E   ModuleNotFoundError: No module named 'autocontrast.optimize.executors'
=========================== short test summary info ============================
ERROR tests/test_optimize_executor.py
!!!!!!!!!!!!!!!!!!!! Interrupted: 1 error during collection !!!!!!!!!!!!!!!!!!!!
=============================== 1 error in 0.09s ===============================
```

Matches the brief's expected failure exactly.

## Step 4: passing-test run (after implementation)

```
$ .venv/bin/python -m pytest tests/test_optimize_executor.py -v
============================= test session starts ==============================
platform linux -- Python 3.14.6, pytest-9.1.1, pluggy-1.6.0
collected 10 items

tests/test_optimize_executor.py::test_every_action_kind_is_executable_and_stays_in_range[local_contrast-moderate-8.0] PASSED [ 10%]
tests/test_optimize_executor.py::test_every_action_kind_is_executable_and_stays_in_range[local_equalize-gentle-8.0] PASSED [ 20%]
tests/test_optimize_executor.py::test_every_action_kind_is_executable_and_stays_in_range[core_hdr-moderate-None] PASSED [ 30%]
tests/test_optimize_executor.py::test_every_action_kind_is_executable_and_stays_in_range[tonal_reshape-strong-None] PASSED [ 40%]
tests/test_optimize_executor.py::test_every_action_kind_is_executable_and_stays_in_range[black_point-gentle-None] PASSED [ 50%]
tests/test_optimize_executor.py::test_every_action_kind_is_executable_and_stays_in_range[chroma-moderate-None] PASSED [ 60%]
tests/test_optimize_executor.py::test_every_action_kind_is_executable_and_stays_in_range[background_neutralize--None] PASSED [ 70%]
tests/test_optimize_executor.py::test_executor_does_not_mutate_its_input PASSED [ 80%]
tests/test_optimize_executor.py::test_stronger_local_contrast_moves_the_image_further PASSED [ 90%]
tests/test_optimize_executor.py::test_unknown_action_kind_is_a_loud_error PASSED [100%]

============================== 10 passed in 0.12s ==============================
```

10/10 pass, as the brief expected.

## Commit

```
415fed3013c5cd4ae9e92c348c91c82870166a6e optimize: Executor seam + numpy stand-in
```

`git status --short` after the commit is clean for these paths (nothing left unstaged
from this task).

## Analysis: `local_contrast` with a large `layer` on a small image

The concern: `local_contrast` calls `starlet_transform(out[..., c], n_scales=layer + 1)`
then boosts `planes[layer]`. What happens for a large `layer` relative to image size
(here, 96x96 test fixtures)?

I benchmarked directly against `starlet_transform` (not through the executor, to isolate
the transform itself) at increasing `layer` values on the 96x96 test fixture:

| layer | à trous kernel width at top scale | wall time | `planes[layer]` std | reconstruction exact? | any NaN? |
|---|---|---|---|---|---|
| 3  | 33      | 0.000s | 2.9e-2  | yes | no |
| 6  | 257     | 0.002s | 1.6e-3  | yes | no |
| 8  | 1025    | 0.008s | 5.2e-6  | yes | no |
| 10 | 4097    | 0.031s | 2.7e-8  | yes | no |
| 12 | 16385   | 0.109s | 1.3e-10 | yes | no |
| 14 | 65537   | 0.441s | 7.2e-14 | yes | no |
| 16 | 262145  | 1.791s | 3.1e-17 | yes | no |

Two separate findings, not one:

1. **Not numerically degenerate.** No NaN/Inf at any layer tested; `planes.sum(axis=0) +
   residual` reconstructs the input exactly at every layer (scipy's `mode="mirror"`
   convolution handles a kernel much wider than the image without producing garbage —
   it just keeps reflecting). But the *magnitude* of `planes[layer]` decays roughly
   geometrically once the à trous kernel width exceeds the image size (each doubling of
   `layer` costs ~2-4 more orders of magnitude of amplitude here). Past layer ~10 on a
   96px image, `planes[layer]` is indistinguishable from floating-point noise around the
   image's own DC component. So `NumpyExecutor.apply()` with a large `layer` is not wrong,
   it is a silent no-op in effect: `planes[layer] *= (1+s)` multiplies numerical noise by
   up to 1.85x, which still clips to "no visible change" — `strong` local_contrast at an
   absurd layer would look identical to leaving the image alone, without ever raising.

2. **Real, separate cost problem: exponential runtime.** The à trous kernel at scale
   `layer` has width `4*2**layer + 1` and `scipy.ndimage.convolve1d` is a direct
   (non-FFT) convolution, so cost scales linearly with kernel width — i.e.
   exponentially with `layer`. Time roughly quadrupled every +2 layers in my
   measurements (0.109s -> 0.441s -> 1.791s for layer 12 -> 14 -> 16). Extrapolating,
   layer ~20 is already tens of seconds and layer ~30 is untenable — my first probe
   script (testing layers up to 30 in one process) hit my own 120s command timeout and
   had to be killed; it produced no output at all because Python fully buffers stdout
   when not attached to a tty, so nothing had flushed before the kill. This is a genuine
   hang risk if a caller (proposer, or a malformed/adversarial `Action`) ever supplies an
   unbounded `layer` — it will not error, it will just take arbitrarily long.

In production, `available_actions()` (`src/autocontrast/optimize/actions.py`) only ever
emits `layer = layer_for_scale(band, pixel_scale_arcsec)` for bands built from
`n_scales`, which is bounded by the image's real fingerprint depth — so the *proposer*
path should never hand the executor a runaway layer. But `NumpyExecutor.apply()` itself
has no such bound; it will happily accept `params={"layer": 30}` from any caller
(hand-constructed `Action`, a future proposer bug, a fuzz/property test) and hang rather
than fail. That is the "silent" failure mode design SS12 is worried about, just
manifesting as silence-via-hang instead of silence-via-wrong-answer. I did not add a
guard — leaving that call per your instruction. My recommendation, for you to rule on: a
bound in the executor (e.g., reject or clamp `layer` once `n_scales` on a given image
would exceed something like `log2(min(H, W))`, matching the same reasoning
`available_actions` already uses for band-limiting) would convert an exponential hang
into a fast, loud `ValueError`, consistent with constraint #3 in the brief (unknown/
invalid actions must fail loudly, not silently do nothing or hang).

## Self-review

- Verified `Action.strength` and `starlet_transform` signatures against the actual
  source before trusting the brief's assumptions — they matched exactly, no adaptation
  needed.
- Confirmed the "stronger moves further" test actually discriminates: `gentle` (strength
  0.25) vs `strong` (strength 0.85) on `local_contrast` boost the same wavelet plane by
  different multipliers, so `|strong - src|.mean() > |gentle - src|.mean()` is a real
  comparison, not a vacuous one — it would fail if the executor ignored `level`.
- Confirmed the mutation test is meaningful: `NumpyExecutor.apply()` starts with
  `np.array(rgb, dtype=np.float64, copy=True)`, so `src` (a float64 array already, from
  the fixture) is never written to in place; the test would fail immediately if that
  `copy=True` were dropped.
- Confirmed the clip-on-every-branch constraint by inspection: every `elif` branch
  either produces `out` unclipped (fine, since the final `return np.clip(out, 0.0, 1.0)`
  runs unconditionally on every branch including `star_split`'s pass-through) or already
  clips internally (`black_point`, `tonal_reshape`) — there is no early `return` anywhere
  in `apply()` that could skip the final clip.
- Did not modify `chroma`'s formula (`gray + (out - gray) * (1.0 + s)`) or attempt to
  "improve" it, per constraint #4.
- Ran the large-layer investigation against `starlet_transform` directly rather than
  guessing from the code, including deliberately reproducing and diagnosing the hang
  (rather than just asserting "it's probably fine") since the brief specifically flagged
  this as worth checking.

## Fix round: guard against unsupportable `local_contrast` layers

Team lead ruled: add the guard (§12 — a silent hang is worse than a silent fallback,
since a fallback at least returns something). Reachability path I hadn't seen: Task 9
serializes recipes (with `Action.params`, including `layer`) to disk for cross-session
resume, and Task 13 replays a stored recipe at a possibly different image geometry — a
recipe persisted under one resolution and reloaded under another is exactly how an
out-of-range `layer` reaches the executor, bypassing `available_actions()`'s
band-limiting entirely. `Action.params` is also `compare=False`, so nothing upstream
would flag a params mismatch either.

### The bound and its justification

Added `_max_supportable_layer(height, width) -> int` in `numpy_exec.py`:

```python
def _max_supportable_layer(height: int, width: int) -> int:
    return int(math.floor(math.log2(min(height, width))))
```

Chose `floor(log2(min(H, W)))`, not the stricter `log2(min(H,W)/4)` implied by the
kernel's exact `4*2**L+1` width, because it matches where my own benchmark data (in the
table above) shows the plane actually goes degenerate for the 96px fixture:
`log2(96) = 6.585 -> floor = 6`. At layer 6 (kernel width 257, ~2.7x the image), the
plane still carries real signal (`std = 1.6e-3`); at layer 8 (kernel 1025, ~10.7x the
image) it's already down to `5.2e-6` — indistinguishable from noise. The chosen bound
sits right at the edge of "still meaningful," which is the more useful place to draw the
line than a bound based purely on kernel geometry that would reject some still-legitimate
layers (e.g. layer 7, kernel 513, roughly 5.3x the image — arguably borderline, but the
`floor(log2(...))` form is simple, matches the standard "how many octaves does an image
of this size support" convention used elsewhere for multiresolution pyramids, and errs
toward rejecting sooner rather than later).

The `local_contrast` branch now checks `layer > max_layer` **before** calling
`starlet_transform`, and raises rather than clamps:

```python
if layer > max_layer:
    raise ValueError(
        f"local_contrast layer {layer} exceeds what a "
        f"{out.shape[0]}x{out.shape[1]} image supports (max layer "
        f"{max_layer}); rejecting rather than hanging on an a trous "
        f"kernel far larger than the image"
    )
```

No clamping, per instruction — clamping would silently execute a different action than
the one the recipe records, breaking the SS12 audit guarantee that replaying a recipe
reproduces its result.

### New tests

```python
def test_local_contrast_rejects_a_layer_beyond_what_the_image_supports():
    # A 96x96 image supports up to layer floor(log2(96)) == 6; 7 is one past the edge.
    ex, src = NumpyExecutor(), _img(h=96, w=96)
    with pytest.raises(ValueError, match="exceeds what a 96x96 image supports"):
        ex.apply(src, Action("local_contrast", "moderate", 8.0, {"layer": 7}),
                 pixel_scale_arcsec=1.0)


def test_local_contrast_accepts_a_layer_at_the_supported_boundary():
    # layer == max_layer is the last legitimate value; it must still execute normally.
    ex, src = NumpyExecutor(), _img(h=96, w=96)
    out = ex.apply(src, Action("local_contrast", "moderate", 8.0, {"layer": 6}),
                   pixel_scale_arcsec=1.0)
    assert out.shape == src.shape
    assert np.all(np.isfinite(out))
    assert not np.array_equal(out, src)
```

Both directions are asserted: the reject test would pass with an implementation that
rejected everything, but the accept test would then fail (it requires the boundary layer
to still execute and actually change the image) — so together they pin down that the
cutoff is exactly at `max_layer`, not somewhere else.

### Command and real output

```
$ .venv/bin/python -m pytest tests/test_optimize_executor.py -v
============================= test session starts ==============================
platform linux -- Python 3.14.6, pytest-9.1.1, pluggy-1.6.0
collected 12 items

tests/test_optimize_executor.py::test_every_action_kind_is_executable_and_stays_in_range[local_contrast-moderate-8.0] PASSED [  8%]
tests/test_optimize_executor.py::test_every_action_kind_is_executable_and_stays_in_range[local_equalize-gentle-8.0] PASSED [ 16%]
tests/test_optimize_executor.py::test_every_action_kind_is_executable_and_stays_in_range[core_hdr-moderate-None] PASSED [ 25%]
tests/test_optimize_executor.py::test_every_action_kind_is_executable_and_stays_in_range[tonal_reshape-strong-None] PASSED [ 33%]
tests/test_optimize_executor.py::test_every_action_kind_is_executable_and_stays_in_range[black_point-gentle-None] PASSED [ 41%]
tests/test_optimize_executor.py::test_every_action_kind_is_executable_and_stays_in_range[chroma-moderate-None] PASSED [ 50%]
tests/test_optimize_executor.py::test_every_action_kind_is_executable_and_stays_in_range[background_neutralize--None] PASSED [ 58%]
tests/test_optimize_executor.py::test_executor_does_not_mutate_its_input PASSED [ 66%]
tests/test_optimize_executor.py::test_stronger_local_contrast_moves_the_image_further PASSED [ 75%]
tests/test_optimize_executor.py::test_unknown_action_kind_is_a_loud_error PASSED [ 83%]
tests/test_optimize_executor.py::test_local_contrast_rejects_a_layer_beyond_what_the_image_supports PASSED [ 91%]
tests/test_optimize_executor.py::test_local_contrast_accepts_a_layer_at_the_supported_boundary PASSED [100%]

============================== 12 passed in 0.15s ==============================
```

12/12 pass in 0.15s total — confirming the guard itself doesn't reintroduce any slowness.

### Confirming the guard fires before `starlet_transform`, not just "fast enough"

Passing `layer=7` (one past the boundary) is cheap to reject either way, since its kernel
isn't yet enormous — that alone wouldn't prove the check happens *before* the expensive
call. So I additionally drove an absurd `layer=40` (deliberately similar to the layer that
made my original diagnostic script hang past 120s) directly through `NumpyExecutor.apply()`:

```
$ timeout 5 .venv/bin/python -c "
... Action('local_contrast', 'strong', 8.0, {'layer': 40}) ...
"
raised in 0.0 s: local_contrast layer 40 exceeds what a 96x96 image supports (max layer 6); rejecting rather than hanging on an a trous kernel far larger than the image
```

0.0s for `layer=40` (which would previously have hung far past the earlier 120s timeout)
confirms the bound is evaluated up front, before any wavelet computation, so the fix
converts the exponential hang into an O(1) rejection regardless of how large the
offending `layer` is.

## Fix-round commit

```
03fca7e6e14a449386cb722ba6a9ab486c8d4da5 optimize/numpy_exec: reject local_contrast layers the image can't support
```

## Fix round 2: give `test_every_action_kind_is_executable_and_stays_in_range` real discriminatory power

Finding (reviewer graded Minor, team lead upgraded to Important): the test asserted only
shape, finiteness, and `[0,1]` range. A branch that silently degraded to `return
np.clip(rgb, 0, 1)` (i.e. did nothing) would pass all three checks for 6 of the 7 action
kinds — only `test_stronger_local_contrast_moves_the_image_further` had any power to
catch a broken branch, and only for `local_contrast`. This mattered beyond this test
file because Task 4's hue-invention guardrail was calibrated over three fix rounds
against the exact `chroma` formula (`gray + (out-gray)*(1.0+s)`); if `chroma` silently
became a no-op, nothing in this suite would fail, and that guardrail calibration would
silently stop being exercised by anything.

### Measured per-kind difference magnitudes

Ran each kind through `NumpyExecutor.apply()` against the shared `_img()` fixture (same
image, same `{"layer": 3, "radius_arcsec": 8.0}` params used by the parametrized test)
and printed `np.abs(out - src).mean()`:

```
local_contrast           mean_abs_diff=1.171180e-02 equal=False
local_equalize           mean_abs_diff=2.035172e-03 equal=False
core_hdr                 mean_abs_diff=2.086464e-01 equal=False
tonal_reshape            mean_abs_diff=1.262359e-01 equal=False
black_point              mean_abs_diff=3.296064e-02 equal=False
chroma                   mean_abs_diff=1.167320e-02 equal=False
background_neutralize    mean_abs_diff=2.333333e-02 equal=False
star_split               mean_abs_diff=0.000000e+00 equal=True
```

Every genuine pixel operation moves the shared fixture by a wide margin (smallest is
`local_equalize` at ~2e-3, still 3+ orders of magnitude above any plausible float-noise
threshold); `star_split` is exactly zero, confirming it's the intended no-op. I did not
need to adjust the fixture — the reviewer's flagged risk (a fixture whose channels
already share a median would make `background_neutralize` a legitimate no-op) does not
apply here: `_img()`'s three channels are `base`, `base*0.9`, `base*1.1`, so their
medians are `0.35`, `0.315`, `0.385` — genuinely different — and `background_neutralize`
moves the image by `2.3e-2`.

### The fix

Restructured `test_every_action_kind_is_executable_and_stays_in_range` to parametrize an
`expect_change` flag per kind (`True` for all 7 genuine pixel ops, `False` for the added
`star_split` case), keeping every existing shape/finite/range assertion and adding:

```python
diff = float(np.abs(out - src).mean())
if expect_change:
    assert diff > 1e-6, f"{kind} did not visibly change the image (mean abs diff {diff:.3e})"
else:
    assert np.array_equal(out, src), f"{kind} is a mode change and must be a pixel no-op"
```

`1e-6` sits four to five orders of magnitude below every measured genuine-op diff above,
so it has essentially zero false-negative risk while still catching a true no-op.

### Command and real output

```
$ .venv/bin/python -m pytest tests/test_optimize_executor.py -v
============================= test session starts ==============================
platform linux -- Python 3.14.6, pytest-9.1.1, pluggy-1.6.0
collected 13 items

tests/test_optimize_executor.py::test_every_action_kind_is_executable_and_stays_in_range[local_contrast-moderate-8.0-True] PASSED [  7%]
tests/test_optimize_executor.py::test_every_action_kind_is_executable_and_stays_in_range[local_equalize-gentle-8.0-True] PASSED [ 15%]
tests/test_optimize_executor.py::test_every_action_kind_is_executable_and_stays_in_range[core_hdr-moderate-None-True] PASSED [ 23%]
tests/test_optimize_executor.py::test_every_action_kind_is_executable_and_stays_in_range[tonal_reshape-strong-None-True] PASSED [ 30%]
tests/test_optimize_executor.py::test_every_action_kind_is_executable_and_stays_in_range[black_point-gentle-None-True] PASSED [ 38%]
tests/test_optimize_executor.py::test_every_action_kind_is_executable_and_stays_in_range[chroma-moderate-None-True] PASSED [ 46%]
tests/test_optimize_executor.py::test_every_action_kind_is_executable_and_stays_in_range[background_neutralize--None-True] PASSED [ 53%]
tests/test_optimize_executor.py::test_every_action_kind_is_executable_and_stays_in_range[star_split--None-False] PASSED [ 61%]
tests/test_optimize_executor.py::test_executor_does_not_mutate_its_input PASSED [ 69%]
tests/test_optimize_executor.py::test_stronger_local_contrast_moves_the_image_further PASSED [ 76%]
tests/test_optimize_executor.py::test_unknown_action_kind_is_a_loud_error PASSED [ 84%]
tests/test_optimize_executor.py::test_local_contrast_rejects_a_layer_beyond_what_the_image_supports PASSED [ 92%]
tests/test_optimize_executor.py::test_local_contrast_accepts_a_layer_at_the_supported_boundary PASSED [100%]

============================== 13 passed in 0.15s ==============================
```

13/13 pass (up from 12; the new `star_split` parametrization case is the extra one).

## Fix round 2 commit

```
18c98b9b0275a2953b9f218d44701382532c9b10 tests/optimize_executor: assert every action kind actually moves pixels
```

Verified: `git log -1` shows this commit as HEAD on `phase2-optimizer`, `git status`
reports a clean working tree.
