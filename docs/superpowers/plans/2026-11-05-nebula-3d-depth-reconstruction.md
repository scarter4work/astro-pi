# Nebula 3D Depth Reconstruction — Implementation Plan (Part 1 of 2: reconstruction core + CLI)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the headless Python core that reconstructs an emission (HII) nebula as a blister/champagne-flow cavity from linear channel masters and renders it to a more-dimensional linear image plus a rotatable 3D model.

**Architecture:** A new `recon/` subpackage under `src/gaia_depth_grade/`, driven by two new sidecar subcommands (`reconstruct`, `export3d`) alongside the existing `prepare`/`render`/`nebula`. It reuses the existing two-stage split: the unchanged `prepare` stage supplies the Gaia-anchored star table (for absolute scale + the exciting-star apex); the new `reconstruct` stage consumes that table plus the **starless** channel masters, operating entirely in the existing linear float32 `[0,1]` domain. A shared `CavityModel` (z-field + emissive shells + dust + scale) is the real artifact; the 2D image and the 3D export are both renders of it.

**Tech Stack:** Python 3.11, NumPy, Astropy (FITS/WCS, already in sidecar), `pygltflib` (new, pure-Python glTF/GLB writer), `imageio` (new, orbit GIF). Built into the Rocky 9 portable sidecar image as static/pure-Python deps. Tests: pytest, synthetic analytic fixtures (no I/O), real NAS masters for the validation task.

**Spec:** `docs/superpowers/specs/2026-11-05-nebula-3d-depth-reconstruction-design.md`

## Global Constraints

Every task's requirements implicitly include these (copied verbatim from the spec):

- **Linear, pre-stretch, flux-preserving.** The reconstruction operates on linear channel masters and must not stretch the working data; any stretch is temporary (display/detection only), on a clone. Outputs are linear float32 clipped `[0,1]`; the user stretches downstream.
- **Honest labeling.** The stellar layer is measured (Gaia); the nebula layer is a *physically-motivated reconstruction consistent with HII-region structure*, never "measured depth." Every written output carries an honesty tag to that effect. No UI/doc/FITS copy claims measured gas depth.
- **Depth is portable.** The `CavityModel` (geometry + emissivity) is stretch- and palette-agnostic; outputs are re-renderable from it at any downstream stretch.
- **Graceful degradation across three capture types.** Mono SHO (separate Hα/O III/S II) = full ratio signal; duo-band (HaO3 + S2O3) = partial; broadband OSC (R≈Hα, G/B≈O III proxy) = ratio term downweighted, geometry prior dominates. Lower ratio confidence must still yield a physically-consistent concave cavity, never a crash or a flat core.
- **Reuse, don't fork.** Reuse `cli._read_image` / `cli.write_fits` (linear FITS), the `prepare_grade` table, and the existing cache pattern. Additive changes to the catalog schema must keep existing `_REQUIRED` consumers passing.
- **No OpenMP.** New deps (`pygltflib`, `imageio`) must be vendorable into the Rocky 9 image (both are pure-Python / wheels).
- **Co-registration required.** The channel masters and the prep grid share one WCS/pixel grid; mismatched shapes are a hard error, never silent misalignment.

## Review Focus

Input classes the spec implies but that no single happy-path test exercises — each gets a test in the owning task:

- **Missing S II** (duo-band / OSC have no S II): ionization + reconstruction must run on `sii=None` without crashing and without placing phantom front structure. → Tasks 3, 8.
- **No per-star colour** (offline TSV or online catalog without `bp_rp`): ionizer pick must fall back to brightest-luminous, not error. → Task 5.
- **A finished/stretched RGB fed to the linear path:** must be detected (non-linearity heuristic), forced to geometry-prior-only, and labeled — never silently emit physics-flavored output from baked data. → Tasks 2, 10.
- **No exciting star in the field:** ionizer apex falls back to the geometric footprint center; a cavity is still produced. → Tasks 5, 8.
- **Channel masters of mismatched shape / not co-registered:** hard, clear error at ingest. → Tasks 2, 10.

