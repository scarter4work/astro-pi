# Planetary AVI Input + Save as SER Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** SERViewer opens Scott's uncompressed 8-bit ASIAIR AVIs (including OpenDML > 1 GB) exactly like SER files, resolves the Bayer pattern from the `.avi.txt` sidecar (with the measured Flip=Vert rule) or a user choice, and converts an AVI to a SER file — shipped as Planetary 0.2.0.0.

**Architecture:** A PCL-free `FrameSource` interface in `src/ser/` is implemented by the existing `SerFile` and a new `AviFile`; `OpenVideo()` dispatches on magic bytes. A PCL-free `ExportSer()` writes any `FrameSource` to SER with progress/cancel. The module (parameters, instance, interface) switches from `SerFile` to `FrameSource` and gains a Bayer combo, `Save as SER…`, and two parameters.

**Tech Stack:** C++17, PCL (module only), Catch2 amalgamated (unit), PJSR probe + XTest GUI smoke on private Xvfb, Python + ffmpeg (test-only real-data checks).

**Spec:** `docs/superpowers/specs/2026-10-21-planetary-avi-input-design.md` (builds on `docs/superpowers/specs/2026-10-20-planetary-ser-viewer-design.md`). Read both.

## Global Constraints

- Branch `feat/planetary-avi` (cut from `main` at the 0.1.0.0 release, 5fb1b683). Commit per task; push only at the release task.
- `src/ser/` stays PCL-free C++17 (unit-testable, builds in the Rocky 9 container). No OpenMP. No new third-party dependency.
- Anti-goals: no ffmpeg/libav/any external program or network at run time; no silent Bayer guess; no silently dropped frames; one decode path (`LoadFrameForDisplay`) for preview and Apply Global.
- Process id `SERViewer`, module id `Planetary`, and the existing parameter ids `filePath`, `frameIndex`, `debayer` are permanent: never rename. 0.1.0.0 instances (no new parameters) must load with the new defaults and behave as before.
- Every PixInsight launch goes through `tools/pi-headless.sh` (probe slot 96, GUI slot 97); scripts sign the module with `/tmp/.pi_codesign_pass`, which the orchestrator writes from memory `ez_stretch_bsc_signing.md` — never ask Scott for it.
- GUI driver rule (measured 2026-10-21): after any synthetic click, leave the pointer on the target while PI handles the release (`click()` already sleeps 0.4 s); never `park()` faster.
- Unsupported-input wording: errors name the real reason (fourcc / bit count / chunk / offset); "not supported" for formats outside the spec.
- Release follows `CLAUDE.md` release rules: bump `modules/planetary/src/module/PlanetaryVersion.h` to 0.2.0.0, release date in `PlanetaryModule.cpp`, `./release.sh`, commit version + `repository/` together.

## Review Focus

1. An AVI whose `indx`/`ix##` entries disagree with the chunk actually at the offset (wrong fourcc or size): the frame and all later ones are dropped with a warning, never decoded from the wrong bytes — test in Task 2 (`bad index offset`).
2. A sidecar present but for a different file size / camera ROI (`Capture Area Size` ≠ AVI dimensions): a reader warning naming both sizes, sidecar Bayer still applied — test in Task 3.
3. Odd frame dimensions (e.g. 1281×961) with a Bayer pattern: Debayer/CFA paths must not read past the frame; DIB row padding to 4 bytes — tests in Task 2 (`width 13 padding`) and Task 5 probe case.
4. Save as SER with the target path equal to the source AVI (same path, or another path to the same file): refused before any byte is written, source unchanged — test in Task 4.
5. Interface open while the user changes the Bayer combo or Debayer on a > 1 GB OpenDML AVI: no full rescan (reopen uses the index only), preview updates — timed in Task 7's real-data run (open budget 2 s applies to reopen too).

---

### Task 1: `FrameSource` interface; `SerFile` implements it; `OpenVideo` dispatch

