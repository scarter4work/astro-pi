# Task 16 review — fingerprint extraction cost + the §3.3 budget argument

Reviewer: task16-reviewer. Diff `ec6c662..8d8f727` (single commit `8d8f727`).
Read-only review. Status: IN PROGRESS (updated as I go).

---

## 1. The equivalence proof — is it independent, exact, and on real data?

### 1.1 Independence — is it comparing new code against itself?

`tests/test_fingerprint_cost.py:_extract_pre_optimization` (line ~831 of the diff)
reproduces the pre-Task-16 composition by calling the two *single-image* entry
points, `energy_spectrum(L, ...)` and `structure_to_gradient_ratio(L, n_scales=...)`.
Both still exist and both still run their own `starlet_transform`. `extract` now runs
**one** transform and feeds both consumers.

So the two sides genuinely differ:

| | transforms run | call graph |
|---|---|---|
| test's "old" path | **2** (`energy.py:174`, `metrics.py:309`) | `energy_spectrum` → `starlet_transform` → `energy_spectrum_from_planes`; `structure_to_gradient_ratio` → `starlet_transform` → `..._from_transform` |
| `extract` (new) | **1** (`extract.py:248` in diff) | `starlet_transform` once → both `*_from_*` helpers |

That is **not** a tautology. It proves (a) the two discarded decompositions were
bit-identical to the shared one, (b) `extract`'s rewiring hands the right arrays to
the right consumer (no swapped `planes`/`residual`, no depth mismatch), (c) the whole
`FingerprintData` composition is unchanged.

**The one blind spot.** Because the diff *moved* the post-transform arithmetic into the
new helpers and made the old functions delegate, both sides of the test execute the
*same* helper code. Anything that changed inside the moved arithmetic is invisible to
this test. Reading the diff, exactly one line of arithmetic text changed:

- `energy.py` (removed): `scale_px = 2.0 ** np.arange(n_scales)`
- `energy.py:161` (added, in `energy_spectrum_from_planes`): `scale_px = 2.0 ** np.arange(len(planes))`

`structure_to_gradient_ratio_from_transform` (`metrics.py:294-298`) is a verbatim move
— three statements, byte-identical.

I closed the `n_scales` → `len(planes)` substitution by reading
`src/autocontrast/fingerprint/starlet.py:59`: `planes = np.empty((n_scales, *image.shape))`
and the function raises for `n_scales < 1`. `len(planes) == n_scales`
**unconditionally** — there is no truncation path. The substitution is provably
equivalent, and it is in fact the safer form (the docstring's reasoning at
`energy.py:150-152` is correct). **Verified by reading, not by the test** — that gap in
the proof is real but benign.

### 1.2 Exactness — any tolerance?

None. `_assert_identical` (diff line ~863) uses `np.testing.assert_array_equal`
throughout for the five array components and bare `==` for the three scalars and the
three metadata fields. **No `allclose`, no `rtol`, no `atol` anywhere in the file**
(grep-confirmed over the diff text). No calibration decision was taken.

### 1.3 Real data?

Yes. `_cached_renders()` globs `data/discovery_cache/*.jpg`, sorts by file size
descending, takes 4, and `load_image(render, max_dim=1600)` runs them through the
production loader at the real §2.5 search-proxy size. I confirmed 35 real renders are
present in that directory on this machine, so the `requires_cache` skipif does **not**
fire here — the parametrized equivalence test really runs on
`eso1723a`, `eso1625a`, `eso1006a`, `heic0601a`.

Caveat recorded under Issues: the proof is `skipif`-guarded, so on a checkout without
the cache the suite goes green with the headline proof absent.

### 1.4 Coverage beyond the single fixture

- `test_equivalence_holds_across_the_scale_depths_the_loop_uses` re-runs the comparison
  at `n_scales ∈ {1,3,5,7,8}` with a different `pixel_scale_arcsec` (1.22) and palette
  ("SHO"). This is the test that would catch a depth mismatch in the sharing.
- `test_fingerprint_survives_the_session_json_round_trip_exactly` goes through real
  `json.dumps`/`loads` text and asserts exact equality — correct instinct, because the
  value now crosses a process boundary and feeds `propose_actions`' hard 0.075/0.15
  magnitude buckets.

---

## 2. Optimization 1 — carrying the parent fingerprint on `Branch`

### Both entry points served?

