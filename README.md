# astro-pi

Unified PixInsight distribution for scarter4work's astrophotography tools.

## Install in PixInsight

Add this repository URL (Resources → Updates → Manage Repositories):

```
https://raw.githubusercontent.com/scarter4work/astro-pi/main/repository/
```

Then Resources → Updates → Check for Updates. Installs:

- **NukeX** — per-pixel distribution-fitting signal estimator and stretching module (`modules/nukex/`)
- **PI Copilot** — AI assistant panel, bring your own Anthropic API key (`modules/pi-copilot/`)
- **Planetary** — SER and AVI video viewer with Save as SER (process SERViewer), and PlanetaryStack, automatic lucky-imaging stacking of those videos (`modules/planetary/`)
- **Gaia Depth Grade** — PJSR scripts + their linux-x64 sidecar (`gaia-depth-grade/`)
- **EZ Stretch / EZ Donut Repair / EZ Haze Kill** — PJSR scripts (`scripts/ez-stretch/`)

## Layout

| Path | What |
|---|---|
| `modules/nukex/` | NukeX C++ module (CMake). Builds `NukeX-pxm.so`. |
| `modules/pi-copilot/` | PI Copilot C++ module (CMake). Builds `PICopilot-pxm.so`. |
| `modules/planetary/` | Planetary C++ module (CMake): SER/AVI reader library, the stack engine, and the SERViewer and PlanetaryStack processes. Builds `Planetary-pxm.so`. |
| `gaia-depth-grade/` | Gaia Depth Grade PJSR scripts + the Python sidecar they run. |
| `scripts/ez-stretch/` | EZ PJSR scripts + native signing tooling. |
| `tools/` | Dev tooling (astro-stretch-studio, build environment, release checks). Not shipped. |
| `archive/` | Frozen prior NukeX versions (v1–v4). Never built or shipped. |
| `repository/` | Signed `updates.xri` + published packages. |
| `release.sh` | Build → sign → package → manifest → verify. |

## License

astro-pi is released under the BSD 3-Clause License (see `LICENSE`), copyright (c) 2025-2026 Brian Scott Carter, **with two exceptions**: `scripts/ez-stretch/src/scripts/EZ Stretch BSC/EZDonutRepair.js` (a derivative of DonutRepair by Chick Dee, (c) 2024, CC BY-NC 4.0) and `.../EZHazeKill.js` (CC BY-NC 4.0) are licensed under the Creative Commons Attribution-NonCommercial 4.0 license, not BSD-3. Their file headers, update-repository descriptions and the carve-out at the end of `LICENSE` say so. `archive/` holds historical snapshots, each of which keeps the third-party licenses it ships with.

The modules link the PixInsight Class Library (PCL), which is licensed by Pleiades Astrophoto under the PCL license and is not covered by this repository's `LICENSE`. PCL is not included in this repository.

### Third-party components

Each component keeps its own license; none of it is relicensed here.

| Component | Where | License |
|---|---|---|
| DonutRepair by Chick Dee (EZDonutRepair is derived from it) | `scripts/ez-stretch/src/scripts/EZ Stretch BSC/EZDonutRepair.js` | CC BY-NC 4.0 (c) 2024 Chick Dee |
| Ceres Solver 2.2.0 (with miniglog) | NukeX | BSD 3-Clause |
| Eigen 5.0.1 | NukeX | Mozilla Public License 2.0 |
| Khronos OpenCL ICD Loader and Headers | NukeX | Apache License 2.0 |
| SQLite 3 amalgamation | `third_party/sqlite/` (PI Copilot) | Public domain (`third_party/sqlite/LICENSE.md`) |
| Catch2 (tests) | `modules/nukex/third_party/catch2/` | Boost Software License 1.0 (`LICENSE.txt`) |
| TweetNaCl (Ed25519 verification) | `modules/nukex/third_party/tweetnacl/` | Public domain (`LICENSE`) |
| stb_image_write | `modules/nukex/third_party/stb/` | MIT or public domain (Unlicense), dual-licensed in the header |
| CFITSIO | fetched at build time (HEASARC/cfitsio), NukeX | NASA/HEASARC CFITSIO license |
| nlohmann/json v3.11.3 | fetched at build time, NukeX and PI Copilot | MIT |
| SQLite amalgamation | fetched at build time (NukeX learning library) | Public domain |
| zstd | `gaia-depth-grade/tools/licenses/LICENSE.zstd.txt` (Gaia Depth Grade sidecar) | BSD 3-Clause or GPLv2 (zstd dual license) |
| Python runtime dependencies of the Gaia Depth Grade sidecar (numpy, scipy, astropy, requests, PyInstaller, others) | pinned in `gaia-depth-grade/tools/sidecar-requirements.txt` | Their own licenses; CPython license text is collected by `gaia-depth-grade/tools/cpython-licenses.py` |

## Release

```bash
printf '%s' '<module-keys-password>' > /tmp/.pi_codesign_pass && chmod 600 /tmp/.pi_codesign_pass
./release.sh
git add repository/updates.xri repository/*.tar.gz repository/*.zip
git commit -m "release: ..." && git push
```
