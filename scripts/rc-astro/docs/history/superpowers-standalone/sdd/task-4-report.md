# Task 4 Report: RCAstroSXT.js (StarXTerminator CLI wrapper)

## Status: DONE — genuine PASS on real headless GPU run.

## Commit
`eae2426` — "feat: StarXTerminator (CLI) wrapper + smoke test"
Files added: `RCAstroSXT.js`, `test/t_sxt.js` (branch `feat/rc-astro-pi-wrappers`, not switched).

## The critical part: discovered stars filename

The task brief's assumed name `<starless-base>_stars.xisf` (underscore) is **wrong**.

Empirical discovery process:
1. Wrote `test/t_sxt.js` per brief Step 1, ran `sxt` with `--output-stars --device gpu --output <dir>/image_starless.xisf` on the real GPU, then enumerated the temp dir with the bare global `searchDirectory(dir + "/*.xisf")` (not `File.searchDirectory`, which doesn't exist in this build).
2. First (RED) run logged to `/tmp/rc_t4_result.log`:
   ```
   wrote: .../image.xisf
   wrote: .../image_starless.xisf
   wrote: .../image_starless-stars.xisf
   FAIL: no stars output at assumed path .../image_starless_stars.xisf
   ```
3. **Real filename: `<starless-output-base>-stars.xisf` — a HYPHEN before "stars", not an underscore.** i.e. for output `image_starless.xisf`, the stars file is `image_starless-stars.xisf`.
4. This also matches the CLI's own `--help` text style ("Each output defaults to `<input_file>-<product>.<ext>`" — hyphen-separated product suffixes), which I should have weighted more heavily before trusting the brief's underscore assumption.
5. Corrected the name in both `test/t_sxt.js` and `runSXT()` in `RCAstroSXT.js`. Re-ran — GREEN.

## TDD narrative

- **RED (raw engine discovery)**: `sxt` run succeeded (`r.ok`), starless output existed, but `assert(File.exists(starsP))` failed against the assumed underscore path. `wrote:` log lines gave the true name.
- **GREEN (raw engine)**: corrected to hyphen name → `PASS t_sxt` on the discovery-only test.
- Extended the test beyond the brief's literal Step 1 script (with the parent task's mandate to assert both starless AND stars outputs, and to genuinely exercise the deliverable) to also drive the real `runSXT()` function end-to-end via the `RCAstroSXT_TESTING` guard, mirroring `t_bxt.js`'s pattern of `#include`-ing the real file with its auto-`main()` suppressed:
  - Ran `runSXT(v)` with `SXTParams.outputStars = true`.
  - Asserted the ORIGINAL view `v` is untouched: not closed, and `stdDev()` unchanged (< 1e-9 delta) — proving SXT does NOT apply in place, unlike BXT.
  - Asserted `<id>_starless` and `<id>_stars` windows both exist via `ImageWindow.windowById`.
  - Also asserted `buildArgs()` argv correctness: no `--output-stars`/`--unscreen` when disabled; both present when `outputStars=true, unscreen=true`; `--unscreen` never appears without `--output-stars` (source-level guarantee in `buildArgs`, since `unscreen` is only pushed inside the `outputStars` branch); `--ml-version 11` appears when set.
- Final GREEN run (`/tmp/rc_t4_result.log`):
  ```
  wrote: .../image.xisf
  wrote: .../image_starless.xisf
  wrote: .../image_starless-stars.xisf
  argv[outputStars=false]: ["IN.xisf","--device","gpu","--output","OUT.xisf"]
  argv[outputStars=true, unscreen=true]: ["IN.xisf","--output-stars","--unscreen","--device","gpu","--output","OUT.xisf"]
  argv[mlVersion=11]: ["IN.xisf","--output-stars","--unscreen","--ml-version","11","--device","gpu","--output","OUT.xisf"]
  PASS t_sxt
  ```
- Verified `RCAstro.cleanup()` actually emptied the run's temp dir (confirmed via `ls` on the run's `rc-astro-<ts>-<rand>` directory — gone after the passing run; only stale dirs from earlier failed/pre-fix runs remained, which I removed for hygiene — not part of the repo, just `/home/scarter4work/pixinsight-swap`).

## PJSR / environment corrections applied

All per the environment contracts already established in Tasks 1-3 (no new gotchas found, all confirmed to hold for SXT too):
- Bare global `searchDirectory()`, not `File.searchDirectory()`.
- No `<pjsr/Label.jsh>` include — `Label` is a native global in this build.
- `PixelMath.prototype.SameAsTarget` (only used inside `RCAstroLib.jsh`, unchanged — SXT doesn't call `applyInPlace`).
- `RCAstroLib.jsh` included exactly once (via `RCAstroSXT.js`'s own include; the test does not double-include it).
- Result verified via `RESULT_LOG` file, never stdout/console under `--automation-mode`.
- Errors loud: `RCAstro.fail("StarXTerminator failed: " + r.errorMsg)` on CLI failure, no silent fallback.
- `RCAstro.cleanup([inP, outP, starsP])` in `finally`, passing the real (hyphenated) stars path so the temp dir is fully emptied.

## Output behavior implemented (differs from BXT)

- `runSXT(view)` never applies in place. Starless always goes to a new window `<id>_starless` via `RCAstro.newWindow`. Stars-only goes to a new window `<id>_stars` only when `SXTParams.outputStars` is true and the file exists on disk.
- The original `view` is never touched, never closed by `runSXT`.
- Dialog: `unscreenCB.enabled` is tied to `starsCB`'s checked state at construction and updated live in `starsCB.onCheck`, so "Unscreen stars" is disabled unless "Also create stars-only image" is checked. `buildArgs()` enforces the same rule at the CLI-argument level (`--unscreen` is only ever pushed inside the `if (this.outputStars)` block), so even a caller bypassing the dialog (e.g. process icon/view-target invocation with stale saved parameters) can't produce an invalid `--unscreen` without `--output-stars`.

## Self-review

- Checked `runSXT` reads `SXTParams.buildArgs`/`outputStars`/`device`/`mlVersion` correctly and that the temp-file list passed to `cleanup` includes all three real paths (input, starless, stars) even when stars output is disabled (harmless — `cleanup` no-ops on a nonexistent path via `File.exists` guard in `RCAstroLib.jsh`).
- Confirmed `id` is captured from `view.id` before any window renaming, so `<id>_starless`/`<id>_stars` naming is stable regardless of what `RCAstro.newWindow` does to the result window's `mainView.id`.
- Confirmed test asserts pixel-level non-mutation of the original view (stdDev diff < 1e-9), not just "still open" — the stronger claim the parent task's "do NOT apply in place" requirement calls for.

## Concerns

- None blocking. One minor note for whoever writes Task 5 (NXT or similar) or does a later audit: the CLI's stars-filename convention (hyphen suffix derived from the OUTPUT basename, not the input basename) should be treated as a per-tool fact to re-verify empirically rather than assumed transitively — it was already wrong once in this task's own brief.
- Left the `mlv` combo box's only alternative to "Latest" as "v11" per the brief; `rc-astro sxt --help` doesn't enumerate valid `--ml-version` values, so if RC-Astro ships a different SXT model-version number later, the dialog's combo box would need updating (does not affect correctness of `buildArgs`, which just forwards whatever `mlVersion` integer is set).
