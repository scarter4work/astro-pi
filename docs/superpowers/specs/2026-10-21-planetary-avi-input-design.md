# Planetary module — increment 2: AVI input + Save as SER (design)

Date: 2026-10-21 · Builds on `2026-10-20-planetary-ser-viewer-design.md` (increment 1, SERViewer 0.1.0.0)

## Intent

Scott's own planetary captures are ASIAIR AVIs, not SER, so SERViewer 0.1.0.0 cannot open
any of them. Scott (2026-10-21): "we need to build the avi->ser conversion into our dialog."
He chose: **SERViewer opens AVI directly** (same preview, slider, info block, Apply Global)
**plus a Save as SER… action** that writes a `.ser` when he wants one. Opening must not
require copying a multi-GB capture first. The future stacker reads frames through the same
abstraction, so it takes AVI or SER without conversion.

Scott also decided: the process keeps the id **`SERViewer`**; 0.1.0.0 ships first as is, and
this increment ships as **0.2.0.0**.

## Evidence (survey of Scott's NAS, 2026-10-21)

All 103 `.avi` files under `/mnt/qnap/astro_data/` (ffprobe + header bytes):

- **Every file is uncompressed 8-bit palettised video** (`rawvideo`, `pal8`): 320×240 up to
  1920×1080, 21 to 21 699 frames.
- **59 of 103 are over 1 GB and are OpenDML (AVI 2.0)**: `indx` super-index in the header and
  further `RIFF AVIX` segments (verified on
  `8_31_2023/2023-08-30-220111-Moon-Bin1 LPro 14.0F.avi`, 3.18 GB). The increment-1
  test converter rejects these; the shipped reader must not.
- **53 have an ASIAIR sidecar `<name>.avi.txt`** (the rest have none). Format: a `[ZWO <camera>]`
  section line, then `Key = Value` lines. Seen on all 53: `Colour Format = RAW8`, `Bayer = RG`.
  Cameras: ASI071MC Pro, ASI2400MC Pro, ASI462MC, ASI585MC Air. Also Gain, Exposure,
  Temperature, Capture Area Size, StartX/StartY, `Duration=<n> Sec`.
- **Index structure (all 103 parsed):** every file has a valid OpenDML `indx` super-index
  whose entries all point at `ix##` chunks, and its frame total equals ffprobe's
  `nb_frames` on every file. 3 files have no `idx1`. 56 are multi-RIFF (`AVIX`). Every
  `strf` has `biHeight < 0` (top-down rows). 64 files carry bytes after the last RIFF
  segment (ignored; frames are complete).
- **`Flip` and the Bayer phase (measured, not assumed):** `Flip = Vert(unimplemented)` on 47
  sidecars (cameras ASI071MC Pro, ASI2400MC Pro, ASI462MC, 2022–2023), `Flip = None` on 6
  (ASI585MC Air, 2026). All say `Bayer = RG`. Measured on frame data (mean of each 2×2 site
  over the brightest 0.5 % of superpixels, 20 frames): in every file the greens are the
  (0,1)/(1,0) sites. On `Flip = None` files the (0,0) site is the red one
  (s00/s11 = 1.21 Saturn, 1.42 Betelgeuse) → **RGGB**, as the sidecar says. On
  `Flip = Vert(unimplemented)` files the (1,1) site is the red one on all seven checked,
  across all three cameras (s00/s11 = 0.88 Mars, 0.91 and 0.66 Saturn, 0.80/0.80/0.73
  Jupiter, 0.68 Moon) → effectively **BGGR**, contradicting the sidecar. White balance gains
  are similar in both groups (B/R ≈ 1.3–1.5), so they don't explain the reversal.
  Testing item 3 turns this measurement into a regression check.
- **Filename time is not trustworthy as UTC.** Names start `YYYY-MM-DD-HHMMSS-<target>`. On
  two files checked the filename time is ~1 hour later than the file's mtime (e.g.
  `2023-05-26-230053-Moon…` has mtime 22:01:03 EDT with `Duration=9 Sec`); which clock is
  wrong is unknown.

## What the user sees

- **Open…** (the existing button, renamed from "Open SER…") with filter
  `Planetary video (*.ser *.avi)` plus per-type filters. Choosing an AVI behaves exactly
  like a SER: frame 0 preview, frame slider, Debayer checkbox, Apply Global opens the frame.
- **Info block for an AVI** adds: container `AVI` (or `AVI (OpenDML)`), frame rate, the
  Bayer pattern **and where it came from** (`sidecar` / `chosen` / `none`), and the
  sidecar's camera, gain, exposure when present. Warnings as in increment 1.
