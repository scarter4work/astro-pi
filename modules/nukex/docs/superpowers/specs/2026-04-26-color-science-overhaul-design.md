# Color Science Overhaul — Design

**Date:** 2026-04-26
**Status:** Design (v5 scope)
**Originating discussion:** Brainstorm session 2026-04-26
**Companion research:** `research/qe_database_research.json` (22 sensors / 55 cameras / 87 filters)
**Replaces / supersedes:** `output_rgb_mapping`, `StackingMode::OSC_HAO3`, `StackingMode::OSC_S2O3` (all dead code in v4.0.1.0)

---

## 1. Context and Motivation

NukeX v4.0.1.0 has two latent gaps in its color pipeline:

1. **OSC-as-LRGB is not implemented.** The `OSC_RGB` mode emits 3-channel R/G/B with no synthesized luminance. Advanced workflows (APP, NINA-Lpro) marry an L channel to RGB chrominance via Lab/LCH for better detail retention. NukeX has no equivalent — even `MONO_LRGB` mode silently ignores its L channel via `output_rgb_mapping[0] = 1` (skips L) at emission time.

2. **Bayer-narrowband decomposition is dead code.** `StackingMode::OSC_HAO3` and `OSC_S2O3` exist as enum values with channel mappings, but `stacking_engine.cpp:107` hardcodes `OSC_RGB` for any Bayer data. Dual-narrowband filter shoots (HaO3, S2O3) are processed as plain OSC, producing miscalibrated output. The user's M27 HaO3 stack on v4.0.0.7 came out green — symptom is QE imbalance: green Bayer photosites have higher QE at 501nm (OIII) than blue photosites, so the naïvely-debayered output trends green/cyan instead of cleanly separating Hα and OIII.

This design closes both gaps with a unified **calibrated color pipeline**: filter taxonomy drives per-frame routing into raw filter-grouped voxel slots; Phase B does fit/select on raw slots, then applies a linear Q-matrix solve to derive semantic emission-line channels; ColorComposer produces the final RGB via Lab/LCH composite using calibrated emission-line palette vectors.

The change is **v5-class scope** — wholesale rebaseline of multi-channel E2E hashes, with the single-channel stretch sweep preserved bit-identically as the regression floor for stretch math correctness.

---

## 2. Locked Decisions (from brainstorm)

| Question | Choice | Rationale |
|---|---|---|
| Scope | **Bundled** (OSC-as-LRGB + Bayer-NB decomposition together) | Both share infrastructure (channel pipeline, composer); single coherent design beats two coupled designs. |
| Output palette | **Calibrated emission-line color** | Hα→red, OIII→cyan-blue, SII→deep red. "No greens" enforced by mathematical construction of Lab vectors, not by post-hoc SCNR. Phase 8 ratings tune saturation/brightness around the calibrated baseline. |
| Filter taxonomy | **Five classes** | `BROADBAND_L`, `BROADBAND_RGB`, `BROADBAND_OSC`, `NARROWBAND_SINGLE`, `DUAL_NB_OSC`. Coarse enough that auto-detection is reliable from FITS metadata; fine enough that decomposition + combine paths stay simple. |
| Ingestion | **Single batch, auto-route by FITS `FILTER`** | UI stays as-is. Classifier resolves missing/unknown FILTER per the tiered policy below — no DisambiguationDialog needed. |
| Combine algorithm | **Lab/LCH composite default + continuum-subtract opt-in** | L from broadband (best detail) preserved; emission lines drive chrominance via calibrated palette vectors. Continuum subtraction is the opt-in advanced mode for high-SNR data. |
| QE source | **Shipped JSON DB + user override** | `share/qe_database.json` (22 sensors, 55 cameras, 87 filters from research) + optional `~/.nukex4/qe_overrides.json` for user-measured values or unknown filters. |
| Regression policy | **Wholesale rebaseline (v5-class)** | Multi-channel E2E hashes change by design; single-channel sweep (GHS/MTF/ArcSinh) stays bit-identical as the stretch-math regression floor. |
| Decomposition locus | **Phase B Q-solve on accumulated cube** | Phase A is pure I/O + slot routing (zero math). Voxel holds raw filter-grouped values. Phase B fits raw slots, then linear Q-solve derives semantic slots. Cube is canonical record. |

