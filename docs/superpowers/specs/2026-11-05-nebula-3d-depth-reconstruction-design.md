# Nebula 3D Depth Reconstruction — gaia-depth-grade

Design document. Subproject: `gaia-depth-grade/`. Date: 2026-11-05.

## Intent and why

gaia-depth-grade today grades the **starless nebula layer** with generic
atmospheric perspective — "dust forward, glow recedes," structure-driven, applied
as a 2D per-pixel modulation (`src/gaia_depth_grade/nebula.py` →
`modulate.py`/`render.py`). It works, but it **flattens bright HII cores**: a
reference frame like Víctor Robles' M8 reads as *looking into the throat of the
nebula*, and our output does not. The bright emitting center comes forward as a
flat wall instead of receding into a cavity.

The cause is that the current grade is a perceptual push-back with no model of the
object. The fix is to reconstruct the **scientifically-agreed 3D structure** of the
nebula and render *from* that reconstruction, so the depth is physically grounded
rather than painted. The user looking into the cavity is then correct, not a trick.

This is **additive**. The existing stellar depth grade (Gaia parallax) and the
existing nebula modulation stay. This adds a new reconstruction stage and two new
outputs, selectable as a mode; the current behavior remains the default path.

**Positioning — a linear, pre-stretch pre-processing stage (decided).** The
physically-grounded signals this depends on (the O III/Hα ionization ratio, emission
measure, Balmer-decrement extinction) are **flux relationships**: they are only valid on
linear data and are destroyed the moment channels are mapped to a palette and stretched.
So this reconstruction belongs **early** in the workflow — same family as NukeX: do the
structural work in linear space, stretch last — not as a cosmetic grade at the end. (The
current cosmetic grade stays available for finished images; it just isn't where the real
reconstruction lives.)

The consequence is the key property of the whole design: **the depth is portable.** The
reconstruction's real artifact is the 3D cavity model (geometry + emissive volume), which
is **stretch- and palette-agnostic**. The user applies their own stretch and palette
downstream and the dimensionality renders faithfully through whatever they choose, instead
of a baked-in look that a later stretch would wreck. The flat "dimensional image" is one
rendered *view* of that model at one stretch, and it is re-renderable; the rotatable export
is the same model from a different camera, which is why it is nearly free.

## The physical object we reconstruct

M8 and M42 — the archetypes the user cares about — are **blister / champagne-flow
HII regions**. The scientifically-agreed structure (research briefs, sources at
end):

- A hot exciting star (or few) carves a **cavity in the near face of a background
  molecular cloud** and sits *in front of* that cloud, inside the cavity, on the
  observer's side. M42: θ¹ Ori C ~0.23 pc in front of the ionization front. M8:
  9 Sgr / HD 165052 / Herschel 36 stand off **1–1.8 pc** — a deeper, looser cavity.
- The glowing gas is a **thin fluorescing skin (~0.06 pc in M42) on a concave
  ionization front** — the far wall of a bowl — plus lower-density ionized gas
  flowing out of the open mouth *toward us*. Emissivity ∝ nₑ², so light is weighted
  onto walls, rims and dense knots: a **membrane on a 3D surface, not a filled
  volume**.
- **We look into the open mouth of the bowl.** The projected center is the deepest
  sightline down the cavity axis; the rim is the wall curving back toward the viewer.
- **Depth is encoded in the ionization.** [O III] (teal) fills the high-ionization
  interior near the hot star; Hα/[N II]/[S II] (red) hug the ionization front and
  outer skin. This stratification is a real radial coordinate within the structure.