Yes, and this was a named risk. `loop._branch_fingerprint` (diff line ~471) is called
from **both**:
- `advance` — `loop.py`, diff line ~600, replacing `_measure(parent, session)`
- `_plan_batch` (the Task-11 §2.5 batched path) — `loop.py`, diff line ~654, replacing
  `_measure(load(branch.image_path), session)`

Population happens at both producers: `ingest_candidate` attaches it to every Branch it
builds (diff line ~545-549) and `begin` attaches the baseline measurement to the root
(diff line ~517).

### Session round-trip

`session.py:_branch_to_dict` writes `"fingerprint": None if ... else b.fingerprint.to_dict()`;
`_branch_from_dict` reads it with `.get("fingerprint")` and revives via
`FingerprintData.from_dict`. I checked that **every** Branch-valued field on `Session`
goes through those two helpers and not through `asdict`:

```
session.py:113  "branches": [_branch_to_dict(b) for b in self.branches],
session.py:114  "best": _branch_to_dict(self.best),
session.py:120  "candidates": [_branch_to_dict(b) for b in self.candidates],
session.py:136/137/143  the three matching _branch_from_dict calls
```

`asdict` is used only on `self.config` (`session.py:112`), a `BeamConfig` of plain
scalars. So no numpy array can reach `json.dumps` un-rendered. **Backward
compatibility holds**: a session file written by the old code has no `"fingerprint"`
key, `.get` yields `None`, and `_branch_fingerprint` falls back to measuring.

`fingerprint: FingerprintData | None = field(default=None, compare=False)` on a
`@dataclass(frozen=True)` — `compare=False` excludes the field from both the generated
`__eq__` and `__hash__` (dataclasses derive hash membership from `compare` unless
`hash=` is set), so numpy arrays cannot reach either. Correct, and the reasoning at
`beam.py:440-443` is right.

### Is the fallback really not a §12 degraded path?

I agree with the implementer. `extract` is a pure function of the pixels at
`image_path` and the session's measurement metadata; the fallback yields the identical
value at higher cost. §12 surfaces degraded *results*. There is no result difference.
Not a finding.

### Is the loop-level equivalence proof sound?

`test_carrying_the_parent_fingerprint_does_not_change_the_offline_run` and
`..._the_batched_run` run the loop twice — once normally, once with
`_forget_fingerprints` stripping every branch's fingerprint before each iteration —
and assert the **whole** `outcome()` dict is equal. I checked `loop.outcome`
(`loop.py:700-719`): it returns only plain values (improved, both distances,
result_path, recipe dict, pixinsight_steps, iterations, convergence_reason,
reference_id, guardrail_log), so `==` is a real comparison and it covers the §12
guardrail log, not just the distance. Good.

Two things make these genuinely independent paths rather than self-comparison:
- the stripped path takes the `branch.fingerprint is None` arm of
  `_branch_fingerprint`, which calls `_measure(load(branch.image_path), session)` —
  literally the pre-Task-16 expression;
- stripping happens **every iteration**, not once, which is necessary because
  `ingest_candidate` re-attaches on every candidate. The test comment says so
  explicitly and the code does it.

These two use a **synthetic** fixture (`_fixture_pair`, seeded rng), not real renders.
Acceptable: the claim under test here is control-flow identity, not fingerprint
numerics, and the fingerprint numerics are proven on real data separately.

`test_the_beam_actually_carries_a_fingerprint_forward` is the right extra test — the
two equivalence tests would pass identically (and identically slowly) if the cache
silently stopped being populated, so the *saving* is asserted separately from the
*correctness*. It pins `without_cache - with_cache == n_branches`.

I checked the one thing that could have made its second assertion vacuous:
`_close_iteration` mutates the session in place (`loop.py:298 session.branches = []`,
`loop.py:305 session.branches = survivors`, `return session`), so `advance` mutates the
object `_count_measures_in_one_advance` was handed. The trailing
`all(b.fingerprint is not None for b in cached.branches)` therefore inspects the
**post-`advance` survivors** — the branches that carry into the next iteration — which
is precisely the property that matters. Not vacuous. The `n_branches == 1` at `begin`
makes the delta assertion narrow (one branch, one saved extraction), but it is the
correct expected value for a single `advance` from the root.

---

## 3. Optimization 2 — one starlet transform per fingerprint