- **Bayer pattern control**: a combo `Auto / None (mono) / RGGB / GRBG / GBRG / BGGR`,
  enabled only for AVI input (a SER carries its own ColorID). `Auto` = the sidecar's
  `Bayer` value; with no sidecar, `Auto` resolves to **None (raw CFA mono) with a warning**
  telling the user to pick the pattern. Never a silent guess.
- **Save as SER…** button, enabled for AVI input: standard save dialog, default name = the
  AVI's name with `.ser`, default folder = the AVI's folder. Shows progress and can be
  cancelled. On success the info block/console says what was written; the viewer stays
  on the AVI.

## Process parameters

Existing `filePath`, `frameIndex`, `debayer` keep their ids and meaning (`filePath` now accepts
`.ser` or `.avi`, chosen by file content, not extension). New:

| Id | Type | Default | Meaning |
|---|---|---|---|
| `bayerPattern` | Enum: Auto, None, RGGB, GRBG, GBRG, BGGR | Auto | AVI only; ignored for SER |
| `saveSerPath` | String | "" | When non-empty, `ExecuteGlobal()` converts `filePath` to a SER at this path instead of opening a frame |

`saveSerPath` makes conversion scriptable and headless-testable. Instances saved by 0.1.0.0
(no new parameters) load with the defaults and behave exactly as before.

## AVI reader — decided rules

PCL-free C++17 beside the SER reader, 64-bit offsets, frames read on demand, header and
index treated as untrusted (same validation discipline as increment 1).

- **Container:** `RIFF 'AVI '` first segment; OpenDML `RIFF 'AVIX'` segments after it.
  Any other RIFF form → error.
- **Stream:** the first video stream (`strh` type `vids`). Other streams are ignored.
- **Supported video:** `strf` BITMAPINFOHEADER with 8 bits per pixel and either
  (a) `biCompression = 0` (BI_RGB) with a palette that is the **identity grey ramp**
  (entry *i* = (*i*,*i*,*i*)), or (b) fourcc `Y800`/`GREY`/`Y8  `. A non-identity palette
  is rejected with the first differing entries in the message. Anything else (compressed
  codecs, 16/24/32-bit) → "not supported" error naming the fourcc/bit count. A palette-change
  chunk (`xxpc`) → error.
- **Rows:** DIB rows padded to 4 bytes; `biHeight > 0` = bottom-up (flip to top-down),
  `< 0` = top-down. Output is top-down like the SER reader. The sidecar `Flip` field is
  recorded in the info block and **not** applied.
- **Frame index:** OpenDML `indx` → `ix##` standard indexes when present; else `idx1`; else a
  linear scan of the `movi` lists with a warning ("no index; scanned"). Zero-length video
  chunks (dropped frames) are skipped and counted in a warning. An index entry that points
  outside the file or at a chunk of the wrong size → that frame and all later ones are
  dropped with a warning (same "recovered count" model as SER); zero usable frames → error.
- **Frame rate:** from `strh` rate/scale (fallback `avih` µs/frame), shown in the info block.
- **Timestamps:** none. `DATE-OBS` is omitted for AVI frames (no trustworthy clock, see
  Evidence). The info block shows the filename's start time labelled "from filename, time
  zone unknown".
- **Sidecar:** `<avi path>.txt`, parsed leniently (`Key = Value`, trim, ignore unknown keys).
  `Bayer` RG/GR/GB/BG → RGGB/GRBG/GBRG/BGGR. **Flip rule (my call, from the measurement
  above):** when `Flip` starts with `Vert`, `Auto` uses the 180°-rotated pattern
  (RGGB↔BGGR, GRBG↔GBRG) and the info block says so ("sidecar RG, Flip=Vert → BGGR");
  `Flip = None` or absent uses the sidecar value as is. The user can override with the combo.
  The section name gives `INSTRUME`. A
  sidecar that exists but cannot be parsed is a warning, not an error. `Colour Format`
  other than RAW8 with a Bayer value → warning, the Bayer value is still used.
- **Values:** 8-bit, normalised by 255. `INSTRUME` from the sidecar; `BAYERPAT` and
  `HISTORY` as for SER.

## Shared frame source