---

## 3. Architecture

### 3.1 Four pillars

1. **`Filter` is a first-class type.** Every frame carries `(FilterClass, FilterName, Camera, BandwidthSpec)` immediately after `FITSReader::read`. The classifier is small (lookup table + alias map + sensor-type fallback for missing FILTER), not a learned model.

2. **`DebayerEngine` stays naïve.** It demosaics RGGB/BGGR/GRBG/GBRG to R/G/B and stops there. Filter-class awareness lives downstream. This preserves raw broadband for alignment, saturation guard, and continuum subtraction.

3. **The voxel grows filter-class-shaped raw slots.** Voxel is the data structure of record (existing rule). Phase A routes raw debayered values into per-filter raw slots — `R_HaO3, G_HaO3, B_HaO3` for HaO3; `L`, `R`, `G`, `B` for broadband; etc. The Q-solve runs at Phase B, operating on accumulated raw slot data, deriving semantic slots `Ha`, `OIII`, `SII`, `L`, `R`, `G`, `B`.

4. **`ColorComposer` is the only place that knows about palettes.** It reads Phase B's derived semantic slots, applies the calibrated emission-line palette to chrominance, takes broadband L for luminance, produces sRGB output. "No greens" is enforced by construction of the palette Lab vectors — none lives in the +b*/-a* (green) quadrant.

### 3.2 Pipeline diagram

```
                                                            ┌──────────────────┐
                                                            │  QE Database     │
                                                            │  share/...json   │
                                                            │  + user override │
                                                            └────────┬─────────┘
                                                                     │ reads
                                                                     ▼
   FITS frame ──► FilterClassifier ──► DebayerEngine ──► flat-correct ──► align ──► cache ──► Phase A Router
                       │                  (naïve R/G/B,                                              │
                       │                   unchanged)                                                │  routes raw values
                       ▼                                                                             │  into per-filter
              {Filter, FilterClass,                                                                  │  raw voxel slots
               Camera, BandwidthSpec}                                                                ▼
                                                              ┌──────────────────────────────────────┐
                                                              │ Cube — raw filter-grouped slots      │
                                                              │  HaO3 → R_HaO3, G_HaO3, B_HaO3       │
                                                              │  S2O3 → R_S2O3, G_S2O3, B_S2O3       │
                                                              │  L    → L                            │
                                                              │  R    → R   G → G   B → B            │
                                                              │  Mixed= union of input filter classes│
                                                              │                                      │
                                                              │  Each slot: Welford + histogram      │
                                                              │  per pixel across all frames         │
                                                              └─────────────┬────────────────────────┘
                                                                            │ Phase B fits + selects
                                                                            │ on raw slots
                                                                            ▼
                                                              ┌──────────────────────────────────────┐
                                                              │ Per-filter-group Q-solve at Phase B  │
                                                              │  HaO3 → (Ha, OIII_HaO3)              │
                                                              │  S2O3 → (SII, OIII_S2O3)             │
                                                              │  multi-source OIII merge:            │
                                                              │    OIII = sample_count_weighted_mean │
                                                              └─────────────┬────────────────────────┘
                                                                            │ derived semantic slots
                                                                            │ {Ha, OIII, SII, L, R, G, B}
                                                                            ▼
                                                              ColorComposer (Lab/LCH default;
                                                                             continuum-subtract opt-in)
                                                                            │
                                                                            ▼
                                                                   stretch → emit RGB
```

---

## 4. Components

### 4.1 New code

