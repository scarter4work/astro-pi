# AutoContrast — Design Document v0.1

**Status:** Initial design. Pre-implementation.
**Target host:** PixInsight (PJSR primary, Python sidecar for metrics/VLM)
**Author context:** Handoff spec for Claude Code. Read §2 and §12 before writing any code.

---

## 1. Purpose

An iterative, reference-guided post-processing tool for PixInsight that takes a stretched-but-flat
astrophoto and drives it toward the *presentation characteristics* of a high-quality professional
render of the same target — without inventing signal the acquisition data does not contain.

The tool converges on a result by repeatedly:

1. Measuring the working image against a **fingerprint** derived from a professional reference.
2. Selecting a bounded transform action expected to reduce fingerprint distance.
3. Applying it, re-measuring, and either keeping, branching, or rolling back.

It is **not** a style-transfer network. It is a guardrailed optimizer over stock PixInsight
processes, with a vision model acting as a comparative director rather than a scorer.

---

## 2. Core Theses (Non-Negotiable)

These four constraints define the project. Violating any of them produces a tool that manufactures
data, and the whole thing becomes worthless. Do not relax them for convenience.

### 2.1 Physics governs linear space. Taste governs nonlinear space.

Channel ratios (SII/Hα, OIII/Hα) in calibrated linear masters are real astrophysics — the SII/Hα
ratio is a classical discriminator between HII regions and shocked/SNR emission. These ratios are
**destroyed** by nonlinear stretch and cannot be recovered from a professional render's published
JPEG/TIFF.

Therefore:

- Channel weighting / color balance constraints are derived **from the user's own linear stack**,
  before any stretch.
- The fingerprint is permitted to influence **only** the nonlinear presentation layer: stretch
  shape, local contrast distribution, tonal placement, saturation structure.
- **The reference's color cloud must never be allowed to push channel ratios.** Any code path that
  lets it is a bug, not a tuning knob.

Mnemonic: *the reference tells us how to present; the user's data tells us what is there.*

### 2.2 Scale is measured in arcseconds, never pixels.

Wavelet energy per scale is instrument-dependent and meaningless across telescopes.

- Reference example: HST/ACS ≈ 0.05 "/px.
- Target example (this user): Askar 71F @ 490mm + ASI585MC (2.9 µm) ≈ **1.22 "/px**.

Scale-1 wavelet energy in an HST render lives more than an order of magnitude below the target's
optical resolving limit. Chasing it will drive the optimizer to sharpen noise into artifacts trying
to reach structure that is not in the photons.

Therefore: every fingerprint stores its own pixel scale and PSF FWHM, all spectra are expressed in
angular units, and **all comparisons are band-limited to the overlap of the two instruments'
resolvable bands** (§4.4).

### 2.3 Palette achievability is a hard gate.

If the user shot broadband LRGB and the reference is a Hubble-palette SHO composite, matching the
reference's chroma fingerprint would require synthesizing narrowband signal from broadband subs.

Therefore: fingerprints are stored **per palette class**. A reference whose palette class does not
match the acquisition's palette class may contribute its **structural and tonal** fingerprint
components only. Its chroma component is discarded.

### 2.4 The reference is a direction, not a destination.

Many hero renders are mosaics, composites, or heavily-worked artistic treatments that are
unreachable from a backyard 71mm refractor under Bortle 5–6. The optimizer moves toward the target
and **stops when its own deterministic gains flatline** — it never demands arrival, and fingerprint
distance is never required to reach zero.

---

## 3. System Architecture

