# Gaia Depth Grade — Design Spec

**Date:** 2026-06-23
**Status:** Approved design, pre-implementation
**Supersedes:** the idea capture in `README.md` (kept as background)

---

## 1. Problem & goal

Apply a **physically-grounded "depth" grade** to astrophotographs. Instead of faking
depth with aesthetic local-contrast tricks, use **measured 3D distance data from Gaia**
to determine how far each part of an object actually is, then modulate the rendering so
nearer structure pops and farther structure recedes. The depth cue is *measured*, not
invented.

**Honesty constraint (load-bearing):** Gaia measures point-source distances (stellar
parallax). Diffuse gas has no parallax, so per-pixel depth is degenerate for emission
nebulosity. Depth is well-defined only for discrete objects (stars, distinct dust
clumps) or where one dust screen dominates a sightline. The output is therefore
**physically motivated, not per-pixel physically correct for gas** — and is labeled as
such in output metadata.

---

## 2. Scope & phasing

One spec, two phases sharing a single **distance → modulation engine**. The distance
*source* is a pluggable interface so Phase 2 reuses the entire downstream pipeline
unchanged.

- **Phase 1 (MVP) — star-field depth.** Gaia DR3 + Bailer-Jones geometric distances →
  per-star depth strength → modulate the stars layer (brightness, size/glow, local
  contrast, saturation). This is the deliverable that must ship and validate first.
- **Phase 2 (stretch) — dust depth.** Same engine; distance source swapped to a 3D dust
  map (Edenhofer+ 2024 via the `dustmaps` package) producing a per-sightline
  dominant-distance raster. Validated on a nearby dust-rich target (e.g. Rho Oph).

Phase 2 is designed-for but not built until Phase 1's look is validated.

---

## 3. Architecture — hybrid (Python core + thin PJSR wrapper)

All science and rendering live in **Python**, behind a clean **FITS-in / FITS-out**
boundary. The PJSR wrapper is thin orchestration that runs inside the existing headless
PixInsight harness (Xvfb + `PixInsight.sh`, as already used by NukeX).

```
┌─ PJSR wrapper (inside Xvfb + PI harness) ─────────────────┐
│  1. Load master XISF                                      │
│  2. Plate-solve (ImageSolver) → WCS into FITS keywords    │
│  3. StarXTerminator → stars layer + starless layer        │
│  4. Export stars layer (+ WCS) to FITS; shell out:        │
│         python -m gaia_depth_grade grade <stars.fits>     │
│  5. Read back modulated stars layer (FITS)                │
│  6. Recombine (screen/add) with starless → save result    │
└───────────────────────────────────────────────────────────┘
                  │ stars.fits + WCS          ▲ modulated_stars.fits
                  ▼                           │
┌─ Python core (testable, zero PI dependency) ──────────────┐
│  detect → query distances → match → transform → modulate  │
│         → render                                          │
└───────────────────────────────────────────────────────────┘
```

**Boundary contract:** Python never imports or depends on PixInsight; PJSR never does
science. The only thing crossing the boundary is FITS files (pixels + WCS header) and a
JSON QA/config sidecar.

---

## 4. Python core — module breakdown

Each module has one job and is independently testable.

| Module | Responsibility | Key deps |
|---|---|---|
| `wcs.py` | Read WCS from FITS header; pixel ↔ sky conversion; compute field footprint (center + radius) for the catalog query. | astropy.wcs |
| `detect.py` | Detect stars in the stars layer; return positions, measured FWHM, instrumental flux. | photutils `DAOStarFinder` |
| `distances.py` | **`DistanceSource` interface.** `GaiaStarSource`: astroquery TAP query over the footprint, joining `gaia_source` ↔ Bailer-Jones `gaiadr3.distances` (geometric `r_med_geo`, `r_lo_geo`, `r_hi_geo`); on-disk cache keyed by (center, radius, catalog version). `DustMapSource` (Phase 2): `dustmaps`/Edenhofer along sightlines → dominant-distance raster. | astroquery.gaia, (Phase 2: dustmaps) |
| `match.py` | Cross-match detected stars ↔ Gaia sources by WCS position (KD-tree, tolerance ≈ a few px). Unmatched detected stars → neutral/interpolated depth. Emits match-rate + residual-offset QA stats. | scipy.spatial / astropy |
| `transform.py` | **Swappable** `depth_strength(distance, uncertainty)`: log-distance, robust 5–95th percentile normalized to `[-1, +1]` (−1 = farthest/recede, +1 = nearest/pop). Bailer-Jones uncertainty → confidence weight attenuating strength for noisy parallaxes. | numpy |
| `modulate.py` | Map (strength, per-attribute global gains) → four deltas: **brightness**, **size/glow**, **local contrast**, **saturation**. Each gain independently tunable, including to zero. | numpy |
| `render.py` | Composite modulated per-star stamps into a full-res stars layer. Footprints Gaussian/Moffat scaled by size delta; brightness/saturation as multipliers; local contrast as a small unsharp around each stamp. | numpy, scipy.ndimage |
| `cli.py` | `grade` (full run), `debug` (emit depth-colored overlay + QA report). Config via dataclass / TOML. | — |