| Path | Single responsibility |
|---|---|
| `src/lib/core/Filter.{hpp,cpp}` | Typed wrapper: `(FilterClass, FilterName, Camera, BandwidthSpec)`. Lives in `core` because every downstream stage consumes it. |
| `src/lib/io/FilterClassifier.{hpp,cpp}` | `FITSMetadata → Filter`. Resolves missing/unknown FILTER per the tiered policy in §5. |
| `src/lib/calibration/QEDatabase.{hpp,cpp}` *(new dir)* | Loads `share/qe_database.json` + `~/.nukex4/qe_overrides.json`; exposes `lookup(camera, wavelength)` and `lookup_filter(name) → BandwidthSpec`. |
| `src/lib/calibration/ChannelDecomposer.{hpp,cpp}` | Builds Q matrix per `(Camera, Filter)`; returns a per-pixel solver `(R, G, B) → recovered line channels` via least-squares pseudo-inverse. |
| `src/lib/compose/Palette.{hpp,cpp}` *(new dir)* | Calibrated emission-line color vectors at 656/501/672 nm in CIE-Lab. The `test_palette_no_greens_invariant` property test enforces no vector in the green quadrant. |
| `src/lib/compose/ColorComposer.{hpp,cpp}` | Cube derived slots → final sRGB. Lab/LCH composite default; continuum-subtract opt-in mode. |
| `share/qe_database.json` | Shipped database from `research/qe_database_research.json`, normalized + reviewed. |
| `tools/import_qe_research.py` | One-shot transform: research JSON → shipping JSON (drops `_meta`, validates schema). |

### 4.2 Refactored existing code

| Path | Change |
|---|---|
| `src/lib/core/channel_config.{hpp,cpp}` | Driven by `Filter`+`FilterClass`, not `StackingMode`. Slot allocation is filter-class-shaped (raw slots in Phase A; derived slots after Phase B). Mixed-class cube takes union of input filter classes. **Removes** dead `output_rgb_mapping[3]`. |
| `src/lib/core/cube.{hpp,cpp}` + `SubcubeVoxel` | Variable-arity slot layout (replaces hardcoded `MAX_CHANNELS` arrays). Each slot still holds Welford + histogram per pixel. SoA-friendly memory layout for GPU preserved. |
| `src/lib/stacker/stacking_engine.cpp` | **Removes** the hardcoded `OSC_RGB` at line 107. Per-frame accumulate calls `Phase A Router`, which routes debayered values into per-filter raw voxel slots. Phase B applies `ChannelDecomposer.solve()` on accumulated cube, then composes via `ColorComposer`. |
| `src/module/NukeXInstance.cpp` | Wires `QEDatabase` + `FilterClassifier` + `ChannelDecomposer` + `ColorComposer` at execution start. Emits `ImageWindow` from composer output (always 3-channel RGB). Optional second window for raw line-emission channels (Hα mono, OIII mono) for advanced users. |
| `src/module/NukeXInterface.cpp` | New "QE override file…" file picker (optional; defaults to `~/.nukex4/qe_overrides.json`). No other UI changes. |

### 4.3 Deletions (dead code per the no-stubs rule)

| Path | What |
|---|---|
| `src/lib/core/include/nukex/core/channel_config.hpp:38` | `output_rgb_mapping[3]` field (never consumed by any caller). |
| `src/lib/core/src/channel_config.cpp:38-46` | `StackingMode::OSC_HAO3` switch case (never instantiated; replaced by `FilterClass::DUAL_NB_OSC`). |
| `src/lib/core/src/channel_config.cpp:47-55` | `StackingMode::OSC_S2O3` switch case (same). |
| `src/lib/core/include/nukex/core/channel_config.hpp:9-16` | `enum class StackingMode` itself — replaced by `FilterClass` + `Filter`. Callers migrate to `FilterClass`. |

**Not in this design:** there is no `DisambiguationDialog`. Earlier draft proposed one; user feedback simplified disambiguation to sensor-type defaults + tiered loud-fail (see §5).

---

## 5. Data Flow

### 5.1 Phase A — streaming I/O, no math

For each frame:

1. `FITSReader::read` → `Image` + `FITSMetadata`
2. `FilterClassifier(meta)` → `Filter{class, name, camera, bandwidth}`
3. `DebayerEngine.debayer(image, pattern)` → 3-channel R/G/B (or mono passthrough)
4. `FlatCalibration.apply` (existing, unchanged)
5. `FrameAligner.align` (existing, uses raw R/G/B for star detection)
6. `FrameCache.write_frame` (existing)
7. **Phase A Router** — per pixel `(x, y)`:
   - If `Filter.class == DUAL_NB_OSC`, name `"HaO3"`: `voxel.slots["R_HaO3"].update(R); voxel.slots["G_HaO3"].update(G); voxel.slots["B_HaO3"].update(B);`
   - If `Filter.class == DUAL_NB_OSC`, name `"S2O3"`: same with `R_S2O3`, `G_S2O3`, `B_S2O3`
   - If `Filter.class == BROADBAND_OSC`: `voxel.slots["R"].update(R); voxel.slots["G"].update(G); voxel.slots["B"].update(B);` plus auto-synth `voxel.slots["L"].update(0.299*R + 0.587*G + 0.114*B)` (rec709) — **enables OSC-as-LRGB**
   - If `Filter.class == BROADBAND_L`: `voxel.slots["L"].update(value)`
   - If `Filter.class == BROADBAND_RGB`, name `"R"`: `voxel.slots["R"].update(value)` (similar for G, B)
   - If `Filter.class == NARROWBAND_SINGLE`, name `"Hα"`: `voxel.slots["Ha"].update(value)` (similar for `OIII`, `SII`)