```
┌─────────────────────────────────────────────────────────────┐
│ PixInsight (PJSR)                                           │
│  • Orchestrator / UI                                        │
│  • Plate solve (ImageSolver)                                │
│  • Process execution (HDRMT, MMT/MLT, Curves, HT, LHE,      │
│    ColorSaturation, BackgroundNeutralization, ...)          │
│  • StarDetector, MRS noise evaluation                       │
│  • Checkpoint / rollback of ImageWindow state               │
└───────────────┬─────────────────────────────────────────────┘
                │  file-protocol handoff (see §3.1)
┌───────────────▼─────────────────────────────────────────────┐
│ Python sidecar (localhost)                                  │
│  • Fingerprint extraction & distance                        │
│  • Metric computation not native to PI                      │
│  • Fingerprint DB (SQLite local cache)                      │
│  • Cone-search + reference ingest (MAST / ESASky / ESO)     │
│  • Ollama client (VLM director)                             │
│  • Optional: mothership sync client                         │
└─────────────────────────────────────────────────────────────┘
```

### 3.1 PJSR ↔ sidecar transport

PJSR exposes a `NetworkTransfer` object. **Verify POST support in the user's actual PI build before
depending on it.** If it is too limited or unstable, fall back to a file protocol:

- PJSR writes `state.json` + a downsampled preview PNG to a work dir.
- Sidecar polls, computes, writes `verdict.json`.
- PJSR polls for `verdict.json`, applies, repeats.

The file protocol is ugly but reliable and avoids blocking the PI event loop. **Default to the file
protocol for Phase 2; treat NetworkTransfer as an optimization.**

> Design constraint: the sidecar must be able to run standalone with no PI present, driven from
> FITS/XISF on disk. This is required for the offline evaluation harness (§10, Phase 0).

---

## 4. The Fingerprint

A fingerprint is a fixed-length statistics vector plus metadata. **It contains no image data.**

### 4.1 Record schema

```json
{
  "schema_version": "0.1",
  "id": "uuid",
  "wcs": {
    "ra_deg": 83.822,
    "dec_deg": -5.391,
    "fov_arcmin": [ 42.1, 28.0 ],
    "rotation_deg": 0.0
  },
  "instrument": {
    "pixel_scale_arcsec": 0.05,
    "psf_fwhm_arcsec": 0.11,
    "psf_measured": true
  },
  "palette": {
    "class": "SHO",
    "channel_map": { "R": "SII", "G": "Ha", "B": "OIII" }
  },
  "fingerprint": {
    "energy_spectrum": { "bin_edges_arcsec": [...], "normalized_energy": [...] },
    "tonal_quantiles": [...],
    "chroma_hist": { "bins_a": 16, "bins_b": 16, "counts": [...] },
    "saturation_quantiles": [...],
    "background": {
      "floor_to_peak_ratio": 0.031,
      "structure_to_gradient_ratio": 2.84,
      "channel_balance": [1.00, 0.97, 1.03]
    }
  },
  "provenance": {
    "source_type": "professional_render",
    "source_url": "https://...",
    "license": "CC BY 4.0",
    "attribution": "ESA/Hubble & NASA",
    "ingested_utc": "2026-07-08T00:00:00Z",
    "wcs_source": "header|manual|solved"
  }
}
```

### 4.2 Components

| Component | Definition | Purpose |
|---|---|---|
| `energy_spectrum` | Multiscale (MLT/MMT à trous) energy per layer, layers mapped to angular bins, L2-normalized **within the comparable band** | The depth signature. This is the flat-vs-3D axis. |
| `tonal_quantiles` | Luminance (CIELAB L*) quantiles at p1, p5, p10, p25, p50, p75, p90, p95, p99, p99.9 | Tonal placement without FOV-dependent histogram shape. |
| `chroma_hist` | Coarse 2D histogram over CIELAB a*/b*, chroma-weighted, luminance-independent | "Looks like a real render" lives here far more than in luminance. |
| `saturation_quantiles` | Quantiles of C* (chroma magnitude) | Distinguishes tasteful saturation structure from a flat global boost. |
| `background.*` | Floor/peak ratio; ratio of high-scale structure energy to low-scale gradient energy; per-channel background balance | Detects muddy floors, uncorrected gradients, color casts. |

