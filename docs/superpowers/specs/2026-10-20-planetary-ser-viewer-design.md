# Planetary module — increment 1: SER viewer (design)

Date: 2026-10-20 · Branch: `feat/planetary`

## Intent

PixInsight has no planetary (lucky-imaging) workflow and cannot even open a SER video
(verified 2026-10-20: no installed module reads `.ser`). Scott wants planetary stacking and
image production inside PixInsight as **one subproject of astro-pi, native PCL**:
`modules/planetary/` → `Planetary-pxm.so`. The roadmap is SER reader → lucky-imaging
stacker → derotation → slider-driven post-processing. This document covers
**increment 1 only: the SER reader**, as an MVP Scott can try in the GUI and we can prove
headlessly.

Reference designs (read 2026-10-20, never ported or shelled out to): Tony Bailey's MIT apps
`tbgh011/lucid` (stacker) and `tbgh011/kepler` (post-processing/derotation). Shelling out to
an external program from a PI process is the "bridge" pattern Pleiades objected to; this
module is self-contained C++.

## What the user sees (MVP)

Process ▸ Planetary ▸ **SERViewer**. The interface has:

1. **Open SER…** button → standard `pcl::OpenFileDialog`, filter `SER video files (*.ser)`.
2. On open: the header is parsed and **frame 0** is decoded and drawn in an embedded
   **preview** inside the interface, scaled to fit while keeping the aspect ratio.
3. An info block: file name, width × height, frame count, colour (MONO / BAYER_RGGB / … /
   RGB / BGR), bit depth, byte order used, first-frame UTC time (or "none"), and any
   warnings (e.g. frame count recovered from file size).
4. A **frame** slider + spin box (0 … N−1, default 0) that redraws the preview. Cheap, and
   the stacker interface will need the same control.
5. The process's **Apply Global** (the round button) opens the selected frame as a normal
   PixInsight image window, so every PI tool can be used on it.

## Process parameters (scriptable)

| Id | Type | Default | Meaning |
|---|---|---|---|
| `filePath` | String | "" | Absolute path of the `.ser` file |
| `frameIndex` | Int32 | 0 | Frame to show / open |
| `debayer` | Boolean | true | Bayer files: open as debayered RGB (true) or raw CFA mono (false) |

`ExecuteGlobal()` opens frame `frameIndex` of `filePath` as a new image window (32-bit float,
samples normalised to [0,1] by `2^depth − 1`) and fails loudly (exception with the real
reason) on any error. No silent fallbacks.

Image window details: id derived from the file name plus `_f<index>`; FITS keywords
`DATE-OBS` (frame timestamp if the trailer exists, else header `DateTime_UTC`, omitted if
neither is valid), `INSTRUME`, `TELESCOP`, `OBSERVER` (trimmed header strings, omitted if
empty), `BAYERPAT` when `debayer=false` on a Bayer file, and a `HISTORY` line naming the
source frame. Derotation will later rely on `DATE-OBS`.

## SER reader (the core) — decided rules

Pure C++17, no PCL dependency, so it is unit-testable outside PixInsight and reusable by
the stacker. Reads frames on demand with 64-bit file offsets (SER files run to tens of GB);
never loads the whole file.

- **Header:** 178 bytes, all integers little-endian. Offsets: FileID 0 (14 B, must be
  `LUCAM-RECORDER`), LuID 14, ColorID 18, LittleEndian 22, Width 26, Height 30,
  PixelDepthPerPlane 34, FrameCount 38, Observer 42 (40 B), Instrument 82 (40 B),
  Telescope 122 (40 B), DateTime 162 (int64), DateTime_UTC 170 (int64).
  Source: SER spec v3 (grischa-hahn.hier-im-netz.de/astro/ser/SER%20Doc%20V3b.pdf), Siril
  `src/io/ser.{h,c}`.
- **Validation (header is untrusted):** width, height > 0; depth 1–16; frame bytes and
  offsets computed in 64-bit; reject sizes that overflow. Reject bad magic.
- **ColorID:** support 0 (MONO), 8–11 (Bayer RGGB/GRBG/GBRG/BGGR), 100 (RGB), 101 (BGR,
  reordered to RGB). 16–19 (CMY Bayer) are **rejected** with a clear "not supported" error
  (Siril does not support them either).
- **Samples:** depth ≤ 8 → 1 byte; 9–16 → 2 bytes. RGB/BGR are interleaved per pixel.
  Values are LSB-aligned per spec; normalise by `2^depth − 1`. Rows are top-down.
- **Byte order (16-bit data):** follow Siril and FFmpeg — `LittleEndian == 0` means
  little-endian, non-zero means big-endian (the field is effectively an inverted flag in
  real files). Builder's call: an additional plausibility check is allowed (e.g. if
  `depth < 16` and the chosen order yields values above `2^depth − 1` on frame 0 while the
  swapped order does not, use the swapped order and report it). Whatever is chosen is shown
  in the info block.
- **Frame count:** if `FrameCount == 0` or the file is shorter than the header claims,
  derive the count of complete frames from the file size and report a warning. Zero
  complete frames is an error.
- **Timestamps:** trailer present iff `fileSize >= 178 + N·frameBytes + 8·N`. Values are
  int64 100-ns ticks since 0001-01-01 UTC; mask off the top two bits. Header `DateTime`/
  `DateTime_UTC` use the same tick format; ≤ 0 means invalid.

## Debayer (preview and `debayer=true`)

Bilinear demosaic for the four RGGB-family patterns, edge pixels handled by mirroring.
Good enough for a viewer; the stacker will work on raw CFA samples (Bayer drizzle) and does
not use this.

## Preview rendering

