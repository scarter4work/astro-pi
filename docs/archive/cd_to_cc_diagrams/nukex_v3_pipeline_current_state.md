# NukeX v3 — Pipeline as it actually exists today

**Purpose:** Ground-truth snapshot of `~/projects/nukex3/` so the v4 diagram (whether drawn in Excalidraw or whiteboarded) has an accurate baseline to push against. Generated 2026-04-26 from a code walk, not from prior memory.

**Audience:** CD (Claude Desktop) + Scott. CC (Claude Code) wrote this from a fresh read.

---

## TL;DR — what's there vs. what the proposal described

| Component (proposal) | Status in code | One-line reality |
|---|---|---|
| Voxel cube as source-of-truth | **Partial — terminology mismatch** | `SubCube` is one float-tensor *per channel*, processed in isolation. No cross-channel "RGB-per-voxel" structure exists. |
| Full pipeline (input → calibration → stack → cube → color space → recombiner → stretch → rating popup → output) | **Mostly present, missing pieces** | Linear stages exist. Color space service, recombiner-as-described, and rating popup do not. |
| Color space service (SHO / LRGB / OSC routing) | **Not found as a component** | OSC debayer is inline in `FrameLoader::DebayerBilinear`. SHO and LRGB paths **do not exist** — channels are always processed as generic R/G/B. Filter metadata is stored on `SubCube` but never read. |
| Recombiner (global scale + saturation/luminance balance) | **Partial** | Per-channel median scaling for background neutralization is implemented (`NukeXStackInstance.cpp:652-686`, `1161-1183`). No saturation, no luminance, no HSL. |
| Rating popup | **Not found** | No post-stretch rating dialog exists. |
| `NukeXStack` + `NukeXStretch` processes | **Found, both present** | `NukeXStackProcess` and `NukeXStretchProcess`. |

---

## Repository layout (depth 2, source only)

```
src/
├── NukeXModule.{h,cpp}                  — module registration
├── NukeXStackProcess.{h,cpp}            — process entry
├── NukeXStackInstance.{h,cpp}           — pipeline driver (1441 lines)
├── NukeXStackInterface.{h,cpp}          — UI
├── NukeXStackParameters.{h,cpp}         — parameter metadata
├── NukeXStretchProcess.{h,cpp}          — process entry
├── NukeXStretchInstance.{h,cpp}         — standalone stretch
├── NukeXStretchInterface.{h,cpp}        — UI
├── NukeXStretchParameters.{h,cpp}
└── engine/
    ├── SubCube.h                        — the "voxel cube" (per-channel)
    ├── FrameLoader.{h,cpp}              — FITS I/O, debayer, metadata
    ├── FrameAligner.{h,cpp}             — star detection + triangle match
    ├── PixelSelector.{h,cpp}            — per-pixel statistical stacking
    ├── FlatCalibrator.{h,cpp}           — flat frame handling
    ├── ArtifactDetector.{h,cpp}         — trails / dust / vignetting
    ├── DustCorrector.{h,cpp}
    ├── TrailDetector.{h,cpp}
    ├── DistributionFitter.{h,cpp}       — Gaussian / Poisson / Skew-Normal / Bimodal
    ├── AutoStretchSelector.{h,cpp}      — picks MTF / GHS / ArcSinh
    ├── StretchLibrary.{h,cpp}           — algorithm registry
    ├── IStretchAlgorithm.{h,cpp}        — strategy interface
    ├── algorithms/                      — 11 stretch implementations
    └── cuda/                            — GPU acceleration (partial)
```

---

## Actual data flow — `NukeXStack::ExecuteGlobal()`

Drawn from `src/NukeXStackInstance.cpp:150-1301`. Read top-to-bottom.

