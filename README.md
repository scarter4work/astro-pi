# RC-Astro CLI wrappers for PixInsight

PJSR scripts that run the GPU-accelerated `rc-astro` command-line tool from
inside PixInsight, as a drop-in stand-in for the compiled RC-Astro
BlurXTerminator / StarXTerminator / NoiseXTerminator plugins.

## Why this exists

The compiled PixInsight plugin builds of BlurXTerminator, StarXTerminator, and
NoiseXTerminator crash on this machine. Their bundled TensorFlow has no
Blackwell (sm_120) kernels for the RTX 5070 Ti, so every run JIT-compiles
kernels from PTX at invocation time, exhausts the machine's RAM into swap, and
the process SIGABRTs before it finishes.

The stand-alone `rc-astro` CLI uses **ONNX Runtime** instead of TensorFlow, has
no such gap, and runs all three tools on the RTX 5070 Ti at roughly **2.7
seconds per 22 MP panel** on GPU, with no crash.

These scripts wrap that CLI so it can be launched from PixInsight's `Script`
menu with a normal parameter dialog, process icons, and headless automation
support — instead of dropping to a terminal for every run.

## GPU requirement

**System cuDNN must be >= 9.13.** NVIDIA's cuDNN release notes document an
issue present in all cuDNN 9.x releases prior to 9.13.0 where the heuristics
engine can recommend convolution engine configs that fail on Blackwell
(sm_120) GPUs. On this machine, cuDNN 9.8.0 hit exactly that: `rc-astro`
silently fell back to CPU (slow, but not obviously wrong) while logging
`CUDNN_FE ... HEURISTIC_QUERY_FAILED` for `smVersion:1200`. Upgrading to
cuDNN 9.24.0 (known good) fixed it. If runs suddenly get much slower, check
`ldconfig -p | grep cudnn` / the cuDNN version before assuming a code
regression.

## The three tools

All three appear under `Script > RC-Astro > <Name> (CLI)` after installation.

### BlurXTerminator (CLI) — `RCAstroBXT.js`

CLI: `rc-astro bxt`. **Modifies the target view in place** (single undoable
step via PixelMath).

| Control | Flag | Range / default |
|---|---|---|
| Sharpen stars | `--ss` | [0, 0.7], default 0.25 |
| Sharpen nonstellar | `--sn` | [0, 1], default 0.90 |
| Adjust star halos | `--ash` | [-0.5, 0.5], default 0.0 |
| Auto nonstellar PSF | `--ansr` / `--no-ansr` | default on |
| Nonstellar radius (manual) | `--nsr` | [0, 4], only when auto-PSF is off |
| Correct only (no sharpening) | `--correct-only` | default off; disables the sharpen sliders |
| AI model | `--ml-version` | Latest (default, flag omitted) / 4 / 2 |
| Device | `--device` | gpu (default) / cpu |

### StarXTerminator (CLI) — `RCAstroSXT.js`

CLI: `rc-astro sxt`. **Leaves the original view untouched** and creates a new
window `<id>_starless`; if "also create stars-only image" is enabled, also
creates `<id>_stars`.

| Control | Flag | Default |
|---|---|---|
| Also create stars-only image | `--output-stars` | off |
| Unscreen stars | `--unscreen` | off; only enabled when stars output is on |
| AI model | `--ml-version` | Latest (default) / 11 |
| Device | `--device` | gpu (default) / cpu |

### NoiseXTerminator (CLI) — `RCAstroNXT.js`

CLI: `rc-astro nxt`. **Modifies the target view in place** (single undoable
step), same as BXT.

Main controls: Denoise `--dn` [0,1] default 0.90 - Iterations `--it` [1,5]
default 2 - Frequency scale `--fs` [1,100] default 5.0 - AI model
`--ml-version` (Latest / 3 / 2) - Device.

**Advanced** section (collapsed `SectionBar`, off by default), each [0,1]
default 0.90: Intensity `--di`, Color `--dc`, Denoise hi-freq `--dhf`, lo-freq
`--dlf`, Intensity hi-freq `--dihf`, lo-freq `--dilf`, Color hi-freq `--dchf`,
lo-freq `--dclf`. These flags are only added to the CLI invocation when the
Advanced section is expanded; otherwise the CLI's own defaults apply.

## Output behavior summary

- **BXT / NXT**: replace the target view's pixels in place. `Ctrl+Z` undoes
  the change, same as any other in-place process.
- **SXT**: never touches the original view. It opens `<id>_starless` as a new
  image window, and — only when "also create stars-only image" is checked —
  also opens `<id>_stars`.

## Error handling

Every failure path (missing `rc-astro` binary, nonzero exit code, a `--json`
`error` event, or a missing/unreadable output file) surfaces as a loud
`console.criticalln` plus a modal `MessageBox` with the real message from the
CLI. There is no silent fallback and no mock/placeholder data — if something
goes wrong you will see it immediately, in the UI, not just buried in the
console log.

## Install

1. `./install.sh` — prints the folder path, the one-time registration steps
   below, and whether `rc-astro` is on `PATH`.
2. In PixInsight: `Script` menu > `Feature Scripts...` > `Add` > select this
   folder (`~/PixInsightScripts/RC-Astro`) > `Done`.
3. The three tools now appear under `Script > RC-Astro > *(CLI)`. This
   survives PixInsight updates and needs no `sudo`; PJSR itself has no API to
   register a Feature Scripts directory, so this one manual step can't be
   automated away.

## Running the headless tests

Each test launches PixInsight under Xvfb via `test/run-headless.sh` and writes
its verdict to its own log file (PJSR's `console.*` output does not reach
stdout under `--automation-mode`, so each test writes `PASS <name>` or
`FAIL: <reason>` directly to disk):

```bash
cd ~/PixInsightScripts/RC-Astro
test/run-headless.sh "$PWD/test/t_lib_roundtrip.js"; cat /tmp/rc_t1_result.log
test/run-headless.sh "$PWD/test/t_lib_runcli.js";    cat /tmp/rc_t2_result.log
test/run-headless.sh "$PWD/test/t_bxt.js";           cat /tmp/rc_t3_result.log
test/run-headless.sh "$PWD/test/t_sxt.js";           cat /tmp/rc_t4_result.log
test/run-headless.sh "$PWD/test/t_nxt.js";           cat /tmp/rc_t5_result.log
```

Each PixInsight startup under Xvfb takes roughly 40 seconds, so the full suite
takes several minutes. The `t_bxt` / `t_sxt` / `t_nxt` tests exercise the
engine and CLI-argument-building path against the real `rc-astro` binary on
GPU; they do not open the interactive dialogs (PJSR dialogs cannot run under
`--automation-mode`), so each tool's dialog still needs a one-time manual
open-and-run check in the PixInsight UI.

## Design doc

See [`docs/2026-07-11-rc-astro-pi-wrappers-design.md`](docs/2026-07-11-rc-astro-pi-wrappers-design.md)
for the full engine design, per-tool parameter tables, automation/Parameters
contract, and error-handling policy.