No Q-solve in Phase A. No math beyond the rec709 luminance synthesis for `BROADBAND_OSC`.

### 5.2 Phase B — fit + select, then Q-solve

For each pixel × each raw slot:
- `distribution = ModelSelector.fit(voxel.slots[slot].histogram)`
- `best_value = PixelSelector.select(distribution)`
- `stacked.raw[slot].at(x, y) = best_value`

Then for each pixel × each filter group with raw RGB:
- `Q = QEDatabase.q_matrix(camera, filter_group)` *(3×2 for dual-NB, 3×3 for broadband-OSC)*
- `pinv_Q = pseudoinverse(Q)`
- `(line1, line2) = pinv_Q @ (stacked.raw[R_HaO3], stacked.raw[G_HaO3], stacked.raw[B_HaO3])`
- e.g. for HaO3: `(Ha, OIII_HaO3) = pinv_Q @ (R*, G*, B*)`

Multi-source OIII merge:
```
n_HaO3 = stacked.raw["G_HaO3"].sample_count
n_S2O3 = stacked.raw["G_S2O3"].sample_count
OIII = (n_HaO3 * OIII_HaO3 + n_S2O3 * OIII_S2O3) / (n_HaO3 + n_S2O3)
```

For broadband-RGB in mono LRGB mode (R/G/B from filtered mono frames): no Q-solve needed; `stacked.derived["R"] = stacked.raw["R"]` straight through (similar for G, B).

For OSC-as-LRGB: `stacked.derived["L"]` comes from Phase A's synthesized L (rec709 luminance from R+G+B per frame, accumulated independently of native L).

Final derived semantic slot set: `{Ha, OIII, SII, L, R, G, B}` (subset depending on which filter classes were present in batch).

### 5.3 ColorComposer — calibrated palette → sRGB

Default mode (Lab/LCH composite):

For each pixel:
1. `L_broadband = stacked.derived["L"]` *(native L, or synthesized from R+G+B for OSC-only batches)*
2. `RGB_natural = (stacked.derived["R"], stacked.derived["G"], stacked.derived["B"])` if broadband present, else identity
3. `Lab_natural = sRGB_to_Lab(RGB_natural with L_broadband)` — extract `(a*_natural, b*_natural)` as natural chrominance
4. `Emission_a* = w_Ha * pal_Ha.a* + w_SII * pal_SII.a* + w_OIII * pal_OIII.a*`
5. `Emission_b* = w_Ha * pal_Ha.b* + w_SII * pal_SII.b* + w_OIII * pal_OIII.b*`
   where `w_X = signal_strength_weight(stacked.derived[X])`, normalized + gamut-clamped
6. `Lab_final = (L_broadband, a*_natural + Emission_a*, b*_natural + Emission_b*)`
7. Convert `Lab → sRGB`, soft-clip to gamut, emit

Calibrated palette vectors (initial values; tunable after real-data validation):

| Line | a* | b* | Visual |
|---|---|---|---|
| Hα 656nm | +50 | +10 | red (slightly warm) |
| OIII 501nm | -15 | -35 | cyan-blue |
| SII 672nm | +60 | +25 | deep red / red-orange |

Constraint enforced as property test: no vector lives where `a* < 0 AND b* > 0` (the green quadrant).

Continuum-subtract opt-in mode: before computing `Emission_a*b*`, subtract estimated continuum:

