# ML Pixel Selector Design Document

**Date**: 2026-02-19
**Status**: Design / Scaffolding
**Author**: ASTRO-ML

## 1. Problem Statement

The current `PixelStackAnalyzer::SelectBestValue` uses hand-crafted, class-specific
heuristics to choose a pixel value from a stack of N registered subframes. These
heuristics encode reasonable domain knowledge (e.g., "for DarkExtended, pick the
minimum non-outlier value") but cannot capture the full complexity of the decision
space. A learned model can discover non-linear interactions between distribution
statistics, segmentation class, spatial context, and per-frame metadata that the
hand-tuned rules miss.

**Key constraint**: The model is called once per pixel per channel. For a 4096x4096
RGB image, that is ~50 million invocations per integration run. Inference must be
extremely fast -- sub-microsecond per pixel in batched mode.

## 2. Input Features

The feature vector for each pixel position combines four groups of information.
All features are pre-computed by existing NukeX infrastructure.

### 2.1 Distribution Statistics (6 floats)

From `StackDistributionParams`, computed by `PixelStackAnalyzer::FitDistribution`:

| Feature | Source | Description |
|---------|--------|-------------|
| `mu` | `dist.mu` | Location parameter (robust mean/median) |
| `sigma` | `dist.sigma` | Scale parameter (robust std dev) |
| `skewness` | `dist.skewness` | Distribution asymmetry |
| `kurtosis` | `dist.kurtosis` | Tail weight |
| `quality` | `dist.quality` | Goodness of fit (0-1) |
| `dist_type` | `dist.type` | Distribution type (one-hot encoded: 5 types) |

Distribution type is one-hot encoded to 5 floats (Gaussian, Lognormal, Skewed,
Bimodal, Uniform), giving **10 floats** total for this group.

### 2.2 Segmentation Context (12 floats)

From `RegionClass` and `SpatialContext`:

| Feature | Source | Description |
|---------|--------|-------------|
| `class_onehot[8]` | `RegionClass` | One-hot encoded 8-class label |
| `class_confidence` | `GetClassConfidence()` | ML confidence in segmentation |
| `num_matching_neighbors` | `SpatialContext` | How many of 8 neighbors share class (normalized to 0-1) |
| `is_transition_zone` | `SpatialContext` | Binary flag |
| `dominant_neighbor_class_matches` | `SpatialContext` | Binary: does dominant neighbor match center? |

**12 floats** total for this group.

### 2.3 Stack Summary Statistics (6 floats)

Derived from the N candidate values at this pixel position:

| Feature | Source | Description |
|---------|--------|-------------|
| `stack_median` | Computed | Median of all N values |
| `stack_iqr` | Computed | Interquartile range (P75-P25) |
| `stack_range` | Computed | Max - Min across frames |
| `outlier_fraction` | `outlierCount / totalFrames` | Fraction of frames rejected |
| `valid_frame_count` | Computed | Number of non-outlier frames (normalized by max) |
| `weighted_mean` | Computed | Frame-weight-adjusted mean |

**6 floats** total for this group.

### 2.4 Per-Frame Candidate Features (N x 3 floats, padded/truncated)

For each of the N candidate frames, we provide:

| Feature | Description |
|---------|-------------|
| `value` | The pixel value from this frame |
| `z_score` | (value - mu) / sigma |
| `frame_weight` | Quality weight for this frame (1.0 if unweighted) |

**Handling variable N**: The model uses a fixed maximum frame count `N_MAX = 64`.
Stacks with fewer frames are zero-padded, with an attention mask to ignore padding.
Stacks exceeding 64 frames are truncated to the 64 highest-weight frames.

Per-frame features: **N_MAX x 3 = 192 floats**.

### 2.5 Total Feature Vector

| Group | Floats |
|-------|--------|
| Distribution stats | 10 |
| Segmentation context | 12 |
| Stack summary | 6 |
| **Global features subtotal** | **28** |
| Per-frame candidates | 192 |
| Frame mask (valid flags) | 64 |
| **Total** | **284** |

## 3. Output

### 3.1 Primary Output: Per-Frame Quality Scores

The model outputs a score for each of the N_MAX candidate frames:

```
output: float[N_MAX]  (64 values, softmax-normalized)
```

The selected frame is `argmax(output * mask)`. The selected value is
`candidates[selected_frame].value`.

**Why scores instead of a single frame index?** Softmax scores over candidates
allow:
- Soft blending in transition zones (weighted average of top-K frames)
- Confidence estimation (entropy of the distribution)
- Gradient flow during training (cross-entropy loss on soft targets)

### 3.2 Secondary Output: Confidence

Confidence is derived from the output distribution:
```
confidence = 1.0 - entropy(softmax_scores) / log(valid_frame_count)
```

High confidence = one frame strongly preferred. Low confidence = multiple frames
roughly equivalent (which is fine -- it means median-like selection is correct).

## 4. Model Architecture

### 4.1 Design: SetTransformer-lite

A pure MLP cannot reason about set-structured input (N candidates with no inherent
ordering). Instead, we use a minimal set-attention architecture:

```
Global Features (28)
       |
  Linear(28, 64) + ReLU
       |
       v
  global_embed (64)

Per-Frame Features (N_MAX x 3)
       |
  Linear(3, 64) + ReLU       (shared across frames)
       |
       v
  frame_embeds (N_MAX x 64)

  ┌─────────────────────────────┐
  │  Cross-Attention Block      │
  │  Q = frame_embeds           │
  │  K,V = global_embed (tiled) │
  │  + frame_embeds residual    │
  │  1 head, dim=64             │
  └─────────────────────────────┘
       |
  conditioned_embeds (N_MAX x 64)
       |
  Linear(64, 32) + ReLU
  Linear(32, 1)               (per-frame score)
       |
  Masked Softmax (mask out padding)
       |
  output: float[N_MAX]
```

### 4.2 Parameter Count

| Layer | Parameters |
|-------|-----------|
| Global Linear(28, 64) | 1,856 |
| Frame Linear(3, 64) | 256 |
| Cross-Attention (Q,K,V projections) | 3 x 64 x 64 = 12,288 |
| Output Linear(64, 32) | 2,080 |
| Output Linear(32, 1) | 33 |
| **Total** | **~16,500** |

This is intentionally tiny. At ~16K parameters, the ONNX model will be under 100KB
and inference is dominated by memory access, not compute.

### 4.3 Why Not a Pure MLP?

A pure MLP would require flattening all N_MAX x 3 per-frame features into a fixed
vector, making it order-dependent (frame 1 vs frame 5 would have different weights).
The set-attention approach is permutation-equivariant over frames, which is the
correct inductive bias: the identity of frame indices should not matter, only their
statistical properties.

### 4.4 Fallback: Pure MLP Alternative

If the attention model proves too slow in ONNX Runtime, a simpler fallback:

```
Input: [global_features(28), sorted_candidate_stats(64*3)] = 220
Linear(220, 128) + ReLU
Linear(128, 64) + ReLU
Linear(64, N_MAX) + Masked Softmax
```

Here candidates are **sorted by value** before input, making the MLP
effectively permutation-invariant. Parameter count: ~30K. This is the backup plan.

## 5. Training Data Generation

### 5.1 Strategy: Expert-Stack Ground Truth

Training data is generated by running NukeX's existing rule-based stacking on
high-quality datasets, then using the result as "ground truth". The model learns
to replicate and generalize from the expert system's decisions.

### 5.2 Ground Truth Sources

1. **Existing NukeX stacking runs**: Process known-good frame stacks through the
   current `SelectBestValue` pipeline. The selected frame index per pixel IS the
   label.

2. **Manual expert stacking**: For specific problematic cases (clouds, satellites,
   trailing), manually identify the best frame per pixel using blink comparison
   in PixInsight.

3. **Synthetic degradation**: Take a single pristine frame, create N synthetic
   "subframes" by adding different noise/artifacts, then the label is always
   frame 0 (the pristine one).

### 5.3 Data Generation Pipeline

```
Input: directory of N aligned FITS subframes + ground truth stacked image

For each sampled pixel (x, y):
  1. Read values[N] from all frames at (x,y)
  2. Compute distribution stats (mu, sigma, skewness, kurtosis, quality, type)
  3. Read segmentation class from pre-computed segmentation map
  4. Compute spatial context (neighbor classes)
  5. Compute stack summary stats (median, IQR, range, etc.)
  6. Label = argmin_f |values[f] - ground_truth[x,y]|
     (which frame's value is closest to the ground truth pixel?)
  7. Store feature vector + label

Output: numpy arrays (features.npy, labels.npy)
```

### 5.4 Sampling Strategy

Not every pixel needs to be in the training set. Sampling priorities:
- **Uniform random**: 50% of samples
- **Transition zones**: 20% (where segmentation class changes)
- **High-variance pixels**: 15% (where frames disagree most)
- **Class-stratified**: 15% (ensure rare classes like DarkExtended are represented)

Target: ~1M samples per stacking run, ~10M total for initial training.

### 5.5 Label Ambiguity

When multiple frames have values within noise of the ground truth, the label is
ambiguous. We handle this with **soft labels**: instead of a hard one-hot label,
use a softmax over frame distances:

```python
distances = |values - ground_truth|
soft_label = softmax(-distances / temperature)
```

With `temperature = noise_estimate`, this gives a peaked distribution when one
frame is clearly best, and a flat distribution when multiple frames are equivalent.

## 6. Training Procedure

### 6.1 Loss Function

KL-divergence between predicted score distribution and soft label distribution:

```python
loss = KLDivLoss(log_softmax(predicted_scores), soft_labels)
```

This is more appropriate than cross-entropy with hard labels because it handles
label ambiguity naturally.

### 6.2 Training Configuration

- **Optimizer**: AdamW, lr=1e-3, weight_decay=1e-4
- **Scheduler**: CosineAnnealingLR over 50 epochs
- **Batch size**: 4096 (each sample is a single pixel, very small)
- **Mixed precision**: FP16 (AMP)
- **GPU**: RTX 5070 Ti (16GB) -- this model fits easily, batch size is the bottleneck
- **Epochs**: 50 (early stopping on validation loss)

### 6.3 Validation

- 80/10/10 train/val/test split
- Stratified by segmentation class
- Test set includes held-out stacking runs (different targets)

### 6.4 Metrics

- **Frame selection accuracy**: Does the model pick the same frame as ground truth?
- **Value MSE**: (selected_value - ground_truth_value)^2
- **Per-class accuracy**: Breakdown by segmentation class
- **Top-3 accuracy**: Is the ground truth frame in the model's top 3?

## 7. Integration into NukeX C++ Code

### 7.1 ONNX Export

The trained PyTorch model is exported to ONNX:

```python
torch.onnx.export(model,
    (global_features, frame_features, frame_mask),
    "pixel_selector.onnx",
    input_names=["global_features", "frame_features", "frame_mask"],
    output_names=["frame_scores"],
    dynamic_axes=None)  # Fixed shapes for maximum ONNX Runtime optimization
```

### 7.2 Integration Point

In `PixelStackAnalyzer::SelectBestValue`, add an alternative code path:

```cpp
// In SelectBestValue:
if (m_useMLSelector && m_mlModel != nullptr)
{
    // Build feature vector from existing computed values
    // Run ONNX inference (batched across row)
    // Return selected value from highest-scoring frame
}
else
{
    // Existing hand-crafted logic (unchanged)
}
```

The ML path is **opt-in** via a configuration flag. The hand-crafted rules remain
as the default and fallback.

### 7.3 Batched Inference

Per-pixel ONNX calls would be far too slow. Instead, batch across an entire row:

```cpp
// In ProcessStackWithMetadata, row-level batching:
// Collect feature vectors for all pixels in row (width x 284 floats)
// Single ONNX inference call: input shape [width, 284]
// Scatter results back to per-pixel metadata
```

This amortizes ONNX Runtime overhead across `width` pixels per call.

### 7.4 Conditional Compilation

Like the existing segmentation model, the ML pixel selector is behind a
compile flag:

```cpp
#ifdef NUKEX_USE_ONNX
    // ML pixel selector available
#endif
```

## 8. Performance Budget

### 8.1 Targets

| Metric | Budget |
|--------|--------|
| Per-pixel inference | < 1 microsecond (batched) |
| Per-row inference | < 4 ms (4096 pixels) |
| Full 4K image, 3 channels | < 50 seconds ML overhead |
| Model file size | < 100 KB |
| Memory overhead | < 50 MB (model + buffers) |

### 8.2 Comparison to Current

The current hand-crafted `SelectBestValue` takes approximately 0.1-0.5 microseconds
per pixel (branch-dependent). The ML model must stay within 10x of this budget,
which is achievable with the ~16K parameter model using ONNX Runtime's optimized
graph execution.

### 8.3 Optimization Strategies

1. **Row-level batching**: Single ONNX call per row, not per pixel
2. **FP16 inference**: ONNX Runtime supports FP16 on CPU (AVX2)
3. **Fixed input shapes**: No dynamic axes, allowing maximum graph optimization
4. **ONNX graph optimization**: Level 3 (extended) optimization in ORT
5. **Feature pre-computation**: Distribution stats are already computed; ML adds
   only the inference step

## 9. Risks and Mitigations

| Risk | Mitigation |
|------|-----------|
| Model too slow | Fallback to MLP architecture; row batching |
| Training data bias (learns current heuristics only) | Add synthetic degradation + manual expert labels |
| ONNX Runtime version conflicts | Pin ORT version, test in CI |
| Class imbalance (Background dominates) | Class-stratified sampling in data generation |
| Overfitting on small training set | Dropout (0.1), weight decay, early stopping |
| Label ambiguity kills training signal | Soft labels with temperature scaling |

## 10. Implementation Order

1. **generate_selection_training_data.py** -- Extract features + labels from existing stacks
2. **train_pixel_selector.py** -- Model definition + training loop + ONNX export
3. **Validate on held-out data** -- Ensure model matches/beats heuristic accuracy
4. **C++ integration** -- Wire ONNX model into SelectBestValue (behind flag)
5. **A/B testing** -- Compare ML vs heuristic on real stacking runs
6. **Ship as opt-in** -- User can enable ML selector in NukeX parameters

## 11. File Inventory

| File | Purpose |
|------|---------|
| `docs/plans/2026-02-19-ml-pixel-selector-design.md` | This document |
| `training_data/scripts/generate_selection_training_data.py` | Training data extraction |
| `training_data/scripts/train_pixel_selector.py` | Model + training loop + ONNX export |
| `src/engine/PixelStackAnalyzer.h` | Add `m_useMLSelector` flag (future) |
| `src/engine/PixelStackAnalyzer.cpp` | Add ML code path in SelectBestValue (future) |
