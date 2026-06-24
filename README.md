# Gaia Depth Grade

**Status:** Idea / pre-MVP — captured 2026-06-23. Conversation to be continued in a fresh session.

A physically-grounded "depth" grade for astrophotographs. Instead of faking depth with
aesthetic local-contrast tricks (LHE, masked curves), use **real 3D distance data from
Gaia** to determine how far away each part of an object actually is, then modulate contrast
/ brightness / size based on that distance relative to the viewer. Nearer structure pops,
farther structure recedes — a depth cue that is *measured*, not invented.

As far as we know, nobody ships this as a processing step. It is the conspicuous gap the
RC-Astro suite never filled (the running joke: "ContrastXTerminator"), precisely because
contrast/depth is aesthetic and has no training ground-truth — but Gaia *gives* us a ground
truth for distance.

---

## Core concept

1. Plate-solve the image (already part of Scott's pipeline / ASTAP / PI).
2. Pull real distances for objects in the field from Gaia.
3. Convert distance → a contrast/brightness/size modulation, masked appropriately.
4. Recombine so depth reads physically rather than by taste.

---

## The key physics caveat (design around this)

- **Gaia measures stars (point sources), not gas.** DR3 gives parallax → distance for
  ~1.5 billion stars. Emission/reflection/dark nebulosity has **no parallax**.
- **Dust has an indirect path:** 3D dust maps (Bayestar, Leike-Enßlin, **Edenhofer+ 2024**)
  are reconstructed *from* Gaia by measuring per-star reddening vs. distance, triangulating
  where dust sits in 3D. So for dark/reflection complexes there *is* a real "distance to this
  dust clump."
- **Line-of-sight projection is the hard limit.** Optically-thin gas at multiple depths lands
  in the same pixel → per-pixel depth is degenerate for diffuse emission. Depth is only
  well-defined for discrete objects (stars, distinct clumps) or where one dust screen
  dominates.

Conclusion: the result is **physically motivated**, not per-pixel physically correct for gas.
As an art-and-communication tool that's legitimate — and "Gaia-derived depth" is a real
differentiator.

---

## Two builds

### MVP — star-field depth (easy, real, striking)
1. Plate-solve.
2. Query Gaia over the field (astroquery TAP / Vizier) → parallaxes.
3. Use **Bailer-Jones geometric distances**, NOT raw 1/parallax (handles noisy/negative
   parallaxes); weight by parallax error.
4. Build a per-star distance; modulate each star's contrast/brightness/size on a star mask —
   nearer stars pop, farther recede.

Result: a physically real 3D star field as a contrast grade. ~A weekend on top of the
existing headless PI harness.

### Stretch — dust depth
1. For a 3D-dust-rich, nearby target (Rho Oph ~140 pc, complex spans tens of pc in depth —
   ideal dynamic range), query a published 3D dust map (Edenhofer 2024, highest-res nearby)
   along the field's sightlines.
2. Collapse to a "dominant-distance map" registered to the frame.
3. Use that distance map as a contrast-modulation map (PixelMath) or to drive local LHE
   strength: nearer = higher contrast, farther = compressed.

---

## Known limitations
- **Resolution mismatch:** 3D dust maps are parsec-scale / coarse angular res — broad distance
  *gradients*, not arcsec detail. Fine for a depth grade, useless for fine structure.
- **Extincted/distant stars** have huge parallax error — Bailer-Jones + error weighting needed.
- **Gas depth is degenerate** (see caveat above).

---

## Data sources
- **Gaia DR3** via `astroquery.gaia` (TAP) or Vizier.
- **Bailer-Jones et al. geometric/photogeometric distances** (Gaia DR3 distance catalog).
- **3D dust maps:** Edenhofer+ 2024 (preferred, nearby high-res), Leike-Enßlin, Bayestar
  (`dustmaps` Python package wraps several).
- Plate solving: ASTAP / PI ImageSolver (already in use).

---

## Why this fits Scott
- Sits at the AI / data-engineering + astrophotography intersection (relevant to current job
  search positioning).
- Builds directly on the existing **headless PixInsight harness** (Xvfb + PJSR, GPU works).
- Natural first test frames: **Rho Oph** (white-whale target, perfect depth regime) and
  **IC1396** (existing processed data + work tree).
- Portfolio-grade and genuinely novel.

---

## Open design questions (for the continued session)
- Contrast-mapping function: linear in distance? log? perceptually tuned? Per-object
  normalization (map the object's own near–far range to the contrast range).
- Where does the grade live — pure Python producing a modulation map that PI/PixelMath
  consumes, or a PJSR step inside the harness?
- Star MVP first, validate the look, then decide if dust is worth the projection headaches.
- How to present/label it honestly ("Gaia-derived depth, physically motivated").

---

## Running the depth grade (PixInsight harness)

The Python core is wrapped in a PJSR script that runs inside PixInsight's automation harness.
The script orchestrates:
1. Plate-solving via ImageSolver
2. Star/starless split via StarXTerminator
3. Passing the stars layer to the Python CLI
4. Recombining the modulated stars over the starless layer

**Invocation:**
```bash
xvfb-run -a /opt/PixInsight/bin/PixInsight.sh --automation-mode \
  --run=/home/scarter4work/projects/gaia-depth-grade/pjsr/gaia_depth_grade.js
```

The script expects the active PixInsight window to contain the master image to be graded.
Output: a new window `<image_id>_depthgraded` containing the depth-graded result.

---

## Next step
Resume with a proper brainstorm → scope the MVP (star-field version on an existing frame),
nail the contrast-mapping function, then write a plan.