```
Ha_pure   = Ha   - k_Ha   * stacked.derived["R"]
OIII_pure = OIII - k_OIII * (stacked.derived["G"] + stacked.derived["B"]) / 2
SII_pure  = SII  - k_SII  * stacked.derived["R"]
```

`k_X` coefficients come from `QEDatabase`: filter passband × camera continuum response. Then `Emission_a*b*` uses `{Ha_pure, OIII_pure, SII_pure}`. Stars stay natural-color; only line-emitting regions get the chrominance boost.

---

## 6. Error Handling

### 6.1 Configuration / startup — refuse to start

| Failure | Behavior |
|---|---|
| `share/qe_database.json` missing or unreadable | ProcessConsole error: "QE database missing — module install corrupt. Reinstall NukeX from PI Resources → Updates." `Execute()` returns false. |
| `~/.nukex4/qe_overrides.json` malformed | Specific error with line/col/parser message. "Override file is malformed at line N: <reason>. Fix or delete and retry." Refuses to start (no silent ignore). |
| `k_continuum` missing for `(filter, camera)` AND continuum-subtract opt-in active | "Continuum subtract requires filter passband data for {filter}; not available in DB. Disable continuum-subtract or update the QE database." Refuses to start opt-in mode; default mode unaffected. |

### 6.2 Per-frame — skip + loud log + continue

Existing behaviors preserved (read failure, dimensions mismatch, alignment failed, saturation > threshold). New:

| Failure | Behavior |
|---|---|
| Frame `BAYERPAT` differs from first-frame | Reject "BAYERPAT mismatch; frame N has BGGR, batch is RGGB" |
| Frame `INSTRUME` differs from prior frames | **Allowed if both cameras in QE DB** — per-frame Q matrix is camera-specific, so multi-camera batches are supported. Log "frame N camera ASI2600MC, prior was ASI585MC — both QE-known". If second camera unknown, falls into 6.3 below. |

### 6.3 Filter / camera resolution (tiered)

Per the user's refinement: no DisambiguationDialog. Sensor-type defaults handle the common cases.

| Condition | Behavior |
|---|---|
| No FITS `FILTER` keyword + Bayer pattern detected | Treat as `BROADBAND_OSC` → OSC-as-LRGB pipeline (with synthesized L). No prompt. |
| No `FILTER` keyword + mono | Treat as `BROADBAND_L` with temp filter name `"L_unnamed"`. Single-channel stack. No prompt. |
| Unknown `FILTER` value + mono | Treat as `BROADBAND_L` with the `FILTER` value as temp name (e.g. `"custom_Hβ"`). Loud Console warning: "Unknown filter '{value}' for mono frame — treating as generic luminance. If this is a narrowband filter, add it to qe_overrides.json." Batch proceeds. |
| Unknown `FILTER` value + Bayer | **Fail loud at batch start.** "FILTER='{value}' on N frames not in QE DB. Add it to ~/.nukex4/qe_overrides.json (see docs) and retry, or remove the FILTER keyword to default to plain OSC." Batch is rejected. |
| Unknown `INSTRUME` (camera) | Falls back to generic Sony IMX OSC defaults from QE DB. Loud warning: "Camera '{value}' unknown; using generic Sony IMX OSC QE values. Output marked as low-confidence in FITS keywords." Batch proceeds. |

The asymmetry between mono and Bayer for unknown filter values is intentional: mono unknown is low-risk (single channel either way), Bayer unknown is the high-impact case where dual-NB-vs-broadband matters and a silent default re-creates the M27-green problem.

### 6.4 Phase B + composer math edges

| Condition | Behavior |
|---|---|
| Q matrix singular for `(camera, filter)` | Fail loud at Phase B start. "Q matrix for ({camera}, {filter}) is singular — QE values must form a non-degenerate basis. Filter QE in DB is suspect. Report bug + check override." |
| Q-solve produces negative emission values | Clamp to 0; count occurrences. Not an error — physically expected in low-photon read-noise regions. Log "clamped N pixels (X% of total) to ≥0" as a quality stat. |
| Lab → sRGB out of gamut | Soft-clip toward gamut boundary preserving hue. Per-pixel count logged. Standard color-science behavior. |
| All frames failed alignment | Existing behavior — empty stacked output, no emit. |
| Multi-source OIII merge with zero contributors | Impossible in a non-empty batch; covered by "all frames failed". |

