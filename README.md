# astro-pi

Unified PixInsight distribution for scarter4work's astrophotography tools.

## Install in PixInsight

Add this repository URL (Resources → Updates → Manage Repositories):

```
https://raw.githubusercontent.com/scarter4work/astro-pi/main/repository/
```

Then Resources → Updates → Check for Updates. Installs:

- **NukeX** — integration + stretching module (`modules/nukex/`)
- **PI Copilot** — AI assistant panel, bring your own Anthropic API key (`modules/pi-copilot/`)
- **Planetary** — planetary (lucky-imaging) tools; first process SERViewer, a SER video viewer (`modules/planetary/`)
- **Gaia Depth Grade** — PJSR scripts + their linux-x64 sidecar (`gaia-depth-grade/`)
- **EZ Stretch / EZ Donut Repair / EZ Haze Kill** — PJSR scripts (`scripts/ez-stretch/`)

## Layout

| Path | What |
|---|---|
| `modules/nukex/` | NukeX C++ module (CMake). Builds `NukeX-pxm.so`. |
| `modules/pi-copilot/` | PI Copilot C++ module (CMake). Builds `PICopilot-pxm.so`. |
| `modules/planetary/` | Planetary C++ module (CMake): SER reader library + the SERViewer process. Builds `Planetary-pxm.so`. |
| `gaia-depth-grade/` | Gaia Depth Grade PJSR scripts + the Python sidecar they run. |
| `scripts/ez-stretch/` | EZ PJSR scripts + native signing tooling. |
| `tools/` | Dev tooling (PCL/PJSR MCP parsers, astro-stretch-studio). Not shipped. |
| `archive/` | Frozen prior NukeX versions (v1–v4). Never built or shipped. |
| `repository/` | Signed `updates.xri` + published packages. |
| `release.sh` | Build → sign → package → manifest → verify. |

## Release

```bash
printf '%s' '<module-keys-password>' > /tmp/.pi_codesign_pass && chmod 600 /tmp/.pi_codesign_pass
./release.sh
git add repository/updates.xri repository/*.tar.gz repository/*.zip
git commit -m "release: ..." && git push
```
