# Phase B model race — Stage 2: the race against Huber, after normalisation

**Status:** measurement only, no product change. Ends in a decision, not a fix.

This is the measurement Stage 1 named as the one the product decision turns on:
`NUKEX_DUMP_MU` and the Huber IRLS estimator were restored and compiled on
2026-09-08 but never run.

## 0. Method, and why it is not the Stage 1 method

The 2026-09-07 analyser (`my_mu_check.py`) lived in a scratch directory and was
lost with it. Its **data** survived, though: four mu dumps at
`~/.cache/nukex_spike_corpora/`. So the analyser was rewritten
(`tools/research/analyze_mu.py`) and checked against the published figures
before being trusted on anything new:

| corpus | published 2026-09-07 | reconstructed analyser |
|---|---|---|
| M27-2023 33f OSC | 1.00x | 0.97x |
| M27-2025 72f LRGB | 1.28x | 1.37x |
| M16 30f dual-NB | 3.77x | 3.47x |
| NGC7635 65f mono | 5.19x | 4.93x |

Same ordering, same magnitudes, within ~10% — the residual is aggregation
detail that cannot be recovered from a deleted script. The reconstruction also
reproduces a distinctive secondary result exactly: the note that Huber runs
*higher* than the race at M16's top 0.01% by +4.57% comes back as **+4.61%**.

Two consequences for how this stage is run:

- **The archived dumps are not used as the experiment's OFF arm.** They predate
  v5.0.4.0, v5.0.4.1 and v5.0.4.2, so "pre-norm vs post-norm" across them would
  confound per-frame normalisation with three releases of other change. Every
  comparison below is four corpora x {normalisation ON, OFF} on **one** code
  state.
- Everything is scored by **one** analyser, so nothing in the comparison turns
  on which script produced which number.

That first precaution turned out to be unnecessary, and now it is *known* to be
unnecessary rather than assumed. Re-running NGC7635 with `NUKEX_SPIKE_NO_NORM=1`
on today's tree reproduces the 2026-09-07 dump **bit-identically** — all
8,294,400 voxels, both planes, zero differing words:

    race  : bit-identical=True  max|diff|=0.000e+00  ndiff=0
    huber : bit-identical=True  max|diff|=0.000e+00  ndiff=0

Three useful facts fall out of that one check. v5.0.4.2's "bit-identical" claim
demonstrably reaches Phase B output; v5.0.4.1's alpha-channel fix demonstrably
never touched the stacking path; and because the underlying data is *identical*,
the ~10% gap between the published ratios and the reconstructed ones is
attributable entirely to analyser aggregation, with no measurement component at
all.

### The noise metric

Pixel-scale noise from **horizontally adjacent differences**, over background
pairs selected automatically (both members below the channel median). MAD about
a global median is unusable here: a stacked astro frame is mostly structure, and
MAD scores nebulosity as noise. Differencing neighbours cancels anything smooth.

Both the `std` and the robust `mad` form of the estimator are reported, and the
gap between them is itself diagnostic. If `std` says 4.9x while `mad` says 1.9x,
the excess lives in a **minority of badly wrong pixels** rather than in a uniform
inflation — which is the signature of a discrete per-pixel model choice.

### What is being measured is what ships

`pixel_selector.cpp:47` sets the output pixel to `dist.true_signal_estimate`
verbatim, with no post-processing. The mu compared here is the stacked pixel.

**And the pipeline cannot see this defect through its own instruments.** The
`out_noise` computed immediately below that line comes from a CCD noise model —
Poisson shot plus read noise, scaled through the per-frame normalisation — not
from the estimator's realised scatter. NukeX reports the noise its inputs *ought*
to produce. An estimator that injects noise of its own is invisible to it. That
is the most plausible reason a 5x penalty survived several releases without
tripping anything.

## 1. The headline: normalisation already paid this bill

Four corpora, both normalisation states, one code state. Pixel-scale noise of
the race divided by that of a Huber M-estimator on the identical voxels — 1.00x
means the two are equally clean, and higher means the race is noisier:

| corpus | frames | samples/voxel | normalisation OFF | normalisation ON |
|---|---|---|---|---|
| NGC7635 mono L | 65 | 65 | **4.93x** | **1.16x** |
| M16 dual-NB OSC | 30 | 30 | **3.47x** | **1.47x** |
| M27-2025 LRGB-mono | 72 | 12 | **1.37x** | **1.19x** |
| M27-2023 broadband OSC | 33 | 33 | **0.97x** | **1.05x** |

