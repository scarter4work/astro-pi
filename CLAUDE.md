# astro-pi — project instructions

Single source of truth for this monorepo (supersedes the per-project CLAUDE.md files
in nukex5 / EZ-Stretch-BSC). Global PixInsight conventions still live in ~/.claude/CLAUDE.md.

## What ships
- `modules/nukex/` → `NukeX-pxm` module (ID string `"NukeX"` is STABLE — never rename; existing installs update by ID).
- `scripts/ez-stretch/` → EZStretch / EZDonutRepair / EZHazeKill (`.js` + `.xsgn`).
Everything in `tools/` and `archive/` is NOT shipped.

## Not distributed from here (2026-10-04)
After Pleiades' open letter (PCL License 2.0 §3 bars PCL from AI code-generation services; they object
to "bridge" scripts that only shell out to a competing program):
- **RC-Astro CLI wrappers** moved, with history, to the PRIVATE repo `scarter4work/rc-astro-pi`
  (`~/projects/rc-astro-pi`). Update path: its `./install.sh` (signs, verifies, copies over
  `/opt/PixInsight/src/scripts/RCAstro/`). Never re-add them to this repo or `updates.xri`.
- **PCL/PJSR parser schemas** (`tools/*_parser/schemas/`) are local-only and gitignored -- they are
  derived from the PCL headers / PJSR reference and exist to feed AI code generation. Never commit
  them. Backup: `~/projects/backups/astro-pi-schemas/`.
Both were also stripped from this repo's history (git filter-repo + force-push, 2026-10-04).

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

## Launching PixInsight (tests, signing, e2e) -- never on the desktop
Every PixInsight launch goes through `tools/pi-headless.sh`: `pi_headless <PixInsight.sh|PixInsight> ...`
(private Xvfb via xvfb-run, WAYLAND_DISPLAY removed, QT_QPA_PLATFORM=xcb, and a guard inside the
launch that refuses unless DISPLAY is served by Xvfb). xvfb-run alone is not enough: with
WAYLAND_DISPLAY inherited, PI 1.9.5's Qt picks the wayland plugin and opens on the real desktop.
Harnesses with their own Xvfb call `pi_headless_env` and `pi_require_private_display` instead.
`tools/check-pi-launches.py` (run by release.sh) fails on any launch outside these.

## Build / test NukeX
```bash
cd modules/nukex && cmake -B build -DPCLDIR=$HOME/PCL -DNUKEX_BUILD_MODULE=ON && cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

## Distribution URL
`https://raw.githubusercontent.com/scarter4work/astro-pi/main/repository/`
