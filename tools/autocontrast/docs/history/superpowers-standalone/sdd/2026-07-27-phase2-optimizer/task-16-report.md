# Task 16 — Fingerprint extraction cost, and the spec's budget reasoning

Branch `phase2-optimizer`. Baseline suite before any change: **413 passed, nothing
deselected, no warnings** (140.57s).

---

## Step 1 — MEASURE FIRST

Profiled on a real cached render, `data/discovery_cache/eso0104a.jpg`, loaded through
the production loader at the real search-proxy size: `load_image(path, max_dim=1600)`
→ `(1600, 1575, 3)` float64. Fingerprint parameters as the ingest layer uses them:
`n_scales=7`, `pixel_scale_arcsec=0.5`, `psf_fwhm_arcsec=2.0`, `palette_class="RGB"`.
`cProfile`, 3 runs, times below are per call.

### `fingerprint.extract` — 1.505 s

| component | calls / extract | time | share |
|---|---|---|---|
| **`starlet_transform`** | **2** | **1.092 s** | **72.6 %** |
| ↳ via `energy_spectrum` | 1 | 0.576 s | 38.3 % |
| ↳ via `structure_to_gradient_ratio` | 1 | 0.574 s | 38.1 % |
| `rgb_to_lab` | 1 | 0.161 s | 10.7 % |
| `chroma_histogram` | 1 | 0.070 s | 4.7 % |
| `np.percentile` × 5 (tonal + saturation + floor/peak) | — | 0.060 s | 4.0 % |
| `background_channel_balance` | 1 | 0.038 s | 2.5 % |
| everything else | — | 0.084 s | 5.6 % |

Inside `starlet_transform` the cost is essentially all
`scipy.ndimage._nd_image.correlate1d`: 1.022 s of the 1.092 s (14 calls per transform
— 7 scales × 2 separable axes).

### `evaluate_guardrails` — 0.903 s

| component | calls / evaluation | time |
|---|---|---|
| `check_hue_invention` | 1 | 0.518 s |
| ↳ `_hue_mass_by_bin` (→ `rgb_to_lab`) | 2 | 0.512 s (of which `rgb_to_lab` 0.329 s) |
| `check_star_integrity` (→ `detect_stars`) | 1 | 0.202 s |
| `check_noise_floor` | 1 | 0.165 s |
| `mrs_noise_sigma` (total, across both above) | **4** | 0.229 s |
| `_gray` (total) | **4** | 0.093 s |
| clipping + channel-ratio checks | 2 | ~0.02 s |

---

## The two hypotheses, adjudicated

### (b) "Two starlet transforms of the same image" — **REFUTED**

They are not the same decomposition, and sharing one would have been a correctness
bug, exactly as the brief warned:

| | fingerprint (`energy_spectrum`) | guardrail (`mrs_noise_sigma`) |
|---|---|---|
| input | CIELAB **L\***, range `[0, 100]` | **RGB channel mean**, range `[0, 1]` |
| `n_scales` | 7 | 1 |
| finest plane identical? | — | **No** (verified with `np.array_equal`) |

The two inputs are different arrays in different units. Verified directly rather than
argued. The guardrail transform is also not where the guardrail cost lives — at
`n_scales=1` it is 0.064 s of the 0.903 s; the guardrail's real driver is
`rgb_to_lab` under `check_hue_invention` (0.329 s).

### (c) A third redundancy — **CONFIRMED, and it is the actual cost driver**

Not in the brief's list; found by profiling. `extract` computes the **same** starlet
transform **twice, on the same array, with the same `n_scales`**:

- `extract.py:109` → `energy_spectrum(L, ..., n_scales=n_scales)` → `starlet_transform(L, 7)`
- `extract.py:115` → `structure_to_gradient_ratio(L, n_scales=n_scales)` → `starlet_transform(L, 7)`