The interface, `ExecuteGlobal()` and later the stacker read frames through one PCL-free
interface implemented by both readers: info (dims, planes, depth/normDepth, colour/Bayer,
frame count, warnings, container-specific fields), `ReadFrame(i)`, `FrameTimestamp(i)`.
Opening dispatches on the file's magic bytes (`LUCAM-RECORDER` vs `RIFF…AVI `). The existing
`LoadFrameForDisplay` path stays the single decode path for preview and Apply Global.

## Save as SER — decided rules

- ColorID from the effective Bayer pattern (None → MONO 0), depth 8, LittleEndian field 0,
  Instrument = sidecar camera (truncated to 40 bytes), Observer/Telescope empty.
- Frames copied in order, bytes as decoded (top-down). FrameCount = frames written.
- **No timestamp trailer; `DateTime_UTC` = 0.** `DateTime` (local) = the filename start time
  when it parses, else 0. Rationale: the filename clock is ~1 h off the file mtime on the
  files checked, so it is not presented as UTC. Derotation will need its own time input.
- Writes to `<target>.partial` in the target folder, then renames; on any error or cancel the
  partial file is deleted and the error is reported. Refuses to overwrite the source file;
  overwriting another existing file asks first (GUI) / fails (script) unless it's the same
  path the user already confirmed in the dialog.
- Fails up front with a clear message if the target filesystem lacks the space for the
  output size (computed exactly: 178 + N·W·H).
- Output must read back through `SerFile` with identical frames (tested).

## Testing and proof

1. **Unit (ctest, synthetic, also in the Rocky 9 container):** a test-side AVI writer
   produces AVI 1.0 (`idx1`), OpenDML (super-index + `ix00`, at least one `AVIX` segment;
   small, the reader must not depend on size), unindexed, bottom-up and top-down, `Y800`,
   non-identity palette (rejected), compressed fourcc (rejected), palette-change chunk
   (rejected), dropped zero-length frames, truncated tail, bad index offsets. Sidecar parser
   cases. Save-as-SER round trip: every frame equal; cancellation leaves no file.
2. **Real data (host-only, NAS):** run the reader over **all 103 NAS AVIs**: frame count
   equals ffprobe `nb_frames` (or the difference is explained by a warning), and frames
   0, N/2 and N−1 of every file match ffmpeg's `-pix_fmt pal8` output byte for byte
   (ffmpeg is test-only). Opening **any** of the 103 over the NAS mount must take under 2 s
   (all are indexed, so no scan); the run reports the slowest. Round-trip one >1 GB file
   through Save as SER.
3. **Bayer phase regression (host-only, NAS):** the spike measurement becomes a test over
   the planetary captures with sidecars: for every file, the pattern `Auto` resolves to must
   put red on the site the frame data says is redder on Mars/Saturn/Jupiter, and greens on
   the equal diagonal. Any file that disagrees fails the run with its numbers.
4. **Headless PixInsight** (`run-probe.sh`, slot 96): `executeGlobal` on AVI fixtures (with
   and without sidecar, each `bayerPattern`), and `saveSerPath` conversion read back by a
   second `executeGlobal` on the written SER.
5. **GUI smoke** (`gui-smoke.sh`, slot 97): open a real AVI, screenshot the preview + info
   block; switch the Bayer combo; drive Save as SER… and assert the file appears.
6. **Portability:** Rocky 9 container build + `verify-portable.sh`.

## Release

Branch `feat/planetary-avi`, cut from `feat/planetary` at the 0.1.0.0 release commit, merged
to `main` after 0.1.0.0 is published. Version 0.2.0.0 (`PlanetaryVersion.h`), release date
bumped, published via `./release.sh` once every proof above passes (Scott chose "AVI follows
as 0.2.0.0").
`doc/SERViewer/SERViewer.html` covers AVI input, the Bayer control, the sidecar, Save as SER,
and states that it reads only the chosen file and its `.txt` sidecar and writes only the
chosen SER path. Module `Description()` updated; PCL acknowledgment sentence stays last.

## Non-goals

Compressed AVI (MJPEG/H.264…), 16-bit or colour (24/32-bit) AVI, MP4/MOV, applying the
sidecar `Flip`, trusting the filename time as UTC, per-frame timestamps for AVI, a
File ▸ Open format module, batch conversion of many files.

## Anti-goals

- ffmpeg, libav or any external program at run time; any network access.
- A silent Bayer guess, or silently dropping frames.
- A second decode path that could disagree with the preview.

## Left to the builder

Class and file names; how the frame-source abstraction is shaped (virtual base vs variant);
the progress UI mechanism for Save as SER (it must not freeze the interface without feedback
and must be cancellable); where the sidecar parser lives; test-fixture layout.