> **Never store a raw histogram.** It will not survive FOV, exposure, or resolution differences.
> Quantiles and normalized distributions only.

### 4.3 Palette classes

`LRGB` · `RGB` · `SHO` · `HOO` · `HaRGB` · `HaOIII-RGB` · `L-only` · `unknown`

On the user's side, palette class is **auto-derived from FITS headers** — the `FILTER` keyword
written by the ZWO EFW makes this deterministic. Never prompt for what the headers already state.

For references, palette class must be declared at ingest. `unknown` references contribute
structural + tonal components only, never chroma.

### 4.4 Band-limiting rule

This is the single most important function in the codebase. Get it right.

```
lo_arcsec = max(ref.psf_fwhm_arcsec, target.psf_fwhm_arcsec)
hi_arcsec = min(ref.max_meaningful_scale, target.max_meaningful_scale)
```

- `max_meaningful_scale` ≈ the largest wavelet layer that still represents structure rather than
  the residual/gradient plane — practically, the second-largest layer.
- Compare **only** spectrum bins whose center lies in `[lo_arcsec, hi_arcsec]`.
- Re-normalize both spectra **over the surviving bins only**, then compare shape.
- Compare **normalized shape, never absolute energy.** The reference will always carry more total
  structure. That is not a deficiency to be corrected.
- If fewer than 3 bins survive, the reference is **unusable for structural guidance**. Fall back to
  tonal + chroma components and log a warning. Do not silently proceed.

### 4.5 Distance function

```
D = w_e · D_spectrum(band-limited, e.g. Jensen-Shannon or L2 on normalized shape)
  + w_t · D_tonal(L2 over quantile vector)
  + w_c · D_chroma(Earth Mover's Distance over a*/b* histogram)   [0 if palette mismatch]
  + w_b · D_background(weighted L2)
```

Weights are configuration, not constants. Ship sane defaults; expose them. `w_c = 0` is enforced
by §2.3, not by user choice.

---

## 5. Reference Acquisition & Fingerprint DB

### 5.1 Lookup