**Files:**
- Create: `modules/planetary/src/ser/FrameSource.h`, `modules/planetary/src/ser/OpenVideo.cpp`
- Modify: `modules/planetary/src/ser/SerFile.h`, `modules/planetary/src/ser/SerFile.cpp`, `modules/planetary/src/ser/CMakeLists.txt`
- Test: `modules/planetary/test/unit/test_frame_source.cpp` (+ register in `modules/planetary/test/CMakeLists.txt`)

**Interfaces:**
- Produces (all in `namespace planetary::ser`):
  - `enum class Container { Ser, Avi, AviOpenDml };`
  - `enum class BayerChoice { Auto, None, RGGB, GRBG, GBRG, BGGR };`
  - `struct AsiairSidecar { std::string camera, colourFormat, bayer, flip, gain, exposure, captureArea; };` (raw strings as read; defined here so `FileInfo` can hold it; parser comes in Task 3)
  - `struct AviDetails { double frameRate = 0; std::string bayerSource; std::optional<AsiairSidecar> sidecar; std::optional<std::string> filenameStart; std::string indexKind; int64_t droppedFrames = 0; };`
  - `FileInfo` gains `Container container = Container::Ser;` and `std::optional<AviDetails> avi;` (SER files leave `avi` empty).
  - `class FrameSource { public: virtual ~FrameSource() = default; virtual const FileInfo& Info() const = 0; virtual Frame ReadFrame(int64_t index) const = 0; virtual std::optional<int64_t> FrameTimestamp(int64_t index) const = 0; };` — not thread-safe, one per thread (same contract as `SerFile`).
  - `class SerFile : public FrameSource` (unchanged public API; `override` on the three methods).
  - `std::unique_ptr<FrameSource> OpenVideo(const std::string& path, BayerChoice bayer = BayerChoice::Auto);` — reads the first 12 bytes: `LUCAM-RECORDER` → `SerFile` (ignores `bayer`); `RIFF....AVI ` → `AviFile` (Task 2; until then throw `SerError("AVI support is not built")`); anything else → `SerError("not a SER or AVI video file: <path>")`. Missing/unreadable file → the same error text `SerFile::Open` gives today.

- [ ] **Step 1: Write the failing tests** — `TEST_CASE("OpenVideo opens a SER by content, not extension")` writes a mono8 SER via `testing::WriteSer` to `x.avi` and to `x.dat`, asserts `OpenVideo(p)->Info().container == Container::Ser` and frame 0 samples equal the written bytes; `TEST_CASE("OpenVideo rejects unknown magic")` writes 200 bytes of `0x00` and asserts `SerError` whose `what()` contains `"not a SER or AVI video file"`; `TEST_CASE("OpenVideo of a missing file")` asserts `SerError`.
- [ ] **Step 2: Run** `cmake --build modules/planetary/build -j && (cd modules/planetary/build && ctest -R test_frame_source --output-on-failure)` — expected FAIL (no `OpenVideo`).
- [ ] **Step 3: Implement** the types and dispatch above; keep every existing `SerFile` behaviour byte-for-byte.
- [ ] **Step 4: Run all unit tests** `(cd modules/planetary/build && ctest --output-on-failure)` — expected 7/7 PASS (the 6 existing + this one).
- [ ] **Step 5: Commit** `refactor(planetary): FrameSource interface + OpenVideo dispatch by content`

### Task 2: `AviFile` reader (RIFF / OpenDML, 8-bit only)