### 6.5 Observability

Per-frame log line includes filter class + camera + QE confidence:

```
Frame 12/100: light_HaO3_005.fits
  filter:   HaO3 (DUAL_NB_OSC)
  camera:   ASI585MC (QE: high confidence)
  debayered, aligned: ok (stars=842, inliers=783, rms=0.412 px)
  accumulated → R_HaO3, G_HaO3, B_HaO3
```

Phase B summary:

```
Cube has 10 raw slots, 7 derived semantic slots
Q-solve coverage:
  HaO3 / ASI585MC: high-confidence Q matrix from share/qe_database.json
  S2O3 / ASI585MC: high-confidence Q matrix
Multi-source OIII merge: 30 HaO3 contributors + 30 S2O3 contributors
Negative-emission clamp: 142 pixels (0.0006% of total)
```

Composer summary:

```
Output mode: Lab/LCH calibrated palette (default)
Out-of-gamut soft-clipped: 23 pixels (0.0001%)
Emit ImageWindow{3ch, 32bf, RGB, "NukeX_stacked"}
```

---

## 7. Testing

### 7.1 Unit tests (boundaries verified in isolation)

```
test_qe_database_load                share/qe_database.json schema OK, 22 sensors loaded
test_qe_database_malformed_fail      malformed override → loud fail with line/col message
test_qe_database_override_merge      shipped + override = override wins on key collision
test_qe_database_missing             no qe_database.json → refuses to start

test_filter_classifier               HaO3 / S2O3 / L / R / G / B / Hα / OIII / SII → correct class
test_filter_classifier_aliases       "L-eXtreme" == "L_eXtreme" == "L Extreme"
test_filter_classifier_unknown_osc   unknown + Bayer → returns FAIL_LOUD sentinel
test_filter_classifier_unknown_mono  unknown + mono → returns DEFAULT with temp name
test_filter_classifier_no_filter_osc no FILTER + Bayer → BROADBAND_OSC class
test_filter_classifier_no_filter_mono no FILTER + mono → BROADBAND_L "L_unnamed"

test_channel_decomposer_synthetic    inject (R, G, B) corresponding to (Ha=0.5, OIII=0.3) →
                                     Q-solve recovers (0.5, 0.3) ± 1e-6
test_channel_decomposer_singular     forced singular Q matrix → throws SingularQError
test_channel_decomposer_multi_filter Q matrix lookup picks (camera, filter) correctly

test_palette_emission_vectors        Ha vector points to a*+50, b*+10 in CIE-Lab
test_palette_no_greens_invariant     no Lab vector has -a* AND +b* (green quadrant) — property test

test_color_composer_lab_basic        known Lab input → known sRGB output
test_color_composer_gamut_clip       saturated emission → soft-clip preserves hue, log count
test_color_composer_continuum        advanced mode: known continuum input → known emission_pure

test_cube_slot_allocation_pure_hao3  3 slots: R_HaO3, G_HaO3, B_HaO3
test_cube_slot_allocation_lrgbsho    10 slots, correct names
test_voxel_slot_routing_per_filter   HaO3 frame routes into R_HaO3 slot, not R slot

test_multi_source_oiii_merge         30 HaO3 contributors + 30 S2O3 → weighted mean correct
test_multi_source_oiii_zero_one_side one side empty → other side wins (no NaN)

test_lab_lch_composer                single-pixel: known (Ha, OIII) → known sRGB
test_continuum_k_lookup              filter passband × camera continuum → known k values
```

### 7.2 Integration tests (pipeline slices)

```
test_int_pure_hao3_synthetic        synthetic FITS frames with known Ha/OIII signal,
                                    ASI585MC Q matrix → recovered values within tolerance

test_int_lrgbsho_synthetic          multi-class batch, signals across L/R/G/B/Ha/OIII/SII
                                    → all derived slots recover signal

test_int_no_filter_osc              Bayer FITS, no FILTER keyword → no prompt, OSC-as-LRGB

test_int_no_filter_mono             mono FITS, no FILTER → no prompt, "L_unnamed" stack output

test_int_unknown_filter_osc_fail    FILTER="ALP-T-fake-2026" + Bayer → fail loud at batch start

test_int_unknown_filter_mono        FILTER="custom_Hβ" + mono → temp name preserved, batch runs

test_int_qe_override_applied        custom override JSON for unknown camera → Q-solve uses
                                    override values; output differs from default
```

