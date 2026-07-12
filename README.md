# astro-pi

Unified PixInsight distribution for scarter4work's astrophotography tools.

## Install in PixInsight

Add this repository URL (Resources → Updates → Manage Repositories):

```
https://raw.githubusercontent.com/scarter4work/astro-pi/main/repository/
```

Then Resources → Updates → Check for Updates. Installs:

- **NukeX 5.0.0** — integration + stretching module (`modules/nukex/`)
- **EZ Stretch / EZ Donut Repair / EZ Haze Kill** — PJSR scripts (`scripts/ez-stretch/`)
- **RC-Astro CLI Wrappers** — BlurX/StarX/NoiseXTerminator via the RC-Astro CLI (`scripts/rc-astro/`)

## Layout

| Path | What |
|---|---|
| `modules/nukex/` | NukeX C++ module (CMake). Builds `NukeX-pxm.so`. |
| `scripts/ez-stretch/` | EZ PJSR scripts + native signing tooling. |
| `scripts/rc-astro/` | PJSR wrappers driving the RC-Astro **CLI** (BlurX/StarX/NoiseXTerminator). |
| `tools/` | Dev tooling (PCL/PJSR MCP parsers, astro-stretch-studio). Not shipped. |
| `archive/` | Frozen prior NukeX versions (v1–v4). Never built or shipped. |
| `repository/` | Signed `updates.xri` + published packages. |
| `release.sh` | Build → sign → package → manifest → verify. |

## RC-Astro CLI wrappers (`scripts/rc-astro/`)

Adds **`Script > RC-Astro > BlurXTerminator / StarXTerminator / NoiseXTerminator (CLI)`**, which run
the **stand-alone `rc-astro` CLI** instead of the compiled PixInsight plug-ins.

Why: on Blackwell GPUs (RTX 50-series, compute capability 12.0 / sm_120) the plug-ins' bundled
TensorFlow has no sm_120 kernels. It JIT-compiles every kernel from PTX, exhausts system RAM into
swap, and aborts (SIGABRT). The `rc-astro` CLI uses ONNX Runtime instead — **~2.7 s per 22 MP panel
on GPU**.

BlurX and NoiseX modify the target view in place (undoable). StarX writes `<id>_starless` (plus
`<id>_stars` when enabled) to new windows, leaving the original untouched and carrying over the
astrometric solution. Full dialogs, saveable process icons, and view-target automation for headless
pipelines.

**Requirements** (these scripts bundle no RC-Astro code — they shell out to it, and report a clear
error if it's missing):

- The RC-Astro CLI (`rc-astro`) on your PATH, with BlurX/StarX/NoiseXTerminator licensed and models
  downloaded (`rc-astro <product> --activate …`, then `rc-astro download-models`).
- For GPU: system **cuDNN ≥ 9.13**. Older cuDNN hits a Blackwell conv-heuristic bug and *silently*
  falls back to CPU.

See `scripts/rc-astro/README.md`.

## Release

```bash
printf '%s' '<module-keys-password>' > /tmp/.pi_codesign_pass && chmod 600 /tmp/.pi_codesign_pass
./release.sh
git add repository/updates.xri repository/*.tar.gz repository/*.zip
git commit -m "release: ..." && git push
```