**Files:**
- Create: `modules/planetary/src/ser/AviFile.h`, `modules/planetary/src/ser/AviFile.cpp`, `modules/planetary/test/unit/AviWriter.h` (test-only AVI writer), `modules/planetary/test/unit/test_avi.cpp`
- Modify: `modules/planetary/src/ser/OpenVideo.cpp`, `modules/planetary/src/ser/CMakeLists.txt`, `modules/planetary/test/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 types.
- Produces: `class AviFile : public FrameSource { public: static AviFile Open(const std::string& path); /* move-only like SerFile */ };` Opened with `colorId = Mono`, `bayerSource = "none"` (Task 3 sets Bayer). `Info()`: `header.width/height`, `header.depth = 8`, `normDepth = 8`, `planes = 1`, `bytesPerSample = 1`, `frameBytes = W*H` (decoded, unpadded), `frameCount` = usable frames, `hasTimestamps = false`, `container = Avi | AviOpenDml`, `avi->frameRate`, `avi->indexKind` ∈ {`"OpenDML indx"`, `"idx1"`, `"scanned"`}, `avi->droppedFrames`. `FrameTimestamp()` always `nullopt` (still throws `SerError` on an out-of-range index). `ReadFrame(i)` returns 1-channel top-down samples.
- `test/unit/AviWriter.h`: `struct AviSpec { int width, height; int bitCount = 8; std::string fourcc /* "" = BI_RGB */; bool bottomUp = false; bool identityPalette = true; int badPaletteEntry = -1; enum Index { Idx1, OpenDml, None } index = Idx1; int framesPerRiff = 0 /* >0 splits into AVIX segments */; double fps = 25; std::vector<int> zeroLengthFrames; bool paletteChangeChunk = false; int64_t truncateBytes = 0; int corruptIndexEntry = -1; size_t trailingJunk = 0; };` and `void WriteAvi(const std::string& path, const AviSpec&, const std::vector<std::vector<uint8_t>>& topDownFrames);` writing real RIFF structures (avih, strh `vids` with rate/scale from fps, strf BITMAPINFOHEADER + 256-entry palette, movi `00db` chunks, idx1 or `indx` super-index → `ix00` standard indexes per segment, `RIFF AVIX` segments).

Decided rules (from the spec, verbatim values): RIFF form must be `AVI ` then any number of `AVIX`; first `vids` stream only; accept `biBitCount == 8` with `biCompression == 0` and an identity grey palette, or fourcc `Y800`/`GREY`/`Y8  `; reject everything else with `"not supported: <fourcc or BI_RGB> <n>-bit video"`; non-identity palette → `SerError` listing the first ≤ 3 differing entries as `i=(r,g,b)`; any `xxpc` chunk → `SerError("palette-change chunk ... not supported")`; rows padded to 4 bytes, `biHeight > 0` bottom-up → flipped to top-down; index preference `indx`→`ix##`, else `idx1`, else linear `movi` scan with warning `"no index; scanned the file for frames"`; zero-length video chunks skipped, warning `"<n> dropped (zero-length) frames skipped"`; an index entry pointing outside the file or at a chunk whose fourcc/size is wrong → that frame and all later ones dropped, warning `"index entry <i> is invalid; using the first <i> frames"`; zero usable frames → `SerError`; frame rate from `strh` rate/scale, fallback `1e6 / avih.dwMicroSecPerFrame`; bytes after the last RIFF segment ignored without warning.

- [ ] **Step 1: Write the failing tests** in `test_avi.cpp`, one `TEST_CASE` each, every one asserting exact frame samples where frames open: `AVI 1.0 idx1 top-down` (3 frames 7×5, values `x + 10*y + 50*f`); `OpenDML with two AVIX segments` (`framesPerRiff = 2`, 5 frames → `frameCount == 5`, `container == AviOpenDml`, `indexKind == "OpenDML indx"`); `bottom-up rows are flipped`; `width 13 padding` (13×4, stride 16); `Y800 fourcc`; `unindexed file is scanned with a warning`; `zero-length frames are skipped and counted` (frames 1 and 3 zero-length of 5 → `frameCount == 3`, `droppedFrames == 2`, warning text above); `bad index offset` (`corruptIndexEntry = 2` of 4 → `frameCount == 2`, warning `"index entry 2 is invalid; using the first 2 frames"`); `truncated tail` (last frame cut → `frameCount == N-1`, warning); `trailing junk after the last RIFF is ignored` (no warnings); rejections: `non-identity palette` (message contains `"5=("`), `MJPG fourcc` (contains `"not supported"` and `"MJPG"`), `24-bit BI_RGB` (contains `"24-bit"`), `palette-change chunk`; `frame rate from strh` (fps 29.97 → `frameRate` within 1e-6); `OpenVideo dispatches RIFF AVI to AviFile`; `ReadFrame out of range throws`.
- [ ] **Step 2: Run** `ctest -R test_avi --output-on-failure` — expected FAIL.
- [ ] **Step 3: Implement `AviFile`** — offsets 64-bit, every size checked against the file before allocation (same untrusted-header discipline as `SerFile`), the frame offset table built once at open from the index (never read frame data at open).
- [ ] **Step 4: Run all unit tests** — expected PASS.
- [ ] **Step 5: Commit** `feat(planetary): AVI reader -- RIFF/OpenDML, 8-bit palettised/Y800, untrusted index`

