# AutoContrast

Reference-guided post-processing optimizer for astrophotography in PixInsight. It drives a
stretched-but-flat astrophoto toward the *presentation characteristics* of a professional render of
the same target — without inventing signal the acquisition data does not contain.

See [`autocontrast-design-v0.1.md`](./autocontrast-design-v0.1.md) for the full design. **Read §2
(Core Theses) and §12 (Non-Goals) before writing any code** — they are non-negotiable constraints,
not preferences.

## Status

**Phase 0 — Metrics & fingerprint library.** Standalone Python, no PixInsight, no AI. Implements the
fingerprint (§4): multiscale energy in arcseconds, tonal quantiles, chroma histogram, background
stats, band-limiting, and the distance function.

Phase 0 exit criterion: hand-fingerprint 5 professional M42 renders and 5 amateur ones; the distance
function must rank them correctly with no tuning gymnastics.

## Layout

```
src/autocontrast/
  fingerprint/    # §4 — the fingerprint and its distance function
  io/             # FITS/XISF loaders (standalone; no PI required)
tests/
data/
  references/     # professional renders (gitignored)
  amateur/        # amateur renders for the ranking test (gitignored)
```

## Development

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -e ".[dev]"
pytest
```

## Running the Phase 0 exit criterion

Drop renders of the same target into the two data directories (raster:
PNG/JPEG/TIFF), then run the ranking harness:

```
data/references/   # 5+ professional renders  (pro)
data/amateur/      # 5+ amateur renders        (amateur)
```

```bash
# Leave-one-out: each pro takes a turn as reference; the rest must beat every amateur.
python -m autocontrast.eval.ranking --pros data/references --amateurs data/amateur

# Or rank everything against one chosen "hero" reference:
python -m autocontrast.eval.ranking --pros data/references --amateurs data/amateur \
    --reference path/to/hero.png
```

Defaults assume a broadband `RGB` palette and a common nominal pixel scale/PSF
(so band-limiting overlaps fully and the comparison is pure presentation). Pass
`--palette`, `--pixel-scale`, `--psf` to override. Exit status is 0 when the pro
set separates cleanly from the amateur set.