Correct. `extract.py` (diff line 248) computes `planes, residual = starlet_transform(L, n_scales=n_scales)`
once, from the **same** `L` (`lab[..., 0]` of `rgb_to_lab(rgb)`) and the **same**
`n_scales` both consumers previously passed:

- old `energy_spectrum(L, pixel_scale_arcsec=..., n_scales=n_scales)` → `starlet_transform(image, n_scales=n_scales)`
- old `structure_to_gradient_ratio(L, n_scales=n_scales)` → `starlet_transform(luminance, n_scales=n_scales)`

`starlet_transform` takes no normalization or boundary parameter — the B3-spline kernel
and `mode="mirror"` are hard-coded inside `_smooth` (`starlet.py:31-34`), so there is
no per-call convention that could differ. Same input array, same `n_scales`, same
function ⇒ same decomposition. The brief's "different n_scales / normalization /
boundary mode" failure mode is structurally impossible here, and the equivalence test
confirms it empirically at five depths.

`energy_spectrum_from_planes` validates `planes.ndim == 3` and derives `n_scales` from
`len(planes)` rather than trusting a parameter — a good defensive choice, and the
docstring's reason (a mismatch would mislabel every §2.2 angular center while looking
well-formed) is the right one.

The brief's hypothesis (b) — sharing the *guardrail's* MRS-noise transform — was
correctly **refuted**, not quietly skipped. Different input (L* in [0,100] vs RGB
channel mean in [0,1]) and different depth (7 vs 1). Sharing it would have been the
correctness bug the brief warned about. Right call.

---

## 4. The spec rewrite — do claims 1-3 hold?

### Claim 2 — "guardrails run BEFORE fingerprinting" — ✅ TRUE

Verified directly at `src/autocontrast/optimize/loop.py:249-290`:

```
249  verdicts = evaluate_guardrails(produced, parent, source, limits)
...
266  failed = [v for v in verdicts if not v.ok]
267  if failed:
273      return None          # <- discarded here
275  if save is not None: save(...)
282  fingerprint = _measure(produced, session)   # <- first and only extraction
```

A discarded candidate never reaches `_measure`. The 1.217s vs 2.136s asymmetry is real
and the causal claim is exactly right.

### Claim 3 — "a branch cannot burn every attempt at full price" — ✅ TRUE

`_Walk.may_issue` (`loop.py:168-169`) is `self.kept < config.top_k and self.attempts <
config.max_attempts`. Reaching `attempts == max_attempts` therefore requires
`kept < top_k`, i.e. at most `top_k - 1` scored candidates. The worst-case shape
"`top_k - 1` scored + the rest discarded" follows. Both entry points share this one
definition, so it holds for the batched path too.

### Claim 1 — "the menu holds 14-22, not 50" — ✅ number correct, ⚠️ reason only half right

`actions.available_actions` (`src/autocontrast/optimize/actions.py:84-88`) *is*
band-limited at construction:

```
bands = [scale_for_layer(i, pixel_scale_arcsec)
         for i in range(n_scales)
         if scale_for_layer(i, pixel_scale_arcsec) >= psf_fwhm_arcsec]
```

so the catalog is bounded by resolvable wavelet planes, as claimed. But that is not the
dominant reason the *ranked menu* is 14-22. What `loop._ranked_menu` consumes is
`propose_actions(..., top_k=None)`, which returns **one action per
(priority, band, kind) group**, collapsing the three magnitude levels
(`propose.py:176-190`). Worked example at the profiled configuration
(`pixel_scale=0.5, psf=2.0, n_scales=7`): 5 resolvable bands ⇒ `available_actions`
returns 5×2×3 + 4×3 + 2 = **44** raw actions, and grouping collapses that to
5×2 + 4 + 2 = **16** — the implementer's own measured "16-entry ranked menu".

So band-limiting takes the catalog from unbounded-in-`n_scales` to 44; the magnitude
grouping takes 44 to 16. The old "50" figure was evidently counting raw actions. The
spec attributes the whole correction to band-limiting and does not mention the
grouping. The *number* is right and the budget is unaffected; the stated *cause* is
incomplete. Minor.