### Task 3: ASIAIR sidecar, Bayer resolution, filename start time

**Files:**
- Create: `modules/planetary/src/ser/AsiairSidecar.h`, `modules/planetary/src/ser/AsiairSidecar.cpp`, `modules/planetary/test/unit/test_sidecar.cpp`
- Modify: `modules/planetary/src/ser/AviFile.cpp`, `modules/planetary/src/ser/OpenVideo.cpp`, CMake lists

**Interfaces:**
- Produces:
  - `std::optional<AsiairSidecar> ParseAsiairSidecar(const std::string& text, std::vector<std::string>& warnings);` — section line `[ZWO ASI2400MC Pro]` → `camera = "ZWO ASI2400MC Pro"`; `Key = Value` lines trimmed (also `Key=Value`), unknown keys ignored; keys used: `Colour Format`, `Bayer`, `Flip`, `Gain`, `Exposure`, `Capture Area Size`. Unparseable text (no section line and no `=` lines) → `nullopt` + warning `"sidecar <name> could not be read; ignored"`.
  - `ColorId ResolveBayer(BayerChoice choice, const std::optional<AsiairSidecar>& sc, std::string& source, std::vector<std::string>& warnings);` — `RG/GR/GB/BG` → `BayerRGGB/GRBG/GBRG/BGGR`; `Auto` + `Flip` starting with `Vert` → rotated 180° (RGGB↔BGGR, GRBG↔GBRG) and `source = "sidecar RG, Flip=Vert → BGGR"` (actual values substituted; the arrow is U+2192 in UTF-8); `Auto` + no Vert → `source = "sidecar"`; explicit choice → `source = "chosen"`; `None` → `Mono`, `source = "chosen"`; `Auto` with no sidecar or no `Bayer` key → `Mono`, `source = "none"`, warning `"no Bayer pattern known (no .avi.txt sidecar); showing the raw CFA as mono -- choose the pattern under Bayer pattern"`; `Colour Format` other than `RAW8` with a Bayer value → warning `"sidecar Colour Format is <x>, not RAW8; using its Bayer value anyway"`.
  - `std::optional<std::string> FilenameStart(const std::string& fileName);` — `YYYY-MM-DD-HHMMSS-…` → `"YYYY-MM-DD HH:MM:SS"`, else `nullopt`.
  - `OpenVideo(path, bayer)` for AVI: reads `<path>.txt` if it exists (≤ 64 KiB, else warning and ignored), fills `avi->sidecar`, `avi->filenameStart`, `colorId` via `ResolveBayer`, `header.instrument = sidecar->camera`; if `Capture Area Size` parses as `W * H` and differs from the AVI size → warning `"sidecar Capture Area Size <W> * <H> differs from the video's <w> x <h>"`.