---

### Task 1: Shared model types (`recon/model.py`)

**Files:**
- Create: `src/gaia_depth_grade/recon/__init__.py`
- Create: `src/gaia_depth_grade/recon/model.py`
- Test: `tests/recon/test_model.py`

**Interfaces:**
- Produces:
  - `@dataclass(frozen=True) ChannelSet(ha: np.ndarray, oiii: np.ndarray, sii: np.ndarray | None, ratio_confidence: float, is_linear: bool)` — all arrays same `(ny,nx)` shape, float32.
  - `@dataclass(frozen=True) IonizationMap(oiii_fraction: np.ndarray, confidence: float)` — `oiii_fraction ∈ [0,1]`, `(ny,nx)`.
  - `@dataclass(frozen=True) Ionizer(x: float, y: float, distance_pc: float | None, source: str)` — `source ∈ {"gaia-blue","gaia-bright","field-center","override"}`.
  - `@dataclass(frozen=True) CavityModel(z: np.ndarray, emissivity: np.ndarray, dust: np.ndarray, pc_per_px: float | None, apex_xy: tuple[float,float], meta: dict)` — `z` in px-relative depth units (observer at `-∞`, larger = farther), `emissivity` `(ny,nx,3)` linear, `dust` `(ny,nx)` opacity `[0,1]`.
  - `validate_shapes(*arrays) -> None` — raises `ValueError` on shape mismatch.

- [ ] **Step 1: Write the failing test** `tests/recon/test_model.py`

```python
def test_channelset_rejects_shape_mismatch():
    import numpy as np, pytest
    from gaia_depth_grade.recon.model import ChannelSet
    a = np.zeros((4,4), np.float32); b = np.zeros((4,5), np.float32)
    with pytest.raises(ValueError):
        ChannelSet(ha=a, oiii=b, sii=None, ratio_confidence=1.0, is_linear=True)

def test_cavitymodel_fields_roundtrip():
    # construct a minimal valid CavityModel, assert z/emissivity/dust shapes consistent
    ...
```

- [ ] **Step 2: Run to verify it fails** — `pytest tests/recon/test_model.py -v` → FAIL (module missing).
- [ ] **Step 3: Implement the dataclasses + `validate_shapes` in `recon/model.py`.** `__post_init__` on `ChannelSet`/`CavityModel` calls `validate_shapes`. `sii=None` is allowed.
- [ ] **Step 4: Run to verify it passes** — `pytest tests/recon/test_model.py -v` → PASS.
- [ ] **Step 5: Commit** — `feat(recon): shared CavityModel/ChannelSet/Ionizer types`.

---

### Task 2: Channel ingest (`recon/channels.py`)

**Files:**
- Create: `src/gaia_depth_grade/recon/channels.py`
- Test: `tests/recon/test_channels.py`

**Interfaces:**
- Consumes: `ChannelSet` (Task 1).
- Produces:
  - `from_mono_sho(ha, oiii, sii) -> ChannelSet` (confidence `1.0`).
  - `from_duoband(d1_hao3, d2_s2o3) -> ChannelSet` — unmix: `oiii = min(d1,d2)` shared estimate; `ha = max(d1 - oiii, 0)`; `sii = max(d2 - oiii, 0)`; confidence `0.6`.
  - `from_osc_rgb(rgb) -> ChannelSet` — `ha = R`, `oiii = 0.5*(G+B)`, `sii = None`; confidence `0.25`.
  - `looks_linear(arr) -> bool` — non-linearity heuristic (histogram of a starless master: a stretched image has a filled midtone/raised black point; linear emission is sky-floored and right-skewed). Returns `False` for an obviously stretched array.
  - `extract_channels(inputs: dict, mode: str) -> ChannelSet` — dispatch on `mode ∈ {"sho","duoband","osc"}`; validates co-registration (shapes equal) else `ValueError`; if `looks_linear` is False, sets `is_linear=False` and zeroes `ratio_confidence` (forces geometry-only downstream).

