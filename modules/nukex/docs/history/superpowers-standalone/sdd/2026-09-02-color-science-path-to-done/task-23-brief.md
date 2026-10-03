### Task 23: Release v5.0.0.0 — version, changelog, README, repository manifest, package, push, updater proof

**State today.** `repository/` does not exist in nukex5 (`tools/release.sh` refuses to run without `repository/updates.xri`); `.gitignore` ignores `repository/*.tar.gz`, yet the PixInsight repository *is* the committed tarball served by raw.githubusercontent (nukex4 force-adds them). There is no README, so a public repository has no install instructions. `CHANGELOG.md` is titled "NukeX v4".

**Decision to confirm with the user before Step 6:** the PixInsight repository URL. Default here: this repo's own `https://raw.githubusercontent.com/scarter4work/nukex5/main/repository/` (v4 users keep v4 until they add the new URL). Alternative: also commit the package into `~/projects/nukex4/repository/` so existing v4 installs auto-update to v5 on their next update check.

**Files:**
- Modify: `src/module/NukeXVersion.h`, `CHANGELOG.md`, `.gitignore`
- Create: `README.md`, `repository/updates.xri`, `repository/<date>-linux-x64-NukeX.tar.gz` (by `release.sh`)

- [ ] **Step 1: Version + date**

`src/module/NukeXVersion.h`: `MAJOR 5, MINOR 0, REVISION 0, BUILD 0`; `RELEASE_YEAR/MONTH/DAY` = output of `date +%Y %m %d` on release day (no leading zeros).

- [ ] **Step 2: CHANGELOG**

Change the title line to `# NukeX — Changelog` and insert above the v4.0.1.0 entry:

```markdown
## v5.0.0.0 — YYYY-MM-DD

Color-science overhaul. NukeX now knows what filter and camera produced
each frame, decomposes dual-narrowband OSC data into its emission lines
through the camera's measured quantum efficiency, and composes colour in
Lab/LCH with a calibrated emission-line palette. The v4.0.0.7 green cast
on HaO3 data is gone at the root: the engine no longer routes every Bayer
frame through the plain-OSC path.

### Added
- Filter taxonomy: BROADBAND_L, BROADBAND_RGB, BROADBAND_OSC, NARROWBAND_SINGLE, DUAL_NB_OSC, resolved from FITS FILTER / BAYERPAT / INSTRUME with a tiered policy (unknown dual-NB names on Bayer stop the batch loudly; unknown mono names warn and stack as luminance).
- Quantum-efficiency database (`share/qe_database.json`): 54 cameras plus a generic Sony OSC fallback, 87 filters plus the canonical HaO3 / S2O3 / L-eXtreme / L-eNhance / L-Ultimate / ALP-T entries. Camera keys match the INSTRUME keyword case-insensitively and by model substring.
- Phase B Q-matrix decomposition (Eigen QR) of dual-NB OSC stacks into Ha / OIII / SII slots; multi-source OIII merge across HaO3 + S2O3 batches.
- ColorComposer: Lab/LCH composite of derived slots with a calibrated emission-line palette (no green quadrant by construction); new `NukeX_composed` window; `NUKEX_GAMUT_CLIPPED` and `NUKEX_QE_CONFIDENCE` provenance keywords.
- OSC-as-LRGB: a rec709 luminance slot synthesised per OSC frame.
- "QE override file…" picker in the interface for user cameras/filters (`docs/qe_overrides_format.md`).
- Broadband light-pollution names (L-Pro, LPS, UV-IR cut, CLS) recognised as plain OSC on Bayer cameras.

### Changed
- Rating DB `user_version` 1 → 2: stored filter classes migrate to the 5-class encoding on first open (pre-v5 narrowband ratings become NARROWBAND_SINGLE).
- Rating popup shows the colour axis for RGB-mono and OSC stacks.
- E2E corpus: v4 LRGB-mono golden preserved bit-identical; new OSC, dual-NB (M16 HaO3) and LRGB-mono (M27) baselines.
- Eigen is taken from the system (`find_package(Eigen3)`), not vendored.

### Removed
- `StackingMode` enum, `ChannelConfig::from_mode`, `output_rgb_mapping`, `is_mono`.
- Module-local `filter_classifier` and `fits_metadata` (superseded by lib/io).
```

- [ ] **Step 3: README**

Create `README.md`:

```markdown
# NukeX

Distribution-fitted image stacking and auto-stretch for PixInsight, with
filter-aware colour science: dual-narrowband OSC data is decomposed into
emission lines through the camera's quantum efficiency and composed with a
calibrated palette.

## Install (PixInsight 1.8.9+, Linux x64)

Resources → Updates → Manage Repositories → Add:

    https://raw.githubusercontent.com/scarter4work/nukex5/main/repository/

then Check for Updates and restart. The package installs `bin/NukeX-pxm.so`
and `share/qe_database.json` under the PixInsight base directory.

## Use

Process → NukeX. Add light frames (and optional flats), pick a stretch
(Auto is recommended), Execute. Outputs: `NukeX_stacked` (linear),
`NukeX_composed` (3-channel sRGB when derived slots exist), `NukeX_stretched`,
`NukeX_noise`. Unknown cameras or filters: see `docs/qe_overrides_format.md`.

## Build from source

    cmake -S . -B build && cmake --build build -j && ctest --test-dir build

Requires PCL at `~/PCL` (or `-DPCLDIR=`), system Eigen 3, glog/Ceres,
OpenCL headers. See `CHANGELOG.md` for release notes.
```