- [ ] **Step 1: Write the failing tests** — `ParseAsiairSidecar` on the verbatim sample from the spec (the ASI2400MC Pro block, including `Flip = Vert(unimplemented)`, `Duration=9 Sec`); `ResolveBayer` table: (Auto, RG, None) → RGGB "sidecar"; (Auto, RG, Vert(unimplemented)) → BGGR "sidecar RG, Flip=Vert → BGGR"; (Auto, GR, Vert) → GBRG; (RGGB explicit, sidecar BG) → RGGB "chosen"; (None, sidecar RG) → Mono "chosen"; (Auto, no sidecar) → Mono "none" + the warning text; non-RAW8 warning; `FilenameStart("2023-05-26-230053-Moon-Bin1 18.3F.avi") == "2023-05-26 23:00:53"`, `FilenameStart("capture.avi") == nullopt`; integration: AVI fixture + sidecar on disk → `OpenVideo(p)->Info().colorId == BayerBGGR` for a Vert sidecar, `header.instrument == "ZWO ASI071MC Pro"`; ROI-mismatch warning (Review Focus 2).
- [ ] **Step 2: Run** `ctest -R test_sidecar` — expected FAIL.
- [ ] **Step 3: Implement** as specified.
- [ ] **Step 4: Run all unit tests** — expected PASS.
- [ ] **Step 5: Commit** `feat(planetary): ASIAIR sidecar + Bayer resolution (Flip=Vert rotates the pattern, measured)`

### Task 4: `ExportSer` — write any `FrameSource` as SER

**Files:**
- Create: `modules/planetary/src/ser/SerExport.h`, `modules/planetary/src/ser/SerExport.cpp`, `modules/planetary/test/unit/test_ser_export.cpp`
- Modify: CMake lists

**Interfaces:**
- Produces:
  - `using ExportProgress = std::function<bool(int64_t framesDone, int64_t frameCount)>;` — return `false` to cancel; called after every frame and once with `(0, N)` before the first.
  - `struct ExportResult { int64_t framesWritten = 0; int64_t bytesWritten = 0; };`
  - `ExportResult ExportSer(const FrameSource& src, const std::string& sourcePath, const std::string& outPath, bool allowOverwrite, const ExportProgress& progress);` — throws `SerError` with the real reason; on cancel throws `ExportCancelled` (`class ExportCancelled : public SerError`).
  - `int64_t FilenameStartTicks(const std::string& fileName);` — local `DateTime` ticks (100 ns since 0001-01-01) from `FilenameStart`, else 0.
- Decided rules (spec): header ColorID from `Info().colorId` (Mono → 0, Bayer 8–11), depth 8, LittleEndian field 0, Instrument = `header.instrument` truncated to 40 bytes, Observer/Telescope empty, FrameCount = frames written, **no trailer**, `DateTime_UTC = 0`, `DateTime = FilenameStartTicks(source file name)`. Only 8-bit 1-channel sources are exportable (an RGB/16-bit SER source → `SerError("Save as SER converts 8-bit AVI video only")`). Write to `<outPath>.partial` then rename; on any error or cancel remove the partial file. Refuse `outPath` equivalent to `sourcePath` (`std::filesystem::equivalent` when both exist; else lexical-normal comparison) → `SerError("refusing to overwrite the source file")`. Existing `outPath` and `!allowOverwrite` → `SerError("<outPath> exists")`. Before writing: `std::filesystem::space(dir).available < 178 + N*W*H` → `SerError("not enough free space: need <n> bytes, <m> available")`.

- [ ] **Step 1: Write the failing tests** — round trip: AVI fixture (Bayer via sidecar, 4 frames 9×7) → export → `SerFile::Open` → every frame equal, `colorId == BayerBGGR` (Vert sidecar), `header.instrument`, `header.dateTimeUtc == 0`, `hasTimestamps == false`, `header.dateTimeLocal == FilenameStartTicks(name)` (fixture named `2023-05-26-230053-Moon-x.avi`); cancel after 2 frames → `ExportCancelled`, neither `out` nor `out.partial` exists; target == source refused and the source file unchanged (size + bytes); existing target with `allowOverwrite=false` refused, `true` replaces; 16-bit SER source refused; progress called N+1 times with monotone `framesDone`.
- [ ] **Step 2: Run** `ctest -R test_ser_export` — expected FAIL.
- [ ] **Step 3: Implement** (buffered sequential writes; never hold more than one frame).
- [ ] **Step 4: Run all unit tests** — expected PASS.
- [ ] **Step 5: Commit** `feat(planetary): ExportSer -- AVI to SER with progress, cancel, atomic rename`