Second-order: the claimed lower bound of 14 does not reproduce at the stated corner —
`n_scales=5, pixel_scale=0.25, psf=2.0` gives 2 bands ⇒ 2×2+4+2 = 10 groups. And the
upper bound 22 (`n_scales=9, pixel_scale=1.5, psf=2.0` ⇒ 8 bands ⇒ 22) rises to 24 if
`psf` is below 1.5. Neither matters: the menu size enters only the *unbounded* arm,
which is rejected either way, and the adopted bounded arm is capped at `max_attempts`
regardless of menu length. Noted, not load-bearing.

### Does the budget arithmetic follow?

Yes — I recomputed every cell of both tables from the unit prices and they are
internally consistent to the stated precision. With `width=3, top_k=3, max_attempts=9,
iteration_cap=20` and worst-case shape "2 scored + 7 discarded per branch":

| arm | per-branch, after | ×3 branches | ×20 iters | spec says |
|---|---|---|---|---|
| bounded retry | 2(2.136)+7(1.217) = 12.791s | 38.37s | 767s = **12.79 min** | ~12.8 min ✅ |
| bounded retry, before | 2(2.719)+7(1.222)+1.497 = 15.489s | 46.47s | 929s = **15.5 min** | ~15.5 min ✅ |
| top_k attempts, after | 3(2.136) = 6.408s | 19.22s | 384s = **6.41 min** | ~6.4 min ✅ |
| top_k attempts, before | 3(2.719)+1.497 = 9.654s | 28.96s | 579s = **9.65 min** | ~9.7 min ✅ |
| unbounded (22-entry menu), after | 2(2.136)+20(1.217) = 28.61s | 85.8s | 1717s = **28.6 min** | ~28.6 min ✅ |
| unbounded, before | +1.497 = 31.38s | 94.1s | 1882s = **31.4 min** | ~31.4 min ✅ |

The "before" column only reproduces if the eliminated **parent re-measure (1.497s per
branch per iteration)** is included — which is exactly the second optimization, and it
is the term that carries 15.5 min down to 12.8. So the two rewrites are consistent with
each other. The `advance` docstring's "~86s per iteration against ~38s for the bounded
retry" matches the per-iteration column. **12.8 < 15. The argument now reaches its own
conclusion**, which is the thing this task existed to fix, and it does so with the cap
untouched.

The two qualifications stated in the spec (sidecar-only scope, PixInsight round trip
unmeasured and additive; worst-case-in-two-senses) are honest and are the right ones to
state rather than gloss. The old §3.3 claimed a 15-minute budget was met when it was
not; the new text says plainly that the figures are a floor, not a ceiling.

### Tunables

Unchanged. In `beam.py` the `BeamConfig` field lines `width: int = 3`, `top_k: int = 3`,
`iteration_cap: int = 20`, `convergence_window: int = 3` appear as **context** lines
(unmodified) and `max_attempts: int | None = None` with `3 * self.top_k` in
`__post_init__` is likewise context. `epsilon` / `epsilon_improve` fall in the
unchanged gap between hunk 1 (old lines 1-20) and hunk 2 (old line 23+) — a unified
diff omits only unchanged lines, so their being absent is proof they were not touched.
No tunable adjusted. ✅ §3.7 satisfied.

---

## 5. Other global constraints

- **No existing test modified.** The diff's file list has exactly one test file,
  `tests/test_fingerprint_cost.py`, marked `new file mode`. No other test file appears
  in the diff at all, so no existing assertion could have been altered. ✅
- **No AI.** Nothing in the diff introduces a model call. ✅
- **Guardrails discard, never score.** `ingest_candidate`'s `return None` on `failed` is
  context (unmodified) in this diff; §7 semantics untouched. ✅
- **Wavelet scale convention.** `2.0 ** np.arange(len(planes)) * pixel_scale_arcsec` ≡
  the old `2.0 ** np.arange(n_scales) * pixel_scale_arcsec`. Convention preserved and
  its preservation is asserted by name in `_assert_identical`. ✅
- **Metric / band-limiting / palette gate / distance function** — none appear in the
  diff. ✅
- **No silent fallbacks (§12).** The one new fallback (`fingerprint is None`) is a cache
  miss producing an identical value; correctly not logged. ✅

---

## 6. Focused verification run

I did **not** re-run the package suite (233s, and the implementer's count cannot be
falsified from the diff either way). Two focused runs, both justified by specific
doubts that no existing run answers.

### 6.1 Does the headline proof actually execute on real data on this machine?