### 7.3 E2E tests (gold-path pixel hashes)

New baselines (replace v4.0.1.0 multi-channel hashes per regression policy C):

```
bayer_rgb_ngc7635_v5     plain OSC stacked / noise / stretched (new L-synth path)
bayer_nb_hao3_m27        HaO3 dual-NB stacked / stretched (the M27 case)
bayer_nb_s2o3_target     S2O3 dual-NB
lrgbsho_target           mixed: L+R+G+B+HaO3 (+ optional S2O3)
mono_lrgb_ngc7635_v5     LRGB-mono with composer (replaces v4.0.1.0 hash)
```

Preserved bit-identical (stretch-math regression floor):

```
sweep_ghs                GHS single-channel hash — MUST NOT CHANGE
sweep_mtf                MTF single-channel hash
sweep_arcsinh            ArcSinh single-channel hash
```

Cold-flake tracking continues; current floor is 6 stretch tests on cold serial. Widening to >6 or any non-stretch flake = real regression to investigate.

### 7.4 Real-data validation

| Target | Visual quality bar |
|---|---|
| M27 HaO3 (motivating) | No green cast; calibrated red+cyan distribution. Specifically: side-by-side vs v4.0.0.7 result must show clear improvement on green pixel histogram. |
| NGC7635 LRGB-mono (existing benchmark) | Detail preserved; color natural. |
| LRGBSHO target (user-supplied) | Broadband natural color base + emission-line boost; no greens in NB-only regions. |

Capture before/after PNG screenshots into `docs/superpowers/specs/2026-04-26-color-science-overhaul/visual-evidence/`.

### 7.5 Performance regression

| Metric | Budget |
|---|---|
| Phase B time | ≤ 10% slower than v4.0.1.0 (linear Q-solve adds work but is cheap) |
| Memory (Phase A) | LRGBSHO worst case: ~+43% histogram memory (10 slots vs 7); must fit disk-cache budget on 6000×4000 images |
| Phase A time | No expected change (debayer + align unchanged; routing is O(slot lookup)) |
| Cold-build flake | ≤ 6 stretch tests (no Ceres math touched) |

---

## 8. Open Items / Future Work

These are explicitly **out of scope** for this design and recorded so they don't surprise us during implementation:

- **Quad-band and tri-band filters** (Optolong L-Quad, Askar D3 etc.) — the research agent gathered some data on these. The QE DB schema supports N-pass filters; the classifier needs `MULTI_NB_OSC` filter class to support them. Out of scope: a future polish release after v5 ships and a user reports they want quad-band.
- **CFA pattern variations beyond RGGB/BGGR/GRBG/GBRG** — IMX294 has a 4-corner pattern that's QGGR-like; treated as RGGB today (per research notes). If a real user reports issues, revisit.
- **Per-frame QE recalibration from bright-star photometry** — would handle atmospheric extinction at scale. Major scope expansion (plate-solving + Gaia/SIMBAD lookup); explicitly deferred.
- **Custom palette presets** (Foraxx, modified-SHO) — only calibrated default ships in v5. Phase 8 ratings tune saturation/brightness around the calibrated baseline. Selectable named presets are post-v5 polish.
- **Phase 8.5 community bootstrap** — already deferred from Phase 8. Color science overhaul ships independently of bootstrap.

---

## 9. References

- v4.0.1.0 ship notes: `docs/plans/2026-04-25-phase8-stats-stretch-tuning.md` (closeout in memory `project_phase8_closeout`)
- M27 motivating dataset: memory `project_m27_first_stretch_feedback`
- Subcube architecture rules: `feedback_subcube_architecture`, `feedback_voxel_is_record`
- QE research output: `research/qe_database_research.json` (22 sensors / 55 cameras / 87 filters)
- No-stub rule (amplified 2026-04-26): `feedback_no_averages_no_stubs`
- Workflow rules: `~/.claude/CLAUDE.md` § Workflow Rules (no workarounds, no silent fallbacks, no stub tickets)