- [ ] **Step 4: Repository manifest + ignore rules**

Create `repository/updates.xri` (the signature line is appended by `release.sh`):

```xml
<?xml version="1.0" encoding="UTF-8"?>
<xri version="1.0">
   <description>
      <title>NukeX — Distribution-Fitted Stacking with Calibrated Colour Science</title>
      <copyright>Copyright (c) 2026 Scott Carter</copyright>
   </description>
   <platform os="linux" arch="x64" version="1.8.0:1.9.9">
      <package fileName="PLACEHOLDER-linux-x64-NukeX.tar.gz" sha1="0000000000000000000000000000000000000000" type="module" releaseDate="20260101">
         <title>NukeX</title>
         <description>NukeX v5.0.0.0 — colour-science overhaul. Filter taxonomy from FITS FILTER/BAYERPAT/INSTRUME; quantum-efficiency database (54 cameras + generic fallback, 87 filters) driving a Q-matrix decomposition of dual-narrowband OSC stacks into Ha/OIII/SII; Lab/LCH ColorComposer with a calibrated emission-line palette; OSC-as-LRGB luminance synthesis; QE override file picker. Fixes the v4 green cast on HaO3 data at the root. Rating DB migrates in place. Package now ships share/qe_database.json beside bin/.</description>
      </package>
   </platform>
</xri>
```

`.gitignore`: delete the line `repository/*.tar.gz` (the tarball is the distribution) and keep `repository/*.xsgn`, `repository/bin/`, `repository/share/`.

- [ ] **Step 5: Clean build, full tests, E2E verify**

```bash
cd build && cmake .. > /dev/null && make clean && make -j$(nproc) 2>&1 | grep -E "error"
ctest 2>&1 | tail -2
./test/integration/test_phase_a_router "[integration]" | tail -1
./test/integration/test_phase_b_qsolve  "[integration]" | tail -1
make e2e 2>&1 | grep -E "golden|status" | head -12
```
Expected: no errors; `100% tests passed`; both integration binaries `All tests passed`; four E2E cases `golden match`.

- [ ] **Step 6: Sign + package** (confirm the repository decision first)

```bash
printf '%s' '[REDACTED-PI-SIGNING-PASSWORD]' > /tmp/.pi_codesign_pass && chmod 600 /tmp/.pi_codesign_pass
tools/release.sh package 2>&1 | tail -12
tar -tzf repository/$(date +%Y%m%d)-linux-x64-NukeX.tar.gz
tail -1 repository/updates.xri | cut -c1-60
```
Expected: `signed: … NukeX-pxm.so / .xsgn`; tarball lists `bin/NukeX-pxm.so`, `bin/NukeX-pxm.xsgn`, `share/qe_database.json`; `updates.xri` ends with `<Signature developerId="scarter4work"`; `fileName`/`sha1`/`releaseDate` patched (grep them).

- [ ] **Step 7: Commit + tag + push**

```bash
git add src/module/NukeXVersion.h CHANGELOG.md README.md .gitignore repository/updates.xri repository/*-linux-x64-NukeX.tar.gz
git commit -m "$(cat <<'EOF'
release: v5.0.0.0 — colour-science overhaul

Filter taxonomy + QE-driven Q-solve + Lab/LCH ColorComposer with a
calibrated emission-line palette. Fixes the v4 green cast on dual-NB OSC
data at the root (stacking_engine.cpp hard-coded OSC_RGB for all Bayer).
Ships share/qe_database.json (54 cameras + generic, 96 filters). Rating
DB user_version 1 -> 2. See CHANGELOG.md.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01NqDedRCQxJy8EbbzWDKMFR
EOF
)"
git tag -a v5.0.0.0 -m "NukeX v5.0.0.0 — colour-science overhaul"
git push origin main && git push origin v5.0.0.0
```

- [ ] **Step 8: Prove the published package installs through the updater**

Remove the dev-staged copies so the proof is real: `sudo rm -f /opt/PixInsight/share/qe_database.json`, and in PixInsight uninstall the dev module (Process → Modules → Install Modules… → remove `NukeX-pxm.so` entry). Then Resources → Updates → Manage Repositories → add the URL from README → Check for Updates → install → restart PixInsight.

Verify:
```bash
ls -la /opt/PixInsight/share/qe_database.json /opt/PixInsight/bin/NukeX-pxm.so
```
and in PixInsight, Process Explorer → NukeX → version reads `5.0.0.0`. Run one 12-frame M16 HaO3 stack from the interface: the Process Console must show no QE-database error and the `NukeX_composed` window must appear. If `share/qe_database.json` is absent after the updater install, the tarball layout is not honoured by PixInsight's updater — that is a release blocker: the fix is to ship the JSON under `bin/` and change `PIShareRoot()` accordingly, then re-package; report which happened.

---

