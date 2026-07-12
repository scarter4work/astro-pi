# RC-Astro CLI wrappers for PixInsight — Design

**Date:** 2026-07-11
**Status:** Approved (design), pending implementation plan
**Author:** Scott Carter + Claude

## 1. Purpose

The compiled PixInsight plugin builds of BlurXTerminator / StarXTerminator /
NoiseXTerminator crash on this machine: their bundled TensorFlow has no Blackwell
(sm_120) kernels, so every run JIT-compiles from PTX, exhausts the 30 GB of RAM
into swap, and aborts (SIGABRT). See memory `pixinsight_bxt_blackwell_jit_crash`.

The stand-alone `rc-astro` CLI (v0.9.10) uses **ONNX Runtime**, not TensorFlow,
and — after upgrading system cuDNN 9.8 → 9.24 — runs these three tools on the
RTX 5070 Ti at ~2.7 s per 22 MP panel with no crash.

This project makes those GPU-accelerated CLI tools usable **from inside
PixInsight's UI**, as three native-feeling processes, so existing interactive and
headless workflows keep working without the crashing plugin.

## 2. Goal / non-goals

**Goals**
- Three menu items under `Scripts > RC-Astro`: BlurXTerminator (CLI),
  StarXTerminator (CLI), NoiseXTerminator (CLI).
- Full parameter dialogs per tool.
- Process-icon + automation support (drag-triangle new instance, ProcessContainer,
  headless replay via `Parameters`).
- Results applied in place with Undo (SXT starless/stars to new windows).
- Loud, real error surfacing — never a silent or mock fallback.

**Non-goals**
- Fixing/replacing the compiled plugin's libtensorflow (superseded by this).
- Reimplementing any AI model logic — we only orchestrate the CLI.
- Windows/macOS support — this is the Linux workstation only.

## 3. Layout

```
~/PixInsightScripts/RC-Astro/
  RCAstroLib.jsh     shared engine (binary discovery, I/O, invoke, apply, errors, Parameters)
  RCAstroBXT.js      BlurXTerminator: params + dialog + main + executeGlobal
  RCAstroSXT.js      StarXTerminator: params + dialog + main + executeGlobal
  RCAstroNXT.js      NoiseXTerminator: params + dialog + main + executeGlobal
  docs/2026-07-11-rc-astro-pi-wrappers-design.md
```

Each tool `.js` is thin: its parameter object + `Dialog` subclass + `main()`.
All heavy lifting lives once in `RCAstroLib.jsh`.

## 4. Shared engine — `RCAstroLib.jsh`

A namespace object (e.g. `RCAstro`) exposing:

- `findBinary()` → resolves the rc-astro path. Order: `/usr/local/bin/rc-astro`,
  then `PATH` via `which`. Returns null if not found (caller shows loud error).
