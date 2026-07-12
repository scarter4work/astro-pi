# astro-pi — project instructions

Single source of truth for this monorepo (supersedes the per-project CLAUDE.md files
in nukex5 / EZ-Stretch-BSC). Global PixInsight conventions still live in ~/.claude/CLAUDE.md.

## What ships
- `modules/nukex/` → `NukeX-pxm` module (ID string `"NukeX"` is STABLE — never rename; existing installs update by ID).
- `scripts/ez-stretch/` → EZStretch / EZDonutRepair / EZHazeKill (`.js` + `.xsgn`).
Everything in `tools/`, `archive/`, and `scripts/rc-astro/` is NOT shipped.

## `scripts/rc-astro/` — do NOT add to the release
PJSR wrappers around the stand-alone RC-Astro **CLI** (BlurX/StarX/NoiseXTerminator), used
because the compiled plug-ins crash on Blackwell/sm_120 (TensorFlow JIT → RAM exhaustion →
SIGABRT). They require the separately-licensed `rc-astro` binary, so they are personal tooling
and **must never be wired into `release.sh` / `repository/updates.xri`** — we cannot redistribute
them. Install locally: `scripts/rc-astro/install.sh` + `Script > Feature Scripts…`.
Tests are headless (Xvfb + `PixInsight --automation-mode`): `scripts/rc-astro/test/run-headless.sh`.
Needs system cuDNN ≥ 9.13 for GPU (older → silent CPU fallback on Blackwell).

## Release rules (MUST follow)
1. NEVER `make install` — users install from the GitHub repository URL.
2. Bump the relevant version before building (NukeX: `modules/nukex/src/module/NukeXVersion.h`;
   EZ scripts: the `#define VERSION` in each `.js`).
3. Run `./release.sh` — it builds, native-signs, packages, writes ONE `repository/updates.xri`,
   signs the manifest LAST, and integrity-checks every declared sha1 vs the on-disk artifact.
4. Signing order is load-bearing: `.xsgn` embeds a timestamp → hash AFTER packaging, sign manifest LAST.
5. Commit the version bump + `repository/` artifacts together, then push.

## Build / test NukeX
```bash
cd modules/nukex && cmake -B build -DPCLDIR=$HOME/PCL -DNUKEX_BUILD_MODULE=ON && cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

## Distribution URL
`https://raw.githubusercontent.com/scarter4work/astro-pi/main/repository/`