```
ExecuteGlobal()                                        [L150]
  │
  ├─ FrameLoader::LoadRaw(framePaths)                  [L177]
  │     • Detects Bayer pattern from FITS keywords
  │     • Bilinear debayer → 3-channel RGB if OSC
  │     • Returns LoadedFrames { pixelData[frame][channel], metadata[] }
  │
  ├─ FlatCalibrator (optional)                         [L202-230]
  │     • addFrame() → buildMasterFlat() → calibrate()
  │
  ├─ alignFrames(channel[0])                           [L249]
  │     • Star detection + triangle matcher
  │     • Returns { offsets[], crop, alignedCube }
  │
  ├─ For each channel ∈ {R, G, B} (or {L} for mono):   [L372-503]
  │     │
  │     ├─ applyAlignment() for ch > 0                 [L403]
  │     ├─ Build SubCube for this channel
  │     ├─ Per-pixel trail rejection mask (median+MAD) [L410-457]
  │     │
  │     └─ PixelSelector::processImage[GPU]()          [L461 / L472]
  │           • For each (y, x):
  │             - Extract Z-column from SubCube
  │             - selectBestZ(): fit distributions,
  │               ESD outlier rejection, score, pick
  │           • Returns channelResults[ch] : vector<float>
  │           • Populates distTypeMaps[ch]
  │
  ├─ Compose linear output image                        [L526-546]
  │     • channelResults → PixInsight Image (32-bit float)
  │     • Window: "NukeX_stack"
  │
  └─ If p_enableAutoStretch:                            [L548-896]
        │
        ├─ AutoStretchSelector::Select(distTypes,...)   [L589]
        │     • Picks MTF / GHS / ArcSinh per heuristic
        │
        ├─ Trial sweep (10k-pixel sample, score by      [L767-811]
        │   entropy + clipping penalty)
        │
        ├─ Background neutralization                    [L652-686]
        │     • scale[ch] = targetMedian / median[ch]
        │     • THIS IS THE ENTIRE "RECOMBINER"
        │
        ├─ Apply stretch per channel (unified MAD)      [L834-890]
        │     • Window: "NukeX_stretched"
        │
        └─ If p_enableRemediation:                      [L902-1257]
              • ArtifactDetector::detectAll(luminance)
              • Re-select trail pixels from SubCube
              • Vignetting correction map
              • Rebuild linear → re-neutralize → re-stretch
```

**Data type lineage**

| Stage | Container | Layout |
|---|---|---|
| Raw load | `LoadedFrames::pixelData[frame][channel]` `vector<float>` | row-major per frame per channel |
| Aligned | `SubCube` (xtensor 3D) | column-major, **Z-contiguous** (one Z per pixel) |
| Stacked | `vector<float>` per channel | row-major, single image |
| PixInsight output | `pcl::Image` 32-bit float | row-major per channel |

---

## The "voxel cube" — terminology audit

The word "voxel" implies (x, y, z) with channel data living *per voxel* — i.e. a 4D structure (x, y, z, {R,G,B}) or a 3D structure where each cell is an RGB tuple. **That is not what `SubCube` is.**

`SubCube` (`src/engine/SubCube.h:33`) is:

- A 3D float tensor `(nSubs, height, width)`, column-major so the Z dimension (frames) is contiguous per pixel
- **One per channel** — built independently for R, G, B during stacking
- The Z axis is "which frame," not "which color"
- Per-sub metadata: FWHM, eccentricity, quality score, **filter** (stored, never read)
- Provenance map: which Z was selected at each pixel
- Distribution-type map: which model fit best at each pixel

So when the proposal says "voxel cube as source-of-truth across the pipeline," the closest match in code is the *set* of three `SubCube` instances during stacking — and they are local to `ExecuteGlobal()`, garbage-collected after the linear output image is composed. They are not the persistent system-wide source-of-truth the proposal implies. They cannot, today, support cross-channel inference (e.g. "this OIII voxel is high but the SII voxel at the same pixel is noise → likely artifact").

If v4 wants the voxel cube to be load-bearing, that's a real architectural change, not a renaming.

---

## What's missing for v4 (the gaps that should be on the diagram)

These are the things the proposal describes that **do not exist in code today**. Each is a stub-in-prose; building them is the work.