- [ ] **Step 1: Write failing tests** `tests/recon/test_channels.py`

```python
def test_sho_passthrough_full_confidence(): # arrays unchanged, confidence==1.0
def test_duoband_unmix_recovers_planted_lines():
    # plant ha,oiii,sii; d1=ha+oiii, d2=sii+oiii; assert recovered within 10%, confidence==0.6
def test_osc_proxy_maps_channels(): # ha==R, oiii==0.5*(G+B), sii is None, confidence==0.25
def test_mismatched_shapes_raise(): # extract_channels with unequal shapes -> ValueError
def test_stretched_input_zeros_ratio_confidence():
    # a synthetic stretched (sigmoid'd) master -> is_linear False, ratio_confidence==0.0
```

- [ ] **Step 2: Run to verify they fail.**
- [ ] **Step 3: Implement `channels.py`** with the signatures above. `looks_linear`: flag as stretched when `median > k*MAD`-style midtone fill exceeds a threshold (one defensible heuristic; implementer tunes the constant against the fixture).
- [ ] **Step 4: Run to verify pass.**
- [ ] **Step 5: Commit** — `feat(recon): linear channel ingest for SHO/duoband/OSC with non-linearity guard`.

---

### Task 3: Ionization map (`recon/ionization.py`)

**Files:**
- Create: `src/gaia_depth_grade/recon/ionization.py`
- Test: `tests/recon/test_ionization.py`

**Interfaces:**
- Consumes: `ChannelSet` (Task 1/2).
- Produces: `ionization_ratio(ch: ChannelSet, eps: float = 1e-6) -> IonizationMap` — `oiii_fraction = oiii / (oiii + ha + eps)`, smoothed, clipped `[0,1]`; `confidence = ch.ratio_confidence`. S II, when present, sharpens the front term but `sii=None` is fully supported.

- [ ] **Step 1: Write failing tests**

```python
def test_oiii_bright_core_reads_high_fraction(): # planted OIII-dominant disc -> oiii_fraction>0.7 there
def test_ha_dominant_edge_reads_low_fraction(): # Ha-only ring -> oiii_fraction<0.3
def test_handles_sii_none(): # ChannelSet(sii=None) -> no crash, valid map
def test_confidence_propagates(): # map.confidence == ch.ratio_confidence
```

- [ ] **Step 2–4:** fail → implement → pass.
- [ ] **Step 5: Commit** — `feat(recon): ionization ratio map (OIII fraction) as depth coordinate`.

---

### Task 4: Carry Gaia colour through the distance path (additive)

**Files:**
- Modify: `src/gaia_depth_grade/distances.py` (schema + `build_adql` + `GaiaStarSource`)
- Modify: `src/gaia_depth_grade/local_gaia.py` (`parallax_to_distance_table`, `_parse_lines`)
- Test: `tests/test_distances.py`, `tests/test_local_gaia.py` (extend)

**Interfaces:**
- Produces: an **optional** `bp_rp` column on the distance `Table`. New constant `_OPTIONAL = ("bp_rp",)`; consumers still read `_REQUIRED` unchanged. Online SELECT adds `bp_rp`; offline TSV gains an optional 5th column; when absent the column is filled with `NaN` (not dropped, not required).

- [ ] **Step 1: Write failing tests**

```python
def test_required_consumers_unaffected(): # existing prepare_grade path still passes (regression guard)
def test_online_select_includes_bp_rp(): # build_adql(...) contains "bp_rp"
def test_offline_tsv_optional_bp_rp():
    # 5-col TSV -> bp_rp populated; 4-col TSV -> bp_rp all-NaN, no error
```

- [ ] **Step 2: Run — the 4-col/regression tests pass, the bp_rp tests FAIL** (column absent). Confirm the regression guard is green against current code *before* changing it.
- [ ] **Step 3: Implement** additive `bp_rp` (optional) in both sources; keep `tbl[list(_REQUIRED)]` consumers intact by selecting `_REQUIRED + present-optionals`.
- [ ] **Step 4: Run — all pass**, including the untouched existing distance tests.
- [ ] **Step 5: Commit** — `feat(distances): carry optional Gaia bp_rp colour (additive, non-breaking)`.

