# Planetary SER Viewer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A new astro-pi PCL module `Planetary-pxm` whose `SERViewer` process opens a SER video, shows a frame in an embedded preview, and opens it as a PixInsight image.

**Architecture:** A PCL-free C++17 SER library (`modules/planetary/src/ser/`: header parse, frame decode, byte-order rules, timestamps, bilinear debayer) unit-tested with Catch2, and a thin PCL module (`src/module/`) that wraps it in a Process/Instance/Interface. Proof is ctest + headless PixInsight probes + a private-Xvfb GUI smoke with screenshots, on synthetic fixtures and on SER files converted from Scott's real Saturn capture.

**Tech Stack:** C++17, CMake, PCL (`~/PCL`, container `/opt/pcl`), Catch2 amalgamated (`modules/nukex/third_party/catch2`), PJSR probe scripts, bash harness on `tools/pi-headless.sh`, python3 + python-xlib (GUI drive, fixture converter), ffmpeg (fixture conversion only).

**Spec:** `docs/superpowers/specs/2026-10-20-planetary-ser-viewer-design.md` (read it first; it holds the SER rules this plan names).

## Global Constraints

- Module name `Planetary` (`Planetary-pxm.so`), process id and interface id `SERViewer`, category `Planetary`, version 0.1.0.0 in `PlanetaryVersion.h` as `PLANETARY_MODULE_VERSION_{MAJOR,MINOR,REVISION,BUILD}`.
- `Description()` ends with exactly: `This product is based on software from the PixInsight project, developed by Pleiades Astrophoto and its contributors (https://pixinsight.com/).` Author/Company `Scott Carter`, Copyright `Copyright (c) 2026 Scott Carter`.
- No network, no external programs, no OpenMP, no new shared-library deps at run time. ffmpeg/python are test-only.
- `src/ser/` must not include any PCL header.
- Every PixInsight launch goes through `tools/pi-headless.sh` (`pi_headless` / `pi_require_private_display`), on an isolated slot in [50,256] — default **94** for this module, never slot 1. `python3 tools/check-pi-launches.py` must pass.
- Never `make install`, never GUI-launch the dev build with `-m=` on the real desktop, never release (Scott OKs the first Planetary release; ids become permanent then).
- Signing pass file `/tmp/.pi_codesign_pass` is provided by Scott; tasks that need it stop with a clear message if it is absent (no unsigned workaround).
- No `pcl::View`/`ImageWindow` copies kept beyond the call that produced them; no moved-from `pcl::String` in std algorithms.
- Errors are loud: throw `SerError` (library) / `pcl::Error` (module) with the real reason. Every recovery (frame count, byte order) is reported as a warning string.
- Commit after each task on branch `feat/planetary`, message ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. A 16-bit file whose `LittleEndian` flag disagrees with its data (FireCapture < 2.7) — expect the plausibility check to pick the right order and say so. Test: `byte order plausibility overrides flag` (Task 2).
2. A capture cut short by a crash (`FrameCount` 0, or header count larger than the file) — expect the complete frames to open with a warning, and no timestamps claimed. Tests in Task 1.
3. A SER > 4 GB (frame offset beyond 2^32) — expect correct seeking. Test: `frame offset uses 64-bit arithmetic` (Task 2) using a sparse file.
4. Opening a second file in the interface while one is loaded, or closing the interface — expect the first handle released and the preview replaced. Covered by the GUI smoke opening two files in sequence (Task 4).
5. A header with absurd dimensions (e.g. 100000 × 100000 × 3 × 2 bytes) — expect a clean rejection before any allocation. Test: `rejects oversized dimensions` (Task 1).

---

### Task 1: Module skeleton + SER header/frame-layout parsing