1. **Cross-channel voxel cube** — a unified `(x, y, z, channel)` structure (or whatever shape Scott actually has in mind) that survives past stacking and is the input to a color space service. Today: three separate per-channel `SubCube`s, scope-local to `ExecuteGlobal`.

2. **Color space service** — discrete class/module that:
   - Reads filter metadata from `SubCube`'s already-stored `filter` field
   - Routes channels by filter: SHO (Ha→R, OIII→G+B, SII→R), LRGB (L+RGB combine), OSC (debayer-then-LRGB)
   - Today: no such class. Channels are always processed as generic R/G/B regardless of filter.

3. **Recombiner with saturation/luminance** — current "recombination" is one line of background median scaling. The proposal's recombiner is a substantially larger component (HSL adjustments, luminance weighting, saturation control). Today: not present.

4. **OSC-as-LRGB collapse** — the proposal calls for OSC to debayer and then flow through the LRGB path. Today: OSC debayers to RGB and that's the end of color-space awareness. There is no LRGB path to flow into.

5. **Rating popup** — post-stretch user-facing dialog. Today: not present.

6. **Plate solving / WCS passthrough** — proposal mentions these as "iffy." Today: not present in the stack pipeline.

---

## Open questions for Scott (CD can answer some, others need Scott)

1. **Voxel cube shape.** Should the v4 cube be `(x, y, frame, channel)` 4D, or `(x, y, channel)` 3D-of-RGB-tuples post-stack, or something else? This decides whether the cube replaces `SubCube` or sits downstream of it.

2. **Filter routing.** Auto-detect from FITS headers (the metadata is already loaded), or explicit user selection in the UI?

3. **LRGB ingestion.** When does L arrive — separate FITS files tagged as L, or a 4-channel FITS, or something the user designates in UI?

4. **Recombiner scope.** Is the recombiner doing color science (HSL, saturation) or pixel science (luminance-weighted blending)? Different algorithms.

5. **Rating popup.** What's being rated — the run quality (entropy, S/N, artifact count = automatic) or the aesthetic result (user 1-5 stars)?

6. **Where does the color space service sit in the pipeline?** Before stacking (route then stack per role) or after stacking (stack then route)? The diagram should make this commitment explicit because it's load-bearing for everything downstream.

---

## Excalidraw MCP — package landscape (for the install decision)

CD's proposal said `npx -y excalidraw-render`. **That package does not exist by that name.** The real packages on npm/GitHub:

- **`excalidraw-mcp`** (npm, 1.0.0, 5 months ago) — full MCP server, supports `export_to_svg`, `export_to_png`, `export_to_json`. Most generic name.
- **`mcp-excalidraw-server`** (npm, 1.0.5, 4 months ago) — element creation + canvas sync.
- **`@cmd8/excalidraw-mcp`** (npm, 1.2.0, 5 months ago) — element-focused.
- **`yctimlin/mcp_excalidraw`** (GitHub) — also distributed as a Claude Code skill, real-time canvas sync.
- **`excalidraw/excalidraw-mcp`** (GitHub, official org) — uses the MCP Apps extension to render interactive HTML in chat. Different model than the others.

**Notes for the install decision:**

- Multiple options means none are clearly canonical yet. Picking the one that best fits the actual workflow Scott described ("I draw, CC reads") matters more than picking the most-starred.
- The official `excalidraw/excalidraw-mcp` is interesting because it returns interactive HTML — but that may not match a "draw locally, CC reads from disk" workflow. Worth a closer look before committing.
- For Scott's stated use case (he draws in Excalidraw locally, CC reads the file at the start of each session), the most important capability is *reading* Excalidraw JSON and converting to something CC can understand — not generating. Several of these packages emphasize generation. Read the READMEs before installing.

---

## Suggested next move for CD

1. CD: react to this doc — flag where I've misread the code, fill the open questions where you have answers, push back on the v4 gap list if any of it isn't actually a gap.
2. Scott: answer the questions that need his architectural intent (especially Q1, Q4, Q6).
3. Then choose the MCP package based on the actual workflow shape — likely after Scott has tried drawing one diagram by hand and seen what he wants the tool to do.