Noise goes as sqrt(n), so the worst case fell from costing ~24x of effective
exposure to ~1.35x. The question this investigation was opened to answer —
"does the model race inject enough noise to matter?" — is answered **no, not
any more**, and the fix was the one already shipped in v5.0.4.0.

Every OFF-arm number above reproduces the 2026-09-07 archive **bit-identically**,
so the ON column is the only thing that changed.

### Huber is still not clipping signal

Percentile comparison at the bright end, post-normalisation, over all 12
channels of the four corpora. At the 99th and 99.9th percentiles every channel
agrees to within 2.7%, and 11 of 12 to within 1%.

At the extreme 99.99th percentile the picture is more textured, and the
**direction** is what matters:

| | channels | Huber vs race at p99.99 |
|---|---|---|
| M27-2023 (4), NGC7635 (1), M27-2025 ch0-2 (3) | 8 | within +/-1% |
| M16 (3) | 3 | Huber **+2.3% to +4.2% HIGHER** |
| M27-2025 ch3 | 1 | Huber **-6.9% LOWER** |

Huber running *higher* is not clipping — it is the race that sits lower there,
and M16 reproduces this from 2026-09-07 (+4.61% archived, +4.24% post-norm). So
on 11 of 12 channels the cheaper estimator either matches the race or preserves
more signal than it.

The one genuine exception is M27-2025's channel 3, where Huber runs 3.4% lower
across p99.9-99.99 and 28% of those pixels differ by more than 5% — about 8,300
pixels of 8.3 million, on one channel of one corpus. Everywhere below the top
0.1% of that channel, agreement is within 0.05%. Which estimator is *right*
there is not established by this measurement and is a named follow-up, not a
result.

## 2. Gating the GMM is no longer the fix

The mu dump's third plane makes a hybrid scorable offline: take the race
everywhere, except fall back to Huber wherever the GMM won.

| corpus | race/Huber ON | hybrid/Huber ON | GMM win rate ON |
|---|---|---|---|
| NGC7635 | 1.16x | 1.11x | 3.5% |
| M16 | 1.47x | 1.41x | 10.1% |
| M27-2023 | 1.05x | 1.01x | 5.2% |
| M27-2025 | 1.19x | 1.19x | 0.0% |

Pre-normalisation, gating the GMM was most of the cure — on NGC7635 it took
4.93x down to 1.62x. Post-normalisation it buys almost nothing, because the
residual is no longer concentrated in one model. Stage 1's "the GMM is the
villain" was true of the world before v5.0.4.0 and is not true of this one.

## 3. The Stage 1 anomaly, resolved by measurement

Stage 1 recorded, and deliberately refused to explain, that turning
normalisation off moved M27-2023 and M16 the opposite way to NGC7635 in tie
rate. The answer is neither of the two things previously guessed (variable sky,
frame count), and it is not the spot check's story either.

**The GMM is admitted to the race on all three, and converges on all three.
What differs is whether the mixture it fits is genuinely two-component.**

| corpus, normalisation OFF | GMM converged | admitted | median pi1 | pi1 in [0.2,0.8] | dAICc<2 |
|---|---|---|---|---|---|
| M16 30f | 89.0% | 78.9% | 0.542 | **35.4%** | 69.8% |
| NGC7635 65f | 98.5% | 72.1% | 0.884 | **11.1%** | 60.7% |
| M27-2023 33f | 99.6% | 75.4% | 0.937 | **0.1%** | **0.1%** |

On M27-2023 the GMM fits a mixture whose first component holds 93.7% of the
weight — degenerate in substance, but just under the 0.95 `SPIKE_OUTLIER`
threshold that would have disqualified it, so it competes and loses decisively.
A 5-parameter model describing one Gaussian cannot survive AICc against
Contamination's 4. Large gaps, no ties, GMM wins 0.0%.

Where the mixture really is two-component (M16, NGC7635) the GMM is
competitive, gaps are small, and a third to two-thirds of decisions are ties.

The spread widens under normalisation-off in *every* corpus — NGC7635 9.9x,
M27-2023 5.9x, M16 4.0x, M27-2025 1.7x, all on uniform hash samples rather than
Stage 1's file-order spot check. So spread width is not the discriminator.
Bimodality is.

### How normalisation actually kills the GMM

Not by making it lose. By making it **disqualify itself**:

