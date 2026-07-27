# Phase 2 — Deterministic Optimizer Loop (Stage 1)

**Date:** 2026-07-27
**Status:** approved, not yet implemented
**Spec sections implemented:** §6 (optimization loop), §7 (guardrails), §3.1 (transport)
**Exit criterion:** §10, Phase 2

---

## 1. Scope

This spec covers **Stage 1 — the refinement loop** only.

The optimizer accepts an **already-stretched** image and improves its nonlinear presentation
toward a professional reference. It never touches linear-space channel ratios (§2.1).

**Stage 0 — stretch selection for linear input — is out of scope and is not committed work.**

Rationale: both §10 exit-criterion inputs are already stretched, so Stage 1 alone proves Phase 2.
Deciding the initial stretch is also the single most consequential move available, and it is an
action class §6.2 never defines — a large new search space bolted onto the phase that has to prove
the deterministic core. Stretched-only is the deliberate scope (decision: 2026-07-27).

The user performs their own initial stretch. AutoContrast refines presentation from there, which is
consistent with §2.4 — the reference is a direction, and the user retains the stretch decision.

If Stage 0 is ever built, it attaches at a defined seam: consume a linear master, emit a stretched
image, hand off to Stage 1 unchanged. Nothing in this spec forecloses that. Nothing in it promises
it either.

Input linearity is detected on load. A linear master handed to Stage 1 is a **loud error** naming
the problem and telling the user to stretch first — never a silent auto-stretch (§12).

**No AI.** Action ranking is heuristic and deterministic. The VLM director is Phase 3 and must earn
its place there.

**Nothing in this spec relaxes §2 or §12.** Where a choice below appears to trade one off, that is a
bug in this document.

---

## 2. Architecture

Approach: **the sidecar owns the loop; PixInsight executes real processes; the executor is
pluggable.**

Two constraints drove this choice:

- §3.1 — *"the sidecar must be able to run standalone with no PI present"*
- §12 — output must be *"reconstructible as a sequence of stock PixInsight processes"*

A design where PJSR owns the loop satisfies the second but violates the first: beam-search and
convergence bugs would only reproduce inside a GUI application that writes nothing useful to
stdout. A design where Python simulates the processes end-to-end satisfies the first but optimizes
a *model* of PixInsight rather than PixInsight itself.

### 2.1 The sidecar is not a server

The file protocol spawns `python -m autocontrast.sidecar request.json response.json` **fresh per
request**. "The sidecar owns the loop" therefore cannot mean holding it in memory. Loop state —
beam contents, checkpoints, iteration count, best-so-far — **serializes to disk between calls**.

The loop is a *resumable state machine*. A crashed or interrupted run leaves an inspectable session
file rather than nothing.

### 2.2 The Executor seam

```
Executor protocol:  apply(state, action) -> state
```

| Implementation | Used by | Fidelity |
|---|---|---|
| `PixInsightExecutor` | production runs | real PI processes |
| `NumpyExecutor` | offline tests, CI | approximate |

Loop logic is **identical** under both. Tests exercise the real beam search, real guardrails, real
convergence, with no PixInsight present. Production never optimizes against an approximation.

### 2.3 Module layout — `src/autocontrast/optimize/`

| Module | Responsibility |
|---|---|
| `actions.py` | §6.2 discretized menu; arcsec→layer mapping; serialization to PI process params |
| `executor.py` | `Executor` protocol definition |
| `executors/numpy_exec.py` | offline approximation |
| `guardrails.py` | §7 table as pure functions of pixels |
| `propose.py` | heuristic action ranking from fingerprint gap |
| `beam.py` | beam expansion, pruning, checkpoint/rollback |
| `session.py` | serialize/resume loop state across sidecar invocations |
| `recipe.py` | ordered audit log → stock PI process sequence (§12) |

### 2.4 Guardrails are computed in Python, from pixels

§7 annotates MRS noise and star detection as "PI native". This design **deliberately computes them
in Python instead.**