Same `L`, same `n_scales`, same deterministic function. `energy_spectrum` uses only
`planes`; `structure_to_gradient_ratio` uses `planes` and `residual`. One
decomposition provides both. **0.574 s of the 1.505 s — 38 % of `extract` — is a
verbatim recomputation.**

### (a) "Redundant parent re-measure" — **CONFIRMED**

`advance()` re-fingerprints every live branch's parent every iteration
(`loop.py:330`), and the §2.5 batched path re-fingerprints it *once per batch*
(`loop.py:496`) — so on an iteration that needs supplementary batches the same parent
is measured three or more times, in three separate sidecar processes. Both parents
were already fingerprinted as candidates, at `loop.py:254`, to score them.

---

## Step 2 — What was changed

### Fix 1: one starlet transform inside `extract`, not two

`energy.py` and `metrics.py` each gained a `*_from_planes` / `*_from_transform` entry
point that consumes an already-computed decomposition; the existing single-image
functions now delegate to them and are otherwise untouched. `extract` computes
`starlet_transform(L, n_scales)` **once** and feeds both components.

Nothing about the metric, the scale convention (`plane i → 2**i * pixel_scale_arcsec`),
the band-limiting, the palette gate or the distance function changed. The two
components consume the identical arrays they would have built for themselves.

### Fix 2: the parent fingerprint is carried on the `Branch`

