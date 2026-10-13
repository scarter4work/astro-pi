# Gaia Depth Grade

A physically-grounded "depth" grade for astrophotographs. Instead of faking depth with
aesthetic local-contrast tricks (LHE, masked curves), use **real 3D distance data from
Gaia** to determine how far away each part of an object actually is, then modulate contrast
/ brightness / size based on that distance relative to the viewer. Nearer structure pops,
farther structure recedes — a depth cue that is *measured*, not invented.

**Status:** shipped from the astro-pi PixInsight repository (Linux x64) as two packages:

| Package | What | Installs to |
|---|---|---|
| *Gaia Depth Grade* (`gaia-depth-grade_v<ver>.zip`, noarch) | signed PJSR scripts + user documentation | `src/scripts/GaiaDepthGrade/`, `doc/scripts/GaiaDepthGrade/GaiaDepthGrade.html` |
| *Gaia Depth Grade sidecar* (`<date>-linux-x64-GaiaDepthGradeSidecar-<ver>.tar.gz`) | the Python core frozen into one program (PyInstaller), + its third-party licences | `bin/gaia-depth-grade-sidecar` (0755), `doc/scripts/GaiaDepthGrade/gaia-depth-grade-sidecar-THIRD_PARTY_NOTICES.txt` |

Users install both with *Resources ▸ Updates ▸ Check for Updates*. The scripts run the sidecar
only from `<PixInsight>/bin/` and never download, unpack, chmod or install code (Certified
PixInsight Developer rules 5.1/5.2); a missing, non-runnable or wrong-version sidecar stops the
script with an explanation. What the product does on the user's machine — network connections,
programs launched, files written — is documented for users in
[`doc/GaiaDepthGrade/GaiaDepthGrade.html`](doc/GaiaDepthGrade/GaiaDepthGrade.html) (CPD rule 6.1);
keep it in step with the code.

Versions 1.0.0–1.0.12 of the scripts downloaded the sidecar from a GitHub Release into
`~/.astro-pi/gaia-depth-grade/`; that path is gone (the old Release assets are left as they are).

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

## Layout

```
pi/                 PJSR scripts (dialog, headless harness, shared .jsh) + their .xsgn
doc/GaiaDepthGrade/ user documentation, shipped as doc/scripts/GaiaDepthGrade/
src/gaia_depth_grade/  the Python core (the sidecar's code)
tests/              pytest suite for the core
tools/
  build-sidecar.sh            freeze + smoke-test + package the sidecar (called by ../release.sh)
  gaia-depth-grade-sidecar.spec, pyinstaller-hooks/   what goes into the frozen program
  sidecar-requirements.{in,txt}                       pinned, hash-locked build inputs
  sidecar-smoke.sh, _smoke_fixture.py                 hermetic smoke test of every offline subcommand
  cpython-licenses.py, licenses/                      licence texts for the notices file
  SignGaiaScriptsNative.js                            script signing (release.sh step 2c)
```

## Developing the core

```bash
cd gaia-depth-grade
uv venv --python 3.14.3 .venv
uv pip install --python .venv/bin/python -r tools/sidecar-requirements.txt
uv pip install --python .venv/bin/python -e .
.venv/bin/python -m pytest -q
```

CLI (the same argv the scripts pass to the frozen sidecar):
```bash
.venv/bin/python -m gaia_depth_grade.cli <grade|prepare|footprint|preview|render|nebula> ...
```
Pass `--config <toml>` with an absolute `cache_dir` for online queries; the default
(`.cache`) is relative to the working directory.

## Building and releasing the sidecar

`tools/build-sidecar.sh <out.tar.gz>` freezes the sidecar inside the portable Rocky 9 build
image (glibc 2.34, PixInsight's own floor) — never on the dev box: a host build bundles the
host's libstdc++/libz and does not start on older distributions (sidecar 1.0.7 failed on
Ubuntu 22.04, Debian 12 and Rocky 9). It then runs `tools/sidecar-smoke.sh` (every offline
subcommand) on the host and in bare Ubuntu 22.04 / Debian 12 / Rocky 9 images, and packages the
result.

The package lives in `repository/` like every other package (raw.githubusercontent.com serves it
directly; PixInsight's updater does not follow redirects), so it must stay well under GitHub's
100 MB per-file limit. The bundle carries only what the sidecar runs — measured by run-time
import/file-open traces of every subcommand, see the spec. 1.0.8: 61.3 MB package (1.0.7 was
100.0 MB), output bit-identical to 1.0.7 on a real master.

To ship a new sidecar: bump `__version__` in `src/gaia_depth_grade/__init__.py` **and**
`SIDECAR_VERSION` in `pi/gaia_depth_grade_lib.jsh` (release.sh refuses a mismatch), bump the
scripts' `GAIA_VER` in `../release.sh` and both package entries in `../repository/updates.xri`,
then run `../release.sh`. A sidecar version already committed in `repository/` is reused byte for
byte, never rebuilt.

## Testing in PixInsight

Launch PixInsight only through `../tools/pi-headless.sh`, in a test instance slot (`-n=94`),
never the user's slot 1. The dialog can be driven without its modal loop by `#define GDG_NO_MAIN`
before `#include`-ing `GaiaDepthGradeDialog.js` and calling `doPrepare()` / `doPreview()` /
`doExecute()`. To exercise the installed layout, extract both packages into a scratch copy of the
PixInsight installation (`cp -a --reflink=always /opt/PixInsight ...`) and run that copy.

## Next steps
- **Phase 2 — dust depth:** add a `DustMapSource` (Edenhofer+ 2024 via `dustmaps`)
  implementing the same `DistanceSource` interface; the entire downstream pipeline is reused.
- StarNet 2 as an alternative to the paid StarXTerminator.
