# astro-pi — project instructions

Single source of truth for this monorepo (supersedes the per-project CLAUDE.md files
in nukex5 / EZ-Stretch-BSC). Global PixInsight conventions still live in ~/.claude/CLAUDE.md.

## What ships
- `modules/nukex/` → `NukeX-pxm` module (ID string `"NukeX"` is STABLE — never rename; existing installs update by ID).
- `scripts/ez-stretch/` → EZStretch / EZDonutRepair / EZHazeKill (`.js` + `.xsgn`).
- `scripts/rc-astro/` → RCAstroBXT / RCAstroSXT / RCAstroNXT + shared `RCAstroLib.jsh` (`.js`/`.jsh` + `.xsgn`).
Everything in `tools/` and `archive/` is NOT shipped.

## `scripts/rc-astro/` — RC-Astro CLI wrappers
PJSR wrappers that drive the stand-alone RC-Astro **CLI** (BlurX/StarX/NoiseXTerminator), because the
compiled plug-ins crash on Blackwell/sm_120: their bundled TensorFlow has no sm_120 kernels, so it
JIT-compiles from PTX, exhausts RAM into swap, and aborts (SIGABRT). The CLI uses ONNX Runtime
instead — ~2.7 s per 22 MP panel on GPU.

- Ships as `rc-astro-cli_vX.Y.Z.zip` → installs to `src/scripts/RCAstro/`. Menu: `Script > RC-Astro`.
- **Every signable file needs a `#script-id`** — including `RCAstroLib.jsh`. PI verifies `#include`d
  files by their OWN signature, so the `.jsh` is signed separately (same as gaia). Without a
  `#script-id`, signing fails with "no script identifier defined".
- Bundles NO RC-Astro code — it shells out to the user's own licensed `rc-astro` binary and fails
  loudly if that binary isn't on PATH. Nothing proprietary is redistributed.
- GPU needs system **cuDNN ≥ 9.13** (older silently falls back to CPU on Blackwell).
- Version bump = `#define VERSION` in each `.js` AND `RCASTRO_VER` in `release.sh`.
- Tests are headless (Xvfb + `PixInsight --automation-mode`): `scripts/rc-astro/test/run-headless.sh`.
  `console.*` never reaches stdout in automation mode — tests write verdicts to `/tmp/rc_tN_result.log`.
- The GUI dialogs cannot run under `--automation-mode`, so they are NOT covered by the tests.

## Release rules (MUST follow)
1. NEVER `make install` — users install from the GitHub repository URL.
2. Bump the relevant version before building (NukeX: `modules/nukex/src/module/NukeXVersion.h`;
   EZ scripts: the `#define VERSION` in each `.js`).
3. Run `./release.sh` — it builds, native-signs, packages, writes ONE `repository/updates.xri`,
   signs the manifest LAST, and integrity-checks every declared sha1 vs the on-disk artifact.
4. Signing order is load-bearing: `.xsgn` embeds a timestamp → hash AFTER packaging, sign manifest LAST.
4a. Shipped modules are built ONLY in the Rocky 9 container (`tools/build-env/build-modules.sh`, which
   release.sh calls) — never ship a host build. A host build carries this box's GLIBC_2.43/GLIBCXX_3.4.32
   and Fedora-only sonames and loads nowhere else (NukeX ≤5.1.0.2 / PICopilot ≤0.2.1.0 shipped that way).
   `tools/build-env/verify-portable.sh` gates the release: ≤GLIBC_2.34/GLIBCXX_3.4.30 (the PI core's
   own floor), no RPATH, and a real `dlopen` on stock Ubuntu 22.04 / Debian 12 / Rocky 9. New third-party
   C++ deps go into the image as static `-fPIC` archives; no OpenMP (libgomp is not on stock distros —
   use `nukex::parallel_for_dynamic`). PCL is rebuilt in the image from the pinned commit, not `~/PCL`.
4b. NukeX's camera (QE) database is compiled into the module AND published for the in-module
   updater as `repository/qe_{manifest,database}.json{,.sig}` (fetched from `nukex::kQEUpdateBaseURL`).
   The signed source is `modules/nukex/repository/`; release.sh step 0 refuses to build unless it names
   the exact bytes of `modules/nukex/share/qe_database.json` (the module embeds its db_version from it),
   copies it to `repository/`, and verifies it there with Python AND the built module's own updater.
   Changed the database? Re-run with `ASTROPI_QE_DB_VERSION=<n+1> ASTROPI_QE_DB_SUMMARY="..."`; it
   signs with the Ed25519 key `~/projects/keys/nukex_qe_signing.key` (not the .xssk -- the module
   verifies Ed25519 against the public key pinned in `qe_update.cpp`).
5. Commit the version bump + `repository/` artifacts together, then push.

## Build / test NukeX
```bash
cd modules/nukex && cmake -B build -DPCLDIR=$HOME/PCL -DNUKEX_BUILD_MODULE=ON && cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

## Distribution URL
`https://raw.githubusercontent.com/scarter4work/astro-pi/main/repository/`
