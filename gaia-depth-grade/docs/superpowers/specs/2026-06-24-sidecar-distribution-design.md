# Gaia Depth Grade — Shippable Distribution (frozen sidecar + signed scripts) — Design

**Date:** 2026-06-24
**Status:** Approved (design); spec under review
**Location:** `astro-pi/gaia-depth-grade/` (merged monorepo component)
**Depends on:** the merged component (PJSR in `pi/`, Python core in `src/`), astro-pi `release.sh` + `updates.xri`, ez-stretch native script signer (`SignScriptsNative.js`).

## Goal

Make gaia-depth-grade installable from the astro-pi PixInsight repository URL by a **public PI
user who has only PixInsight** — no Python, no uv, no terminal setup. The PJSR scripts install
through the signed PI repository; the Python core ships as a **frozen, zero-dependency sidecar
binary** fetched on first run.

## The constraint that shapes everything

`updates.xri` references packages by **relative filename** resolved against
`https://raw.githubusercontent.com/scarter4work/astro-pi/main/repository/`. Every manifest package
must therefore live in the git repo, under GitHub's **100 MB file limit**. A PyInstaller bundle of
numpy + scipy + astropy + photutils + astroquery + pillow is ~300–500 MB. So the binary **cannot**
live where the manifest points.

**Resolution:** ship only the small signed scripts through the PI repository; host the frozen
sidecar as a **GitHub Release asset** (2 GB limit) and have the PJSR fetch it on first run.

## Platform scope