- **Dark lanes** (the "Lagoon," Orion's dark bay) are **near-side dust occluding**
  the emission behind them.
- The **Orion Bar** is the same ionization front seen **edge-on** — bright because
  of the long tangent path length, not because it is a separate object.

## The honesty constraint (hard; no-overclaim rule)

Line-of-sight depth of the gas is **not measurable** from the data this tool has.
There is no parallax for gas; Gaia measures the stars. Recovering true depth needs
spectroscopic expansion kinematics the user does not shoot.

What the tool does is **reconstruct depth from a physical prior (the blister
geometry), constrained — not determined — by the 2D maps the user already captures**
(the O III/Hα(/S II) ionization ratio, Hα surface brightness / emission measure, and
dust extinction). This is exactly the paradigm pro photoionization / morpho-kinematic
modeling uses when kinematic data is absent.

Therefore every output, label, and doc describes this as **"physically-motivated
depth reconstruction consistent with HII-region structure,"** never as "measured
depth." The stellar layer (Gaia parallax) *is* measured and may be described as such;
the nebula layer is reconstructed. This separation must be visible to the user.

## Goals

1. Reconstruct an explicit 3D model of an emission (HII) nebula as a blister cavity:
   concave ionization front, exciting star in the cavity, emitting gas placed on/within
   that surface, dust on the near skin, all at Gaia-anchored absolute scale.
   The reconstruction operates in **linear space** and the 3D model is the real,
   **stretch- and palette-agnostic artifact**; the two outputs below are renders of it.
2. **Primary output:** a more three-dimensional **flat image**, rendered from the 3D
   model back to the original camera viewpoint — correct occlusion, limb-brightening,
   and depth-keyed structure so the bright core reads as a concave cavity. Stays
   registered to the input framing so it composites with the user's other processing, and
   is **re-renderable** at any stretch/palette the user later chooses (the depth is carried
   as geometry, not baked into one look).
3. **Secondary output (the addition):** the same reconstruction exported as a
   **rotatable 3D model**, openable outside PixInsight, plus an optional orbit
   animation. Nearly free once the reconstruction exists — same model, different camera.
4. Work across all three of the user's capture types, degrading gracefully:
   - **Mono SHO** — separate Hα/O III/S II: full stratification signal (best case).
   - **Duo-band** (Askar ColorMagic D1 = HaO3, D2 = S2O3): partially separable.
   - **Broadband OSC** (ASI2400MC): no true line separation — R≈Hα, G/B≈O III proxy;
     the cavity leans on the geometry prior + brightness, with the ratio signal
     downweighted.
5. Reuse the existing pipeline: plate-solved WCS, offline Gaia DR3 (`local_gaia.py`,
   `distances.py`), star/nebula separation, and the Gaia stellar depth grade — all
   unchanged. Ship through the existing architecture (signed thin PJSR +
   linux-x64 sidecar).

## Non-goals

- Non-blister morphologies in this version: planetary nebulae, supernova remnants,
  reflection nebulae, galaxies. They need different priors; they are future work.
- Spectroscopic / kinematic (Doppler expansion) reconstruction — out of data reach.
- Any claim of measured gas depth.
- Replacing or removing the current nebula grade or stellar grade.

## Anti-goals (failure even if it runs)

- A result that **looks** 3D but **contradicts the physics**: O III placed at the far
  wall instead of the interior; a cavity bulging convex *toward* the viewer; the
  exciting star behind the front.
- The bright core still rendering **flat** — the exact defect this exists to fix.
- Overclaiming measurement anywhere in UI or docs.
- Breaking the existing depth-grade outputs or the repo-pull install path.

## Constraints

- **Inputs:** a plate-solved PI image (WCS required, as today, for Gaia anchoring)
  plus whatever channel data the capture provides (SHO / duo-band / OSC per Goal 4).
- **Gaia:** offline DR3 `.xpsd` already in the pipeline; reused both to anchor absolute
  distance/scale and to identify the exciting star(s).
- **Architecture:** PJSR launches everything; the user never runs Python by hand
  (consistent with the whole project). New third-party deps must be vendorable; prefer
  the most capable option that can be (project rule: "light does not make right").
  Sidecar is built in the Rocky 9 portable image like the rest.
- **Registration:** the 2D output must stay pixel-registered to the input so it drops
  into the user's existing workflow.
- **Linear, pre-stretch, flux-preserving:** the full-reconstruction path operates on
  linear channel masters and must not stretch the working data; any stretch is temporary
  (display/detection only), applied to a clone, as in NukeX. Outputs are linear; the user
  stretches downstream. This is what makes the depth portable and is non-negotiable for the
  ratio/EM/extinction terms to be physical.

## The reconstruction — decided approach

Stages, building on the existing pipeline (`pipeline.py`):

1. **Star / nebula separation** — as today (StarXTerminator path).
2. **Exciting-star identification** — pick the dominant ionizer(s): the hottest /
   bluest luminous O/B star(s) in the field from Gaia photometry + catalog, within the
   nebula footprint. User-overridable (click the star). Its parallax anchors absolute
   distance and the cavity apex position. *(Auto-detect heuristic details left to the
   builder.)*
3. **Ionization map** — from available channels, compute a per-pixel excitation ratio
   (O III/Hα, and O III/S II where present). This is the depth-ordering signal. Its
   confidence is set by capture type (SHO high → OSC low); low confidence downweights
   this term in stage 4.
4. **Cavity reconstruction** — place each nebula patch on/within a 3D concave front
   z(x,y) (a bowl opening toward the viewer, apex anchored near the exciting star)
   from four physically-grounded terms:
   - **Geometry prior:** radial position in the projected nebula → depth down the
     bowl axis (center deep, rim near).
   - **Ionization ratio:** O III-bright → inner/near-star cavity volume; Hα/S II-bright
     → ionization front (far wall) and outer skin.
   - **Emission measure / brightness:** bright filamentary edges = tangent sightlines
     on the shell → limb-brightening, walls bulging along the line of sight.
   - **Extinction / dark lanes:** pulled to a **near-side opaque dust skin** (occlusion).
   The gas has thickness (near wall, interior, far wall), so the representation is an
   **emissive volume or layered shells**, not a single height map. *(Exact math and
   representation — voxel emissive volume vs. layered depth shells vs. displaced
   surface — left to the builder; see Open choices.)*
5. **Dust layer** — dark lanes become near-side occluding sheets in the scene.
6. **Stars** — placed by Gaia parallax (existing grade) into the same 3D scene:
   foreground stars in front of the gas, embedded stars behind.

## Rendering — decided approach

- **2D enhanced image (primary):** render the emissive 3D scene back to the original
  camera viewpoint with correct depth-ordering, occlusion, limb-brightening, and a
  depth-keyed color/contrast lift so the cavity reads concave. Same framing, now with
  physically-correct depth cues. Output stays registered to the input.
- **Rotatable 3D (addition):** export the emissive model as a standard 3D asset plus an
  optional orbit animation, openable outside PixInsight. *(Format — glTF with emissive
  / volumetric, point cloud, or voxel — left to the builder; proposed default glTF +
  orbit render, see Open choices.)*

## Validation

- **Synthetic blister fixtures** with known geometry and known O III/Hα stratification:
  the reconstruction must recover the planted cavity (apex, concavity, inner-vs-front
  ionization) within tolerance. Extends `tests/test_e2e_synthetic.py` /
  `test_nebula.py`.
- **Real data (confirmed on NAS `/mnt/qnap/astro_data/`):** reconstruct M8 and M42 and
  check against the published geometry — O III interior, Hα/S II front, concave bowl,
  *deeper* cavity for M8 than M42 — and compare qualitatively to the Robles reference.
  Available masters:
  - **M42 duo-band:** `9_24_2023/M42/` — 300 s HaO3 (D1) + S2O3 (D2) subs → Hα/O III/S II
    extractable (full stratification case).
  - **M42 broadband:** `2_4_2023/M42_*` — 30 s broadband mosaic panels.
  - **M8 duo-band:** `8_8_2023/M8/` — HaO3 subs.
  - **M8 broadband:** `5_10_2023/M8/`, `5_26_2023/M8/`.
  The **same target in two capture types** (duo-band + broadband, both M8 and M42) makes
  the degradation check runnable on real data without new captures. No mono-SHO master of
  these targets on the NAS — clean three-channel SHO is covered by synthetic fixtures.
- **Degradation:** the same target through SHO vs duo-band vs OSC must stay
  physically consistent (same cavity sense), just softer as the ratio signal weakens.
- **No-overclaim check:** UI/doc copy audited so nothing claims measured gas depth.

## What is left to the builder

- Surface/volume representation and the reconstruction math (voxel emissive volume vs.
  layered depth shells vs. displaced surface) and the ratio→depth transfer functions
  and their tuning.
- The exciting-star auto-detect heuristic.
- The 3D export format and the orbit-animation renderer.
- The gaia-depth-grade panel UI for the new mode and its controls.
- Per-capture-type channel extraction details (duo-band O III/Hα unmixing; OSC proxy).

## Open choices surfaced to the user (decide before planning)

**Decided by the user:**

- **Input contract (the crux) — DECIDED.** The full-reconstruction mode ingests
  **separate linear channel masters** — mono Hα/O III/S II, or the two duo-band masters
  (HaO3 + S2O3) — captured **before the nonlinear stretch**, inserting *earlier* in the
  user's workflow than the current cosmetic grade (see "Positioning" in Intent). If handed
  only a finished/stretched RGB, the tool degrades to **geometry-prior-only** depth (no
  ratio term) and says so — this is the OSC-pretty-picture fallback, not the headline path.

**Settled by the author (builder's calls; flagged for the user to check):**

- **Ionizer identification stays offline-first.** Auto-pick the bluest luminous star in the
  nebula footprint from Gaia BP/RP (the offline export must stop dropping BP/RP, which it
  currently does), with a **user click-to-override**. An online SIMBAD / O-star-catalogue
  enrichment is optional, behind the **same opt-in as the existing online Bailer-Jones
  checkbox**, so the "runs fully offline" guarantee is preserved by default.
- **Acceptance bar for the headline perceptual fix.** The judge is the user, by the project's
  standard repo-pull human test, against a **specified before/after**: current grade vs. the
  new reconstruction on the real M42 (duo-band) and M8 masters above, plus the Robles M8
  reference, checking (a) the bright core reads as a *concave cavity*, not a flat wall;
  (b) O III interior / Hα–S II front ordering is visible; (c) M8's cavity reads deeper than
  M42's. Synthetic fixtures carry the quantitative apex/concavity/stratification tolerances
  (builder-tuned).

## Sources (research briefs, 2026-11-05)

- Wen & O'Dell 1995 (Orion 3D front); O'Dell et al. 2018 "Layers in the Central Orion
  Nebula" (arXiv 1804.06491, depth numbers); Henney et al. 2005 (blister photoevap,
  astro-ph/0504221); O'Dell et al. 2009 (arXiv 0810.4375, [O III] fills cavity).
- Damiani et al. 2017 (Gaia-ESO M8 dynamics, arXiv 1705.08194 — blister, star
  standoffs, Lagoon Veil, ~1.25 kpc, stratification); Chakraborty & Anandarao 1999
  (Hourglass champagne flow, CLOUDY stratification).
- Osterbrock & Ferland, *Astrophysics of Gaseous Nebulae* (Strömgren, ionization
  structure, Case B, emission measure); arXiv 2109.03967 (limits of
  ionization-parameter mapping); arXiv 1202.3334 (ionization-/density-bounded via
  [S II]/[O III]); arXiv 1206.1867 (Balmer decrement → extinction); arXiv 0708.1515
  (Hα surface brightness → emission measure).
- Verdict brief: depth is prior-driven, constrained by O III/Hα ratio + emission
  measure + extinction; not measured. No parallax for gas.