| GMM fits fitting to SPIKE_OUTLIER (excluded by design) | OFF | ON |
|---|---|---|
| NGC7635 | 26.8% | 69.9% |
| M16 | 11.4% | 33.5% |
| M27-2023 | 24.3% | 39.1% |

Once per-frame sky offsets are removed, the second component has nothing real
left to describe, so it collapses onto a sliver of the data and
`model_selector.cpp:189` throws the fit out. That is a more precise account than
Stage 1's, and it is the same mechanism seen from the other side.

### The frame-count leg, as a controlled experiment

M16 was run again at 12 frames — the E2E cap, and below
`min_samples_for_gmm` = 30 — against the same corpus, code and analyser, so the
only variable is whether the GMM is eligible at all:

| M16 | 12 frames (GMM never runs) | 30 frames (GMM eligible) |
|---|---|---|
| race/Huber, norm OFF | **1.61x** | **3.47x** |
| race/Huber, norm ON | 1.52x | 1.47x |
| dAICc < 2, norm OFF | 5.6% | 69.8% |
| dAICc < 2, norm ON | 39.3% | 34.7% |

The 12-frame figures reproduce Stage 1's M16 row exactly (39.3% / 5.6%), which
also confirms Stage 1's table was measured at 12 frames while the noise ratios
it was compared against were measured at 30 — the two halves of that row were
never the same experiment.

**Admitting the GMM to the race more than doubles the pre-norm noise penalty on
identical frames: 1.61x -> 3.47x.** That is the "GMM is the villain" claim
established by controlled experiment rather than inferred from win rates.

### One mechanism, two doors

The M16 and M27-2023 results are the same finding. There are two ways for the
GMM to be effectively absent from the race:

- **Ineligible** — fewer than 30 samples (M16 at 12f, M27-2025 throughout).
- **Admitted but degenerate** — it runs, converges, is admitted, and fits a
  mixture that is one Gaussian in all but parameter count (M27-2023 OFF,
  median pi1 0.937).

Either way the race is really Student-t vs Contamination, the AICc gaps are
decisive, ties collapse, and the noise penalty stays low. Where the GMM is
*effectively* present, ties are common and the penalty is large. Stage 1's
"opposite directions" were one mechanism seen through two different doors.

*(Caveat carried openly: this explains the OFF-arm ordering and the frame-count
leg. It does not fully predict the ON-arm tie rates — NGC7635-ON has the least
bimodality and the most ties. When the GMM is absent, the tie rate appears to
track how distinguishable Student-t and Contamination are, which tracks spread;
that is consistent with four of five observations but M27-2025 barely moves. A
general model of the tie rate is NOT established here, and is not needed for any
conclusion in this document.)*

## 2. Tooling, and two defects fixed in it

`tools/research/spike_stack.cpp` runs StackingEngine headless with no
PixInsight, so a measurement never borrows `/opt/PixInsight/bin` out from under
a live imaging session. Stage 1 put it in the repo; Stage 2 fixed two things
that would have cost another session:

- **The build command existed only in a session transcript**, which is exactly
  how the previous copy was lost. `spike_stack` is now a CMake target behind
  `-DNUKEX_BUILD_RESEARCH_TOOLS=ON`, so it builds from the tree.
- **Runs must start at the repo root.** `StackingEngine` resolves the QE
  database at the *relative* path `share/qe_database.json`; the first batch died
  instantly with "QE database missing or unreadable" because the runner had
  `cd`'d to its own directory. `run_mu_corpus.sh` now resolves and enters the
  repo root itself.

The mu dump gained a **third plane** carrying the winning model's
`DistributionShape`, so a hybrid — "the race, except fall back to Huber wherever
the GMM won" — can be scored offline without re-running any stack. Without it,
the benefit of gating one model could only be inferred from an unrelated
statistic, and this ratio's driver has already been guessed wrong twice.

The plane cross-validates against the independent voxel dump: on NGC7635 it
reports 3.47% GMM / 96.04% Contamination against the voxel dump's 3.4% / 96.1%.

Reproducing:

    cmake .. -DNUKEX_BUILD_RESEARCH_TOOLS=ON && cmake --build . --target spike_stack
    tools/research/run_all_mu.sh          # the whole matrix, one corpus at a time
    tools/research/report_stage2.sh       # every table in this document

Instrumentation is env-gated and costs one `getenv` at Phase B entry when
unset. It is research code and is not shipped.

## 4. What the race costs, and what it buys

Stage 1 established the cost: the Ceres model race is **96.1% of Phase B and
65.9% of total wall time**, with all three GPU kernels plus staging extraction
making up 3.9% of Phase B.