**Config (TOML / dataclass):** per-attribute gains, percentile bounds, match tolerance,
neutral-depth policy, input/output paths, cache dir.

---

## 5. Data flow (Phase 1)

```
master XISF
  → [PJSR] plate-solve + StarXTerminator
  → stars.fits (+ WCS)
  → detect stars
  → Gaia DR3 + Bailer-Jones distances (cached)
  → cross-match detected ↔ catalog
  → per-star depth strength (transform)
  → 4 modulation deltas (modulate)
  → render modulated stars.fits
  → [PJSR] recombine with starless layer
  → graded master (+ honesty metadata)
```

---

## 6. Modulation model (defaults; all tunable)

Given normalized strength `s ∈ [-1, +1]` and confidence `c ∈ [0, 1]`, effective strength
`e = s · c`. Each attribute applies its own gain `g_x`:

- **Brightness:** multiplier `1 + g_b · e` (nearer brighter, farther dimmer).
- **Size/glow:** footprint scale `1 + g_z · e`; halo weight scales with `e`.
- **Local contrast:** unsharp amount `g_c · e` over a small radius around each stamp.
- **Saturation:** color saturation multiplier `1 + g_s · e` (aerial-perspective cue).

Per-object normalization (map the field's own near–far range to the strength band) is the
default via the 5–95th percentile clip in `transform.py`.

---

## 7. Error handling (loud — no silent fallbacks)

Per project rules, real failures surface; mock/silent fallbacks are forbidden.

- **No WCS in header** → hard error (cannot query Gaia without it).
- **Gaia/network failure** → fail with the real error. A present cache is reused, but a
  stale-but-present cache hit is logged explicitly, never silently masked.
- **Low match rate** (< configurable threshold) → loud warning in the QA report; the run
  does not pretend depth is reliable.
- **Negative/invalid parallax** → handled by Bailer-Jones geometric distances (never raw
  `1/parallax`); excluded sources logged.
- **Honesty metadata:** output FITS carries a `HISTORY`/keyword tag:
  `GAIA depth-derived; physically motivated, not per-pixel correct for gas`.

---

## 8. Testing strategy

- **Unit:** `transform`, `modulate`, `match` against synthetic inputs with known injected
  distances; assert depth ordering preserved, monotonicity, gain-zero is a no-op.
- **Component:** `distances.GaiaStarSource` against a recorded/cached TAP response
  (no live network in CI).
- **E2E (synthetic):** a small synthetic star field with planted Gaia-like distances →
  assert near stars brightened, far stars dimmed, transform monotonic. Mirrors the NukeX
  golden-harness style.
- **Manual validation:** live PI harness run on a real master (GPU + StarXTerminator not
  in CI).

---

## 9. Inputs & repository layout

- **Validation input:** a configurable master XISF path; default = newest combined RGB
  master in `~/projects/processing/master/`
  (e.g. `LN_Reference_Light_BIN-1_3840x2160_EXPOSURE-300.00s_FILTER-HaO3_RGB_(6).xisf`).
  Any integrated, plate-solvable master works.
- **Project location:** new standalone Python package at `~/projects/gaia-depth-grade/`.
  - `src/gaia_depth_grade/` — the Python core modules above.
  - `pjsr/` — the thin PJSR orchestration wrapper.
  - `tests/` — unit/component/E2E.
  - `docs/superpowers/` — specs & plans.
  - Decision: standalone (not folded into `astro-pi`) for now; can be vendored later.

---

## 10. Data sources

- **Gaia DR3** via `astroquery.gaia` (TAP).
- **Bailer-Jones et al. geometric distances** (Gaia DR3 distance catalog,
  `gaiadr3.distances`).
- **3D dust maps (Phase 2):** Edenhofer+ 2024 (preferred, nearby high-res) via the
  `dustmaps` package.
- **Plate solving:** PI ImageSolver inside the harness.
- **Star separation:** StarXTerminator inside the harness.

---

## 11. Assumptions & risks

- **StarXTerminator is headless-scriptable** in the existing PI harness (same
  `PixInsight.sh` + Xvfb mechanism NukeX uses). *Risk:* if SXT can't run headless, fall
  back to a PI StarMask — flagged, not silently substituted.
- **Bailer-Jones table reachable** via the same TAP service as `gaia_source`.
- **astroquery TAP availability/rate limits** — mitigated by the on-disk query cache.
- **Resolution mismatch (Phase 2):** 3D dust maps are parsec/coarse-angular scale — good
  for broad depth gradients, useless for arcsec structure. Acceptable for a depth grade.
- **Gas depth degeneracy (Phase 2):** inherent; addressed by honest labeling and limiting
  dust depth to dust-dominated sightlines.

---

## 12. Out of scope (YAGNI)

- Real-time / interactive GUI tuning (CLI + config only for now).
- Multi-frame mosaicking of depth maps.
- Per-pixel gas depth for diffuse emission (physically degenerate — explicitly not
  attempted).