`beam.Branch` gained `fingerprint: FingerprintData | None = field(default=None,
compare=False)`. It is populated where the measurement already happens —
`ingest_candidate` (every candidate) and `begin` (the root's baseline) — and consumed
through one new helper, `loop._branch_fingerprint`, which **both** entry points call:
`advance` (line ~360) and `_plan_batch` (line ~505). `session.py` serializes it.

- `compare=False` keeps numpy arrays out of the frozen dataclass's generated `__eq__`
  and `__hash__`, where they would raise rather than compare.
- `_branch_from_dict` uses `.get("fingerprint")`, so a session file written before the
  field existed still loads and simply re-measures.
- The `None` path is a **cache miss, not a degraded path**: `extract` is deterministic
  in the pixels at `image_path`, so the fallback yields the identical value and only
  the cost differs. It is therefore documented rather than logged — §12 surfaces
  degraded *results*, and there is no result difference to surface. Flagging for the
  reviewer in case that reading is disputed.
- **Session record widening**: ~300 numbers per branch (the same §4.1 stats block
  `reference_fp` already stores whole). Measured worth: it removes one full extraction
  per branch per *batch*, and the §2.5 path plans a batch in a freshly spawned process,
  so there is nowhere else the value could live — an in-memory cache would be discarded
  before the next process could use it.

---

## The equivalence proof — `tests/test_fingerprint_cost.py` (9 tests, all passing)

Deliberately **not** a golden file. A recorded constant proves only that today's code
agrees with the day it was recorded. Instead the tests recompute the *pre-optimization
composition* from the single-image entry points that still exist and still do their own
starlet transform, and compare it against what `extract` now returns from one shared
transform. Both sides are live code.

**Exact equality throughout — `np.testing.assert_array_equal`, never `allclose`.** No
tolerance was chosen anywhere; §3.7 makes that the user's call, and it was not needed.

| test | what it proves |
|---|---|
| `test_extract_is_bit_identical_to_the_pre_optimization_composition[×4]` | Bit-exact on the 4 largest **real cached renders** under `data/discovery_cache/`, loaded through the production loader at the real proxy size (`max_dim=1600`), component by component. |
| `test_equivalence_holds_across_the_scale_depths_the_loop_uses` | Same, at `n_scales ∈ {1, 3, 5, 7, 8}` — a sharing bug that mismatched depth shows up here. |
| `test_fingerprint_survives_the_session_json_round_trip_exactly` | Through real `json.dumps`/`loads` text, not just `to_dict`/`from_dict` — the session file is what sits between two sidecar processes, and the value feeds `propose_actions`'s hard magnitude buckets. |
| `test_carrying_the_parent_fingerprint_does_not_change_the_offline_run` | The loop run twice — once normally, once with every branch's fingerprint stripped each iteration (the pre-Task-16 re-measure, which is also the backward-compatible `None` path). Asserts the **whole `outcome()` dict** is equal: recipe, distances, result path, convergence reason and §12 guardrail log. |
| `test_carrying_the_parent_fingerprint_does_not_change_the_batched_run` | The same, driving the §2.5 batched protocol — the path where the saving is largest, so it gets its own proof rather than inheriting the offline one's. |
| `test_the_beam_actually_carries_a_fingerprint_forward` | Guards the *saving*, which the equivalence tests cannot: they would pass identically (and identically slowly) if the cache stopped being populated. Counts `_measure` calls with and without the cache and pins the difference to **exactly one per live branch**. |

### An independent end-to-end check

Three real iterations at the 1600px proxy, run against the pre-change code (`git stash`)
and the post-change code, same fixture and seed. **The best-so-far distance is identical
at every iteration** — `0.1177`, `0.1058`, `0.0866` — while the time dropped:

| | before | after | |
|---|---|---|---|
| `begin` | 1.58s | 0.98s | −38% |
| iteration 1 | 10.16s | 6.99s | −31% (extractions 4 → 3) |
| iteration 2 | 31.27s | 21.49s | −31% (extractions 12 → 9) |
| iteration 3 | 33.83s | 24.73s | −27% (extractions 11 → 8) |
| **3 iterations** | **75.26s** | **53.21s** | **−29%** |

The extraction counts confirm the mechanism exactly: three fewer per iteration, one per
live branch (`width=3`).

---

## Before / after timings

Unit costs on one fixed image and action so the two columns are directly comparable
(`eso0104a.jpg` flattened at `max_dim=1600` → 1600×1575, `n_scales=7`; executor is the
mean over the whole 16-entry ranked menu):

| | before | after | change |
|---|---|---|---|
| **`fingerprint.extract`** | **1.497s** | **0.919s** | **−38.6%** |
| §7 guardrails | 0.855s | 0.857s | untouched |
| executor (NumpyExecutor) | 0.367s | 0.360s | untouched |
| **candidate SCORED** | **2.719s** | **2.136s** | **−21.4%** |
| candidate DISCARDED by a guardrail | 1.222s | 1.217s | unchanged |
| parent re-measure, per branch per iteration | 1.497s | **0s** | eliminated |

---

## Step 3 — The spec fix

`docs/superpowers/specs/2026-07-27-phase2-optimizer-design.md` §3.3 and the mirrored
rationale at `beam.py` are rewritten. Re-measuring found **three of the four inputs to
the old argument were wrong**, in both directions:

1. **The menu does not hold 50 actions.** Measured across the configurations the loop
   runs (`pixel_scale` 0.25–1.5″/px × `n_scales` 5–9), it holds **14–22**. Actions are
   constructed band-limited (§2.2/§4.4), so the menu is bounded by the number of
   resolvable wavelet planes, not by the catalog of action kinds. This makes the
   *unbounded* arm far cheaper than claimed: ~28.6 min, not ~127 min.
2. **A discarded candidate does not cost a full candidate.** `ingest_candidate` runs the
   guardrails *before* it fingerprints, so a trip never pays for an extraction —
   1.217s against 2.136s. The old arithmetic charged every attempt the full price, which
   is what produced the over-budget ~23 min figure. The cap binds precisely when trips
   are frequent, i.e. when candidates are cheapest.
3. **A branch cannot burn every attempt at full price.** It stops at `top_k` kept, so the
   most expensive branch is `top_k − 1` scored plus the rest discarded — not
   `max_attempts` scored.

### The corrected budget (width=3, top_k=3, max_attempts=9, iteration_cap=20)

| Policy | Attempts/iteration | 20 iterations (before) | 20 iterations (after) |
|---|---|---|---|
| `top_k` attempts (original) | 9 | ~9.7 min | ~6.4 min |
| **bounded retry (adopted)** | **≤27** | ~15.5 min | **~12.8 min worst case** |
| unbounded until `top_k` live | ≤66 | ~31.4 min | ~28.6 min |

**The 15-minute budget is now genuinely met — and met by attacking the cost, not by
narrowing the cap.** Before Task 16 the adopted policy cost ~15.5 min worst case and
did not fit; it now costs ~12.8 min, and a nominal run (no trips) costs ~6.4 min. The
unbounded retry stays rejected on its own merits at ~28.6 min, so §3.3's *conclusion*
was right all along — only its numbers were wrong. `max_attempts = 3 × top_k` is
unchanged, and no tunable was adjusted.

Two qualifications are stated in the spec rather than glossed:

1. The budget covers the **sidecar's** work only. In production the executor is
   PixInsight over a file round trip (§2.5), not the in-process NumPy executor measured
   here. That round trip is additive and **unmeasured**. These figures are a floor for
   the real pipeline, not a ceiling.
2. It is a worst case in two independent senses at once — every branch burning every
   attempt *and* the run going the full 20 iterations. A cost model built from these
   unit prices over-predicts three consecutively measured real iterations by 9–12%, so
   it errs toward pessimism, which is the right direction for a budget.

---

## Verification

- **Full suite: 422 passed** (`.venv/bin/python -m pytest -q`, 233s). Baseline was 413;
  the 9 additions are `tests/test_fingerprint_cost.py`. **Nothing deselected, no
  warnings, and no existing test was modified** — every one of the 413 passes unchanged.
- Equivalence proven on real cached renders, exact equality, as above.

---

## Concerns / findings for the controller

1. **Guardrails are now the single largest sidecar-side cost** (0.857s vs the
   fingerprint's 0.919s), and profiling shows measurable redundancy in them that I did
   **not** touch — `guardrails.py` is outside this task's stated file list and is a
   reviewed §7 surface. Quantified, so the decision is informed rather than deferred:
   - `mrs_noise_sigma` is called **4×** per evaluation (0.229s) where 2 would do:
     `check_noise_floor` and `detect_stars` each recompute it for the same baseline and
     the same candidate. `_gray` likewise runs 4× (0.093s). Sharing within one
     `evaluate_guardrails` call is contained and bit-exact. **~0.16s/candidate.**
   - `check_hue_invention` recomputes `_hue_mass_by_bin(source)` — and inside it
     `rgb_to_lab(source)` — for **every candidate in the whole run**, though `source` is
     invariant. Same for `_channel_ratios(source)`. **~0.26s/candidate**, but this needs
     a cross-candidate cache, which is a real architectural change to a §7 module.

   Together ~0.42s/candidate (~20%). **Not load-bearing for the spec conclusion**: the
   budget is already met at 12.8 min without it. Recommend it be ruled on separately
   rather than smuggled into this task.

2. **The new test file costs ~108s of the suite's 233s**, most of it the two loop
   equivalence tests that each run the loop to convergence twice. That is inherent to
   proving equivalence by comparing two full runs. Flagging the trade rather than
   quietly trimming the proof.

3. **Pre-existing, not mine:** under `-W error` three tests fail
   (`test_degrade`, `test_ingest`, `test_optimize_loop`) on `ResourceWarning: unclosed
   database` from an unclosed `sqlite3.Connection` in the fingerprint store. Verified
   present on the pre-change code by the same run. Under the project's actual invocation
   (`pytest -q`) the suite is clean. Reporting it because it is a real resource leak,
   not a test artifact.

4. **The `None`-fingerprint fallback is documented, not logged.** My reading is that it
   is a cache miss producing an identical value, not a §12 degraded path. If the
   reviewer disagrees, the fix is a one-line log entry in `_branch_fingerprint`.