### Task 5: Module process side — parameters, `ExecuteGlobal`, info text, probe

**Files:**
- Modify: `modules/planetary/src/module/SERViewerParameters.{h,cpp}`, `SERViewerProcess.cpp`, `SERViewerInstance.{h,cpp}`, `SerInfoText.h`, `modules/planetary/test/unit/test_preview.cpp`, `modules/planetary/test/make-fixtures.cpp`, `modules/planetary/test/probe-execute.js`, `modules/planetary/test/run-probe.sh` (only if the case count/assert changes)

**Interfaces:**
- Consumes: `OpenVideo`, `ExportSer`, `BayerChoice`.
- Produces:
  - Parameter `bayerPattern` (`SVBayerPattern : MetaEnumeration`, elements in order with ids `BayerAuto, BayerNone, BayerRGGB, BayerGRBG, BayerGBRG, BayerBGGR`, default `BayerAuto`) → `pcl_enum p_bayerPattern`; parameter `saveSerPath` (`SVSaveSerPath : MetaString`, default "") → `String p_saveSerPath`.
  - `DisplayFrame LoadFrameForDisplay(const planetary::ser::FrameSource& file, int64_t index, bool debayer);` (signature widened; body unchanged).
  - `ExecuteGlobal()`: opens with `OpenVideo(path, bayer)`. If `p_saveSerPath` non-empty: SER input → error `"saveSerPath converts AVI input only"`; else `ExportSer(..., allowOverwrite=false, progress)` with progress through `pcl::StatusMonitor` + `StandardStatus` (Console abort → cancel), console line `"SERViewer: wrote <n> frames to <path>"`, opens no window, returns true. Else opens the frame as today; for AVI no `DATE-OBS`; `INSTRUME` from sidecar; `HISTORY` gains `"SERViewer: Bayer <PAT> (<bayerSource>)"` when Bayer; reader warnings as HISTORY like SER.
  - `planetary::preview::InfoText(...)`: for AVI add lines `Container: AVI` / `AVI (OpenDML)`, `Frame rate: <x.xx> fps`, `Bayer: <PAT or none> (<bayerSource>)`, `Camera: …`, `Gain: …`, `Exposure: …`, `Flip (sidecar, not applied): …` when present, `Start: <filenameStart> (from filename, time zone unknown)`; SER output unchanged byte-for-byte.
- [ ] **Step 1: Write the failing tests** — `test_preview.cpp`: `InfoText` for an AVI `FileInfo` contains each line above; for a SER `FileInfo` equals today's expected string (copy the existing assertion). `make-fixtures.cpp`: AVI fixtures via `AviWriter.h` (`avi-vert.avi` + Vert sidecar, `avi-nosidecar.avi`, `avi-opendml.avi` with 2 AVIX segments, `avi-odd.avi` 13×9 Bayer) and probe cases: sidecar Auto debayered (sample values computed independently with the same bilinear math as the existing Bayer cases, pattern BGGR), `bayerPattern=BayerRGGB` override on the same file, no-sidecar Auto → 1 channel + HISTORY warning, OpenDML last frame, odd-size debayered, `DATE-OBS` absent, `INSTRUME` present; conversion case `{saveSerPath}` followed by a read-back case of the written SER (frames equal); error cases `saveSerPath` on SER input, `saveSerPath` = source path. Extend `Case`/`ToJson` with `bayerPattern` (element id string) and `saveSerPath`; `probe-execute.js` sets `P.bayerPattern = SERViewer.prototype[c.bayerPattern]` when given and `P.saveSerPath`, and for conversion cases asserts no window opened and the file exists.
- [ ] **Step 2: Run** unit tests (FAIL on InfoText) and `bash modules/planetary/test/run-probe.sh` (FAIL: unknown parameter).
- [ ] **Step 3: Implement** the parameters (register in `SERViewerProcess`), instance and `InfoText` changes.
- [ ] **Step 4: Run** unit tests PASS; `bash modules/planetary/test/run-probe.sh` → `PASS: <all> SERViewer probe cases` (the 13 existing still pass unchanged).
- [ ] **Step 5: Commit** `feat(planetary): bayerPattern + saveSerPath parameters; AVI in ExecuteGlobal and the info block`