The decoded frame (float, [0,1]) is converted to an 8-bit RGB `pcl::Bitmap` with a linear
stretch from 0 to the frame's maximum (planetary frames are a bright disc on black, so
max-scaling looks right; a flat-zero frame renders black). Painted in an `OnPaint` handler,
scaled to fit. Decoding runs on the UI thread for MVP (one frame is milliseconds); no
worker threads.

## Robustness

- Never retain `pcl::View`/`ImageWindow` copies beyond the call that created them
  (memory: PCL view-handle use-after-free).
- No moved-from `pcl::String` in std algorithms.
- The interface keeps the open file handle only while a file is loaded; reopening or
  closing the interface releases it.

## Testing and proof (required before calling it done)

1. **Unit tests (ctest, no PI):** a test-side SER writer produces synthetic files with known
   pixel patterns; tests cover every supported ColorID, depth 8/12/16, both byte orders,
   trailer present/absent, FrameCount 0 recovery, truncated file, bad magic, CMY rejection,
   overflow-sized headers, BGR→RGB order, and the debayer on a known mosaic.
2. **Real data:** no SER captures exist on this machine or the NAS (searched
   2026-10-20; public samples are 0.5–32 GB). Scott said to convert a recent video. The
   source is his own ASIAIR Saturn capture,
   `/mnt/qnap/astro_data/2026-09-06/Saturn/2026-09-07-020421-Saturn-Bin1 Lqef 16.7F.avi`
   (ASI585MC Air, RAW8, Bayer RGGB per its `.avi.txt`, 320×240, 563 frames, rawvideo `pal8`).
   A test-only converter (ffmpeg + a small writer, under `modules/planetary/test/`, never
   shipped) turns it into: `BAYER_RGGB` 8-bit (the true raw samples; verify the AVI palette
   is the identity grey ramp so palette indices = sensor values), `BAYER_RGGB` 16-bit with
   12-bit LSB-aligned values (raw × 16, plus one big-endian-flagged variant), `RGB` 8-bit
   (debayered by ffmpeg) and `MONO` 8-bit. **My call:** these are generated into a scratch
   directory by the host-only e2e harness, not committed; the harness fails loudly if the NAS
   source is missing. Unit tests use only synthetic files generated in-test, so ctest also
   runs in the Rocky 9 container.
3. **Headless PixInsight (isolated slot, never slot 1, via `tools/pi-headless.sh`):**
   signed dev build loaded with `-m=`; a probe script runs `SERViewer` with each fixture
   (`executeGlobal`), checks the new window's dimensions, channel count, a few sample values
   against values computed independently from the fixture, and the FITS keywords; writes a
   result marker the shell asserts.
4. **Preview proof:** on the private Xvfb display, open the interface with an instance whose
   `filePath` points at a converted fixture (interface `ImportProcess` loads it), and capture
   a PNG screenshot showing frame 0 in the preview with the info block. The screenshot is
   the evidence that "show it in a preview" works.
5. **Portability:** the module builds in the Rocky 9 container and passes
   `verify-portable.sh`.

The `OpenFileDialog` itself cannot be driven headlessly; Scott checks it when he pulls the
release from the repository.

## Repo wiring

- New tree `modules/planetary/` copying the PI Copilot pattern: top CMake + `cmake/PCLConfig.cmake`,
  `src/module/` (Module, Process, Instance, Interface, Parameters, `PlanetaryVersion.h` with
  `PLANETARY_MODULE_VERSION_{MAJOR,MINOR,REVISION,BUILD}` = 0.1.0.0,
  `pixinsight-module.map`), `src/ser/` (reader + debayer, PCL-free), `test/`.
- `Description()` ends with the PCL acknowledgment sentence; `doc/SERViewer/SERViewer.html`
  (what it does, files it reads, no network, no external programs) and
  `THIRD_PARTY_NOTICES.txt` (PCL ack + full PCL License 2.0 text), per CPD rules 6.1/7.3.
- Packaging: `release.sh` packages docs under `doc/tools/<Name>/` with `<Name>` = module
  name. PixInsight's Browse Documentation looks up `doc/tools/<ProcessId>/`, and the
  process id (`SERViewer`) differs from the module name (`Planetary`). Extend
  `build_module_tgz` / `module_pkg_check` / `module_doc_check` with an optional doc-id
  argument (default = module name), so NukeX and PI Copilot packages stay byte-identical.
- `release.sh`, `tools/build-env/build-modules.sh`: add Planetary alongside NukeX/PICopilot.
  **My call:** this wiring lives on `feat/planetary` and reaches `main` only when Scott OKs
  the first Planetary release, so no unrelated release can publish the module ids early.
- Headless proofs need the signing pass file `/tmp/.pi_codesign_pass` (0600); only Scott
  holds the password. It is shredded after the test/release run.
- `README.md` and `CLAUDE.md`: list the module, its version header, and its build/test
  commands.

## Non-goals (this increment)

Stacking, alignment, quality ranking, AVI/MP4 input, a File ▸ Open file-format module for
`.ser`, CMY Bayer, writing SER, derotation, sharpening, the release itself (a PixInsight
release needs Scott's go-ahead; the module id `Planetary` and process id `SERViewer` become
permanent once published).

## Anti-goals

- Any external program or network access at run time.
- Copying Lucid/Kepler code.
- Silently "fixing" bad files: every recovery is reported in the info block / console.

## Later increments (direction only)

Stacker (`PlanetaryStack`): quality ranking, global + AP alignment, per-AP lucky
selection, CFA drizzle, sigma-clipped weighting — improving on the Lucid defects recorded
in memory. Derotation using `pcl::Position` with the bundled DE440 ephemeris (offline).
Post-processing: Scott wants live sliders with a real-time preview (Kepler/waveSharp style),
even if PixInsight processes run underneath.