---

### Task 5: Exciting-star identification (`recon/ionizer.py`)

**Files:**
- Create: `src/gaia_depth_grade/recon/ionizer.py`
- Test: `tests/recon/test_ionizer.py`

**Interfaces:**
- Consumes: the `prepare_grade` table (with optional `bp_rp` from Task 4), `lum` shape, `FieldFootprint`.
- Produces:
  - `pick_ionizer(table, shape, footprint) -> Ionizer` — among stars inside the footprint, pick the **bluest luminous** (low `bp_rp`, high `flux`); if no `bp_rp` present/finite, fall back to brightest (`source="gaia-bright"`); if the table is empty, return the footprint-center apex with `distance_pc=None` (`source="field-center"`).
  - `ionizer_from_xy(x, y, distance_pc) -> Ionizer` (`source="override"`).

- [ ] **Step 1: Write failing tests**

```python
def test_picks_blue_luminous_star(): # table with a blue bright star + red bright star -> blue one
def test_falls_back_to_brightest_without_colour(): # bp_rp all NaN -> brightest flux
def test_empty_table_returns_field_center(): # source=="field-center", apex at shape center
def test_override_returns_specified_point(): # ionizer_from_xy echoes inputs, source=="override"
```

- [ ] **Step 2–4:** fail → implement → pass.
- [ ] **Step 5: Commit** — `feat(recon): Gaia-anchored exciting-star (ionizer) identification with fallbacks`.

---

### Task 6: Dust / extinction near-side layer (`recon/extinction.py`)

**Files:**
- Create: `src/gaia_depth_grade/recon/extinction.py`
- Test: `tests/recon/test_extinction.py`

**Interfaces:**
- Produces:
  - `dust_from_structure(lum) -> np.ndarray` — `[0,1]` opacity; dark lanes embedded in bright surroundings → high opacity (near-side occluder), sky floor → 0.
  - `dust_from_balmer(ha, hb, eps=1e-6) -> np.ndarray` — optional; higher `ha/hb` above the Case B ratio 2.86 → more extinction. Used only when an Hβ master is supplied.

- [ ] **Step 1: Write failing tests**

```python
def test_dark_lane_reads_high_opacity(): # planted dark stripe in bright field -> high dust there, ~0 in sky
def test_balmer_decrement_tracks_extinction(): # raised Ha/Hb region -> higher dust_from_balmer
```

- [ ] **Step 2–4:** fail → implement → pass.
- [ ] **Step 5: Commit** — `feat(recon): near-side dust layer from structure and optional Balmer decrement`.

---

### Task 7: Cavity depth transfer functions (`recon/cavity.py`, part 1)

**Files:**
- Create: `src/gaia_depth_grade/recon/cavity.py`
- Test: `tests/recon/test_cavity_terms.py`

**Interfaces:**
- Produces three pure, independently testable term functions, each returning a `(ny,nx)` depth contribution normalized to `[0,1]` (0 = nearest observer, 1 = farthest):
  - `geometry_prior(shape, apex_xy, footprint_mask) -> z_geo` — concave bowl: depth increases from rim toward a well whose axis passes through `apex_xy`; the projected center is the deepest sightline.
  - `ionization_depth(ion_map: IonizationMap) -> z_ion` — high `oiii_fraction` (interior, near the hot star) → nearer/forward on the cavity wall; low (front/outer) → farther. (Physically: O III fills the cavity interior; Hα/S II hug the far ionization front.)
  - `emission_measure_relief(lum) -> z_em` — bright filamentary edges (tangent sightlines, limb-brightening) bulge along the line of sight; returns a signed relief centered on 0.

- [ ] **Step 1: Write failing tests**

```python
def test_geometry_prior_is_concave_about_apex(): # radial profile from apex increases then the center sightline is deepest
def test_ionization_depth_places_oiii_forward(): # high oiii_fraction -> smaller z than low
def test_emission_measure_relief_bulges_bright_edges(): # bright rim -> relief != 0, sky -> ~0
```