**Files:**
- Create: `modules/planetary/CMakeLists.txt`, `modules/planetary/cmake/PCLConfig.cmake` (copy of pi-copilot's), `modules/planetary/src/ser/CMakeLists.txt`, `modules/planetary/src/ser/SerFile.h`, `modules/planetary/src/ser/SerFile.cpp`, `modules/planetary/src/ser/SerTime.h`, `modules/planetary/src/ser/SerTime.cpp`
- Create: `modules/planetary/test/CMakeLists.txt`, `modules/planetary/test/unit/SerWriter.h` (test-side writer), `modules/planetary/test/unit/test_ser_header.cpp`
- Create: `modules/planetary/.gitignore` (`build/`, `build-portable/`)

**Interfaces:**
- Produces (namespace `planetary::ser`):
  - `class SerError : public std::runtime_error`
  - `enum class ColorId : int32_t { Mono = 0, BayerRGGB = 8, BayerGRBG = 9, BayerGBRG = 10, BayerBGGR = 11, RGB = 100, BGR = 101 };`
  - `enum class ByteOrder { Little, Big };`
  - `struct Header { int32_t luId; int32_t colorIdRaw; int32_t littleEndianField; int32_t width, height, depth, frameCount; std::string observer, instrument, telescope; int64_t dateTimeLocal, dateTimeUtc; };` (strings trimmed of NULs/trailing spaces)
  - `struct FileInfo { Header header; ColorId colorId; int planes; int bytesPerSample; int64_t frameBytes; int64_t frameCount; bool hasTimestamps; ByteOrder byteOrder; std::vector<std::string> warnings; };`
  - `class SerFile { public: static SerFile Open(const std::string& path); const FileInfo& Info() const; std::optional<int64_t> FrameTimestamp(int64_t index) const; /* Task 2 adds ReadFrame */ };` — movable, not copyable; owns a `std::ifstream` (binary).
  - `std::optional<std::string> TicksToIsoUtc(int64_t ticks);` in `SerTime.h` — masks top 2 bits; `nullopt` if ≤ 0; format `YYYY-MM-DDThh:mm:ss.fff` (proleptic Gregorian days from 0001-01-01; Howard Hinnant `civil_from_days`).
  - CMake: static library target `planetary_ser` (`-fPIC`), option `PLANETARY_BUILD_MODULE` (default OFF, gates `include(PCLConfig)` and `add_subdirectory(src/module)` — the latter added in Task 3), option `PLANETARY_BUILD_TESTS` (default ON) adding `test/`. Catch2 from `${CMAKE_CURRENT_SOURCE_DIR}/../nukex/third_party/catch2` via a `planetary_add_test(name source)` helper mirroring `nukex_add_test`.
  - Test helper `SerWriter.h`: `struct SerSpec { int32_t colorId, littleEndianField, width, height, depth, frameCount; int64_t dateTimeUtc = 0; std::string observer, instrument, telescope; };` and `void WriteSer(const std::string& path, const SerSpec&, const std::vector<std::vector<uint8_t>>& frameBytes, const std::vector<int64_t>& trailerTicks = {});` writing the exact 178-byte header (offsets in the spec) + raw frame bytes + optional trailer.

- [ ] **Step 1: Write failing tests** in `test_ser_header.cpp` (each writes into a `std::filesystem::temp_directory_path()` unique dir):
  - `parses mono 8-bit header`: 64×48, depth 8, 3 frames, Observer "Scott", → `colorId==Mono, planes==1, bytesPerSample==1, frameBytes==3072, frameCount==3, header.observer=="Scott", warnings.empty()`.
  - `parses rgb 16-bit header`: colorId 100, depth 12 → `planes==3, bytesPerSample==2, frameBytes==W*H*6`.
  - `bgr is accepted`: colorId 101 → `colorId==BGR`.
  - `rejects bad magic` (FileID "NOT-A-SERFILE.") → throws `SerError` whose `what()` contains "LUCAM-RECORDER".
  - `rejects cmy bayer`: colorId 16 → throws, message contains "not supported".
  - `rejects depth 0 and 17`, `rejects zero width`, `rejects oversized dimensions` (width or height > 65536 → throws before reading frames).
  - `recovers frame count 0`: header FrameCount 0, 5 frames of data → `frameCount==5`, one warning containing "frame count", `hasTimestamps==false`.
  - `truncated file uses complete frames`: header says 10, data holds 4.5 frames → `frameCount==4`, warning; `hasTimestamps==false`.
  - `zero complete frames is an error`.
  - `detects timestamp trailer`: 3 frames + 3 ticks → `hasTimestamps`, `FrameTimestamp(1)` == second tick; without trailer → `FrameTimestamp(0)==nullopt`; index out of range throws.
  - `ticks to iso`: `TicksToIsoUtc(638614286610000000)` == `"2024-09-07T02:04:21.000"` — **compute the expected tick value in the test from first principles** (days 0001-01-01→2024-09-07 × 864e9 + time) rather than trusting this literal; plus `TicksToIsoUtc(0)==nullopt`, and a value with the top bit set equals the masked value.
- [ ] **Step 2:** `cmake -S modules/planetary -B modules/planetary/build && cmake --build modules/planetary/build -j && (cd modules/planetary/build && ctest --output-on-failure)` → FAIL (missing symbols).
- [ ] **Step 3:** Implement `SerFile::Open`, `Info`, `FrameTimestamp`, `TicksToIsoUtc` per the spec's rules (64-bit sizes; `frameCount = min(header, complete frames in file)`, `header==0 → complete frames`; `hasTimestamps` only when the count was not recovered and the trailer fits). `byteOrder` = `littleEndianField == 0 ? Little : Big` (Task 2 adds the plausibility override).
- [ ] **Step 4:** Re-run ctest → all PASS.
- [ ] **Step 5:** Commit `feat(planetary): SER header parsing library + tests`.

### Task 2: Frame decoding, byte-order plausibility, debayer

**Files:**
- Modify: `modules/planetary/src/ser/SerFile.{h,cpp}`
- Create: `modules/planetary/src/ser/Debayer.h`, `modules/planetary/src/ser/Debayer.cpp`
- Test: `modules/planetary/test/unit/test_ser_frames.cpp`, `modules/planetary/test/unit/test_debayer.cpp`

**Interfaces:**
- Consumes: Task 1 types.
- Produces:
  - `struct Frame { int width, height, channels; std::vector<uint16_t> samples; };` — interleaved, top-down rows, colour always in **R,G,B** order (BGR reordered), raw values (not normalised).
  - `Frame SerFile::ReadFrame(int64_t index) const;` — throws `SerError` on out-of-range or short read.
  - `struct FloatImage { int width, height, channels; std::vector<float> data; };` — interleaved.
  - `FloatImage Normalize(const Frame&, int depth);` — divides by `2^depth − 1` (in `Debayer.h`).
  - `FloatImage DebayerBilinear(const Frame& cfa, ColorId pattern, int depth);` — 1-channel CFA in, 3-channel RGB out, normalised; edges by mirroring; throws for non-Bayer `pattern`.
  - `bool IsBayer(ColorId);` and `const char* ColorIdName(ColorId);` (`"MONO"`, `"BAYER_RGGB"`, …, `"RGB"`, `"BGR"`) and `const char* BayerPatternKeyword(ColorId)` (`"RGGB"` …) in `SerFile.h`.
  - `FileInfo` gains nothing new; plausibility may flip `byteOrder` and append a warning containing "byte order".

- [ ] **Step 1: Write failing tests:**
  - `reads mono 8-bit frame`: frame k pixel (x,y) = `(x + 2y + 7k) & 0xFF` → `ReadFrame(2)` matches exactly.
  - `reads 16-bit little-endian` (flag 0) and `reads 16-bit big-endian` (flag 1, bytes written swapped) with depth 16 → identical decoded values.
  - `byte order plausibility overrides flag`: depth 12, values ≤ 4095 written little-endian but flag = 1 → `byteOrder==Little`, warning contains "byte order", values correct.
  - `no override at depth 16`: same data, depth 16, flag 1 → `byteOrder==Big` (flag trusted).
  - `rgb order kept, bgr reordered`: one pixel (R=10,G=20,B=30) written as RGB (100) and as BGR (101: bytes 30,20,10) → both decode to samples `{10,20,30}`.
  - `frame offset uses 64-bit arithmetic`: create a sparse file (`std::filesystem::resize_file`) with 1 frame of 4096×4096×3×2 bytes × 90 frames (> 4 GiB, header at offset 0, write a known pixel at the start of frame 89) → `ReadFrame(89)` returns it. Tag `[large]` but keep it in the default run (sparse = no real disk use); skip with a clear `SKIP` only if the filesystem refuses sparse files.
  - `short read throws`.
  - `normalize`: depth 12 value 4095 → 1.0f, 0 → 0.0f.
  - `debayer rggb flat colours`: a 6×6 RGGB mosaic built from a constant colour (R=0.8,G=0.5,B=0.2 at depth 16) → every output pixel within 1e-4 of that colour; same for GRBG, GBRG, BGGR.
  - `debayer interior bilinear`: a known 4×4 mosaic → hand-computed G at a red site = mean of 4 neighbours, R at a blue site = mean of 4 diagonals.
- [ ] **Step 2:** Build + ctest → FAIL.
- [ ] **Step 3:** Implement. Plausibility rule (exact): only when `bytesPerSample==2 && depth<16`: decode frame 0 with the flag's order; if its max > `2^depth−1` and the swapped-order max ≤ `2^depth−1`, switch `byteOrder` and add warning `"byte order: header flag says <X>, data is <Y>; using <Y>"`. Run once inside `Open`.
- [ ] **Step 4:** ctest → PASS.
- [ ] **Step 5:** Commit `feat(planetary): SER frame decoding, byte-order check, bilinear debayer`.

### Task 3: PCL module + SERViewer process (ExecuteGlobal) + docs + headless probe

**Files:**
- Create: `modules/planetary/src/module/CMakeLists.txt` (mirror pi-copilot's: `OUTPUT_NAME "Planetary-pxm"`, version-script map, `-Wl,-z,defs`, PCL static libs, links `planetary_ser`), `pixinsight-module.map` (copy), `PlanetaryVersion.h`, `PlanetaryModule.{h,cpp}`, `SERViewerProcess.{h,cpp}`, `SERViewerInstance.{h,cpp}`, `SERViewerParameters.{h,cpp}`, `SERViewerInterface.{h,cpp}` (minimal stub this task: registers, empty form; Task 4 fills it)
- Create: `modules/planetary/doc/SERViewer/SERViewer.html`, `modules/planetary/doc/SERViewer/THIRD_PARTY_NOTICES.txt` (copy pi-copilot's notices file structure: PCL ack sentence + full PCL License 2.0 text; plus Catch2 is test-only so not listed)
- Create: `modules/planetary/test/harness.sh` (adapted from `modules/pi-copilot/test/harness-lib.sh` + `run-load.sh`: slot `PLANETARY_TEST_SLOT` default 94, slot settings wiped before/after, seeded Modules list excluding any installed Planetary, private TMPDIR, sign step reading `/tmp/.pi_codesign_pass`), `modules/planetary/test/run-probe.sh`, `modules/planetary/test/probe-execute.js`, `modules/planetary/test/make-fixtures.cpp` (small CLI built only in tests that writes synthetic SER fixtures with `SerWriter.h` to a given dir)
- Modify: `modules/planetary/CMakeLists.txt` (add `src/module` under `PLANETARY_BUILD_MODULE`)

**Interfaces:**
- Consumes: Task 1/2 library.
- Produces:
  - Parameters `SVFilePath` (String, id `filePath`, default ""), `SVFrameIndex` (Int32, id `frameIndex`, default 0, min 0), `SVDebayer` (Boolean, id `debayer`, default true).
  - `SERViewerInstance` members `String p_filePath; int32 p_frameIndex; pcl_bool p_debayer;`, `bool CanExecuteGlobal(String&) const` (false + reason if `filePath` empty), `bool ExecuteGlobal()`.
  - A shared helper `pcl::Image`-free function in the module: `FloatImage LoadFrameForDisplay(const SerFile&, int64_t index, bool debayer)` in `SERViewerInstance.h` (Bayer+debayer → RGB; Bayer+!debayer → 1-channel normalised CFA; others → Normalize) — Task 4 uses it for the preview.
  - Window: id = sanitized file stem (`[A-Za-z0-9_]`, leading digit prefixed with `_`) + `_f<index>`; keywords per spec (`DATE-OBS` from frame timestamp else header `dateTimeUtc`; `INSTRUME`/`TELESCOP`/`OBSERVER` if non-empty; `BAYERPAT` when CFA kept; `HISTORY "SERViewer: <file name> frame <i>"`); console line listing warnings.

- [ ] **Step 1: Write the failing probe.** `probe-execute.js` (PJSR) reads fixture paths + expected values from a JSON file whose path is in env `PLANETARY_PROBE_IN` (via `File.readTextFile`), for each case: `var P = new SERViewer; P.filePath = c.path; P.frameIndex = c.frame; P.debayer = c.debayer; P.executeGlobal();` then on `ImageWindow.activeWindow.mainView.image` checks width, height, numberOfChannels, `sample(x,y,ch)` at the listed points (tolerance 1e-6), and listed FITS keywords; closes the window; writes `{"ok":bool,"cases":[...]}` to `PLANETARY_PROBE_OUT`. An error case (`c.expectError` substring) asserts `executeGlobal` throws with that text. `make-fixtures` writes mono8, rgb16 (depth 12), bgr8, bayerRGGB16 (depth 12, with trailer), a FrameCount-0 file, and the JSON with expected values computed by the same generator formula.
- [ ] **Step 2:** `bash modules/planetary/test/run-probe.sh` → FAIL (module/process missing). If `/tmp/.pi_codesign_pass` is absent, the script prints `BLOCKED: signing pass file missing` and exits 2 — report that to the controller, don't work around it.
- [ ] **Step 3:** Implement the module, process, instance, parameters, stub interface, docs. `SERViewer.html`: what the process does, parameters, that it only reads the chosen `.ser` file, no network, no external programs, writes nothing to disk.
- [ ] **Step 4:** Build with `-DPLANETARY_BUILD_MODULE=ON`, ctest still green, `run-probe.sh` → `PASS` with every case ok; `python3 tools/check-pi-launches.py` → ok.
- [ ] **Step 5:** Commit `feat(planetary): Planetary module with SERViewer process + headless probe`.

### Task 4: SERViewer interface — Open dialog, preview, info, frame slider + GUI smoke

**Files:**
- Modify: `modules/planetary/src/module/SERViewerInterface.{h,cpp}`
- Create: `modules/planetary/src/module/FramePreview.{h,cpp}` (a `pcl::Control` subclass that owns a `pcl::Bitmap` and paints it scaled to fit, letterboxed on black)
- Create: `modules/planetary/test/gui-smoke.sh`, `modules/planetary/test/gui_drive.py`, `modules/planetary/test/gui-smoke-setup.js` (adapted from `modules/pi-copilot/test/gui-smoke.sh` / `gui_drive.py`: private Xvfb, real PI GUI on slot 94, XTest clicks, `import -window root` screenshots)

**Interfaces:**
- Consumes: `SERViewerInstance`, `LoadFrameForDisplay`, `SerFile`.
- Produces:
  - Interface controls: `PushButton "Open SER..."`, `FramePreview` (min 400×300), info `Label`/`TextBox` (file name, `W × H`, frames, `ColorIdName`, depth, byte order, first-frame UTC or "none", warnings), `Slider` + `SpinBox` for frame (range 0…N−1, disabled until a file loads).
  - `ImportProcess` with a non-empty `filePath` loads that file and frame (this is how the smoke test and saved process icons restore state); `NewProcess` carries `filePath/frameIndex/debayer`.
  - `pcl::Bitmap MakePreviewBitmap(const FloatImage&)` in `FramePreview.h`: linear 0…max → 0…255 (max ≤ 0 → black), grey replicated for 1 channel.
  - The interface holds a `std::unique_ptr<SerFile>`; opening a new file replaces it; on load failure the previous state is cleared and the error shown in the info block and via `MessageBox` only in interactive mode.

- [ ] **Step 1: Write the failing GUI smoke.** `gui-smoke.sh <out-dir>` starts PI GUI (same isolation as pi-copilot's), `gui-smoke-setup.js` opens the SERViewer interface empty and signals ready; `gui_drive.py`: (a) screenshot `01-empty.png`; (b) click **Open SER...**, type the absolute path of fixture A into the file dialog's name field, Enter, wait, screenshot `02-loaded.png`; (c) move the frame slider to the last frame, screenshot `03-last-frame.png`; (d) open fixture B the same way, screenshot `04-second-file.png`; (e) quit PI. PASS requires PI exit 0, all four PNGs present, and pixel checks on the PNGs (python, PIL if available else `magick ... txt:`) that the preview region is non-black in 02–04 and that 02 and 03 differ (fixture frames differ by design). Also add a `gui-smoke-import.js` path: launch with an instance whose `filePath` is set (`P.launchInterface()` or the PJSR equivalent) → screenshot `05-import.png`.
- [ ] **Step 2:** Run → FAIL (stub interface).
- [ ] **Step 3:** Implement the interface and `FramePreview` per Interfaces. Open dialog: `pcl::OpenFileDialog`, caption `"Open SER video"`, filter `FileFilter("SER video files", ".ser")`, single selection.
- [ ] **Step 4:** Run the smoke → PASS; **look at every PNG yourself** (Read tool) and describe what each shows in the report. Re-run Task 3's `run-probe.sh` → PASS.
- [ ] **Step 5:** Commit `feat(planetary): SERViewer interface with preview, info and frame slider + GUI smoke`.

### Task 5: Real-data fixtures from Scott's Saturn capture + e2e

**Files:**
- Create: `modules/planetary/test/avi_to_ser.py` (stdlib only + ffmpeg/ffprobe on PATH), `modules/planetary/test/run-real-data.sh`

**Interfaces:**
- Consumes: `run-probe.sh`/`probe-execute.js` JSON contract from Task 3; `gui-smoke.sh` from Task 4 (accepts a fixture path override via env `PLANETARY_GUI_FIXTURE_A/B`).
- Produces: `avi_to_ser.py <in.avi> <out-dir> [--frames N]` writing `saturn_rggb8.ser` (raw palette indices as BAYER_RGGB depth 8, with a trailer of frame times = header start time + i × frame interval from ffprobe), `saturn_rggb16_12bit.ser` (values ×16, depth 12, flag 0), `saturn_rggb16_12bit_beflag.ser` (same little-endian data, flag 1 — exercises the plausibility override), `saturn_rgb8.ser` (ffmpeg debayer `-pix_fmt rgb24` of the same frames), `saturn_mono8.ser` (ffmpeg `gray`), plus `expected.json` for the probe with values read **directly from the AVI bytes** (independent of the C++ reader).

- [ ] **Step 1:** Verify the source: default `/mnt/qnap/astro_data/2026-09-06/Saturn/2026-09-07-020421-Saturn-Bin1 Lqef 16.7F.avi` (override `PLANETARY_SATURN_AVI`). The script must prove the `pal8` palette is the identity grey ramp (palette entry i == (i,i,i)) before treating indices as sensor values, else fail with the palette dump. Missing source → `FAIL: source AVI not found` (exit 1, never skip).
- [ ] **Step 2:** `run-real-data.sh` converts the first 100 frames into a private temp dir, runs the Task 3 probe on all five files (expect `byteOrder` warning present for the `_beflag` file — check via a probe case asserting the console/`HISTORY` or a probe-readable keyword; builder's call how to surface it), then runs the Task 4 GUI smoke with fixture A = `saturn_rggb16_12bit.ser`, B = `saturn_rgb8.ser`, copying the PNGs to `modules/planetary/test/output/real/` (gitignored).
- [ ] **Step 3:** Run → PASS. Look at `02-loaded.png`: Saturn's disc and rings must be visible in colour. Describe it in the report.
- [ ] **Step 4:** Commit `test(planetary): real-data e2e from the 2026-09-07 Saturn capture`.

### Task 6: Build/release wiring + portability + docs

**Files:**
- Modify: `tools/build-env/build-modules.sh` (configure/build `modules/planetary` → `build-portable` with `-DPLANETARY_BUILD_MODULE=ON`, run its ctest in the container, add its .so to the final loop)
- Modify: `release.sh` (Planetary version via `modver ... PLANETARY`, `PLANETARY_DOC="$ROOT/modules/planetary/doc/SERViewer"`, doc gates, find `.so`, verify-portable, sign, `package_module`, manifest entry, `write_pkg`, `--package-test` signature gains the Planetary pair); add an optional trailing **doc-id** argument to `build_module_tgz`, `module_pkg_check`, `package_module`, `published_pkg_reusable` (default = `<Name>`), so packages deploy `doc/tools/SERViewer/`. `module_doc_check` takes the doc id for the html name.
- Modify: `README.md` (Installs list incl. PI Copilot + Planetary; Layout row), `CLAUDE.md` (What ships; rule 2 version header `modules/planetary/src/module/PlanetaryVersion.h`; "Build / test Planetary" section with the ctest, `run-probe.sh`, `gui-smoke.sh`, `run-real-data.sh` commands), `modules/planetary/README.md` (what it is, status, how to build/test, roadmap one-liner).

- [ ] **Step 1:** `tools/build-env/build-modules.sh` → all three modules build, Planetary ctest green in the container.
- [ ] **Step 2:** `tools/build-env/verify-portable.sh modules/planetary/build-portable/src/module/Planetary-pxm.so` → PASS (GLIBC ≤ 2.34, GLIBCXX ≤ 3.4.30, no RPATH, 3 exports, dlopen on ubuntu:22.04/debian:12/rockylinux:9).
- [ ] **Step 3:** `./release.sh --package-test <scratch> <NukeX so> <xsgn> <PICopilot so> <xsgn> <Planetary so> <xsgn>` (stand-in `.xsgn` allowed as today) → all three package checks pass; Planetary tarball lists exactly `bin/Planetary-pxm.{so,xsgn}` + `doc/tools/SERViewer/{SERViewer.html,THIRD_PARTY_NOTICES.txt}`; NukeX/PICopilot verdicts unchanged ("published and up to date" or "new package" exactly as on the branch base). `git status repository/` clean.
- [ ] **Step 4:** Run the Task 3 probe against the **container** build (sign it first) → PASS, proving the shipped-style binary works, not just the host build.
- [ ] **Step 5:** Commit `build(planetary): container build, release packaging (doc id), docs`.
