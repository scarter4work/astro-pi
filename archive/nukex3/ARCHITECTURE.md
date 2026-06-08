# NukeX Stacker — Version 3 Architecture

## Overview

NukeX v3 is a complete architectural rethink of the stacking approach. Previous versions 
attempted ML-based per-pixel segmentation with an ONNX model trained on arcsinh-stretched 
subs. V3 abandons that approach in favor of a mathematically rigorous per-pixel statistical 
inference model operating entirely on **unstretched linear data**.

The goal is a real PixInsight Process Module (PCL/C++) — not a script — that produces 
demonstrably better stacks than WBPP by making per-pixel rejection decisions informed by 
per-sub quality metadata.

---

## Core Concept: The 3D Sub Cube

### Data Model

The fundamental data structure is a 3D matrix where:

- **X, Y** = spatial pixel position
- **Z** = sub index (0 to N-1)
- Each Z slice = one calibrated, registered sub
- Each (X,Y) column through Z = the complete value history of that pixel across all subs
- FITS header metadata rides along as per-Z attributes

```
Sub Stack (Z axis)
     z=0  (Sub 1)   ─────────────────
     z=1  (Sub 2)   ─────────────────
     z=2  (Sub 3)   ─────────────────
     ...
     z=N  (Sub N)   ─────────────────
                          │
                    (X,Y) column = pixel distribution across all subs
```

### Key Insight

The selected "true" pixel value does **not** have to come from the same Z index across 
the entire image. Pixel (100,200) might come from sub 7 while pixel (101,200) comes from 
sub 23. This is **per-pixel selection**, not per-sub selection — fundamentally different 
from everything WBPP does and a genuine advancement in stacking methodology.

---

## Algorithm: Per-Pixel Distribution Inference

For each (X,Y) column in the 3D cube:

### Step 1 — Build the value vector
Collect all Z values for this pixel position. Apply per-Z quality weights derived from 
FITS metadata (see Metadata Weighting below).

### Step 2 — Distribution characterization
Test the value vector against candidate distributions:
- **Normal/Gaussian** — clean sky background pixels
- **Skewed** — contamination present (satellite trail, cosmic ray) in specific Z slices
- **Bimodal** — target moved between subs, or variable star present
- **Poisson** — very low signal pixels near sky background

Use AIC/BIC scoring to select the best-fit distribution model per pixel. This means 
rejection parameters are chosen **per pixel** rather than globally — a fundamental 
improvement over classical sigma clipping which assumes Gaussian everywhere.

### Step 3 — Select the highest probability Z value
Given the fitted distribution, select the Z value with the highest likelihood of 
representing the true signal. Reject outlier Z values (cosmic rays, satellites, 
thermal noise spikes) identified as low-probability under the fitted model.

### Step 4 — Record provenance
Store which Z index was selected for each (X,Y) position. This provenance map is 
valuable for diagnostics and visualization.

---

## Metadata Weighting

Each Z slice (sub) carries FITS header data that informs per-pixel weighting:

| FITS Attribute | Weight Effect |
|---|---|
| FWHM | Higher FWHM = blurrier sub = downweight |
| Eccentricity | Tracking error indicator = downweight |
| Sky background | Moon/light pollution contamination = downweight |
| HFR (Half-Flux Radius) | Overall sub quality score |
| Altitude | Atmospheric refraction proxy |

A sub with one satellite trail is **locally bad** — reject specific pixels, not the 
whole sub. A sub with bad seeing is **globally soft** — downweight everything from it. 
This partial sub rejection at the pixel level is something WBPP cannot do.

---

## Implementation Stack

### Core Framework
- **PCL/C++** — Real PixInsight Process Module, not a script
- **Eigen** — Matrix math, equivalent to NumPy for C++
- **Boost.Math** — Distribution fitting, AIC/BIC scoring, statistical functions

### PCL Process Architecture
- `ProcessImplementation` — Core stacking algorithm
- `ProcessInterface` — User-facing UI, parameters, preview
- `ProcessParameter` — User configurable weights and thresholds
- Real-time preview through PCL's standard preview mechanism

### Development Tooling
- PJSR Language Server (custom built) — provides Claude Code accurate PCL API 
  signatures, class hierarchies, and method signatures to prevent hallucination 
  during AI-assisted development

---

## Variable Stack Depth Handling

Stack depth ranges from ~40 to ~500 subs depending on the imaging session. The 
architecture handles this naturally because:

- The (X,Y) column distribution fitting operates on whatever Z values exist
- No padding or truncation needed
- AIC/BIC scoring naturally accounts for sample size
- Eigen matrix operations are size-agnostic at runtime

---

## Visualization (Post-V1)

The ideal visualization — a 3D point cloud per pixel showing Z distribution with 
accepted/rejected values color coded — is deferred until the core algorithm is proven.

**Note:** PCL WebView was evaluated and ruled out due to implementation constraints. 
Options for a future visualization companion:

- **Separate companion app** (Electron or web) that reads NukeX output data files
- NukeX writes a provenance/distribution data file (JSON or FITS extension)
- Companion app renders the 3D pixel distribution viewer independently
- User workflow: run NukeX in PixInsight → click Analyze → companion app opens

This is not as seamless as embedded visualization but it is real and shippable. 
The visualization is a feature. A better stack is the product.

---

## Training Data (if ML re-introduced in future versions)

If a learned component is reintroduced in a future version:

- Train exclusively on **unstretched linear subs** — stretch destroys the statistical 
  signal the model needs
- Use real-world data from multiple imagers and target types for generalization
- Input vector = statistical features derived from the Z column (mean, median, std dev, 
  percentiles, skewness, kurtosis, stack depth) — fixed size regardless of stack depth
- Ground truth = known-good manually verified stacks

---

## Development Strategy

### Branch: v3
Start fresh from the data structure layer up. Do not attempt to salvage v1/v2 
ML architecture.

### Phase 1 — Data Model
Get the 3D sub cube loading correctly with FITS headers attached per Z slice. 
Prove the data model before touching statistics.

### Phase 2 — Statistical Selection
Implement per-pixel distribution fitting and Z value selection. Benchmark against 
WBPP on same dataset. This is the core deliverable.

### Phase 3 — PCL Process UI
Wire up ProcessInterface, parameters, preview. Make it a first-class PixInsight citizen.

### Phase 4 — Visualization (TBD)
Companion app or native PCL graphics for distribution visualization and provenance map.

---

## Why This Beats WBPP

| Capability | WBPP | NukeX v3 |
|---|---|---|
| Rejection parameters | Global or regional | Per-pixel |
| Distribution assumption | Gaussian everywhere | Fitted per pixel |
| Partial sub rejection | No — all or nothing | Yes — pixel level |
| Sub quality weighting | Limited | Full FITS metadata |
| Provenance tracking | No | Yes — per pixel Z map |
| Variable stack depth | Yes | Yes |

---

## Open Questions

- Eigen vs raw PCL matrix types for the sub cube — evaluate memory layout implications 
  for large stacks (500 subs × 4K image = significant RAM)
- Whether Boost.Math distribution fitting performance is acceptable at per-pixel scale 
  or needs optimization (SIMD, threading via PCL's thread pool)
- Companion app technology choice if visualization is pursued
- PixInsight certified developer submission timeline

---

*Architecture document derived from design session — March 2026*
