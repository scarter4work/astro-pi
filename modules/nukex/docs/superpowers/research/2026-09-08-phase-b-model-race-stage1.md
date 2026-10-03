# Phase B model race — Stage 1 measurement

**Status:** measurement only, no product change. Ends in a decision, not a fix.

Run 2026-09-08 on the research branch `research/phase-b-model-race`, which
restores the reverted instrumentation from `6b3d874` (`NUKEX_DUMP_VOXELS` in
`model_selector.cpp`, Huber IRLS + `NUKEX_DUMP_MU` in `gpu_executor.cpp`) and
adds a headless driver, `tools/research/spike_stack.cpp`, so none of this needs
PixInsight.

## 1. Where Phase B's time actually goes

From the v5.0.4.2 E2E logs, 38 batches across all seven stacks — no new runs:

| stage | time | share of Phase B |
|---|---|---|
| staging extract + kernels 1 & 2 (GPU) | 25.7 s | 2.5% |
| **Ceres model race (CPU)** | **1000.1 s** | **96.1%** |
| kernel 3 + writeback (GPU) | 14.9 s | 1.4% |

**The race is 65.9% of total wall time.** Everything else in Phase B is 3.9%.

For context, the phase split on these four corpora, post-frame-major:
measuring 10.7%, Phase A 20.7%, Phase B 68.6%. The spec's section 1 claim that
"Phase A is the problem, not Phase B" was measured on a 156-frame M63 batch
that is not in the E2E manifest and does not generalise: Phase A is per-frame,
Phase B is per-voxel, so which dominates is a property of the batch shape.

## 2. Controlled experiment — the mechanism, confirmed

Same corpus, same frames, single variable (`Config::normalize_frames`):

| NGC7635, 65f mono | normalisation OFF | normalisation ON |
|---|---|---|
| GMM win rate | **23.6%** | **3.4%** |
| dAICc < 2 vs runner-up | 60.7% | 50.9% |
| Phase B wall time | 76.9 s | 60.3 s |

This is the hypothesis in `project_stacking_noise_vs_huber` tested rather than
inferred: **the GMM was winning on per-frame sky level variation.** Normalising
it away collapses the win rate ~7x. The race also got 1.28x FASTER on cleaner
input — when the sample really is bimodal the GMM optimiser converges to a
genuine two-component solution and works harder to get there. The old pipeline
paid twice for unnormalised input: extra fitting work, and the coin-flip noise
that fitting injected.

## 3. What the race decides today (post-normalisation, as shipped)

| corpus | Student-t | GMM | Contamination | dAICc<2 | GMM eligible |
|---|---|---|---|---|---|
| NGC7635 65f mono | 0.5% | 3.4% | **96.1%** | 50.9% | yes |
| M27-2023 33f OSC | 1.8% | 5.3% | **92.9%** | 35.7% | yes |
| M16 12f dual-NB | 10.4% | 0.0% | **89.1%** | 39.3% | **no** |
| M27-2025 72f LRGB | 0.7% | 0.0% | **97.3%** | 46.2% | **no** |

Two things follow, neither of which needed a product decision to establish:

- **Contamination wins 89-97% everywhere.** Three optimisers run per voxel per
  channel to reach that answer.
- **GMM does not run at all below 30 samples** (`min_samples_for_gmm`). M16 has
  12 frames; M27-2025 is LRGB-mono with ~18 frames per filter. So on those the
  "three-way race" is already only two-way, and nobody noticed.
- **35-51% of decisions are within dAICc < 2 of the runner-up** — statistical
  ties by the code's own cited authority (Burnham & Anderson 2002).

## 4. UNRESOLVED — do not guess

Switching normalisation off moves M27-2023 and M16 the **opposite** way to
NGC7635:

| corpus | dAICc<2 ON | dAICc<2 OFF | GMM ON | GMM OFF |
|---|---|---|---|---|
| NGC7635 | 50.9% | **60.7%** | 3.4% | **23.6%** |
| M27-2023 | 35.7% | **0.1%** | 5.3% | **0.0%** |
| M16 | 39.3% | **5.6%** | 0.0% | 0.0% |
| M27-2025 | 46.2% | 46.5% | 0.0% | 0.0% |

On NGC7635, removing normalisation makes the models harder to tell apart. On
M27-2023 and M16 it makes them dramatically EASIER (0.1% ties). No explanation
is offered here on purpose. A spot check of the first 4000 records of
M27-2023 shows the median robust scale rising 5x with normalisation off
(0.000215 -> 0.00109) and the AICc gaps widening with it, which is consistent
with "wider spread makes Contamination decisively better" — but that check is
biased: those records are in file-write order, and the ones inspected had 1-5
samples per voxel, so they are low-coverage edge voxels rather than a fair
sample. **The full-file win rates above are unbiased (hash-sampled, 1 in 512);
the scale diagnostic is not.** Redo it over a uniform sample before believing
any story about it.

The record in `project_stacking_noise_vs_huber` is that the driver of the
race/Huber ratio has been guessed wrong twice already. This is the third
opportunity and it should be measured, not reasoned.

## 5. What Stage 1 did NOT answer

- **Win rate against the recorded noise ratios.** The ratios (M27-2023 1.00x,
  M27-2025 1.28x, M63 3.15x, M16 3.77x, NGC7635 5.19x) were all measured
  pre-normalisation on 2026-09-07. Pre-norm win rates are now in hand for four
  corpora, but they do not line up monotonically: NGC7635 23.6% at 5.19x
  against M63's recorded 30.7% at 3.15x. Win rate alone does not explain the
  ratio.
- **Race vs Huber re-measured post-normalisation.** `NUKEX_DUMP_MU` and the
  Huber IRLS estimator are restored and build, but were not run. That is the
  next measurement.

## 6. Reproducing

    g++ -std=c++17 -O2 -fopenmp -o spike_stack tools/research/spike_stack.cpp \
        -I... (see the session transcript; links the nukex4_*.a plus
        -lcfitsio -lceres -lOpenCL -lz -ldl -lpthread)

    NUKEX_DUMP_VOXELS=out.bin NUKEX_DUMP_MAX=500000 \
      [NUKEX_SPIKE_NO_NORM=1] [NUKEX_SPIKE_MAX_FRAMES=12] \
      ./spike_stack <lights-dir>

    python3 tools/research/analyze_voxels.py out.bin "label"

Run corpora ONE AT A TIME. Do not use PixInsight for this — the driver exists
so a measurement never has to borrow /opt/PixInsight/bin out from under a live
imaging session.