```
.venv/bin/python -m pytest tests/test_fingerprint_cost.py \
    -k "bit_identical or scale_depths or round_trip" -v -rs -p no:randomly

test_extract_is_bit_identical_to_the_pre_optimization_composition[eso1723a] PASSED
test_extract_is_bit_identical_to_the_pre_optimization_composition[eso1625a] PASSED
test_extract_is_bit_identical_to_the_pre_optimization_composition[eso1006a] PASSED
test_extract_is_bit_identical_to_the_pre_optimization_composition[heic0601a] PASSED
test_equivalence_holds_across_the_scale_depths_the_loop_uses               PASSED
test_fingerprint_survives_the_session_json_round_trip_exactly              PASSED
6 passed, 3 deselected in 25.77s
```

Zero skips. The four parametrized cases really ran on four real cached renders at the
1600px proxy. (The "3 deselected" is my own `-k` filter, not the implementer's — the
three loop-level tests, which I left out because they are the slow ones and the diff
answers what they do.)

### 6.2 Do the assertions bite? — mutation test

A passing equivalence test proves nothing unless a real drift would fail it. I injected
four mutations into `extract`'s module namespace from a scratchpad script (no working
tree change) and re-ran `_assert_identical` against the pre-optimization composition on
`eso1723a`:

| injected fault | result |
|---|---|
| **1 ULP** drift in one energy bin (`np.nextafter`) | **caught** |
| §2.2 scale convention broken (`2**i` → `3**i` centers) | **caught** |
| 1e-15 *relative* drift in structure/gradient ratio | **caught** |
| shared transform run at `n_scales - 1` | **caught** |

The proof has genuine teeth down to the last bit — which is the correct sensitivity
given that the 0.075/0.15 magnitude buckets and the §10 exit criterion are calibrated
against these values.

### 6.3 Not verified

- The claimed **422 passed / 413 baseline / no warnings** full-suite result. Cannot be
  checked from the diff and I did not re-run the suite. Risk is bounded: the diff
  touches no existing test file (so no assertion was edited), and the only behavioral
  surface it changes is `extract`, which the run in 6.1 proves bit-identical.
- The **before/after timing table** (1.497s → 0.919s etc.). Not reproducible from the
  diff. The *relative* claim is structurally sound — one starlet transform instead of
  two, and the report's own profile puts a transform at 0.574s of a 1.505s extraction,
  so 1.497 − 0.574 ≈ 0.92 is arithmetically consistent with itself.
- Concern 3 in the implementer's report (pre-existing `ResourceWarning: unclosed
  database` under `-W error`) — disclosed, claimed verified against pre-change code,
  out of this task's scope. Not a finding against Task 16, but it is a real leak and
  should be routed somewhere rather than dropped.

---

## 7. Strengths

1. **The proof is the deliverable, and it was built as one.** Explicitly refusing a
   golden file, and saying why ("a recorded number proves only that today's code agrees
   with the day the number was recorded"), is the right instinct. Recomputing the
   pre-optimization composition from live code is a materially stronger warranty, and it
   survives mutation testing at 1 ULP.
2. **Hypothesis (b) was refuted, not quietly implemented.** Sharing the guardrail's
   MRS-noise starlet with the fingerprint's would have been a correctness bug in an
   optimization's clothing — different input array (L\* in [0,100] vs RGB channel mean
   in [0,1]), different depth (1 vs 7). The implementer verified with `np.array_equal`
   rather than arguing, and reported the refutation.
3. **The real cost driver was found by measuring, not by following the brief's list.**
   The duplicated transform *inside* `extract` was not in the brief; profiling found it,
   and it turned out to be the 38% win.
4. **No mutable-intermediate aliasing.** The classic hazard when sharing a computed
   array between two consumers is one of them writing to it. Neither does:
   `energy_spectrum_from_planes` builds a fresh `np.sum(planes**2, axis=(1,2))`, and
   `structure_to_gradient_ratio_from_transform` only reads `planes` and `residual`.
5. **`n_scales` derived from `len(planes)` rather than passed** — closes a whole class
   of caller-mismatch bug that would have silently mislabeled every angular center.
6. **The spec rewrite reports a cost rather than choosing a comfortable one.** It states
   plainly that the figures are a floor (the PixInsight round trip is unmeasured and
   additive) and that the worst case is doubly pessimistic. It also explains why
   *narrowing the cap* was not the fix — `local_contrast@2"` ranks fifth. That is the
   honest outcome the brief asked for.