### Task 6: Interface — Open…, Bayer combo, Save as SER…, GUI smoke

**Files:**
- Modify: `modules/planetary/src/module/SERViewerInterface.{h,cpp}`, `modules/planetary/test/gui_drive.py`, `modules/planetary/test/gui-smoke.sh`, `modules/planetary/test/make-fixtures.cpp` (GUI AVI fixture)

**Interfaces:**
- Consumes: Task 5 parameters; `OpenVideo`; `ExportSer`.
- Produces (behaviour): `m_file` becomes `std::unique_ptr<planetary::ser::FrameSource>`. Button text `Open...`; dialog caption `Open planetary video`; filters in this order: `Planetary video (*.ser *.avi)`, `SER video (*.ser)`, `AVI video (*.avi)`. A `Bayer pattern:` label + `ComboBox` (items `Auto`, `None (mono)`, `RGGB`, `GRBG`, `GBRG`, `BGGR`, index = enum value) enabled only when the loaded file is AVI; changing it sets `p_bayerPattern` and reopens via `OpenVideo` keeping the frame index. `Save as SER...` button enabled only for AVI: `SaveFileDialog` (caption `Save as SER`, filter `SER video (*.ser)`, initial path = AVI folder + name with `.ser`, overwrite prompt on) → `ExportSer(..., allowOverwrite=true, progress)` with `StatusMonitor`/`StandardStatus` (abortable from the console); success → console line as Task 5; failure/cancel → `Fail()` with the reason; the viewer stays on the AVI. Layout: combo + Save on a row under the Open/Debayer row (the GUI driver measures positions from the preview, so update its offsets from a screenshot, not by guess).
- [ ] **Step 1: Extend the GUI smoke first** (`gui_drive.py --open-avi OUT AVI OUTSER`): open the AVI fixture through the dialog (type the path), screenshot `08-avi.png`, assert preview lit and red/blue per the fixture; select a different Bayer item from the combo (keyboard: click the combo, `Down`, `Return`), screenshot `09-bayer.png`, assert the preview changed; click `Save as SER...`, type `OUTSER`, `Return`; wait (≤ 30 s) for the file to exist and for `<OUTSER>.partial` to be gone; screenshot `10-saved.png`. `gui-smoke.sh` runs it after the existing steps and checks `OUTSER` reads back with the same frame count (via `make-fixtures`' expectations or a `check` mode).
- [ ] **Step 2: Run** `bash modules/planetary/test/gui-smoke.sh <out>` — expected FAIL (no combo/button).
- [ ] **Step 3: Implement** the interface changes.
- [ ] **Step 4: Run** `gui-smoke.sh` three times — expected 3/3 PASS; open every new PNG and check it shows what its name says.
- [ ] **Step 5: Commit** `feat(planetary): interface -- Open... for SER/AVI, Bayer pattern combo, Save as SER...`

### Task 7: Real-data proof over Scott's NAS AVIs

**Files:**
- Create: `modules/planetary/test/avi-probe.cpp` (test-only CLI, linked to `planetary_ser`), `modules/planetary/test/avi_real_check.py`, `modules/planetary/test/run-avi-real.sh`
- Modify: `modules/planetary/test/CMakeLists.txt` (build `avi-probe`, not a ctest case), `modules/planetary/test/run-real-data.sh` (add an AVI leg: probe + GUI smoke on one real sidecar AVI)

**Interfaces:**
- `avi-probe <avi> [--frames i,j,k] [--out DIR] [--export SER]` prints one JSON line: `{"frames":N,"open_s":t,"container":…,"indexKind":…,"colorId":…,"bayerSource":…,"warnings":[…]}`, writes requested frames' raw bytes to `DIR/<i>.raw`, and with `--export` runs `ExportSer` and reports `export_s`.
- [ ] **Step 1: Write `avi_real_check.py`** (run with `python3 -I`): for every `*.avi` under `/mnt/qnap/astro_data` (or `$PLANETARY_AVI_ROOT`): run `avi-probe` → assert `frames == ffprobe nb_frames` (or a warning explains the difference), `open_s < 2.0` (report the slowest file), and frames `0, N//2, N-1` equal ffmpeg's `-f rawvideo -pix_fmt pal8` output (first `w*h` bytes of each frame) byte for byte. Bayer regression: for sidecar files whose target (5th `-` field of the name) is Mars, Saturn, Jupiter, Moon or Betelgeuse, compute 2×2 site means over the brightest 0.5 % of superpixels of 20 frames (as in the spec's measurement) and assert the pattern `avi-probe` resolved puts red on the brighter non-green site and greens on the equal diagonal; print every file's numbers. Round trip: the smallest AVI over 1 GB → `--export` to a temp dir on local disk → `SerFile` frame count and frames `0, N//2, N-1` equal the AVI's; delete the temp output.
- [ ] **Step 2: Run** `bash modules/planetary/test/run-avi-real.sh` — expected `PASS` with 103/103 files, the slowest open time, the Bayer table, and the round-trip line. A failure is a real finding: fix the reader (or stop and report if it contradicts the spec's rules), never loosen the check.
- [ ] **Step 3: Run** `bash modules/planetary/test/run-real-data.sh` — expected PASS for the existing Saturn SER leg and the new AVI leg; look at the AVI leg's PNGs.
- [ ] **Step 4: Commit** `test(planetary): real-data AVI proof over all NAS captures (frames vs ffmpeg, open time, Bayer phase, >1 GB round trip)`

### Task 8: Docs, version 0.2.0.0, container build, release

**Files:**
- Modify: `modules/planetary/src/module/PlanetaryVersion.h` (0.2.0.0), `modules/planetary/src/module/PlanetaryModule.cpp` (`Description()` covers AVI input + Save as SER; PCL acknowledgment sentence stays last; release date = the release day), `modules/planetary/doc/SERViewer/SERViewer.html`, `modules/planetary/README.md`, `README.md`, `release.sh` manifest description text (`SER and AVI video viewer…`), `CLAUDE.md` (Planetary line: 0.2.0.0; AVI test commands), `docs/superpowers/specs/2026-10-21-planetary-avi-input-design.md` only if a ruling changed during the build.

- [ ] **Step 1: Docs** — `SERViewer.html` covers: AVI input (uncompressed 8-bit only; what is rejected), the Bayer pattern control and the sidecar (incl. the Flip=Vert rule and that it can be overridden), Save as SER (what the file contains, no timestamps and why), and that it reads only the chosen file and its `.txt` sidecar and writes only the chosen SER path; no network, no external programs.
- [ ] **Step 2: Container build + gates** — `tools/build-env/build-modules.sh` (NukeX 85/85, Planetary all PASS), `tools/build-env/verify-portable.sh` via release.sh, `python3 tools/check-pi-launches.py` OK, `bash modules/planetary/test/run-probe.sh` and `gui-smoke.sh` against `PLANETARY_SO=build-portable/...`.
- [ ] **Step 3: Release** — write the pass file (memory `ez_stretch_bsc_signing.md`), `./release.sh` → `RELEASE OK`, commit version + `repository/` together (`release: Planetary 0.2.0.0 -- AVI input + Save as SER`), fast-forward `main`, push, verify the served manifest equals the committed one and the Planetary tarball sha1 matches, shred the pass file.