Routing guardrails through PixInsight would make them behave differently offline than in
production — gutting the offline tests exactly where correctness matters most. Since §7 violations
are what stop the tool from fabricating, they are the last thing that should be untested.

- **MRS noise σ** — implementable directly on the existing `fingerprint/starlet.py` transform.
- **Star integrity** — threshold at background + kσ, `scipy.ndimage.label` for connected
  components, second moments for FWHM and eccentricity. Deterministic; no new dependencies.

Consequence: §7 is enforced **identically** in tests and in production.

### 2.5 Instructions are batched per iteration

The sidecar returns *all* candidates for the current beam expansion in a single response. This
turns roughly 225 round trips into roughly 15.

```
PJSR                          sidecar (spawned per call)
────                          ─────────────────────────
optimize_begin  ──────────►   load image, resolve reference, measure baseline
                ◄──────────   session_id + instruction BATCH 1
apply N processes in PI
save N candidates
optimize_step   ──────────►   measure all N, guardrail, score, prune beam
                ◄──────────   BATCH 2  |  or  converged + recipe
      ⋮                              ⋮
                ◄──────────   converged + recipe + improved: true/false
replay recipe at full res
```

New sidecar ops: `optimize_begin`, `optimize_step`. Existing response contract unchanged
(`{ok, op, result}` / `{ok, op, error}`).

---

## 3. Loop mechanics

### 3.1 Scoring

`score = -D(candidate, reference)` per §6.1.

Guardrail violations are **not scored** — a violation discards the candidate outright (§7). This
distinction is load-bearing: as a score penalty, a large enough distance gain could buy its way
past a noise explosion. Guardrails are a filter, never a term.

### 3.2 The fail-safe is structural

Best-so-far is seeded with **the input image and its own baseline distance**. A candidate replaces
it only on a strict improvement exceeding `epsilon_improve`. If nothing clears that bar, the loop
returns **the original input, byte-for-byte unmodified**, with `improved: false`.

"Unmodified" is literal. On decline, any `star_split` that was applied during the search is
discarded along with everything else — the user gets back the file they handed in, not a
split-and-recombined approximation of it.

This works because the fingerprint distance is a **valley** — D=0 at the reference, rising in
*both* directions, including the over-processed direction (established in Phase 0; see the
`reference_is_local_minimum` harness in `eval/degrade.py`). An already-well-processed image starts
near the valley floor, every available action moves it up the wall, every candidate scores worse
than baseline, and the loop declines.

"Fails safe" is therefore not a guard bolted on top. It is what a valley-shaped objective does on
its own once best-so-far is seeded with the input. A monotone "more contrast is better" objective
could not produce this behavior at any amount of guardrailing.

### 3.3 Beam

Width 3. Each iteration expands every live branch by the top-k proposed actions (k=3), yielding at
most 9 candidates, pruned back to the 3 best **distinct** ones. Distinctness is by recipe, so two
branches cannot converge onto the same action sequence and waste a beam slot.

Checkpoint on every improvement to best-so-far (§6.1).

### 3.4 Convergence

The OR of §6.3. Condition 2 (VLM director) is absent in Phase 2.

| # | Condition | Terminates with |
|---|---|---|
| 1 | ΔD over the last *k*=3 iterations < `epsilon` | best-so-far |
| 3 | every branch dead from guardrail trips | best-so-far (may be the input) |
| 4 | iteration cap reached (default 20) | best-so-far |

Condition 3 is reported honestly as *exhausted*, never dressed up as success.

### 3.5 Proposer

Deterministic, no AI. `fingerprint/distance.py` already decomposes D into spectrum / tonal /
chroma / background components. The proposer ranks actions by **which component carries the largest
gap**:

- energy deficit at a given arcsec band → `local_contrast` at that band
- lifted black quantiles → `black_point`
- chroma shortfall → `chroma@hue`, **only when the palette gate admits chroma at all** (§2.3);
  otherwise chroma actions are never proposed