Huber's cost is below this measurement's noise floor. The instrumented runs
compute `huber_irls` for **every** voxel and channel and write a 100 MB dump,
and NGC7635's Phase B came in at 59.6 s against Stage 1's uninstrumented
60.3 s. Fitting a Huber M-estimator to all 8.3 million voxels costs less than
the run-to-run variation of the thing it would replace.

Phase B wall time across the matrix:

| corpus | Phase B OFF | Phase B ON |
|---|---|---|
| NGC7635 | 76.0 s | 59.6 s |
| M27-2025 | 101.1 s | 98.8 s |
| M16 | 385.7 s | 334.6 s |
| M27-2023 | 292.7 s | **467.3 s** |

**A Stage 1 generalisation needs correcting here.** Stage 1 concluded from
NGC7635 that "the race got 1.28x FASTER on cleaner input — the old pipeline
paid twice." NGC7635 reproduces exactly (1.28x), and M16 and M27-2025 agree in
direction (1.15x, 1.02x). M27-2023 goes the other way: normalisation makes its
Phase B **1.60x slower**. Once the sky offsets are gone its GMM stops fitting
degenerate mixtures and starts doing real optimisation work. The "pays twice"
framing holds on three of four corpora, not universally.

### What the race's extra outputs are used for

Nothing, presently. `ModelSelector` produces `true_signal_estimate`,
`signal_uncertainty`, `confidence` and `shape` per voxel-channel. The pixel
value goes to `PixelSelector` and becomes the stacked image. The other three
feed `OutputAssembler::assemble_quality_map()`, which is cropped and assigned to
`ExecuteResult::quality_map` — and **no in-tree consumer reads it**: not
`NukeXInstance`, not the composer, not the stretches, not the tests.
`SubcubeVoxel::quality_score` has no reader anywhere.

So the race's only live output today is the pixel value.

## 5. The decision

The question `project_stacking_noise_vs_huber` posed was: is per-voxel
distribution fitting the **product thesis**, or a **means** to the best pixel
value? Stage 1 could not answer it because the noise comparison had not been
re-run post-normalisation. It has now.

**What is settled:**

- The safety case is closed. Post-normalisation the race is not dangerous; the
  worst corpus costs ~1.35x of effective exposure, not ~24x.
- **The race is nonetheless still the noisier estimator on all four corpora**
  (1.05x, 1.16x, 1.19x, 1.47x). It is not buying lower noise. It never wins on
  this axis.
- It costs ~65.9% of total wall time. The alternative costs less than the
  measurement noise floor.
- Its non-pixel outputs currently reach no consumer.
- Huber preserves signal at the bright end on 11 of 12 channels, and on M16
  preserves *more* than the race does. One channel of one corpus is a genuine
  open question.

**What is not settled, and is the user's call:**

Whether per-voxel distribution fitting is worth keeping *as the product's
scientific thesis* — the thing that distinguishes NukeX from a sigma-clipped
average — given that on present evidence it is slower and slightly noisier than
a 40-line M-estimator, and that the classification it produces is not yet
surfaced to anyone.

Three coherent positions, all defensible:

1. **Thesis.** Keep the race, and make its extra outputs earn their keep by
   surfacing the quality map, shape classification and confidence. The
   measurement then has to be redone against *that* deliverable, not against mu.
2. **Means.** Replace the race with Huber for the pixel value. Cuts ~66% of
   wall time and lowers noise on every corpus measured. Loses a differentiator
   that is currently invisible to users.
3. **Both, gated.** Huber for the pixel value by default; run the race only
   where it can matter and where its output is used. Note that gating on the
   GMM specifically no longer helps (section 2) — a useful gate would need a
   different criterion, and none is identified here.

This document does not choose. It is a measurement.

## 6. Follow-ups this raised, none of them filed as stubs

- **The noise map cannot see estimator noise.** `PixelSelector` computes
  `out_noise` from a CCD model, so NukeX's own diagnostic could not have
  detected the 5x penalty at any point. Whatever is decided above, this is worth
  fixing on its own: a noise map that reports modelled rather than realised
  scatter will hide the next such defect too.
- **M27-2025 channel 3, top 0.1%.** Race and Huber differ by ~3-7% there and
  agree to 0.05% everywhere else on that channel. Direction of correctness
  unknown.
- **A general model of the tie rate.** Section 3 explains the OFF-arm ordering
  but not the ON-arm one.