Key on **cone search (RA/Dec + radius)**, never target name. Catalog naming ("M42 / NGC 1976 /
Orion Nebula / Sh2-281") is a swamp; cone search handles arbitrary framing and mosaic panels
naturally.

Query is `(ra, dec, search_radius, palette_class)`. A record matches if its WCS footprint overlaps
the query cone and its palette class is compatible (§2.3).

### 5.2 The not-found path

When no fingerprint exists for a solved position:

1. Cone-search a **local positional index** of professional gallery renders for one covering the
   position. See §5.2.1.
2. Attempt to obtain WCS: FITS header → embedded AVM → blind plate solve.
3. **Expect a meaningful fraction of the best renders to fail all three.** Star-suppressed or
   starless treatments have nothing to match; heavy composites and mosaics have no coherent
   astrometric solution.
4. On failure, prompt the user for manual scale/orientation annotation, or skip the reference.

Do not treat a failed solve as an error state. It is the expected outcome for a nontrivial share of
the prettiest targets. Handle it as a first-class branch.

#### 5.2.1 There is no cone-search API over renders — hence the local index

**Step 1 originally read "query archives (MAST, ESASky, ESO) for a public release image covering
the position." That was wrong, and the correction matters enough to record here.** Verified
2026-07-11 and again 2026-07-26:

- MAST and ESASky cone-search return **science observations** (linear FITS), not press-release
  renders.
- The ESA/Hubble and ESO public galleries — where the professional renders actually live — expose
  no JSON API (`?format=json` serves `text/html`; `/api/v1/` 404s), and their advanced-search
  forms carry **no RA/Dec fields at all**.

Ingesting a linear science FITS as `professional_render` would violate §2.1: a linear master has
no presentation layer, fingerprints as maximally flat, and poisons the reference set with an
**anti-target** that drives user images toward flatness. Never do this, however convenient the
cone-search API.

Discovery therefore works by **crawling the galleries once into a local position index** (id,
position, field of view, palette, license, attribution, image URL), then cone-searching that index
locally. Positions come from the galleries' own published, AVM-derived metadata, so candidates are
filtered against the cone *before* any multi-hundred-megapixel download.

Searching by object name is **not** an acceptable substitute: §5.1 already rules it out, and the
galleries demonstrate exactly why — for one object ESA/Hubble publishes `Messier 42` while ESO
publishes `M 42`, and the query that actually returns results is `Orion Nebula`.

Full design, including the fail-closed position-verification gate: `docs/superpowers/specs/
2026-07-26-p1e-archive-discovery-design.md`.

### 5.3 Provenance is a first-class field, not metadata

**The failure mode this prevents is autophagy.** If users contribute fingerprints derived from
images this tool produced, the reference distribution converges on the tool's own output and drifts
away from the professional renders it exists to chase. This would be slow, silent, and fatal.

```
source_type ∈ { professional_render, user_render, tool_output }
```

- `tool_output` → **rejected at ingest.** Never stored, never consensused.
- `user_render` → stored, flagged, **excluded from consensus** by default.
- `professional_render` → eligible for consensus.

### 5.4 Consensus

Seed each `(position_cell, palette_class)` with one best professional reference. As additional
professional references accumulate, compute a **median-of-fingerprints with MAD-based outlier
rejection**, per component.

This resolves the single-vs-ensemble question by aging into it: single-reference clarity at seed
time, ensemble robustness at maturity, no up-front choice required.

Chroma histograms are median-combined **bin-wise after registration to a common a*/b* grid** —
naive averaging of chroma clouds produces mud.

### 5.5 Licensing

| Source | License | Obligation |
|---|---|---|
| NASA (incl. JWST via NASA) | Public domain | None |
| ESA/Hubble | CC BY 4.0 | Attribution |
| ESO | CC BY 4.0 | Attribution |

Attribution string and source URL travel **with every fingerprint record** and must surface in any
UI that displays a reference. Public-domain records carry an empty obligation, not an absent field.

---

## 6. The Optimization Loop

### 6.1 Shape

This is a **guardrailed agent loop with rollback**, not a scalar hill-climb. Improvement is
non-monotonic: the transform that adds depth at iteration 3 crushes dust lanes at iteration 8. A
greedy loop walks straight into the over-cooked attractor because each individual step "increased
contrast."

```
checkpoint(best)
branches = [ current ]                       # beam width 2–3
while not converged:
    for b in branches:
        actions = propose(b)                 # deterministic ranking, VLM re-ranks periodically
        for a in top_k(actions):
            candidate = apply(b, a)
            m = measure(candidate)
            if guardrail_violated(m):
                discard(candidate); continue
            score(candidate) = -D(m, fingerprint)
    branches = prune(all_candidates, width)
    if best_improved: checkpoint(best)
```

### 6.2 Discretized action space

Do **not** let any model set free-form process parameters. The search space is intractable and the
model will flail. Actions are a bounded menu; each has 2–3 magnitude levels (`gentle`, `moderate`,
`strong`).

| Action | Process | Notes |
|---|---|---|
| Local contrast @ scale band N | MLT / MMT | Band selected in arcsec, mapped to layer |
| Core HDR compression | HDRMultiscaleTransform | Masked |
| Global tonal reshape | CurvesTransformation | Constrained to monotone |
| Black point adjustment | HistogramTransformation | Clip-limited (§7) |
| Local histogram equalization | LocalHistogramEqualization | Masked, kernel in arcsec |
| Chroma curve @ hue range | ColorSaturation | Hue range from fingerprint gap analysis |
| Background neutralization | BackgroundNeutralization | Idempotent, early-only |
| Star/nebula split | StarXTerminator or StarNet | Once, early; enables unmasked pushes |

Star/nebula separation is strongly recommended as an early action: it lets the optimizer push local
contrast on the starless layer hard without blowing star cores, then recombine.

### 6.3 Convergence

Terminate on the **OR** of:

1. Deterministic ΔD across the last *k* iterations falls below `epsilon`.
2. VLM director reports diminishing returns or early signs of over-processing.
3. A guardrail trips and rollback exhausts the branch set.
4. Iteration cap reached.

**Never let the model alone decide it is done.** Condition 2 is advisory input to the loop, never
the sole terminator.

---

## 7. Guardrails

Guardrails are **hard constraints**, evaluated every iteration, cheap and deterministic. A
violation discards the candidate and rolls back. They are not part of the score.

| Guardrail | Metric | Trip condition |
|---|---|---|
| Noise floor | MRS noise σ per channel (PI native) | σ increases > X% from checkpoint |
| Star integrity | StarDetector: count, median FWHM, eccentricity | count drops, or FWHM/ecc balloons |
| Shadow clipping | % pixels at 0 per channel | exceeds threshold (default ~0.01%) |
| Highlight clipping | % pixels at 1.0 per channel | exceeds threshold |
| Hue invention | Chroma mass appearing in a*/b* cells with no support in the linear stack | any mass above ε |
| Channel ratio drift | Post-stretch ratios vs. linear-derived constraints | exceeds tolerance |

The last two enforce §2.1 and §2.3 mechanically. They are the difference between a tool that
enhances and a tool that fabricates.

> This is the same guardrail-watcher pattern as a supervised agent pipeline: bounded proposals,
> deterministic constraint checks, rollback on violation, checkpointed best-so-far.

---

## 8. VLM Integration (Ollama)

### 8.1 It must be multimodal

A text-only model cannot see the image and therefore cannot judge contrast or depth. Use a vision
model via Ollama (`llava`, `qwen2-vl`, `llama3.2-vision`, or successor). This is a hard requirement,
not a preference.

### 8.2 Comparative, never absolute

VLMs are **unreliable at absolute image-quality scoring** and their numeric scores are inconsistent
between calls. Optimizing directly against a VLM score produces reward hacking: images that game the
judge's biases and look worse to a human.

They are **substantially better at comparison.** Therefore the only sanctioned prompt shapes are:

- *"Here are candidate A, candidate B, and the reference. Which candidate is closer to the reference,
  and in what respect?"*
- *"Here is the current image and the reference. Name the single largest qualitative difference."*
- *"Does this image show signs of over-processing (halos, plastic texture, crushed shadows)?"*

Never: *"Rate this image 1–10."*

### 8.3 Role: periodic director, not per-iteration critic

The user has accepted slower runtime, but the binding argument is robustness, not speed: a
per-iteration critic maximizes exposure to score-gaming and injects noise into every step.

- Deterministic optimizer runs *N* iterations (default 5) on fingerprint distance alone.
- VLM is invoked at the boundary to: re-rank the action menu, flag over-processing, and confirm or
  veto the branch selection.
- VLM output is a **preference over a bounded action menu**, never a raw parameter value.

This keeps the model in the loop where it is strong (qualitative direction) and out of it where it
is weak (fine-grained numeric judgment).

---

## 9. Mothership (Opt-In Fingerprint Contribution)

### 9.1 Payload

```json
{ "fingerprint": {...}, "wcs": {...}, "instrument": {...},
  "palette": {...}, "provenance": {...} }
```

**No image data. Ever.** The payload is a stats vector, a sky position, a palette tag, and a source
URL. That makes the opt-in disclosure short and honest.

### 9.2 Rules

- Opt-in only. Default off. No dark pattern, no bundled consent.
- `source_type: tool_output` is rejected server-side, not merely flagged (§5.3).
- Contributions land in a review queue before entering consensus.
- Client-side, the exact payload is inspectable before upload.

### 9.3 Deployment

Small enough that a Render service + Backblaze object store for the reference-image cache is
sufficient. SQLite locally, Postgres server-side. No streaming infrastructure required.

---

## 10. Implementation Phases

**Build in this order. Do not skip ahead — the deterministic core must be proven before a model
touches it.**

### Phase 0 — Metrics & fingerprint library (no PI, no AI)
Standalone Python. Reads FITS/XISF. Implements §4 in full: multiscale energy in arcsec, tonal
quantiles, chroma histogram, background stats, band-limiting, distance function.
**Exit criterion:** hand-fingerprint 5 professional M42 renders and 5 amateur ones; the distance
function must rank them correctly with no tuning gymnastics. If it cannot, the fingerprint
definition is wrong and everything downstream is built on sand.

### Phase 1 — Fingerprint DB + reference ingest
SQLite, cone search, archive query, WCS acquisition with the three-tier fallback, provenance
enforcement, license capture.
**Exit criterion:** solve → look up → miss → fetch → fingerprint → store, end to end, unattended,
with a clean manual-annotation path for solve failures.

### Phase 2 — Deterministic optimizer loop (still no AI)
PJSR orchestrator + file-protocol sidecar. Discretized action space, guardrails, checkpoint/rollback,
beam search. VLM absent; action ranking is heuristic.
**Exit criterion:** measurably improves a flat image, and — more importantly — **fails safe** on an
already-well-processed image by declining to make it worse.

### Phase 3 — VLM director
Ollama integration per §8. Comparative prompts only. A/B against Phase 2 output.
**Exit criterion:** VLM-directed runs beat deterministic-only runs in blind human comparison. If
they do not, ship Phase 2 and drop the model. The model must earn its place.

### Phase 4 — Mothership
Opt-in contribution, review queue, consensus with MAD outlier rejection.

---

## 11. Open Questions

1. **Multiscale transform choice.** MLT (linear, à trous) vs MMT (median, artifact-resistant) for
   energy spectrum extraction — MMT is gentler on noise but its energy per layer is harder to
   interpret. Test both in Phase 0.
2. **PSF FWHM for references.** Measurable from stars in the render, but starless/star-suppressed
   professional treatments have none. Fallback: use the instrument's diffraction limit and mark
   `psf_measured: false`.
3. **Position cell size for consensus.** Fixed HEALPix cells vs. adaptive radius by target angular
   size. HEALPix is simpler; adaptive is correct. Start with HEALPix.
4. **Reprojection.** Full reprojection of the reference into the target's WCS is expensive and lossy.
   Determine in Phase 0 whether fingerprint components are sufficiently reprojection-invariant that
   comparison on the raw reference suffices — the spectrum and quantiles likely are, the background
   stats likely are not.
5. **Do mosaic panels need per-panel fingerprints?** Probably. Defer.

---

## 12. Non-Goals — Do Not Build These

- **Any neural style transfer, GAN, or diffusion model touching pixels.** The output must be
  reconstructible as a sequence of stock PixInsight processes. Auditability is the point.
- **Absolute VLM quality scoring.** See §8.2.
- **Pixel-space loss against the reference** (MSE, raw SSIM). Meaningless across instruments (§2.2).
- **Any path where the reference influences linear-space channel weights.** See §2.1. This is the
  failure that turns the tool into a fabrication engine.
- **Uploading image data to the mothership.** Fingerprints only (§9.1).
- **Silent proceed on band-limit failure, palette mismatch, or WCS failure.** Every degraded path
  logs and surfaces.

---

## 13. Summary of the Thesis

The tool's implicit claim is: *"present my data the way a professional imaging team chose to present
this object."*

That is a **taste**, not a fact. Professional renders are aesthetic artifacts, not ground truth.
This is a defensible and interesting goal precisely because the user wants beauty — but the design
must never confuse matching someone's presentation choices with recovering physical reality. Every
guardrail in §7 exists to hold that line.