Ranking is by gap magnitude and is fully reproducible for a given input and reference.

### 3.6 Hybrid proxy validation

Search runs on a ~1600px proxy — the resolution the fingerprint already reduces to, so the
*measurement* never sees full resolution regardless.

**PJSR creates the proxy once, at `optimize_begin`**, and every search-time process execution runs
against it. The full-resolution original is touched only at validation checkpoints and at final
replay.

Every 5 iterations, the best-so-far recipe is replayed at full resolution and re-measured. If proxy
and full-resolution distance diverge beyond tolerance, the divergence is **surfaced loudly and the
run flagged** — never silently trusted (§12).

Actions are denominated in arcseconds (§2.2), which is what makes proxy→full-res transfer
plausible. It is not assumed. It is checked.

### 3.7 Tunables and calibration discipline

These carry defaults; the rest are calibrated empirically during implementation:

| Parameter | Default |
|---|---|
| beam width | 3 |
| top-k actions per branch | 3 |
| iteration cap | 20 |
| convergence window *k* | 3 |
| proxy longest side | 1600 px |
| full-res validation interval | 5 iterations |
| shadow / highlight clipping | ~0.01% of pixels |

**Calibrated against real data, not assumed:** `epsilon` (convergence), `epsilon_improve`
(minimum improvement to replace best-so-far), noise-floor σ tolerance, star FWHM/eccentricity
tolerance, channel-ratio drift tolerance, hue-invention ε, proxy/full-res divergence tolerance.

**The calibration hazard, stated plainly.** `epsilon_improve` is the single knob that trades the
two halves of the §10 exit criterion against each other. Lower it and test 3 (improve a flat image)
passes more easily while tests 1 and 2 (decline on well-processed input) get more fragile. Raise it
and the reverse.

Therefore: **no tunable may be adjusted to make one exit-criterion test pass without re-running all
three.** A value that satisfies test 3 by breaking the fail-safe has not been calibrated, it has
been defeated. If no single value satisfies all three, that is a finding about the fingerprint —
report it, do not tune around it. The Phase 0 exit criterion was reframed on exactly this kind of
finding rather than papered over.

---

## 4. Action space

Actions are constructed **band-limited**: each carries its scale in arcseconds, and the constructor
**refuses to emit any action whose scale is below the image's own resolvable limit** (PSF FWHM,
§4.4).

This enforces §2.2 at the point of proposal rather than catching it later as a guardrail trip. The
optimizer is structurally incapable of proposing that 0.05″/px HST detail be sharpened into
1.01″/px backyard data.

| Action | PI process | Magnitude levels | Constraint |
|---|---|---|---|
| `local_contrast@Narcsec` | MultiscaleLinearTransform | gentle / moderate / strong | band ≥ PSF limit |
| `core_hdr` | HDRMultiscaleTransform | 2 / 3 / 4 layers | masked |
| `tonal_reshape` | CurvesTransformation | gentle / moderate / strong | monotone-constrained |
| `black_point` | HistogramTransformation | gentle / moderate / strong | clip-limited (§7) |
| `local_equalize` | LocalHistogramEqualization | gentle / moderate / strong | kernel in arcsec |
| `chroma@hue` | ColorSaturation | gentle / moderate / strong | only if palette-compatible |
| `background_neutralize` | BackgroundNeutralization | — | idempotent, early only, once |
| `star_split` | StarXTerminator | — | once, early |

`star_split` is a **mode change**, not a scored move. Once applied to a branch, all subsequent
actions on that branch operate on the **starless layer only**; the star layer is held aside
unmodified and recombined at the end of the branch's recipe. This is what lets local contrast be
pushed hard without blowing star cores.

Measurement and guardrails always run on the **recombined** image, never on the starless layer
alone — otherwise star integrity (§7) would be measured against a frame with no stars in it.

StarXTerminator is confirmed installed at `/opt/PixInsight/bin/StarXTerminator-pxm.so`.