- [ ] **Step 2–4:** fail → implement → pass. Each formula is a defensible first version; constants are the implementer's to tune against the Task 8 blister fixture.
- [ ] **Step 5: Commit** — `feat(recon): cavity depth transfer functions (geometry, ionization, emission-measure)`.

---

### Task 8: Cavity assembly (`recon/cavity.py`, part 2)

**Files:**
- Modify: `src/gaia_depth_grade/recon/cavity.py`
- Test: `tests/recon/test_cavity.py` (add synthetic blister fixture to `tests/recon/conftest.py`)

**Interfaces:**
- Consumes: `ChannelSet`, `IonizationMap`, `Ionizer`, `dust` (Tasks 1–7), `pc_per_px` from the Gaia-anchored table, a `ReconConfig` (gains/weights; new frozen dataclass in `recon/model.py` or `config.py`).
- Produces: `reconstruct_cavity(channels, ionization, ionizer, lum, dust, pc_per_px, config) -> CavityModel` — combines the three depth terms with **confidence-weighted** blending: weight on `ionization_depth ∝ ionization.confidence`, so OSC (low confidence) leans on `geometry_prior`; builds `z`, `emissivity` (linear colour from the channels placed by depth), carries `dust`, `apex_xy`, `pc_per_px`, and a `meta` dict recording term weights + capture mode.

**Fixture:** `synthetic_blister` — a planted concave front (known apex + curvature), O III-bright interior, Hα/S II-bright rim, one dark near-side lane; plus a `truth` describing apex, concavity sign, interior-vs-rim ionization.

- [ ] **Step 1: Write failing tests**

```python
def test_reconstructs_concave_cavity_with_apex(): # recovered z concave; deepest near planted apex
def test_oiii_interior_placed_forward_of_front(): # mean z(OIII-region) < mean z(Ha/SII-region)
def test_dust_lane_is_near_side(): # dust region has small z (foreground), occludes behind
def test_low_ratio_confidence_still_concave(): # same fixture via OSC ingest -> still concave (geometry carries it)
def test_sii_none_supported(): # duoband/OSC path reconstructs without SII
```

- [ ] **Step 2–4:** fail → implement → pass.
- [ ] **Step 5: Commit** — `feat(recon): assemble confidence-weighted blister CavityModel`.

---

### Task 9: 2D render of the model (`recon/render2d.py`)

**Files:**
- Create: `src/gaia_depth_grade/recon/render2d.py`
- Test: `tests/recon/test_render2d.py`

**Interfaces:**
- Consumes: `CavityModel` (Task 8).
- Produces: `render_cavity_2d(model: CavityModel, config) -> np.ndarray` — a linear `(ny,nx,3)` float32 `[0,1]` image at the original camera viewpoint: front-to-back **depth ordering / occlusion** (dust and near gas over far gas), **limb-brightening** on the walls, and depth-keyed contrast so the bright core reads as a concave well. Output stays pixel-registered to the input grid. A degenerate model (flat `z`, zero dust) returns the emissivity ~unchanged (identity guard).

- [ ] **Step 1: Write failing tests**

```python
def test_output_registered_linear_rgb(): # shape (ny,nx,3), float32, within [0,1]
def test_core_reads_concave(): # a measurable depth-shading metric: center recedes relative to rim vs the flat emissivity
def test_dust_occludes_background(): # pixels behind a near dust lane are darkened/hidden
def test_degenerate_model_is_near_identity(): # flat z + zero dust -> np.allclose(out, emissivity, atol=small)
```

- [ ] **Step 2–4:** fail → implement → pass.
- [ ] **Step 5: Commit** — `feat(recon): render CavityModel to a registered linear dimensional image`.

---

### Task 10: `reconstruct` CLI subcommand + multi-master reader

**Files:**
- Modify: `src/gaia_depth_grade/cli.py` (subparser, handler, a `_read_masters` helper)
- Test: `tests/test_cli_reconstruct.py`