- `exportView(view) → tempInPath` — write the view's image to a temp 32-bit-float
  XISF, preserving FITS keywords, ICC, and astrometric (WCS) metadata. Uses a
  per-run unique subdir under the PI swap dir `~/pixinsight-swap` (already on NVMe,
  set by the launcher's `TMPDIR`); falls back to `File.systemTempDirectory`.
- `run(tool, argv, onProgress) → {exitCode, events, stderr}` — invoke
  `rc-astro <tool> <argv...> --json --overwrite` via `ExternalProcess`. Parse the
  `--json` NDJSON event stream: `progress` events drive a PI progress bar/console;
  `warning`/`error`/`status` events are collected. Non-blocking read loop so the
  UI stays responsive.
- `importResult(path) → Image` — read an output XISF back into an `Image`.
- `applyInPlace(view, image)` — `view.beginProcess(); …assign pixels…;
  view.endProcess();` so the change is a single undoable step.
- `newWindow(id, image, keywordsFrom)` — create + show a new ImageWindow named
  `id`, copying astrometric/FITS keywords from the source.
- `cleanup(paths)` — remove temp files (always, even on failure).
- Parameters helpers: `writeParameters(p)` / `readParameters(p)` mapping a plain
  params object to `Parameters.set*` / `Parameters.get*`.
- `fail(message)` — `console.criticalln` + `(new MessageBox(...)).execute()` with
  the real rc-astro message. Used for: binary missing, nonzero exit, license
  failure, JSON `error` event, unreadable output.

### Data flow (one Apply)
1. Validate target view (must exist; warn if it looks nonlinear/stretched — BXT/NXT
   expect linear data — but do not block).
2. `exportView` → `tempIn.xisf`.
3. Build argv from params (+ `--device gpu|cpu`, `--output tempOut.xisf`,
   `--ml-version`, tool-specific flags).
4. `run(...)`; stream progress. On nonzero exit or `error` event → `fail(...)`, stop.
5. `importResult(tempOut)`.
6. In place: `applyInPlace(targetView, result)`. SXT: `newWindow(id_starless)`,
   and if enabled `newWindow(id_stars)`.
7. `cleanup`.

## 5. Per-tool specifications

### 5.1 BlurXTerminator — `RCAstroBXT.js`
CLI: `rc-astro bxt`. Output replaces target view (undo).

| UI control | Param | CLI flag | Range / default |
|---|---|---|---|
| Sharpen stars | sharpenStars | `--ss` | [0, 0.7], def 0.25 |
| Sharpen nonstellar | sharpenNonstellar | `--sn` | [0, 1], def 0.90 |
| Adjust star halos | adjustHalos | `--ash` | [−0.5, 0.5], def 0.0 |
| Auto nonstellar PSF | autoPSF | `--ansr` / `--no-ansr` | bool, def true |
| Nonstellar radius (manual) | psfRadius | `--nsr` | [0, 4], enabled when autoPSF off |
| Correct only (no sharpen) | correctOnly | `--correct-only` | bool, def false; disables sharpen sliders |
| AI model | mlVersion | `--ml-version` | Latest (default) / 4 / 2 |
| Device | device | `--device` | gpu / cpu, def gpu |

The AI-model dropdown lists "Latest" plus the specific downloaded versions.
"Latest" omits `--ml-version` so the CLI selects the newest installed model; a
specific choice passes that integer. Same convention for SXT and NXT below.

### 5.2 StarXTerminator — `RCAstroSXT.js`
CLI: `rc-astro sxt`. Starless → new window `<id>_starless`; original preserved.

| UI control | Param | CLI flag | Default |
|---|---|---|---|
| Also create stars-only image | outputStars | `--output-stars` | false |
| Unscreen stars | unscreen | `--unscreen` | false; enabled only when outputStars on |
| AI model | mlVersion | `--ml-version` | Latest (default) / 11 |
| Device | device | `--device` | gpu |

When `outputStars` is on, the stars image → new window `<id>_stars`. The CLI writes
the stars file beside the starless output; the engine imports both.

### 5.3 NoiseXTerminator — `RCAstroNXT.js`
CLI: `rc-astro nxt`. Output replaces target view (undo).

Main controls: Denoise `--dn` [0,1] def 0.90 · Iterations `--it` [1,5] def 2 ·
Frequency scale `--fs` [1,100] def 5.0 · AI model `--ml-version` (Latest / 3 / 2) ·
Device.

**Advanced** section (collapsed `SectionBar`), each [0,1] def 0.90:
Intensity `--di` · Color `--dc` · Denoise hi-freq `--dhf` · lo-freq `--dlf` ·
Intensity hi-freq `--dihf` · lo-freq `--dilf` · Color hi-freq `--dchf` ·
lo-freq `--dclf`. Advanced values are only added to argv when the section is
expanded/enabled, otherwise the CLI defaults apply.

## 6. Automation / Parameters

Each `main()`:
- `Parameters.isViewTarget` → headless: read params from `Parameters`, run on
  `Parameters.targetView`, no dialog. (Path used by the headless mosaic harness.)
- `Parameters.isGlobalTarget` → run on `window.mainView`.
- else → interactive dialog on the active view.

Dialog "new instance" (drag triangle) calls `writeParameters` +
`this.newInstance()`, enabling saved process icons and ProcessContainer use.

## 7. Error handling

Per the workspace rule "loud errors, no silent fallbacks":
- rc-astro not found → `fail("rc-astro CLI not found at /usr/local/bin/rc-astro
  or on PATH")`.
- Nonzero exit / JSON `error` event / license failure → `fail(<real message>)`.
- Unreadable/missing output file → `fail(...)`.
- The CLI's own GPU→CPU auto-fallback prints a `warning` event; we echo it to the
  PI console (surfaced, not hidden). No mock data, ever.
- Temp files are cleaned up on every path, including failure.

## 8. Testing

1. **Headless smoke test** (`test/smoke.js`, run via `PixInsight -r=…`):
   load `~/astro_work/cygnus/gxp/panel_1-1.xisf`, for each tool set `Parameters`
   and `executeGlobal`, assert (a) exit success, (b) output view exists,
   (c) pixels differ from input (mean/stdev delta > epsilon). SXT additionally
   asserts a `_starless` window exists.
2. **Interactive checks:** open one image; run each dialog; confirm Apply works,
   Undo restores (BXT/NXT), SXT spawns `_starless` (+`_stars` when enabled), and
   a forced bad input (e.g. device typo) shows a loud MessageBox.
3. **Regression anchor:** the argv the engine builds must match the invocations
   already verified working at the shell (`rc-astro bxt … --ss 0.25 --sn 0.90`).

## 9. Installation

1. Files copied to `~/PixInsightScripts/RC-Astro/`.
2. One-time: PixInsight → `SCRIPT > Feature Scripts… > Add`, select that folder,
   Done. Menu entries appear under `Scripts > RC-Astro`. Survives PI updates; no
   sudo. (Guided at install time.)

## 10. Risks / open considerations

- **Nonlinear-data warning** is advisory only; user may intentionally run on
  stretched data. Non-blocking.
- **Large mosaics**: engine streams whole-image XISF to/from disk; the CLI tiles
  internally, so RAM stays low (verified 1.8 GB peak). NVMe temp keeps I/O fast.
- **rc-astro version drift**: flags are read from the current CLI's help; the
  `--json` schemaVersion is 4. If a future CLI bumps the schema, the engine ignores
  unknown event keys (forward-compatible per the CLI's own contract).