---

## 5. Guardrails (§7)

Hard constraints, evaluated every iteration, cheap and deterministic. A violation discards the
candidate and rolls back. Each records a **reason**, so a declined run can explain why it declined.

| Guardrail | Metric | Trip condition |
|---|---|---|
| Noise floor | MRS noise σ per channel, via starlet | σ increases beyond tolerance from checkpoint |
| Star integrity | count, median FWHM, eccentricity | count drops, or FWHM/ecc balloons |
| Shadow clipping | % pixels at 0 per channel | exceeds threshold (default ~0.01%) |
| Highlight clipping | % pixels at 1.0 per channel | exceeds threshold |
| Hue invention | chroma mass in a\*/b\* cells with no support in the source | any mass above ε |
| Channel ratio drift | post-stretch ratios vs. source-derived constraints | exceeds tolerance |

The last two enforce §2.1 and §2.3 mechanically and are the difference between a tool that enhances
and one that fabricates.

---

## 6. Error handling

- **Guardrail trip** — discards that candidate only; reason recorded and reported.
- **Executor failure** (a PI process erroring on a candidate) — kills that candidate only, logged.
- **All branches dead** — convergence condition 3, reported as exhausted.
- **Proxy/full-res divergence** — surfaced loudly, run flagged (§12).
- **Linear input to Stage 1** — loud error, never a silent auto-stretch (§12).
- **Sidecar op errors** — existing `{ok: false, error}` contract; always surfaced (§12).

No degraded path proceeds silently. This is a §12 requirement, not a preference.

---

## 7. Testing

### 7.1 Unit — fast, no PI, no network

Arcsec→layer mapping; band-limit refusal; each §7 guardrail independently, using synthetic images
built to trip exactly one apiece (noise-injected → noise floor; clipped → shadow clipping; a\*/b\*
mass with no source support → hue invention); proposer ranking; beam pruning and distinctness; each
convergence condition; session serialize/resume round-trip.

### 7.2 Offline integration — `NumpyExecutor`, no PI

The full loop, end to end:

- `flatten(good_render, strength=0.6)` → must measurably reduce D
- pro render in → must decline, `improved: false`
- guardrail-hostile input → must terminate via condition 3 and say so

### 7.3 Live — real PixInsight, real data

The §10 exit criterion:

1. `eso1103a` (cached pro render) → must decline. CI-runnable, needs no user data.
2. Finished HOO M42 from `/mnt/qnap/astro_data/prints/` → must decline. Authentic scenario.
3. Linear M42 stack, hand-stretched → must measurably improve.

Live tests run **by default**, consistent with Phase 1. Politeness comes from the existing
persistent caches, not from deselection. No test is deselected to make a suite look green.

### 7.4 What the offline tests do and do not prove

`NumpyExecutor` approximations **do not match** PixInsight's processes. This is acceptable for
testing loop logic — beam pruning and convergence do not care whether LHE is exact.

**The offline tests prove the loop is correct. They do not prove the output is good.** Only the
live tests can prove the latter. A green offline suite must never be read as evidence that the
optimizer produces beautiful images.

---

## 8. Deliverable

A PJSR script `pixinsight/autocontrast_optimize.js` registering `AutoContrast > Optimize`,
producing:

- a new image window with the result (or the untouched input, when it declines)
- the recipe printed to console **and** written as JSON (§12 auditability)
- a `STATUS_FILE` report — headless PixInsight writes nothing useful to stdout

The recipe is the audit artifact: an ordered list of stock PixInsight processes and parameters that
reproduces the result without AutoContrast present.

**Note:** this script inherits the open release-compliance defect affecting
`autocontrast_analyze.js` — unversioned, unsigned, unpackaged, never registered with PI. That is
tracked separately and is not resolved by this spec.

---

## 9. Out of scope

- Stage 0, stretch selection for linear input — not committed work; see §1
- Any VLM involvement — Phase 3, and it must earn its place
- Anything in §12