**Interfaces:**
- Consumes: `extract_channels`, `ionization_ratio`, `pick_ionizer`, `dust_from_structure`, `reconstruct_cavity`, `render_cavity_2d`, existing `load_prep`/`write_fits`.
- Produces: subcommand
  `reconstruct <cache_dir> <out_model.npz> <out_image.fits> --mode {sho,duoband,osc} --ha P [--oiii P --sii P | --d1 P --d2 P | --rgb P] [--hb P] [--ionizer X,Y[,DIST]]`
  → loads the prep table (ionizer + `pc_per_px`), reads the masters via `_read_masters` (co-registration checked), runs the pipeline, writes the `CavityModel` as `.npz` and the 2D image as linear FITS via `write_fits` with an honesty tag (`"Reconstructed HII cavity; physically motivated, not measured gas depth"`). A non-linear input (`looks_linear` False) proceeds geometry-only and records `meta["degraded"]="stretched-input"`.

- [ ] **Step 1: Write failing tests**

```python
def test_reconstruct_sho_end_to_end(tmp_path):
    # main(["reconstruct", cache, model_npz, img_fits, "--mode","sho","--ha",..,"--oiii",..,"--sii",..])
    # -> both files exist; FITS is linear float32; honesty tag present; model z concave
def test_reconstruct_osc_degrades(tmp_path): # --mode osc -> meta["degraded"] or confidence 0 path, still writes
def test_mismatched_master_shapes_exit_nonzero(tmp_path):
```

- [ ] **Step 2–4:** fail → implement → pass. Reuse `_read_image` per master inside `_read_masters`.
- [ ] **Step 5: Commit** — `feat(cli): reconstruct subcommand (masters -> CavityModel + linear image)`.

---

### Task 11: 3D export (`recon/export3d.py`) + `export3d` CLI

**Files:**
- Create: `src/gaia_depth_grade/recon/export3d.py`
- Modify: `src/gaia_depth_grade/cli.py` (subparser + handler)
- Modify: packaging (add `pygltflib` to the sidecar deps / Rocky 9 image spec)
- Test: `tests/recon/test_export3d.py`, `tests/test_cli_export3d.py`

**Interfaces:**
- Consumes: `CavityModel`.
- Produces:
  - `export_glb(model: CavityModel, path: str, decimate: int = 2) -> None` — emit a GLB: the cavity surface as a mesh (vertices from the `z` field on a decimated grid) with per-vertex emissive colour from `emissivity`; dust as an opacity/near occluder layer or a second surface. Uses `pygltflib`.
  - CLI: `export3d <model.npz> <out.glb> [--decimate N]`.

- [ ] **Step 1: Write failing tests**

```python
def test_glb_has_expected_vertex_grid(): # (ny/decimate)*(nx/decimate) vertices, valid GLB header
def test_glb_surface_is_concave(): # vertex z reproduces model concavity
def test_cli_export3d_writes_glb(tmp_path): # main(["export3d", model, out.glb]) -> file exists, loads
```

- [ ] **Step 2–4:** fail → implement → pass.
- [ ] **Step 5: Commit** — `feat(recon): GLB export of the cavity model + export3d subcommand`.

---

### Task 12: Orbit animation

**Files:**
- Modify: `src/gaia_depth_grade/recon/export3d.py` (`render_orbit`)
- Modify: `src/gaia_depth_grade/cli.py` (`--orbit out.gif --frames N` on `export3d`)
- Modify: packaging (add `imageio`)
- Test: `tests/recon/test_orbit.py`

**Interfaces:**
- Produces: `render_orbit(model, path, frames=36, amplitude_deg=12) -> None` — render N views by tilting the camera about the model (small parallax orbit using `render_cavity_2d` with a shifted viewpoint / re-projected `z`), write an animated GIF via `imageio`.

- [ ] **Step 1: Write failing tests**

```python
def test_orbit_writes_frames(tmp_path): # GIF exists, N frames
def test_orbit_frames_differ(): # first vs middle frame differ beyond noise (rotation happened)
```

