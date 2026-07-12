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

## Layout

| Path | What |
|---|---|
| `modules/nukex/` | NukeX C++ module (CMake). Builds `NukeX-pxm.so`. |
| `scripts/ez-stretch/` | EZ PJSR scripts + native signing tooling. |
| `scripts/rc-astro/` | PJSR wrappers driving the RC-Astro **CLI** (BlurX/StarX/NoiseXTerminator). **Not shipped** — see below. |
| `tools/` | Dev tooling (PCL/PJSR MCP parsers, astro-stretch-studio). Not shipped. |
| `archive/` | Frozen prior NukeX versions (v1–v4). Never built or shipped. |
| `repository/` | Signed `updates.xri` + published packages. |
| `release.sh` | Build → sign → package → manifest → verify. |

## RC-Astro CLI wrappers (`scripts/rc-astro/`)

PJSR wrappers that run **BlurXTerminator / StarXTerminator / NoiseXTerminator via the
stand-alone `rc-astro` CLI** instead of the compiled PixInsight plug-ins.

Why: on Blackwell GPUs (RTX 50-series, compute capability 12.0 / sm_120) the plug-ins'
bundled TensorFlow has no sm_120 kernels. It JIT-compiles every kernel from PTX, exhausts
system RAM into swap, and aborts (SIGABRT). The `rc-astro` CLI uses ONNX Runtime instead
and runs ~2.7 s per 22 MP panel on GPU. Requires system **cuDNN ≥ 9.13** — below that,
a Blackwell conv-heuristic bug makes it silently fall back to CPU.

**These are NOT part of the signed PixInsight repository distribution** (`release.sh` /
`updates.xri` do not touch them). They depend on the separately-purchased, separately-installed
RC-Astro CLI and its licenses, so they cannot be redistributed. Install them locally via
`scripts/rc-astro/install.sh` + PixInsight's `Script > Feature Scripts…`.

See `scripts/rc-astro/README.md`.

## Release

```bash
printf '%s' '<module-keys-password>' > /tmp/.pi_codesign_pass && chmod 600 /tmp/.pi_codesign_pass
./release.sh
git add repository/updates.xri repository/*.tar.gz repository/*.zip
git commit -m "release: ..." && git push
```