7. **Backward compatibility was thought about before it bit.** `.get("fingerprint")`
   means an old session file resumes rather than failing to load, and the
   cache-defeating test path is deliberately the same code path an old session takes —
   so the compatibility branch is exercised by two tests rather than merely asserted.

---

## 8. Issues

### Critical (Must Fix)

**None.**

### Important (Should Fix)

**None.** Nothing found that changes behavior, weakens the proof materially, or leaves
the §3.3 argument short of its conclusion.

### Minor (Nice to Have)

**M1 — the equivalence test cannot see the one line of arithmetic that actually moved.**
`tests/test_fingerprint_cost.py:848` calls `energy_spectrum`, which post-diff delegates
to `energy_spectrum_from_planes` — the same helper the new path calls. So both sides of
the comparison run the changed arithmetic, and the substitution
`2.0 ** np.arange(n_scales)` → `2.0 ** np.arange(len(planes))` (`energy.py:161`) is
invisible to it. *Why it matters:* that expression **is** the §2.2 scale convention the
task was forbidden to change; the test asserts it only relative to itself.
*Verified safe by reading:* `starlet.py:59` allocates `np.empty((n_scales, *shape))`
and rejects `n_scales < 1`, so `len(planes) == n_scales` unconditionally.
*Fix:* one absolute assertion in `_assert_identical`, pinning the convention rather
than comparing it to itself —
`np.testing.assert_array_equal(new.energy.centers_arcsec, 2.0 ** np.arange(n_scales) * pixel_scale_arcsec)`.

**M2 — the headline proof is `skipif`-guarded on a data directory.**
`tests/test_fingerprint_cost.py:822` skips all four real-data cases when
`data/discovery_cache/` is empty. On this machine 35 renders are present and the tests
run (§6.1), but on a fresh checkout the suite goes **green with the calibration warranty
absent**. The skip reason is honest and explicitly forbids faking it with synthetic
input, and a skip is not a deselect — but this repo's standing rule is that acceptance
criteria run on real data. *Fix:* nothing to change in this task; the controller should
decide whether the project's acceptance run treats this skip as a failure. Worth a line
somewhere that says the cache is a prerequisite, not an optimization.

**M3 — spec claim 1 attributes the menu size to the wrong dominant cause.**
`docs/.../2026-07-27-phase2-optimizer-design.md` (§3.3, "The menu does not hold 50
actions") says the menu is 14-22 "because actions are constructed band-limited... bounded
by the number of resolvable wavelet planes." Band-limiting is real
(`actions.py:84-88`), but at the profiled configuration it yields **44** raw actions,
not 16. The step from 44 to 16 is `propose_actions` contributing **one action per
(priority, band, kind) group**, collapsing the three magnitude levels
(`propose.py:176-190`). *Why it matters:* this section exists specifically because its
previous version reasoned from wrong inputs; a half-right cause is a weaker artifact
than the number deserves. The number itself is measured and correct, and the budget is
unaffected. *Fix:* one clause — "...and because the ranked menu contributes one action
per (priority, band, kind) group rather than one per magnitude level."

**M4 — the stated 14-22 range does not reproduce at its own lower corner.**
`n_scales=5, pixel_scale=0.25″/px, psf=2.0″` gives 2 resolvable bands ⇒ 2×2+4+2 = **10**
groups, not 14; and the upper bound 22 rises to 24 if `psf_fwhm` drops below 1.5″.
*Why it matters:* almost not at all — menu size enters only the **unbounded** arm, which
is rejected at any value in this range, and the adopted bounded arm is capped by
`max_attempts` independently of menu length. Recording it so the range is not later
treated as a hard bound. *Fix:* state the psf assumption alongside the range, or widen
it to 10-24.

---

## 9. Assessment

**Task quality: Approved.**

The constraint this review existed to verify — exact, behavior-preserving equivalence
proven on real data by genuinely independent code paths — is met, and it survives
mutation testing at 1 ULP. Both optimizations are correct, both entry points are served,
the session record round-trips and stays backward compatible, no tunable moved, no
existing test was touched, and the §3.3 budget arithmetic now actually reaches its own
conclusion (12.8 min against 15) with the cap unchanged. The four Minor items are
polish: one absolute assertion that would close a blind spot I verified by hand, and
three wording corrections in the spec's causal narrative.