- [ ] **Step 2–4:** fail → implement → pass.
- [ ] **Step 5: Commit** — `feat(recon): parallax orbit GIF export`.

---

### Task 13: Real-data validation harness (acceptance)

**Files:**
- Create: `gaia-depth-grade/test/run-recon-validation.sh` (host-only; reads the NAS; no PixInsight launch)
- Create: `src/gaia_depth_grade/recon/validate.py` (metrics)
- Test: `tests/recon/test_validate_metrics.py` (unit-tests the metric functions on synthetic truth)

**Interfaces:**
- Produces:
  - `cavity_depth_index(model) -> float` — a scalar summarizing cavity depth (e.g. apex-to-rim z span over field scale).
  - `interior_vs_front_ratio(model, ionization) -> float` — mean z(low-OIII front) − mean z(high-OIII interior); positive means front is correctly farther.
  - The shell script: on the confirmed NAS masters, run `prepare` + `reconstruct` for **M42 duo-band** (`9_24_2023/M42`), **M42 broadband** (`2_4_2023/M42_*`), **M8 duo-band** (`8_8_2023/M8`), **M8 broadband** (`5_10_2023/M8`); write side-by-side renders + a metrics JSON; assert programmatically: (a) `interior_vs_front_ratio > 0` for each; (b) `cavity_depth_index(M8) > cavity_depth_index(M42)` (deeper cavity); (c) duo-band vs broadband of the same target agree in cavity *sign* (degradation consistency).

**Acceptance (spec):** the quantitative asserts above run green on real data (never a stub, never on deselected tests), **and** Scott signs off on the before/after renders — current grade vs reconstruction on M42 (duo-band) and M8, against the Robles M8 reference — confirming the bright core reads as a concave cavity. Record the sign-off row in `docs/qa/` per project convention.

- [ ] **Step 1: Write failing tests** for the metric functions on synthetic `CavityModel`s with known depth ordering (`test_validate_metrics.py`).
- [ ] **Step 2–4:** fail → implement metrics → pass.
- [ ] **Step 5:** Run `test/run-recon-validation.sh` on the NAS masters; confirm the three programmatic asserts pass; produce the before/after renders for Scott's sign-off.
- [ ] **Step 6: Commit** — `test(recon): real-data validation harness (M42/M8, duoband+broadband)`.

---

## Not in this plan (Part 2: PixInsight / PJSR integration)

A separate plan covers: multi-channel view selection in the dialog, per-channel `saveAs` into the private work dir, passing the master paths as new positional argv (the contract is already files-on-disk + argv), the new panel controls (mode, ionizer click-override, weights), loading the GLB/orbit for preview, and the recombine point for the reconstructed image. Part 1 is independently shippable and testable through the sidecar CLI, which is how it gets validated on the NAS data before any PI wiring.

## Self-review notes

- **Spec coverage:** blister model → Tasks 7–8; honesty labeling → Global Constraints + Task 10 tag; three capture types → Task 2 + confidence weighting Task 8; ionization stratification → Tasks 3,7,8; Gaia anchor/ionizer → Tasks 4,5; dust occlusion → Tasks 6,8,9; primary 2D image → Task 9/10; rotatable model + orbit (the addition) → Tasks 11,12; portability (model is the artifact) → Task 8 `CavityModel` + `.npz`; linear/pre-stretch → Global Constraints enforced in Tasks 2,9,10; NAS validation incl. "M8 deeper than M42" → Task 13.
- **Review Focus:** all five lines have an owning test (Tasks 2,3,5,8,10).
- **Type consistency:** `ChannelSet`/`IonizationMap`/`Ionizer`/`CavityModel` defined once in Task 1 and consumed by name thereafter; `pc_per_px` sourced from the prep table in Tasks 8/10.
- **Dependencies:** `pygltflib` (Task 11) and `imageio` (Task 12) are the only new deps; both pure-Python/wheel, added to the Rocky 9 sidecar image in their tasks.