A frozen binary is platform-specific; "public PI users" span Windows/macOS/Linux, but only
**linux-x64** can be built in the current environment. **Phase 1 ships linux-x64 only.** The
bootstrap is platform-aware and fails loudly on unsupported platforms ("sidecar not yet available
for <platform>") — never a silent failure. Windows/macOS use the identical mechanism later, adding
Release assets built on CI runners; no script changes beyond the platform→asset table.

## Runtime prerequisite

StarXTerminator (a paid PixInsight module) must be installed — the split step requires it. The
scripts already throw if SXT fails; the message will name SXT explicitly as a prerequisite.
ImageSolver (AdP) ships with PixInsight, so no extra dependency there.

## Architecture (signed thin scripts + frozen sidecar on Releases + first-run fetch)

```
PI repository (updates.xri, <100MB):  gaia-depth-grade_v<ver>.zip  (pi/*.js + *.jsh + *.xsgn)
        │ installs into PI scripts dir
        ▼
PJSR lib bootstrap (first run)  ──fetch──>  GitHub Release asset
   ~/.astro-pi/gaia-depth-grade/bin/             gaia-depth-grade-sidecar-<ver>-linux-x64.tar.gz
   verify sha256, chmod +x, cache               (frozen Python core + deps, no user Python)
        │
        ▼
   sidecar prepare|preview|render|grade …   (same CLI as today, just a frozen binary)
```

## Components

### 1. Frozen sidecar binary (`gaia-depth-grade-sidecar`)
- PyInstaller bundle of the `gaia_depth_grade` package + runtime deps. Entry point dispatches the
  existing CLI subcommands (`prepare|preview|render|grade`) — reuse `cli.main(argv)`; add a
  `--version` top-level flag that prints a version string.
- `tools/build-sidecar.sh` + a PyInstaller `.spec` that handles astropy data files
  (`collect_data_files('astropy')`), and scipy/photutils/astroquery hidden imports. Output:
  `dist/gaia-depth-grade-sidecar`, packaged as `gaia-depth-grade-sidecar-<ver>-linux-x64.tar.gz`
  with a printed sha256.
- Hosted as a GitHub Release asset (manual upload in Phase 1; CI later).

### 2. First-run bootstrap (`pi/gaia_depth_grade_lib.jsh`)
- Replace the hardcoded `PY_BIN` with `sidecarBin()`:
  - Platform check via `CoreApplication.platform`; only linux-x64 supported in Phase 1 (else throw
    a clear "not yet available for <platform>" error).
  - Cache dir `~/.astro-pi/gaia-depth-grade/bin/` (user-writable; resolved from the HOME env).
  - If the cached binary exists and `<bin> --version` equals the pinned `SIDECAR_VERSION` → return it.
  - Else download the pinned `SIDECAR_URL` (a Release asset) to a temp file, verify against the
    pinned `SIDECAR_SHA256`, extract, move into the cache, `chmod +x`, return it.
  - Download via PI `NetworkTransfer` (portable). Verify the downloaded archive's sha256 by
    running `sha256sum` through `ExternalProcess` (coreutils, present on linux-x64) and comparing
    the hex digest to the pinned `SIDECAR_SHA256`. On any failure, surface the real error and abort
    (no silent fallback). (Win/macOS later: use PI's hashing API or the platform's hash tool.)
- `pyCmd(args)` → `sidecarCmd(args)` = `gdgQuote(sidecarBin()) + " " + args.join(" ")` (the frozen
  binary takes the subcommand directly: `sidecar prepare …`). `run()` unchanged.
- Pinned constants (`SIDECAR_VERSION`, `SIDECAR_URL`, `SIDECAR_SHA256`) live at the top of the lib
  and are bumped per sidecar release.

### 3. Script signing + packaging
- `tools/sign-scripts.sh` for gaia-depth-grade, modeled on ez-stretch's native signer
  (`SignScriptsNative.js` → `Security.generateScriptSignatureFile()`; the externally-reimplemented
  `pi_codesign.py` path is known-bad — do NOT use it). Signs `pi/gaia_depth_grade.js`,
  `pi/GaiaDepthGradeDialog.js`, and `pi/gaia_depth_grade_lib.jsh` → `.xsgn`.
- `release.sh` (astro-pi) gains a step: sign gaia scripts, package
  `gaia-depth-grade_v<ver>.zip` (the `.js`/`.jsh` + their `.xsgn`), and add a noarch
  `<package type="script" fileName="gaia-depth-grade_v<ver>.zip" …>` entry to `updates.xri`. Reuse
  the existing `write_pkg` sha1/date stamping and the final integrity check. Signing order stays
  load-bearing: sign scripts → package → stamp manifest → sign manifest LAST.
- The sidecar binary is NOT in `updates.xri` (it's on Releases). `release.sh` prints a reminder to
  build+upload the sidecar and that `SIDECAR_SHA256` in the lib must match the uploaded asset.

## Error handling (loud; no silent fallbacks)
- Unsupported platform → clear "sidecar not available for <platform>"; abort.
- Download failure / sha256 mismatch / extract failure → real error to the dialog status + a
  MessageBox; abort. Never run a partial/placeholder.
- SXT missing → name StarXTerminator in the error.
- Version mismatch of a cached binary → re-download (treated as missing).

## Testing
- **Python (existing):** the 38 tests are unchanged (the frozen binary wraps the same code).
- **Entry point:** a unit test that `cli.main(["--version"])` prints the version and the frozen
  entry dispatches `prepare/preview/render/grade` (argv routing).
- **build-sidecar.sh:** build the binary, then smoke-test `dist/…/sidecar --version` and
  `sidecar grade <synthetic stars.fits> <out.fits>` (reusing a synthetic fixture) succeeds.
- **Bootstrap (PJSR):** live-validated headless — serve the sidecar tarball from a local
  `file://`/HTTP, run a driver that calls `sidecarBin()` and confirms download→verify→cache→run;
  and a negative test with a wrong `SIDECAR_SHA256` that MUST abort.
- **Signing/release:** `release.sh` dry-run path: scripts sign to `.xsgn`, the zip is produced,
  `updates.xri` integrity check passes, and the manifest signature verifies.

## Out of scope (YAGNI / later)
- Windows/macOS sidecar builds (same mechanism; needs CI runners + per-platform Release assets).
- CI automation of the sidecar build/upload (Phase 1 is a documented manual `build-sidecar.sh` +
  manual Release upload).
- Shrinking dependencies to fit the 100 MB repo limit (rejected: risky rewrites, still likely
  >100 MB due to numpy+astropy).
- Auto-update of the sidecar beyond the pinned-version check (re-download on version bump only).
