# PI Copilot — Increment 3: Vision (ViewContext + ViewPreview + ProcessCatalog) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** When the user sends a chat message, the PI Copilot panel attaches the active view: an auto-stretched JPEG preview plus a JSON summary of the view (geometry, per-channel stats, FITS keywords). Claude can then answer questions about the image, like "what object is this?". This increment also adds a native process catalog (`list_processes` / `describe_process` JSON) that increment 4 will expose as tools.

**Architecture:** There are three headless-testable units. `ViewContext` builds a JSON of a `View`. `ViewPreview` renders a JPEG from a private copy of the view: copy → downscale → auto-STF → `Bitmap::Render` → temp `.jpg` → base64. `ProcessCatalog` builds JSON from `Process`/`ProcessParameter` introspection, merged with summaries compiled in from `data/process-summaries.json`. A small `VisionTurn` unit composes the multimodal user turn and strips images from older turns. `AnthropicMessage` gains an optional base64 JPEG. The request builder emits a content-block array only for messages that carry an image. The panel captures the context and preview **on the UI thread at Send time, before the worker starts**. There is no tool loop in this increment (tools arrive in increment 4).

**Tech Stack:** C++17, PCL SDK (`$HOME/PCL`), nlohmann/json v3.11.3 (already FetchContent'd), PixInsight 1.9.5 headless harness (`--automation-mode`), module signing, `release.sh`.

**Spec:** `docs/superpowers/specs/2026-09-20-pi-copilot-native-pcl-design.md`. Increment 3 is "Grounding + get_view_context + ViewPreview. Vision round-trip." (§3 tool table rows `list_processes` / `describe_process` / `get_view_context`, §4 Grounding/ViewPreview, §5 threading, §7 vision data flow, §11 risk 4). This plan builds on increment 2 (main @ 8337706, released as 0.1.0.2).

**Corrections to the spec (binding, from pre-plan research):**
- Spec §4/§7 say ViewPreview is "ported from PICopilot.js renderPreview" and that vision is "the same path proven in the sidecar". **Both claims are false.** The retired sidecar never had vision (its design doc says "No multimodal … v1 — context is numeric stats + history"). ViewPreview and image content blocks are **new work**. Only the view-context *shape* is ported (old `getViewContext`: viewId, geometry, per-channel {median, mad, min, max}, fitsKeywords).
- Spec §1/§3 list "process history" in the view context. **PCL has no process-history API.** The context has **no `history` field**. Do not fake one.
- The research notes counted 26 entries in `process-summaries.json`. There are actually **25** (verified with `json.load`).

## Global Constraints

- The module ID string is `"PICopilot"` and is STABLE. Never rename it; existing installs update by ID.
- C++17. Flags are exactly `-fPIC -fvisibility=hidden -fvisibility-inlines-hidden`. Defines are exactly `__PCL_LINUX __PCL_BUILDING_MODULE _REENTRANT`. Output is `PICopilot-pxm.so` (+ `PICopilot-pxm.xsgn`).
- **Root-thread rules (hard):** `ImageWindow`, `View`, `Bitmap`, every `Control`, `MetaModule::EvaluateScript` and `ProcessInstance` execution are **root (UI) thread only**. Increment 2 proved this: constructing a `Control` off-root throws `CreateControl(): API function error`. All view reads, the preview render and the catalog introspection run on the UI thread. The worker `pcl::Thread` (`ChatThread`) only `Perform()`s the already-built HTTPS request. `Thread::Run()` must never touch GUI or console.
- **Never mutate the user's image.** `View::Image()` returns a SHARED alias (`View.h:466-475`). Read it under `AutoViewWriteLock`, copy it (`ImageVariant::CopyImage`) and transform only the copy. Compute stats with explicit `(rect, c, c)` channel arguments. **Never call `SelectChannel`** on a view's image.
- **No masked failures:** capture errors (context or preview) are shown in the chat log and the text still sends. "No active image" gets a visible note. API errors are shown verbatim (as in increment 2). Nothing is silently skipped, and there is no mock fallback.
- **Image limits (named constants, exact values):** preview long edge ≤ **1024** px (`PICopilotPreviewMaxEdge`), JPEG quality **85** (`PICopilotPreviewJpegQuality`), base64 ≤ **5 MiB** (`PICopilotMaxImageBase64Bytes`, the Anthropic per-image limit), MAD→σ factor **1.4826** (`PICopilotMadToSigma`). FITS cap: first **60** keywords (`PICopilotMaxFitsKeywords`), values truncated to **80** chars (`PICopilotMaxFitsValueChars`).
- **History cost rule:** only the LATEST user turn carries the image. Every older turn's image is replaced by the text note `kPICopilotImageOmittedNote`. The older turns' context JSON stays; it is small and capped.
- The Anthropic transport, `PICOPILOT_DEFAULT_MODEL` (`claude-opus-4-8`), the 300 s request deadline, cancel, and the non-streamed request shape are unchanged from increment 2. The request JSON has no `"stream"` key.
- **Never `make install`.** Dev tests load the module only through the headless harness (`test/run-selftest.sh`, `-m=` + `-r=` + `--force-exit` + `timeout`). **The user tests only by repo pull** from `https://raw.githubusercontent.com/scarter4work/astro-pi/main/repository/`. Never do a local `-m=` GUI load in the user's PixInsight: it persists the module path and caused a "Duplicate MetaProcess identifier" failure.
- **The GUI cannot be tested headlessly.** This covers the panel checkbox, capture-on-Send, and right-edge placement. The user verifies them after the repo-pull release (Task 7). Do not claim any of them as "tested" from a headless run.
- **Signing password file:** `test/run-selftest.sh` and `./release.sh` both read `/tmp/.pi_codesign_pass`. Create it once before Task 1, with mode 0600, containing the password from `~/.claude/CLAUDE.md` § "Module Signing". Shred it after Task 7. **Never write the password into any committed file (including this plan).**
- **Test API key:** never echo it and never commit it. Sources, in order: the system keyring (`secret-tool lookup service anthropic account default`), then the gitignored `modules/pi-copilot/test/.test_api_key`, then skip.
- Work on branch `feat/pi-copilot-inc3` (created in Task 1 Step 0). Every commit message ends with `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`.

## File Structure

```
modules/pi-copilot/
  src/module/CMakeLists.txt              # MODIFY: new sources; generate ProcessSummaries.h from data/process-summaries.json
  src/module/ProcessSummaries.h.in       # NEW: configure_file template -> build/.../generated/ProcessSummaries.h (raw string literal)
  src/module/PICopilotVisionSelfTest.h/.cpp  # NEW: increment-3 headless self-test sections + synthetic test image helpers
  src/module/PICopilotSelfTest.cpp       # MODIFY: call RunVisionSelfTest(), merge its keys into the verdict
  src/module/ViewContext.h/.cpp          # NEW: BuildViewContext(View) -> nlohmann::json
  src/module/ViewPreview.h/.cpp          # NEW: RenderViewPreview(View) -> ViewPreviewResult (base64 JPEG)
  src/module/ProcessCatalog.h/.cpp       # NEW: ListProcesses() / DescribeProcess(id) -> nlohmann::json (+ compiled-in summaries)
  src/module/AnthropicClient.h/.cpp      # MODIFY: AnthropicMessage::imageJpegBase64; BuildMessagesRequestBody() (content blocks)
  src/module/VisionTurn.h/.cpp           # NEW: ComposeUserTurn(), StripOlderImages(), kPICopilotImageOmittedNote
  src/module/PICopilotInterface.h/.cpp   # MODIFY: "Include view" CheckBox, capture on Send, system prompt, right-edge placement
  src/module/PICopilotVersion.h          # MODIFY: BUILD 2 -> 3
  src/module/PICopilotModule.cpp         # MODIFY: release date, Description()
  test/run-selftest.sh                   # MODIFY: list-based verdict assertion; keyring key source; timeout 300
  README.md                              # MODIFY: Increment 3 section + Verified line
repository/                              # REGENERATED by ./release.sh (Task 7)
```

Each unit has one responsibility. `ViewContext` reads numbers, `ViewPreview` makes pixels, `ProcessCatalog` introspects the registry, `VisionTurn` shapes messages, and the panel only orchestrates. All of them except the panel are exercised headlessly by `PICopilotVisionSelfTest.cpp`, which is kept out of the already long `PICopilotSelfTest.cpp`.

---

## Task 1: Headless vision smoke test (ImageWindow + View::Image + Bitmap::Render + JPEG save)

This task proves the platform capabilities that everything else relies on, under `PixInsight --automation-mode`, before any production code is written. **If it fails, stop and report BLOCKED** (see Step 5). Do not guess a workaround.

**Files:**
- Create: `modules/pi-copilot/src/module/PICopilotVisionSelfTest.h`
- Create: `modules/pi-copilot/src/module/PICopilotVisionSelfTest.cpp`
- Modify: `modules/pi-copilot/src/module/PICopilotSelfTest.cpp` (end of `RunSelfTest`, lines 246-272)
- Modify: `modules/pi-copilot/src/module/CMakeLists.txt` (`MODULE_SOURCES`)
- Modify: `modules/pi-copilot/test/run-selftest.sh` (python verdict block, lines 65-76)

**Interfaces:**
- Consumes: `RunSelfTest( String& )` (existing), nlohmann/json.
- Produces (used by Tasks 2-5, all in `PICopilotVisionSelfTest.cpp`'s anonymous namespace):
  - `constexpr int kSynthW = 2000, kSynthH = 1500, kSquareX0 = 900, kSquareY0 = 650, kSquareSize = 200;`
  - `double SynthBackground( int x )`: returns `0.02 + 0.03*x/(kSynthW-1)`.
  - `ImageWindow CreateSyntheticWindow()`: a hidden 2000×1500, 3-channel, 32-bit float RGB window with id `PICopilotSelfTest`. It holds a dim horizontal grey gradient plus a pure-red 200×200 square at (900,650).
  - `struct WindowCloser { ImageWindow window; ~WindowCloser(); }`: calls `ForceClose()` on destruction.
  - `bool IsJpeg( const ByteArray& )`: SOI `FF D8` at the start and EOI `FF D9` at the end.
  - `std::string U8( const String& )`: UTF-16 → UTF-8 `std::string`.
  - Public: `bool pcl::RunVisionSelfTest( nlohmann::json& out );` Later tasks insert sections above the marker comment `// ---- inc3 sections end ----`.

- [ ] **Step 0: Branch + prerequisites.**

```bash
cd /home/scarter4work/projects/astro-pi
git checkout main && git pull --ff-only && git checkout -b feat/pi-copilot-inc3
( umask 077; test -s /tmp/.pi_codesign_pass || echo "create /tmp/.pi_codesign_pass (0600) from ~/.claude/CLAUDE.md Module Signing before continuing" )
cd modules/pi-copilot && cmake -B build -DPCLDIR=$HOME/PCL -DPICOPILOT_BUILD_MODULE=ON
```
Expected: on branch `feat/pi-copilot-inc3`; CMake configures with `PCL found at /home/scarter4work/PCL -- building PICopilot module`.

- [ ] **Step 1: Write the failing assertion.** Replace the python block and the final echo in `test/run-selftest.sh` (lines 65-76) with a list-based check, so later tasks only append keys:

```bash
python3 - "$R" <<'PY' || { echo "FAIL: self-test verdict not all green"; exit 1; }
import json, sys
d = json.load(open(sys.argv[1]))
required_true = [
    'evalOk', 'processInstanceValid', 'keyStoreOk', 'anthropicOk', 'workerThreadOk',
    'cancelOk', 'deadlineOk', 'plainTextOk',
    # increment 3
    'visionSmokeOk',
    'ok',
]
missing = [k for k in required_true if d.get(k) is not True]
if d.get('evalResult') != 3: missing.append('evalResult==3')
if d.get('stallSkipped') is not False: missing.append('stallSkipped==false')
print('anthropic check: %s' % ('SKIPPED (no key)' if d.get('anthropicSkipped') else 'RAN against real API'))
if missing:
    print('FAILED keys: ' + ', '.join(missing))
    sys.exit(1)
PY
echo "PASS: self-test verdict all green"
```

- [ ] **Step 2: Run it to verify RED.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) && bash test/run-selftest.sh`
Expected: the verdict JSON prints, then `FAILED keys: visionSmokeOk` and `FAIL: self-test verdict not all green` (exit 1).

- [ ] **Step 3: Implement the smoke section.** `PICopilotVisionSelfTest.h`:

```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_VisionSelfTest_h
#define PICopilot_VisionSelfTest_h

#include <nlohmann/json.hpp>

namespace pcl
{

// Increment-3 headless self-test sections (vision smoke, ViewContext,
// ViewPreview, ProcessCatalog, request shape, gated real vision call).
// Root thread only (ImageWindow/View/Bitmap are UIObjects). Adds its keys
// to `out`; returns true iff every section passed.
bool RunVisionSelfTest( nlohmann::json& out );

} // namespace pcl

#endif // PICopilot_VisionSelfTest_h
```

`PICopilotVisionSelfTest.cpp`:

```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PICopilotVisionSelfTest.h"

#include <pcl/AutoViewLock.h>
#include <pcl/Bitmap.h>
#include <pcl/ByteArray.h>
#include <pcl/Exception.h>
#include <pcl/File.h>
#include <pcl/Image.h>
#include <pcl/ImageVariant.h>
#include <pcl/ImageWindow.h>
#include <pcl/View.h>

#include <string>

namespace pcl
{

namespace
{

// Synthetic test image: a dim horizontal grey gradient (identical in R,G,B)
// with a pure-red square. 2000 px wide so ViewPreview must downscale it.
constexpr int kSynthW = 2000, kSynthH = 1500;
constexpr int kSquareX0 = 900, kSquareY0 = 650, kSquareSize = 200;

double SynthBackground( int x )
{
   return 0.02 + 0.03*x/(kSynthW - 1);
}

std::string U8( const String& s )
{
   return std::string( s.ToUTF8().c_str() );
}

bool IsJpeg( const ByteArray& b )
{
   const size_type n = b.Length();
   return n >= 4 && b[0] == 0xFF && b[1] == 0xD8 && b[n-2] == 0xFF && b[n-1] == 0xD9;
}

ImageWindow CreateSyntheticWindow()
{
   // Hidden window (ImageWindow.h:348 -- "The new image window will be hidden").
   ImageWindow window( kSynthW, kSynthH, 3, 32, true/*floatSample*/, true/*color*/,
                       false/*initialProcessing*/, IsoString( "PICopilotSelfTest" ) );
   if ( window.IsNull() )
      throw Error( "ImageWindow construction returned a null window" );
   View view = window.MainView();
   AutoViewLock lock( view );
   ImageVariant v = view.Image();
   if ( !v || !v.IsFloatSample() || v.BitsPerSample() != 32 )
      throw Error( "synthetic window is not a 32-bit float image" );
   Image& img = static_cast<Image&>( *v );
   for ( int y = 0; y < kSynthH; ++y )
      for ( int x = 0; x < kSynthW; ++x )
      {
         const bool sq = x >= kSquareX0 && x < kSquareX0 + kSquareSize
                      && y >= kSquareY0 && y < kSquareY0 + kSquareSize;
         const float bg = float( SynthBackground( x ) );
         img.Pixel( x, y, 0 ) = sq ? 1.0f : bg;
         img.Pixel( x, y, 1 ) = sq ? 0.0f : bg;
         img.Pixel( x, y, 2 ) = sq ? 0.0f : bg;
      }
   return window;
}

struct WindowCloser
{
   ImageWindow window;
   ~WindowCloser()
   {
      try
      {
         if ( !window.IsNull() )
            window.ForceClose();
      }
      catch ( ... )
      {
      }
   }
};

struct SmokeTempGuard
{
   String path;
   ~SmokeTempGuard()
   {
      try
      {
         if ( !path.IsEmpty() && File::Exists( path ) )
            File::Remove( path );
      }
      catch ( ... )
      {
      }
   }
};

} // namespace

bool RunVisionSelfTest( nlohmann::json& out )
{
   bool allOk = true;

   // ---- Section 1: platform smoke (Task 1) --------------------------------
   // Proves, under --automation-mode: ImageWindow creation, View::Image()
   // read under a write lock, ImageVariant copy, Bitmap::Render, and
   // Bitmap::Save to a temp .jpg that reads back as a JPEG and is removed.
   {
      bool windowOk = false, readOk = false, renderOk = false, jpegOk = false;
      String error, path;
      try
      {
         WindowCloser wc{ CreateSyntheticWindow() };
         windowOk = true;
         View view = wc.window.MainView();
         ImageVariant copy;
         copy.CreateFloatImage( 32 );
         {
            AutoViewWriteLock lock( view );
            ImageVariant src = view.Image();
            readOk = bool( src ) && src.Width() == kSynthW && src.Height() == kSynthH
                  && src.NumberOfChannels() == 3;
            copy.CopyImage( src );
         }
         Bitmap bmp = Bitmap::Render( copy, -4/*1:4*/, DisplayChannel::RGBK, false/*transparency*/ );
         renderOk = !bmp.IsNull() && bmp.Width() == kSynthW/4 && bmp.Height() == kSynthH/4;
         SmokeTempGuard guard;
         guard.path = path = File::UniqueFileName( File::SystemTempDirectory(), 12, "picopilot-smoke-", ".jpg" );
         bmp.Save( path, 85 );
         jpegOk = IsJpeg( File::ReadFile( path ) );
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }
      const bool tempRemoved = !path.IsEmpty() && !File::Exists( path );
      const bool smokeOk = windowOk && readOk && renderOk && jpegOk && tempRemoved;
      out["smokeWindowOk"] = windowOk;
      out["smokeReadOk"] = readOk;
      out["smokeRenderOk"] = renderOk;
      out["smokeJpegOk"] = jpegOk;
      out["smokeTempRemoved"] = tempRemoved;
      out["smokeError"] = U8( error );
      out["visionSmokeOk"] = smokeOk;
      allOk = allOk && smokeOk;
   }

   // ---- inc3 sections end ----
   return allOk;
}

} // namespace pcl
```

In `PICopilotSelfTest.cpp`, add `#include "PICopilotVisionSelfTest.h"` and merge the new keys. Replace the tail of `RunSelfTest` from `nlohmann::json j = {` through `return ok;` with:

```cpp
   nlohmann::json j = {
      { "evalResult", evalResult },
      { "evalOk", evalOk },
      { "processInstanceValid", piValid },
      { "keyStoreOk", keyStoreOk },
      { "anthropicOk", anthropicOk },
      { "anthropicSkipped", anthropicSkipped },
      { "workerThreadOk", workerThreadOk },
      { "workerHttpStatus", workerHttpStatus },
      { "workerError", workerError.ToUTF8().c_str() },
      { "stallSkipped", stallSkipped },
      { "cancelOk", cancelOk },
      { "cancelError", cancelError.ToUTF8().c_str() },
      { "cancelSeconds", cancelSeconds },
      { "deadlineOk", deadlineOk },
      { "deadlineError", deadlineError.ToUTF8().c_str() },
      { "deadlineSeconds", deadlineSeconds },
      { "plainTextOk", plainTextOk },
      { "plainTextBack", plainTextBack.ToUTF8().c_str() }
   };

   // Increment 3: vision/grounding sections. Never let an escape here lose
   // the increment-1/2 verdict -- record it as a failure instead.
   bool visionOk = false;
   try
   {
      nlohmann::json vision;
      visionOk = RunVisionSelfTest( vision );
      j.update( vision );
   }
   catch ( const std::exception& x )
   {
      j["visionException"] = x.what();
   }
   catch ( ... )
   {
      j["visionException"] = "unknown exception";
   }

   ok = ok && visionOk;
   j["ok"] = ok;
   jsonOut = String::UTF8ToUTF16( j.dump().c_str() );
   return ok;
```
(Leave the existing non-const `bool ok = evalOk && … && plainTextOk;` at line 246 unchanged. The block above reassigns it.)

Add `PICopilotVisionSelfTest.cpp` to `MODULE_SOURCES` in `src/module/CMakeLists.txt` (after `ChatThread.cpp`).

Update the `PICopilotSelfTest.h` comment block: add "8. Increment-3 vision/grounding sections (PICopilotVisionSelfTest.cpp)". Also add the new keys to the "Populates jsonOut with" list.

- [ ] **Step 4: Run to verify GREEN.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) && bash test/run-selftest.sh`
Expected: the verdict shows `"visionSmokeOk":true`, `"smokeWindowOk":true`, `"smokeReadOk":true`, `"smokeRenderOk":true`, `"smokeJpegOk":true`, `"smokeTempRemoved":true`, `"smokeError":""`, and the last line is `PASS: self-test verdict all green`.

- [ ] **Step 5: If any smoke key is false, STOP and report BLOCKED. Do not continue to Task 2.** Include the full verdict JSON (`smokeError` carries the core's message) in the report. Use these fallbacks only with controller approval, depending on which key failed:
  - `smokeWindowOk:false` (ImageWindow cannot be created headless): Tasks 2-3 would have to test `ViewContext`/`ViewPreview` through an `ImageVariant` overload (`BuildImageContext(const ImageVariant&)`, `RenderImagePreview(const ImageVariant&)`) on an in-memory `Image`. The `View` wrappers would become GUI-only. That is a design change, so the controller decides.
  - `smokeRenderOk:false` or `smokeJpegOk:false` (Bitmap unusable headless, or no Qt JPEG writer): encode through PixInsight's own JPEG file format instead. Use `FileFormat fmt( ".jpg", false/*read*/, true/*write*/ ); FileFormatInstance file( fmt ); file.Create( path, "quality 85" ); file.WriteImage( image8 ); file.Close();` (`FileFormat.h:134`, `FileFormatInstance.h:729/992/123`) on an 8-bit copy of the stretched image, then `File::ReadFile` as before. The exact hint string `"quality 85"` for the JPEG format module is **unverified**. Confirm it before relying on it.
  - `smokeReadOk:false`: report the dimensions observed. Do not "fix" the assertion.

- [ ] **Step 6: Commit.**

```bash
cd /home/scarter4work/projects/astro-pi
git add modules/pi-copilot/src/module/PICopilotVisionSelfTest.h modules/pi-copilot/src/module/PICopilotVisionSelfTest.cpp \
        modules/pi-copilot/src/module/PICopilotSelfTest.h modules/pi-copilot/src/module/PICopilotSelfTest.cpp \
        modules/pi-copilot/src/module/CMakeLists.txt modules/pi-copilot/test/run-selftest.sh
git commit -m "test(pi-copilot): headless vision smoke — ImageWindow, View::Image, Bitmap::Render, JPEG save

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: ViewContext (view JSON: geometry, per-channel stats, capped FITS keywords)

**Files:**
- Create: `modules/pi-copilot/src/module/ViewContext.h`
- Create: `modules/pi-copilot/src/module/ViewContext.cpp`
- Modify: `modules/pi-copilot/src/module/PICopilotVisionSelfTest.cpp` (new section + keyword helper)
- Modify: `modules/pi-copilot/src/module/CMakeLists.txt`
- Modify: `modules/pi-copilot/test/run-selftest.sh` (`required_true`)

**Interfaces:**
- Consumes: `CreateSyntheticWindow()`, `WindowCloser`, `U8()`, `kSynth*` constants (Task 1).
- Produces:

```cpp
namespace pcl {
constexpr int PICopilotMaxFitsKeywords   = 60;
constexpr int PICopilotMaxFitsValueChars = 80;
// Root thread only. Throws pcl::Error for a null view / no image / complex image.
nlohmann::json BuildViewContext( const View& view );
}
```
JSON shape (exact keys): `viewId`, `fullId`, `isPreview`, `filePath` (`""` when unsaved), `geometry{width,height,channels,nominalChannels,bitsPerSample,floatSample,color}`, `channelStats[{channel,median,mad,mean,min,max}]` (one per nominal channel), `statsNote`, `fitsKeywords[{name,value[,valueTruncated]}]`, `fitsKeywordsTotal`, `fitsKeywordsOmitted`. **No `history` key.**

- [ ] **Step 1: Write the failing test.** In `PICopilotVisionSelfTest.cpp`, add `#include "ViewContext.h"` and `#include <pcl/FITSHeaderKeyword.h>`. Add this helper to the anonymous namespace:

```cpp
// 70 keywords: OBJECT, FILTER, then KW001..KW068 with 100-char values, so
// the 60-keyword cap and the 80-char value cap both engage.
void AddSyntheticKeywords( ImageWindow& window )
{
   FITSKeywordArray k;
   k.Add( FITSHeaderKeyword( "OBJECT", "'M42'", "synthetic" ) );
   k.Add( FITSHeaderKeyword( "FILTER", "'Full'", "synthetic" ) );
   const IsoString longValue = "'" + IsoString( 'x', 100 ) + "'";
   for ( int i = 1; i <= 68; ++i )
      k.Add( FITSHeaderKeyword( IsoString().Format( "KW%03d", i ), longValue, "" ) );
   window.SetKeywords( k );
}
```
Insert this section directly above `// ---- inc3 sections end ----`:

```cpp
   // ---- Section 2: ViewContext (Task 2) -----------------------------------
   {
      bool ctxOk = false;
      String error;
      nlohmann::json ctx;
      try
      {
         WindowCloser wc{ CreateSyntheticWindow() };
         AddSyntheticKeywords( wc.window );
         ctx = BuildViewContext( wc.window.MainView() );

         const nlohmann::json& g = ctx.at( "geometry" );
         const nlohmann::json& s = ctx.at( "channelStats" );
         const nlohmann::json& k = ctx.at( "fitsKeywords" );
         auto near = []( double a, double b, double tol ) { return a >= b - tol && a <= b + tol; };
         bool medians = s.size() == 3;
         for ( const nlohmann::json& c : s )
            medians = medians && c.at( "median" ).get<double>() > 0.03 && c.at( "median" ).get<double>() < 0.04
                              && c.at( "mad" ).get<double>() > 0;
         ctxOk = ctx.at( "viewId" ).get<std::string>().rfind( "PICopilotSelfTest", 0 ) == 0
              && ctx.at( "isPreview" ) == false
              && ctx.at( "filePath" ) == ""
              && !ctx.contains( "history" )
              && g.at( "width" ) == kSynthW && g.at( "height" ) == kSynthH
              && g.at( "channels" ) == 3 && g.at( "nominalChannels" ) == 3
              && g.at( "bitsPerSample" ) == 32 && g.at( "floatSample" ) == true && g.at( "color" ) == true
              && medians
              && near( s[0].at( "max" ).get<double>(), 1.0, 1e-6 )                  // red square
              && near( s[0].at( "min" ).get<double>(), SynthBackground( 0 ), 1e-6 )
              && near( s[1].at( "min" ).get<double>(), 0.0, 1e-9 )                  // square is 0 in G
              && near( s[1].at( "max" ).get<double>(), SynthBackground( kSynthW - 1 ), 1e-6 )
              && s[0].at( "mean" ).get<double>() > s[1].at( "mean" ).get<double>()
              && ctx.at( "fitsKeywordsTotal" ) == 70
              && ctx.at( "fitsKeywordsOmitted" ) == 10
              && k.size() == size_t( PICopilotMaxFitsKeywords )
              && k[0].at( "name" ) == "OBJECT" && k[0].at( "value" ) == "M42"
              && k[2].at( "name" ) == "KW001"
              && k[2].at( "value" ).get<std::string>().size() == size_t( PICopilotMaxFitsValueChars )
              && k[2].at( "valueTruncated" ) == true
              && !k[0].contains( "valueTruncated" );
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }

      // A null view must be rejected loudly, not produce an empty context.
      bool nullRejected = false;
      try { BuildViewContext( View() ); }
      catch ( const pcl::Exception& ) { nullRejected = true; }

      ctx.erase( "fitsKeywords" );   // keep the verdict readable; counts stay
      out["viewContext"] = ctx;
      out["viewContextError"] = U8( error );
      out["viewContextNullRejected"] = nullRejected;
      out["viewContextOk"] = ctxOk && nullRejected;
      allOk = allOk && ctxOk && nullRejected;
   }
```
In `run-selftest.sh`, add `'viewContextOk',` on the line after `'visionSmokeOk',`. Add `ViewContext.cpp` to `MODULE_SOURCES`.

- [ ] **Step 2: Run to verify RED.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc)`
Expected: compile FAIL, with `ViewContext.h: No such file or directory` (or `BuildViewContext was not declared`).

- [ ] **Step 3: Implement.** `ViewContext.h`:

```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ViewContext_h
#define PICopilot_ViewContext_h

#include <pcl/View.h>

#include <nlohmann/json.hpp>

namespace pcl
{

// FITS keyword caps for the context sent to the model (token cost).
constexpr int PICopilotMaxFitsKeywords   = 60;
constexpr int PICopilotMaxFitsValueChars = 80;

/*
 * JSON description of a view for the model: identity, geometry, robust
 * per-channel statistics of the REAL (usually linear) data, and the first
 * PICopilotMaxFitsKeywords FITS keywords. There is deliberately no process
 * history: PCL exposes no API for it.
 *
 * Root thread only. Reads the image under AutoViewWriteLock (blocks writers,
 * readers unaffected) and never modifies it: statistics use explicit
 * (rect, channel, channel) arguments -- no SelectChannel() calls.
 * Throws pcl::Error for a null view, a view with no image, or a complex image.
 */
nlohmann::json BuildViewContext( const View& view );

} // namespace pcl

#endif // PICopilot_ViewContext_h
```

`ViewContext.cpp`:

```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ViewContext.h"

#include <pcl/AutoViewLock.h>
#include <pcl/Exception.h>
#include <pcl/FITSHeaderKeyword.h>
#include <pcl/ImageVariant.h>
#include <pcl/ImageWindow.h>

#include <string>

namespace pcl
{

namespace
{

std::string U8( const String& s )
{
   return std::string( s.ToUTF8().c_str() );
}

// FITS text is 8-bit; String( const char* ) decodes ISO-8859-1, so any
// high byte survives as a code point and is re-encoded as valid UTF-8.
std::string FitsU8( const IsoString& s )
{
   return U8( String( s.c_str() ) );
}

} // namespace

nlohmann::json BuildViewContext( const View& view )
{
   if ( view.IsNull() )
      throw Error( "BuildViewContext: null view" );

   View v = view;                       // alias; locking needs a non-const View
   ImageWindow window = v.Window();

   nlohmann::json ctx;
   ctx["viewId"] = std::string( v.Id().c_str() );
   ctx["fullId"] = std::string( v.FullId().c_str() );
   ctx["isPreview"] = v.IsPreview();
   ctx["filePath"] = window.IsNull() ? std::string() : U8( window.FilePath() );

   {
      AutoViewWriteLock lock( v );
      ImageVariant img = v.Image();
      if ( !img )
         throw Error( "BuildViewContext: view has no image" );
      if ( img.IsComplexSample() )
         throw Error( "BuildViewContext: complex-sample images are not supported" );

      const Rect r = img.Bounds();
      const int nominal = img.NumberOfNominalChannels();
      ctx["geometry"] = {
         { "width", img.Width() },
         { "height", img.Height() },
         { "channels", img.NumberOfChannels() },
         { "nominalChannels", nominal },
         { "bitsPerSample", img.BitsPerSample() },
         { "floatSample", img.IsFloatSample() },
         { "color", img.IsColor() }
      };

      nlohmann::json stats = nlohmann::json::array();
      for ( int c = 0; c < nominal; ++c )
      {
         const double median = img.Median( r, c, c );
         stats.push_back( {
            { "channel", c },
            { "median", median },
            { "mad", img.MAD( median, r, c, c ) },
            { "mean", img.Mean( r, c, c ) },
            { "min", img.MinimumSampleValue( r, c, c ) },
            { "max", img.MaximumSampleValue( r, c, c ) }
         } );
      }
      ctx["channelStats"] = stats;
      ctx["statsNote"] = "Statistics of the real image data (normalized [0,1] sample range), "
                         "not of the preview. mad is the RAW median absolute deviation "
                         "(multiply by 1.4826 for a Gaussian-equivalent sigma).";
   }

   const FITSKeywordArray keywords = window.IsNull() ? FITSKeywordArray() : window.Keywords();
   nlohmann::json fits = nlohmann::json::array();
   const size_type kept = Min( keywords.Length(), size_type( PICopilotMaxFitsKeywords ) );
   for ( size_type i = 0; i < kept; ++i )
   {
      const FITSHeaderKeyword& kw = keywords[i];
      IsoString value = kw.StripValueDelimiters();
      nlohmann::json entry = { { "name", FitsU8( kw.name ) } };
      if ( value.Length() > size_type( PICopilotMaxFitsValueChars ) )
      {
         entry["value"] = FitsU8( value.Left( PICopilotMaxFitsValueChars ) );
         entry["valueTruncated"] = true;
      }
      else
         entry["value"] = FitsU8( value );
      fits.push_back( entry );
   }
   ctx["fitsKeywords"] = fits;
   ctx["fitsKeywordsTotal"] = keywords.Length();
   ctx["fitsKeywordsOmitted"] = keywords.Length() - kept;
   return ctx;
}

} // namespace pcl
```
(`Min` is `pcl::Min` from `<pcl/Utility.h>`, which `ImageVariant.h` pulls in. If the compiler disagrees, use `std::min` with `<algorithm>`.)

- [ ] **Step 4: Run to verify GREEN.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) && bash test/run-selftest.sh`
Expected: `"viewContextOk":true`, `"viewContextNullRejected":true`, and `viewContext.fitsKeywordsTotal` = 70. The last line is `PASS: self-test verdict all green`.

- [ ] **Step 5: Commit.**

```bash
cd /home/scarter4work/projects/astro-pi
git add modules/pi-copilot/src/module/ViewContext.h modules/pi-copilot/src/module/ViewContext.cpp \
        modules/pi-copilot/src/module/PICopilotVisionSelfTest.cpp modules/pi-copilot/src/module/CMakeLists.txt \
        modules/pi-copilot/test/run-selftest.sh
git commit -m "feat(pi-copilot): ViewContext — view geometry, per-channel stats, capped FITS keywords

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: ViewPreview (copy → downscale ≤ 1024 → auto-STF → JPEG q85 → base64)

**Files:**
- Create: `modules/pi-copilot/src/module/ViewPreview.h`
- Create: `modules/pi-copilot/src/module/ViewPreview.cpp`
- Modify: `modules/pi-copilot/src/module/PICopilotVisionSelfTest.cpp`
- Modify: `modules/pi-copilot/src/module/CMakeLists.txt`
- Modify: `modules/pi-copilot/test/run-selftest.sh` (`required_true`)

**Interfaces:**
- Consumes: Task 1 helpers (`CreateSyntheticWindow`, `WindowCloser`, `IsJpeg`, `U8`, `kSynth*`, `kSquare*`).
- Produces:

```cpp
namespace pcl {
constexpr int       PICopilotPreviewMaxEdge      = 1024;
constexpr int       PICopilotPreviewJpegQuality  = 85;
constexpr size_type PICopilotMaxImageBase64Bytes = 5*1024*1024;
constexpr double    PICopilotMadToSigma          = 1.4826;
struct ViewPreviewResult {
   bool      ok = false;
   String    error;
   IsoString base64;      // standard Base64 of the JPEG bytes; empty unless ok
   int       width = 0, height = 0;
   size_type jpegBytes = 0;
   String    tempPath;    // staging path used (already deleted) -- for the self-test
};
// Root thread only. Never throws; never modifies the view's image.
ViewPreviewResult RenderViewPreview( const View& view );
}
```

- [ ] **Step 1: Write the failing test.** In `PICopilotVisionSelfTest.cpp`, add `#include "ViewPreview.h"` and `#include <pcl/Color.h>`. Add this helper to the anonymous namespace:

```cpp
// Order-dependent 64-bit hash of every channel's pixel buffer (Image::Hash64,
// Image.h:13786). Synthetic windows are 32-bit float, so Image is exact.
uint64 ViewImageHash( View view )
{
   AutoViewWriteLock lock( view );
   ImageVariant v = view.Image();
   const Image& img = static_cast<const Image&>( *v );
   uint64 h = 0;
   for ( int c = 0; c < img.NumberOfChannels(); ++c )
      h = img.Hash64( c, h );
   return h;
}
```
Insert this section above `// ---- inc3 sections end ----`:

```cpp
   // ---- Section 3: ViewPreview (Task 3) -----------------------------------
   {
      bool previewOk = false, unchanged = false, tempRemoved = false, pixelsOk = false;
      String error;
      ViewPreviewResult p;
      int sqR = -1, sqG = -1, sqB = -1, bgR = -1, bgG = -1, bgB = -1;
      try
      {
         WindowCloser wc{ CreateSyntheticWindow() };
         const View view = wc.window.MainView();
         const uint64 before = ViewImageHash( view );
         p = RenderViewPreview( view );
         unchanged = ViewImageHash( view ) == before;
         tempRemoved = !p.tempPath.IsEmpty() && !File::Exists( p.tempPath );
         if ( p.ok )
         {
            const ByteArray jpeg = p.base64.FromBase64();
            // Decode the JPEG and sample it: the red square must read red and
            // the stretched background must be neutral grey, not black.
            Bitmap decoded( jpeg.Begin(), jpeg.Length(), "JPG" );
            const double s = double( p.width )/kSynthW;
            const RGBA sq = decoded.Pixel( int( (kSquareX0 + kSquareSize/2)*s ), int( (kSquareY0 + kSquareSize/2)*s ) );
            const RGBA bg = decoded.Pixel( int( 200*s ), int( 200*s ) );
            sqR = Red( sq ); sqG = Green( sq ); sqB = Blue( sq );
            bgR = Red( bg ); bgG = Green( bg ); bgB = Blue( bg );
            pixelsOk = sqR > 180 && sqG < 80 && sqB < 80
                    && Min( bgR, Min( bgG, bgB ) ) > 4
                    && Max( bgR, Max( bgG, bgB ) ) - Min( bgR, Min( bgG, bgB ) ) < 24;
            previewOk = IsJpeg( jpeg )
                     && jpeg.Length() == p.jpegBytes
                     && Max( p.width, p.height ) <= PICopilotPreviewMaxEdge
                     && Max( p.width, p.height ) >= PICopilotPreviewMaxEdge - 8
                     && p.base64.Length() == 4*((p.jpegBytes + 2)/3)
                     && p.base64.Length() > 1000
                     && p.base64.Length() <= PICopilotMaxImageBase64Bytes;
         }
         else
            error = p.error;
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }

      const ViewPreviewResult nullResult = RenderViewPreview( View() );
      const bool nullRejected = !nullResult.ok && !nullResult.error.IsEmpty() && nullResult.base64.IsEmpty();

      const bool ok = previewOk && unchanged && tempRemoved && pixelsOk && nullRejected;
      out["previewWidth"] = p.width;
      out["previewHeight"] = p.height;
      out["previewJpegBytes"] = p.jpegBytes;
      out["previewBase64Len"] = p.base64.Length();
      out["previewTempRemoved"] = tempRemoved;
      out["previewUserImageUnchanged"] = unchanged;
      out["previewSquareRGB"] = { sqR, sqG, sqB };
      out["previewBackgroundRGB"] = { bgR, bgG, bgB };
      out["previewNullRejected"] = nullRejected;
      out["previewError"] = U8( error );
      out["previewOk"] = ok;
      allOk = allOk && ok;
   }
```
In `run-selftest.sh`, add `'previewOk',` after `'viewContextOk',`. Add `ViewPreview.cpp` to `MODULE_SOURCES`.

- [ ] **Step 2: Run to verify RED.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc)`
Expected: compile FAIL, with `ViewPreview.h: No such file or directory`.

- [ ] **Step 3: Implement.** `ViewPreview.h`:

```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ViewPreview_h
#define PICopilot_ViewPreview_h

#include <pcl/String.h>
#include <pcl/View.h>

namespace pcl
{

constexpr int       PICopilotPreviewMaxEdge      = 1024;           // px, long edge
constexpr int       PICopilotPreviewJpegQuality  = 85;
constexpr size_type PICopilotMaxImageBase64Bytes = 5*1024*1024;    // Anthropic per-image limit
constexpr double    PICopilotMadToSigma          = 1.4826;

struct ViewPreviewResult
{
   bool      ok = false;
   String    error;
   IsoString base64;       // standard Base64 of the JPEG; empty unless ok
   int       width = 0;
   int       height = 0;
   size_type jpegBytes = 0;
   String    tempPath;     // staging path used (already deleted)
};

/*
 * Display-only JPEG preview of a view for the model:
 *   private float copy -> integer box pre-shrink (when >= 2x too big) ->
 *   bicubic-spline Resample to a long edge <= PICopilotPreviewMaxEdge ->
 *   auto-STF (DisplayFunction::ComputeAutoStretch; center = per-channel
 *   median, sigma = MAD x 1.4826; unlinked; PCL defaults -2.8 / 0.25) ->
 *   Bitmap::Render -> temp .jpg (q85) -> File::ReadFile -> delete -> Base64.
 * The stretch is computed from the data, independent of the user's own
 * screen STF, so the preview is deterministic.
 *
 * Root thread only (View/Bitmap are UIObjects). Never throws; failures come
 * back as ok=false + error. Never modifies the view's image (reads it under
 * AutoViewWriteLock into a copy). The temp file is removed on every path.
 * Cost: one transient full-resolution float copy of the image.
 */
ViewPreviewResult RenderViewPreview( const View& view );

} // namespace pcl

#endif // PICopilot_ViewPreview_h
```

`ViewPreview.cpp`:

```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ViewPreview.h"

#include <pcl/AutoViewLock.h>
#include <pcl/Bitmap.h>
#include <pcl/DisplayFunction.h>
#include <pcl/Exception.h>
#include <pcl/File.h>
#include <pcl/ImageVariant.h>
#include <pcl/IntegerResample.h>
#include <pcl/PixelInterpolation.h>
#include <pcl/Resample.h>
#include <pcl/Vector.h>

#include <algorithm>

namespace pcl
{

namespace
{

struct TempFileGuard
{
   String path;
   ~TempFileGuard()
   {
      try
      {
         if ( !path.IsEmpty() && File::Exists( path ) )
            File::Remove( path );
      }
      catch ( ... )
      {
      }
   }
};

} // namespace

ViewPreviewResult RenderViewPreview( const View& view )
{
   ViewPreviewResult res;
   TempFileGuard guard;
   try
   {
      if ( view.IsNull() )
      {
         res.error = "no view to preview";
         return res;
      }

      // 1. Private 32-bit float copy -- the user's image is only read.
      ImageVariant work;
      work.CreateFloatImage( 32 );
      {
         View v = view;
         AutoViewWriteLock lock( v );
         ImageVariant src = v.Image();
         if ( !src )
         {
            res.error = "view has no image";
            return res;
         }
         if ( src.IsComplexSample() )
         {
            res.error = "complex-sample images cannot be previewed";
            return res;
         }
         work.CopyImage( src );
      }
      if ( work.NumberOfChannels() > work.NumberOfNominalChannels() )
         work.DeleteAlphaChannels();

      // 2. Downscale to a long edge <= PICopilotPreviewMaxEdge.
      int longEdge = std::max( work.Width(), work.Height() );
      if ( longEdge > PICopilotPreviewMaxEdge )
      {
         const int factor = longEdge/PICopilotPreviewMaxEdge;
         if ( factor >= 2 )
         {
            IntegerResample shrink( -factor, IntegerDownsampleMode::Average );
            shrink >> work;
            longEdge = std::max( work.Width(), work.Height() );
         }
         if ( longEdge > PICopilotPreviewMaxEdge )
         {
            BicubicSplinePixelInterpolation bicubic;
            Resample resample( bicubic, double( PICopilotPreviewMaxEdge )/longEdge );
            resample >> work;
         }
      }

      // 3. Auto-STF on the (downscaled) copy.
      const int n = work.NumberOfNominalChannels();   // 1 (grey) or 3 (RGB)
      const Rect r = work.Bounds();
      DVector center( n ), sigma( n );
      for ( int c = 0; c < n; ++c )
      {
         center[c] = work.Median( r, c, c );
         sigma[c] = PICopilotMadToSigma*work.MAD( center[c], r, c, c );
      }
      DisplayFunction stf;
      stf.ComputeAutoStretch( sigma, center );
      stf >> work;

      // 4. Render to an 8-bit bitmap.
      Bitmap bmp = Bitmap::Render( work, 1/*zoom*/, DisplayChannel::RGBK, false/*transparency*/ );
      res.width = bmp.Width();
      res.height = bmp.Height();
      if ( std::max( res.width, res.height ) > PICopilotPreviewMaxEdge )
      {
         res.error = String().Format( "preview is %dx%d, over the %d px limit",
                                      res.width, res.height, PICopilotPreviewMaxEdge );
         return res;
      }

      // 5. JPEG through a temp file (PCL has no in-memory JPEG encoder).
      guard.path = File::UniqueFileName( File::SystemTempDirectory(), 12, "picopilot-preview-", ".jpg" );
      res.tempPath = guard.path;
      bmp.Save( guard.path, PICopilotPreviewJpegQuality );
      const ByteArray jpeg = File::ReadFile( guard.path );
      File::Remove( guard.path );   // eager; the guard covers every other exit
      if ( jpeg.Length() < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8 )
      {
         res.error = "Bitmap::Save did not produce a JPEG";
         return res;
      }
      res.jpegBytes = jpeg.Length();
      res.base64 = IsoString::ToBase64( jpeg );
      if ( res.base64.Length() > PICopilotMaxImageBase64Bytes )
      {
         res.error = String().Format( "preview JPEG is too large to send (%u base64 bytes)",
                                      unsigned( res.base64.Length() ) );
         res.base64.Clear();
         return res;
      }
      res.ok = true;
   }
   catch ( const pcl::Exception& x )
   {
      res.error = "preview failed: " + x.Message();
   }
   catch ( const std::exception& x )
   {
      res.error = String( "preview failed: " ) + String( x.what() );
   }
   catch ( ... )
   {
      res.error = "preview failed: unknown error";
   }
   if ( !res.ok )
      res.base64.Clear();
   return res;
}

} // namespace pcl
```

- [ ] **Step 4: Run to verify GREEN.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) && bash test/run-selftest.sh`
Expected: `"previewOk":true`, `"previewWidth":1024`, `"previewHeight":768`, `"previewUserImageUnchanged":true`, `"previewTempRemoved":true`, and `previewSquareRGB` red-dominant. The last line is `PASS: self-test verdict all green`. If `previewSquareRGB`/`previewBackgroundRGB` fail the thresholds, report the observed values. Do not loosen the thresholds without controller approval.

- [ ] **Step 5: Commit.**

```bash
cd /home/scarter4work/projects/astro-pi
git add modules/pi-copilot/src/module/ViewPreview.h modules/pi-copilot/src/module/ViewPreview.cpp \
        modules/pi-copilot/src/module/PICopilotVisionSelfTest.cpp modules/pi-copilot/src/module/CMakeLists.txt \
        modules/pi-copilot/test/run-selftest.sh
git commit -m "feat(pi-copilot): ViewPreview — auto-STF JPEG preview of a view copy (<=1024 px, q85, base64)

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: ProcessCatalog (list_processes / describe_process JSON + compiled-in summaries)

**Files:**
- Create: `modules/pi-copilot/src/module/ProcessSummaries.h.in`
- Create: `modules/pi-copilot/src/module/ProcessCatalog.h`
- Create: `modules/pi-copilot/src/module/ProcessCatalog.cpp`
- Modify: `modules/pi-copilot/src/module/CMakeLists.txt` (configure_file + generated include dir + source)
- Modify: `modules/pi-copilot/src/module/PICopilotVisionSelfTest.cpp`
- Modify: `modules/pi-copilot/test/run-selftest.sh` (`required_true`)

**Interfaces:**
- Consumes: `modules/pi-copilot/data/process-summaries.json` (flat `{"<ProcessId>": "<one-line summary>"}`, 25 entries). It is read **at CMake configure time only**, with no runtime file dependency.
- Produces (consumed as tools in increment 4):

```cpp
namespace pcl {
constexpr size_type PICopilotMaxProcessDescriptionChars = 2000;
const nlohmann::json& CompiledProcessSummaries();  // the embedded JSON, parsed once
nlohmann::json ListProcesses();                    // {"count":N,"processes":[{"id","categories":[..],"summary"?}]} sorted by id
nlohmann::json DescribeProcess( const IsoString& id );
   // {"id","categories","summary"?,"description","parameters":[{"id","type","readOnly","required",
   //   "default"?,"min"?,"max"?,"enumeration"?:[{"id","value"}],"columns"?:[...]}]}
   // or {"error":"unknown process id: <id> (<core message>)"}
}
```

- [ ] **Step 1: Write the failing test.** In `PICopilotVisionSelfTest.cpp`, add `#include "ProcessCatalog.h"`. Insert above `// ---- inc3 sections end ----`:

```cpp
   // ---- Section 4: ProcessCatalog (Task 4) --------------------------------
   {
      bool listOk = false, pmOk = false, htOk = false, unknownOk = false;
      String error;
      size_t count = 0, summaries = 0;
      nlohmann::json unknownSummaryIds = nlohmann::json::array();
      try
      {
         const nlohmann::json& sums = CompiledProcessSummaries();
         summaries = sums.size();

         const nlohmann::json list = ListProcesses();
         count = list.at( "count" ).get<size_t>();
         bool sawPM = false, sawHT = false;
         std::string prev;
         bool sorted = true;
         for ( const nlohmann::json& row : list.at( "processes" ) )
         {
            const std::string id = row.at( "id" ).get<std::string>();
            sorted = sorted && prev <= id;
            prev = id;
            if ( id == "PixelMath" )
               sawPM = row.value( "summary", std::string() ) == sums.at( "PixelMath" ).get<std::string>();
            if ( id == "HistogramTransformation" )
               sawHT = row.contains( "summary" );
         }
         // Informational: summary keys that name no installed process.
         for ( auto it = sums.begin(); it != sums.end(); ++it )
         {
            bool found = false;
            for ( const nlohmann::json& row : list.at( "processes" ) )
               if ( row.at( "id" ) == it.key() ) { found = true; break; }
            if ( !found )
               unknownSummaryIds.push_back( it.key() );
         }
         listOk = summaries == 25 && count > 50 && count == list.at( "processes" ).size()
               && sorted && sawPM && sawHT;

         const nlohmann::json pm = DescribeProcess( "PixelMath" );
         for ( const nlohmann::json& prm : pm.at( "parameters" ) )
            if ( prm.at( "id" ) == "expression" && prm.at( "type" ) == "String" )
               pmOk = true;
         pmOk = pmOk && pm.value( "summary", std::string() ) == "Arbitrary per-pixel expression evaluation.";

         const nlohmann::json ht = DescribeProcess( "HistogramTransformation" );
         for ( const nlohmann::json& prm : ht.at( "parameters" ) )
            if ( prm.at( "id" ) == "H" && prm.at( "type" ) == "Table" && !prm.value( "columns", nlohmann::json::array() ).empty() )
               htOk = true;

         const nlohmann::json bad = DescribeProcess( "NoSuchProcessXYZ" );
         unknownOk = bad.contains( "error" )
                  && bad.at( "error" ).get<std::string>().rfind( "unknown process id: NoSuchProcessXYZ", 0 ) == 0;
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }

      const bool ok = listOk && pmOk && htOk && unknownOk;
      out["catalogCount"] = count;
      out["catalogSummaries"] = summaries;
      out["catalogUnknownSummaryIds"] = unknownSummaryIds;
      out["catalogListOk"] = listOk;
      out["catalogPixelMathOk"] = pmOk;
      out["catalogHistogramTransformationOk"] = htOk;
      out["catalogUnknownIdOk"] = unknownOk;
      out["catalogError"] = U8( error );
      out["catalogOk"] = ok;
      allOk = allOk && ok;
   }
```
In `run-selftest.sh`, add `'catalogOk',` after `'previewOk',`.

- [ ] **Step 2: Run to verify RED.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc)`
Expected: compile FAIL, with `ProcessCatalog.h: No such file or directory`.

- [ ] **Step 3: Compile the summaries in.** `src/module/ProcessSummaries.h.in`:

```cpp
// GENERATED at CMake configure time from modules/pi-copilot/data/process-summaries.json.
// Do not edit the generated copy; edit the JSON and re-run CMake.
#ifndef PICopilot_ProcessSummaries_h
#define PICopilot_ProcessSummaries_h

namespace pcl
{
static const char kProcessSummariesJson[] = R"PCSUMMARY(@PICOPILOT_PROCESS_SUMMARIES_JSON@)PCSUMMARY";
} // namespace pcl

#endif
```
In `src/module/CMakeLists.txt`, after the `set(MODULE_SOURCES …)` block, add `ProcessCatalog.cpp` to `MODULE_SOURCES`. After `add_library(PICopilot …)`, add:

```cmake
# Process summaries are COMPILED IN (no runtime file dependency). configure_file
# re-runs whenever the JSON changes because CMake tracks file(READ) inputs via
# CMAKE_CONFIGURE_DEPENDS below.
set(PICOPILOT_SUMMARIES_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../data/process-summaries.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PICOPILOT_SUMMARIES_SRC}")
file(READ "${PICOPILOT_SUMMARIES_SRC}" PICOPILOT_PROCESS_SUMMARIES_JSON)
configure_file(ProcessSummaries.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/ProcessSummaries.h" @ONLY)
target_include_directories(PICopilot PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
```

- [ ] **Step 4: Implement ProcessCatalog.** `ProcessCatalog.h`:

```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ProcessCatalog_h
#define PICopilot_ProcessCatalog_h

#include <pcl/String.h>

#include <nlohmann/json.hpp>

namespace pcl
{

constexpr size_type PICopilotMaxProcessDescriptionChars = 2000;

// The compiled-in data/process-summaries.json, parsed once.
const nlohmann::json& CompiledProcessSummaries();

// Grounding for the model (exposed as tools in increment 4). Native
// introspection of the installed process registry (Process::AllProcesses,
// ProcessParameter), merged with the one-line compiled-in summaries.
// Root thread only. Exceptions other than "unknown process id" propagate.
nlohmann::json ListProcesses();
nlohmann::json DescribeProcess( const IsoString& id );

} // namespace pcl

#endif // PICopilot_ProcessCatalog_h
```

`ProcessCatalog.cpp`:

```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ProcessCatalog.h"
#include "ProcessSummaries.h"   // generated: kProcessSummariesJson

#include <pcl/Exception.h>
#include <pcl/Process.h>
#include <pcl/ProcessParameter.h>
#include <pcl/Variant.h>

#include <algorithm>
#include <cfloat>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pcl
{

namespace
{

std::string U8( const String& s )
{
   return std::string( s.ToUTF8().c_str() );
}

nlohmann::json StringListJson( const IsoStringList& list )
{
   nlohmann::json a = nlohmann::json::array();
   for ( const IsoString& s : list )
      a.push_back( std::string( s.c_str() ) );
   return a;
}

const char* TypeName( ProcessParameter::data_type t )
{
   switch ( t )
   {
   case ProcessParameterType::UInt8:       return "UInt8";
   case ProcessParameterType::Int8:        return "Int8";
   case ProcessParameterType::UInt16:      return "UInt16";
   case ProcessParameterType::Int16:       return "Int16";
   case ProcessParameterType::UInt32:      return "UInt32";
   case ProcessParameterType::Int32:       return "Int32";
   case ProcessParameterType::UInt64:      return "UInt64";
   case ProcessParameterType::Int64:       return "Int64";
   case ProcessParameterType::Float:       return "Float";
   case ProcessParameterType::Double:      return "Double";
   case ProcessParameterType::Boolean:     return "Boolean";
   case ProcessParameterType::Enumeration: return "Enumeration";
   case ProcessParameterType::String:      return "String";
   case ProcessParameterType::Block:       return "Block";
   case ProcessParameterType::Table:       return "Table";
   default:                                return "Invalid";
   }
}

nlohmann::json DefaultValueJson( const ProcessParameter& p )
{
   const Variant v = p.DefaultValue();
   if ( !v.IsValid() )
      return nullptr;
   switch ( p.Type() )
   {
   case ProcessParameterType::Boolean:
      return v.ToBoolean();
   case ProcessParameterType::Enumeration:
      {
         const int value = v.ToInt();
         for ( const ProcessParameter::EnumerationElement& e : p.EnumerationElements() )
            if ( e.value == value )
               return std::string( e.id.c_str() );
         return value;
      }
   case ProcessParameterType::String:
      return U8( v.ToString() );
   case ProcessParameterType::Block:
   case ProcessParameterType::Table:
      return nullptr;
   default:
      return p.IsNumeric() ? nlohmann::json( v.ToDouble() ) : nlohmann::json( nullptr );
   }
}

nlohmann::json ParameterJson( const ProcessParameter& p )
{
   nlohmann::json j = {
      { "id", std::string( p.Id().c_str() ) },
      { "type", TypeName( p.Type() ) },
      { "readOnly", p.IsReadOnly() },
      { "required", p.IsRequired() }
   };
   const nlohmann::json def = DefaultValueJson( p );
   if ( !def.is_null() )
      j["default"] = def;
   if ( p.IsNumeric() )
   {
      double lo = 0, hi = 0;
      p.GetNumericRange( lo, hi );
      if ( lo > -DBL_MAX )
         j["min"] = lo;
      if ( hi < DBL_MAX )
         j["max"] = hi;
   }
   if ( p.IsEnumeration() )
   {
      nlohmann::json e = nlohmann::json::array();
      for ( const ProcessParameter::EnumerationElement& el : p.EnumerationElements() )
         e.push_back( { { "id", std::string( el.id.c_str() ) }, { "value", el.value } } );
      j["enumeration"] = e;
   }
   if ( p.IsTable() )
   {
      nlohmann::json cols = nlohmann::json::array();
      for ( const ProcessParameter& col : p.TableColumns() )
         cols.push_back( ParameterJson( col ) );
      j["columns"] = cols;
   }
   return j;
}

} // namespace

const nlohmann::json& CompiledProcessSummaries()
{
   static const nlohmann::json summaries = nlohmann::json::parse( kProcessSummariesJson );
   return summaries;
}

nlohmann::json ListProcesses()
{
   const nlohmann::json& summaries = CompiledProcessSummaries();
   std::vector<std::pair<std::string, nlohmann::json>> rows;
   for ( const Process& P : Process::AllProcesses() )
   {
      const std::string id( P.Id().c_str() );
      nlohmann::json row = { { "id", id }, { "categories", StringListJson( P.Categories() ) } };
      if ( summaries.contains( id ) )
         row["summary"] = summaries.at( id );
      rows.emplace_back( id, std::move( row ) );
   }
   std::sort( rows.begin(), rows.end(),
              []( const auto& a, const auto& b ) { return a.first < b.first; } );
   nlohmann::json list = nlohmann::json::array();
   for ( auto& r : rows )
      list.push_back( std::move( r.second ) );
   nlohmann::json out;
   out["count"] = list.size();
   out["processes"] = std::move( list );
   return out;
}

nlohmann::json DescribeProcess( const IsoString& id )
{
   nlohmann::json out;
   std::unique_ptr<Process> P;
   try
   {
      P.reset( new Process( id ) );   // throws Error for an unknown id (Process.h:92)
   }
   catch ( const pcl::Exception& x )
   {
      out["error"] = "unknown process id: " + std::string( id.c_str() ) + " (" + U8( x.Message() ) + ")";
      return out;
   }

   const std::string sid( P->Id().c_str() );
   out["id"] = sid;
   out["categories"] = StringListJson( P->Categories() );
   const nlohmann::json& summaries = CompiledProcessSummaries();
   if ( summaries.contains( sid ) )
      out["summary"] = summaries.at( sid );
   String description = P->Description();
   if ( description.Length() > PICopilotMaxProcessDescriptionChars )
      description = description.Left( PICopilotMaxProcessDescriptionChars ) + "...";
   out["description"] = U8( description );
   nlohmann::json params = nlohmann::json::array();
   for ( const ProcessParameter& p : P->Parameters() )
      params.push_back( ParameterJson( p ) );
   out["parameters"] = params;
   return out;
}

} // namespace pcl
```

- [ ] **Step 5: Run to verify GREEN.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake -B build -DPCLDIR=$HOME/PCL -DPICOPILOT_BUILD_MODULE=ON && cmake --build build -j$(nproc) && bash test/run-selftest.sh`
Expected: `"catalogOk":true`, `"catalogSummaries":25`, `catalogCount` > 50. Report `catalogUnknownSummaryIds` (informational; an empty list is expected). The last line is `PASS: self-test verdict all green`. If PixelMath's text parameter is not named `expression` or HT's table is not `H`, report the observed parameter ids from a one-off dump. Do not guess new names.

- [ ] **Step 6: Commit.**

```bash
cd /home/scarter4work/projects/astro-pi
git add modules/pi-copilot/src/module/ProcessSummaries.h.in modules/pi-copilot/src/module/ProcessCatalog.h \
        modules/pi-copilot/src/module/ProcessCatalog.cpp modules/pi-copilot/src/module/CMakeLists.txt \
        modules/pi-copilot/src/module/PICopilotVisionSelfTest.cpp modules/pi-copilot/test/run-selftest.sh
git commit -m "feat(pi-copilot): ProcessCatalog — native list/describe process JSON + compiled-in summaries

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Image content blocks + vision turn composition + gated real vision check

**Files:**
- Modify: `modules/pi-copilot/src/module/AnthropicClient.h` (struct `AnthropicMessage`, new free function)
- Modify: `modules/pi-copilot/src/module/AnthropicClient.cpp` (lines 103-129: request body build)
- Create: `modules/pi-copilot/src/module/VisionTurn.h`
- Create: `modules/pi-copilot/src/module/VisionTurn.cpp`
- Modify: `modules/pi-copilot/src/module/PICopilotVisionSelfTest.cpp`
- Modify: `modules/pi-copilot/src/module/CMakeLists.txt`
- Modify: `modules/pi-copilot/test/run-selftest.sh` (key source, timeout, `required_true`, vision report line)

**Interfaces:**
- Consumes: `BuildViewContext` (Task 2), `RenderViewPreview` / `ViewPreviewResult` (Task 3), `AnthropicClient::Send` (increment 2).
- Produces:

```cpp
// AnthropicClient.h
struct AnthropicMessage {
   IsoString role;               // "user" | "assistant"
   String    content;
   IsoString imageJpegBase64;    // optional; non-empty => content-block array [image, text]
};
// Throws std::exception on a JSON build failure.
std::string BuildMessagesRequestBody( const IsoString& model, const String& systemPrompt,
                                      const Array<AnthropicMessage>& history );

// VisionTurn.h
extern const char* const kPICopilotImageOmittedNote;
AnthropicMessage ComposeUserTurn( const String& userText, const nlohmann::json* viewContext,
                                  const IsoString& jpegBase64 );
void StripOlderImages( Array<AnthropicMessage>& history );   // all but the LAST message
```

- [ ] **Step 1: Write the failing tests.** In `PICopilotVisionSelfTest.cpp`, add `#include "AnthropicClient.h"`, `#include "VisionTurn.h"` and `#include <cstdlib>`. Insert above `// ---- inc3 sections end ----`:

```cpp
   // ---- Section 5: request shape + history stripping (Task 5, no network) --
   {
      bool shapeOk = false, stripOk = false, composeOk = false;
      String error;
      try
      {
         AnthropicMessage img;
         img.role = "user";
         img.content = "look";
         img.imageJpegBase64 = "QUJD";
         Array<AnthropicMessage> h;
         h.Add( AnthropicMessage{ IsoString( "user" ), String( "plain" ), IsoString() } );
         h.Add( AnthropicMessage{ IsoString( "assistant" ), String( "ok" ), IsoString() } );
         h.Add( img );
         const nlohmann::json j = nlohmann::json::parse( BuildMessagesRequestBody( PICOPILOT_DEFAULT_MODEL, "sys", h ) );
         const nlohmann::json& m = j.at( "messages" );
         shapeOk = !j.contains( "stream" )
                && m[0].at( "content" ).is_string() && m[0].at( "content" ) == "plain"
                && m[1].at( "content" ) == "ok"
                && m[2].at( "content" ).is_array() && m[2].at( "content" ).size() == 2
                && m[2]["content"][0].at( "type" ) == "image"
                && m[2]["content"][0].at( "source" ).at( "type" ) == "base64"
                && m[2]["content"][0].at( "source" ).at( "media_type" ) == "image/jpeg"
                && m[2]["content"][0].at( "source" ).at( "data" ) == "QUJD"
                && m[2]["content"][1].at( "type" ) == "text"
                && m[2]["content"][1].at( "text" ) == "look";

         const nlohmann::json ctx = { { "viewId", "V" } };
         const AnthropicMessage t = ComposeUserTurn( "what is this?", &ctx, "QUJD" );
         const AnthropicMessage plain = ComposeUserTurn( "hi", nullptr, IsoString() );
         composeOk = t.role == "user" && t.imageJpegBase64 == "QUJD"
                  && t.content.StartsWith( String( "[PixInsight view context]" ) )
                  && t.content.EndsWith( String( "what is this?" ) )
                  && t.content.Contains( String( "\"viewId\":\"V\"" ) )
                  && plain.content == "hi" && plain.imageJpegBase64.IsEmpty();

         Array<AnthropicMessage> hist;
         hist.Add( t );
         hist.Add( AnthropicMessage{ IsoString( "assistant" ), String( "an image" ), IsoString() } );
         hist.Add( t );
         StripOlderImages( hist );
         StripOlderImages( hist );   // idempotent
         const String note = String::UTF8ToUTF16( kPICopilotImageOmittedNote );
         stripOk = hist[0].imageJpegBase64.IsEmpty()
                && hist[0].content.StartsWith( note )
                && hist[0].content.Find( note, note.Length() ) == String::notFound   // not doubled
                && hist[0].content.EndsWith( String( "what is this?" ) )
                && hist[1].content == "an image"
                && hist[2].imageJpegBase64 == "QUJD";
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }
      const bool ok = shapeOk && composeOk && stripOk;
      out["requestShapeOk"] = shapeOk;
      out["composeTurnOk"] = composeOk;
      out["historyStripOk"] = stripOk;
      out["turnError"] = U8( error );
      out["visionTurnOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section 6: gated REAL vision round-trip (Task 5) -------------------
   // Runs only when PICOPILOT_TEST_API_KEY is set (harness: keyring -> file).
   {
      bool visionSkipped = true, visionOk = true;
      String answer, error;
      if ( const char* key = std::getenv( "PICOPILOT_TEST_API_KEY" ) )
      {
         visionSkipped = false;
         visionOk = false;
         try
         {
            WindowCloser wc{ CreateSyntheticWindow() };
            const View view = wc.window.MainView();
            const nlohmann::json ctx = BuildViewContext( view );
            const ViewPreviewResult p = RenderViewPreview( view );
            if ( !p.ok )
               error = "preview failed: " + p.error;
            else
            {
               AnthropicClient client{ String( key ) };
               const AnthropicResult r = client.Send( "You are a test. Answer with exactly one word.",
                  { ComposeUserTurn( "What colour is the square in this image? Answer with exactly one word.",
                                     &ctx, p.base64 ) } );
               answer = r.text;
               error = r.error;
               visionOk = r.ok && r.text.ContainsIC( String( "red" ) );
            }
         }
         catch ( const pcl::Exception& x ) { error = x.Message(); }
         catch ( const std::exception& x ) { error = String( x.what() ); }
         catch ( ... )                     { error = "unknown exception"; }
      }
      out["visionSkipped"] = visionSkipped;
      out["visionAnswer"] = U8( answer );
      out["visionError"] = U8( error );
      out["visionOk"] = visionOk;
      allOk = allOk && visionOk;
   }
```
In `run-selftest.sh`:
1. Add `'visionTurnOk', 'visionOk',` after `'catalogOk',` in `required_true`. After the `anthropic check:` print, add:
```python
print('vision check: %s' % ('SKIPPED (no key)' if d.get('visionSkipped') else 'RAN against real API, answer=%r' % d.get('visionAnswer')))
```
2. Replace the key block (lines 26-34) with (keyring first, then file, never echo the key):
```bash
# Gated real-API checks (text + vision). Key source order: system keyring,
# then the gitignored local file, else skip. The key is never printed.
KEYFILE="$ROOT/test/.test_api_key"
if command -v secret-tool >/dev/null 2>&1 \
   && KR_KEY="$(secret-tool lookup service anthropic account default 2>/dev/null)" && [ -n "$KR_KEY" ]; then
   export PICOPILOT_TEST_API_KEY="$KR_KEY"
   echo "using API key from the system keyring (secret-tool); real-API checks will run"
elif [ -f "$KEYFILE" ]; then
   export PICOPILOT_TEST_API_KEY="$(cat "$KEYFILE")"
   echo "using local test API key file: $KEYFILE; real-API checks will run"
else
   echo "no API key (keyring or $KEYFILE); real-API checks will be skipped"
fi
unset KR_KEY
```
3. Change `timeout 180` to `timeout 300` and the matching message to `(300s)`. The vision call adds up to about 30 s on top of path 5's bounded 150 s wait.

Add `VisionTurn.cpp` to `MODULE_SOURCES`.

- [ ] **Step 2: Run to verify RED.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc)`
Expected: compile FAIL: `VisionTurn.h: No such file or directory` / `'BuildMessagesRequestBody' was not declared` / `'struct pcl::AnthropicMessage' has no member named 'imageJpegBase64'`.

- [ ] **Step 3: Extend AnthropicMessage + the request builder.** In `AnthropicClient.h`, replace the struct (lines 30-35) and add the declaration below it. Also add `#include <string>`:

```cpp
// One turn of chat history sent to the Anthropic Messages API. A message
// with a non-empty imageJpegBase64 is sent as a content-block array
// [image(base64 JPEG), text]; every other message stays a plain string.
struct AnthropicMessage
{
   IsoString role;               // "user" | "assistant"
   String    content;
   IsoString imageJpegBase64;    // optional, standard Base64, no data: prefix
};

// The Messages API request body (UTF-8 JSON, non-streamed: no "stream" key).
// Pure function, any thread. Throws std::exception if JSON building fails.
std::string BuildMessagesRequestBody( const IsoString& model, const String& systemPrompt,
                                      const Array<AnthropicMessage>& history );
```
In `AnthropicClient.cpp`, add this above `struct AnthropicRequest::Impl`, then replace the body-building `try` block in the constructor (lines 111-129):

```cpp
namespace
{

nlohmann::json MessageContent( const AnthropicMessage& msg )
{
   const std::string text( msg.content.ToUTF8().c_str() );
   if ( msg.imageJpegBase64.IsEmpty() )
      return text;
   nlohmann::json image = {
      { "type", "image" },
      { "source", { { "type", "base64" },
                    { "media_type", "image/jpeg" },
                    { "data", std::string( msg.imageJpegBase64.c_str() ) } } }
   };
   nlohmann::json textBlock = { { "type", "text" }, { "text", text } };
   nlohmann::json blocks = nlohmann::json::array();
   blocks.push_back( std::move( image ) );      // image first, then the question
   blocks.push_back( std::move( textBlock ) );
   return blocks;
}

} // namespace

std::string BuildMessagesRequestBody( const IsoString& model, const String& systemPrompt,
                                      const Array<AnthropicMessage>& history )
{
   nlohmann::json messages = nlohmann::json::array();
   for ( const AnthropicMessage& msg : history )
      messages.push_back( { { "role", msg.role.c_str() }, { "content", MessageContent( msg ) } } );
   nlohmann::json req = {
      { "model", model.c_str() },
      { "max_tokens", 4096 },
      { "system", systemPrompt.ToUTF8().c_str() },
      { "messages", messages }
   };
   return req.dump();
}
```
```cpp
   try
   {
      m->body = String::UTF8ToUTF16( BuildMessagesRequestBody( model, systemPrompt, history ).c_str() );
   }
   catch ( const std::exception& x )
   {
      m->buildError = String( "failed to build request: " ) + String( x.what() );
      return;
   }
```
The two existing aggregate initializations in `PICopilotInterface.cpp` (`AnthropicMessage{ IsoString( "user" ), prompt }` / `{ IsoString( "assistant" ), r.text }`) and those in `PICopilotSelfTest.cpp` stay valid, because the new member value-initializes. If `-Wmissing-field-initializers` warns, add `, IsoString()` as the third initializer at each site; do not suppress the warning.

- [ ] **Step 4: Implement VisionTurn.** `VisionTurn.h`:

```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_VisionTurn_h
#define PICopilot_VisionTurn_h

#include "AnthropicClient.h"

#include <nlohmann/json.hpp>

namespace pcl
{

// Replaces an older turn's image in re-sent history (token cost): only the
// latest user turn carries pixels. UTF-8.
extern const char* const kPICopilotImageOmittedNote;

// One user turn. With a view context, the text is
//   "[PixInsight view context]\n<compact JSON>\n[/PixInsight view context]\n\n<userText>"
// otherwise just userText. jpegBase64 may be empty (no image block).
AnthropicMessage ComposeUserTurn( const String& userText, const nlohmann::json* viewContext,
                                  const IsoString& jpegBase64 );

// In place: every message except the LAST loses its image, and each message
// that loses one gets kPICopilotImageOmittedNote prepended to its text.
// Idempotent (a message without an image is never touched).
void StripOlderImages( Array<AnthropicMessage>& history );

} // namespace pcl

#endif // PICopilot_VisionTurn_h
```

`VisionTurn.cpp`:

```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "VisionTurn.h"

namespace pcl
{

const char* const kPICopilotImageOmittedNote =
   "[An auto-stretched preview of the view was attached to this message when it was sent; "
   "it is omitted from the re-sent history.]\n";

AnthropicMessage ComposeUserTurn( const String& userText, const nlohmann::json* viewContext,
                                  const IsoString& jpegBase64 )
{
   AnthropicMessage m;
   m.role = "user";
   if ( viewContext != nullptr )
      m.content = "[PixInsight view context]\n"
                + String::UTF8ToUTF16( viewContext->dump().c_str() )
                + "\n[/PixInsight view context]\n\n"
                + userText;
   else
      m.content = userText;
   m.imageJpegBase64 = jpegBase64;
   return m;
}

void StripOlderImages( Array<AnthropicMessage>& history )
{
   if ( history.Length() < 2 )
      return;
   const String note = String::UTF8ToUTF16( kPICopilotImageOmittedNote );
   for ( size_type i = 0; i + 1 < history.Length(); ++i )
      if ( !history[i].imageJpegBase64.IsEmpty() )
      {
         history[i].imageJpegBase64.Clear();
         history[i].content = note + history[i].content;
      }
}

} // namespace pcl
```

- [ ] **Step 5: Run to verify GREEN (with the real vision call).**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) && bash test/run-selftest.sh`
Expected: `using API key from the system keyring (secret-tool)…` (or the key-file line), `"visionTurnOk":true`, `"visionOk":true`, `"visionSkipped":false`, and `vision check: RAN against real API, answer='Red'` (any casing, possibly with punctuation). The last line is `PASS: self-test verdict all green`. If no key is available on this machine, the output shows `vision check: SKIPPED (no key)`. **Report that explicitly**: the real round-trip is then unproven, and the controller must supply a key before Task 7.

- [ ] **Step 6: Commit.**

```bash
cd /home/scarter4work/projects/astro-pi
git add modules/pi-copilot/src/module/AnthropicClient.h modules/pi-copilot/src/module/AnthropicClient.cpp \
        modules/pi-copilot/src/module/VisionTurn.h modules/pi-copilot/src/module/VisionTurn.cpp \
        modules/pi-copilot/src/module/PICopilotVisionSelfTest.cpp modules/pi-copilot/src/module/CMakeLists.txt \
        modules/pi-copilot/test/run-selftest.sh
git commit -m "feat(pi-copilot): image content blocks + vision turn composition; gated real vision round-trip

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: Panel — "Include view" capture on Send, system prompt, right-edge default placement (GUI-only)

**Files:**
- Modify: `modules/pi-copilot/src/module/PICopilotInterface.h`
- Modify: `modules/pi-copilot/src/module/PICopilotInterface.cpp`

**Interfaces:**
- Consumes: `BuildViewContext` (Task 2), `RenderViewPreview` (Task 3), `ComposeUserTurn` / `StripOlderImages` (Task 5).
- Produces (private to the panel): `AnthropicMessage ComposeTurnWithActiveView( const String& prompt )`, `void e_Show( Control& )`, `CheckBox IncludeView_CheckBox`.

- [ ] **Step 1: Header changes.** In `PICopilotInterface.h`, add `#include <pcl/CheckBox.h>`. In the private section, below `void SetBusy( bool busy );`, add:
```cpp
   // UI thread only: captures the active view's context + preview (when
   // "Include view" is checked) and returns the composed user turn. Every
   // capture problem is written to the chat log; the text always sends.
   AnthropicMessage ComposeTurnWithActiveView( const String& prompt );

   // One-time default placement: flush right, full height (see e_Show).
   void ApplyDefaultPlacement();
```
In `GUIData`, add `CheckBox IncludeView_CheckBox;` after `ComboBox Mode_ComboBox;`. In the event handlers, add `void e_Show( Control& sender );`.

- [ ] **Step 2: System prompt (H).** Replace `kSystemPrompt` in `PICopilotInterface.cpp` with (still ONE named constant; the mode selector stays inert):
```cpp
// The single system prompt for every chat turn.
const char* const kSystemPrompt =
   "You are PI Copilot, an assistant embedded in PixInsight, the astronomical "
   "image processing application. Help the user plan and understand their "
   "PixInsight workflow: processes, scripts, parameters and processing order "
   "for their astrophotography data. Be concise and concrete.\n\n"
   "A user message may begin with a [PixInsight view context] block (JSON: view "
   "identity, geometry, per-channel statistics, FITS keywords) and may include an "
   "image. The image is an automatically stretched (auto-STF) JPEG preview, "
   "downscaled to at most 1024 px, for DISPLAY ONLY: the underlying data is usually "
   "still LINEAR (unstretched). Base any statement about the data's levels, noise or "
   "clipping on the statistics in the context block, which describe the real data "
   "(mad is the raw median absolute deviation; multiply by 1.4826 for sigma). Only "
   "the latest message carries an image; earlier images are omitted from history.";
```

- [ ] **Step 3: Capture on Send.** Add to `PICopilotInterface.cpp`: `#include "ViewContext.h"`, `#include "ViewPreview.h"`, `#include "VisionTurn.h"`, `#include <pcl/Console.h>`, `#include <pcl/GlobalSettings.h>`, `#include <pcl/ImageWindow.h>`, `#include <pcl/Settings.h>`. Add the method:
```cpp
AnthropicMessage PICopilotInterface::ComposeTurnWithActiveView( const String& prompt )
{
   if ( !GUI->IncludeView_CheckBox.IsChecked() )
      return ComposeUserTurn( prompt, nullptr, IsoString() );

   ImageWindow window = ImageWindow::ActiveWindow();
   if ( window.IsNull() )
   {
      AppendToLog( PlainText( String::UTF8ToUTF16( "(no active image \xE2\x80\x94 sending text only)" ) ) + "\n\n" );
      return ComposeUserTurn( prompt, nullptr, IsoString() );
   }

   const View view = window.CurrentView();   // may be a preview
   nlohmann::json ctx;
   bool haveCtx = false;
   try
   {
      ctx = BuildViewContext( view );
      haveCtx = true;
   }
   catch ( const pcl::Exception& x )
   {
      AppendToLog( PlainText( "View context failed: " + x.Message() ) + "\n\n" );
   }
   catch ( const std::exception& x )
   {
      AppendToLog( PlainText( String( "View context failed: " ) + String( x.what() ) ) + "\n\n" );
   }

   const ViewPreviewResult p = RenderViewPreview( view );
   if ( p.ok )
      AppendToLog( PlainText( "(attached " + String( view.FullId() ) + ": auto-stretched preview "
                              + String( p.width ) + "x" + String( p.height ) + ", "
                              + String( int( p.jpegBytes/1024 ) ) + " KiB)" ) + "\n\n" );
   else
      AppendToLog( PlainText( String::UTF8ToUTF16( "Preview failed: " ) + p.error
                              + String::UTF8ToUTF16( " \xE2\x80\x94 sending without an image" ) ) + "\n\n" );

   return ComposeUserTurn( prompt, haveCtx ? &ctx : nullptr, p.ok ? p.base64 : IsoString() );
}
```
In `SendCurrentInput()`, replace the line `m_history.Add( AnthropicMessage{ IsoString( "user" ), prompt } );` with:
```cpp
   // Root thread, BEFORE the worker starts: ImageWindow/View/Bitmap are
   // UIObjects. Older turns lose their images (token cost) before the new
   // turn -- the only one carrying pixels -- is appended.
   AnthropicMessage turn = ComposeTurnWithActiveView( prompt );
   StripOlderImages( m_history );
   m_history.Add( turn );
```
(The rest of `SendCurrentInput` is unchanged. It still logs `You:` first, stores `m_pendingPrompt = prompt` (the bare prompt, for resend), and builds `ChatThread` from `m_history`.)

- [ ] **Step 4: Checkbox + layout.** In `GUIData::GUIData`, after the `Mode_ComboBox` setup:
```cpp
   IncludeView_CheckBox.SetText( "Include view" );
   IncludeView_CheckBox.SetChecked( true );
   IncludeView_CheckBox.SetToolTip( "<p>Send the active view with each message: an auto-stretched "
                                    "preview (display only) plus its geometry, statistics and FITS keywords.</p>" );
```
In the `Top_Sizer` block, add `Top_Sizer.Add( IncludeView_CheckBox );` right after `Top_Sizer.Add( Mode_ComboBox );`. Change `ChatLog.SetScaledMinSize( 500, 300 );` to `ChatLog.SetScaledMinSize( 360, 200 );`, so the 420-logical-px default width is actually reachable.

- [ ] **Step 5: Right-edge default placement (G).** PCL has no docking API (no "dock" member in any `pcl/*.h`; `ProcessInterface::Launch` has no dock-area parameter). The only screen-geometry source in the SDK headers is `PixInsightSettings::GlobalInteger( "Workspace/PrimaryScreenCenterX" / "…CenterY" )` (`GlobalSettings.h:233-234`, physical device pixels, read-only). There is no available-area / taskbar query, so this assumes the primary screen's origin is (0,0). `Control::Move` / `Resize` take physical pixels (`SetScaledMinSize` converts logical→physical via `LogicalPixelsToPhysical`, `Control.h:464`).

Use a one-time **marker setting**, not "no saved geometry". The user already has saved floating geometry from 0.1.0.2 (`Interfaces/PICopilot/Geometry` = 1661,873 516×369), so a "first launch" test would never fire for them. After the one-time placement, PI's auto-save geometry (on by default, `ProcessInterface.h:2568`) remembers the user's own moves. Add to the anonymous namespace:
```cpp
// One-time default placement (flush right, full height). Logical px.
const char* const kPlacementMarkerKey = "PICopilot/DefaultPlacementApplied";
constexpr int kDefaultPanelWidth  = 420;
constexpr int kDefaultTopMargin   = 40;   // below the main menu/title bar
constexpr int kDefaultBottomMargin = 60;  // above a desktop taskbar
constexpr int kDefaultRightMargin = 8;
```
Add the methods:
```cpp
void PICopilotInterface::ApplyDefaultPlacement()
{
   if ( !PixInsightSettings::IsGlobalVariableDefined( "Workspace/PrimaryScreenCenterX" )
     || !PixInsightSettings::IsGlobalVariableDefined( "Workspace/PrimaryScreenCenterY" ) )
   {
      Console().WarningLn( "PI Copilot: primary-screen geometry unavailable; default right-side placement skipped." );
      return;
   }
   const int screenW = 2*PixInsightSettings::GlobalInteger( "Workspace/PrimaryScreenCenterX" );
   const int screenH = 2*PixInsightSettings::GlobalInteger( "Workspace/PrimaryScreenCenterY" );
   const int w      = LogicalPixelsToPhysical( kDefaultPanelWidth );
   const int top    = LogicalPixelsToPhysical( kDefaultTopMargin );
   const int bottom = LogicalPixelsToPhysical( kDefaultBottomMargin );
   const int right  = LogicalPixelsToPhysical( kDefaultRightMargin );
   Resize( w, screenH - top - bottom );
   Move( screenW - w - right, top );
}

void PICopilotInterface::e_Show( Control& )
{
   // OnShow fires after the core has restored any saved geometry, so this
   // one-time placement wins exactly once; afterwards the user's own
   // moves/resizes are remembered by PI's auto-save geometry.
   bool applied = false;
   Settings::Read( kPlacementMarkerKey, applied );
   if ( applied )
      return;
   Settings::Write( kPlacementMarkerKey, true );
   ApplyDefaultPlacement();
}
```
In `Launch()`, inside `if ( GUI == nullptr ) { … }` after `SetWindowTitle( "PI Copilot" );`, add:
```cpp
      OnShow( (Control::event_handler)&PICopilotInterface::e_Show, *this );
```

- [ ] **Step 6: Build + regression.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) 2>&1 | grep -E "warning|error" ; bash test/run-selftest.sh`
Expected: no new warnings or errors from `PICopilotInterface.cpp`, and `PASS: self-test verdict all green`. **No headless test covers this task.** The checkbox, capture-on-Send, the no-image note and the placement are GUI-only and are verified by the user in Task 7. In the task report, name the screen-geometry API used (`PixInsightSettings::GlobalInteger("Workspace/PrimaryScreenCenterX/Y")`) and state the primary-screen-at-origin assumption.

- [ ] **Step 7: Commit.**

```bash
cd /home/scarter4work/projects/astro-pi
git add modules/pi-copilot/src/module/PICopilotInterface.h modules/pi-copilot/src/module/PICopilotInterface.cpp
git commit -m "feat(pi-copilot): panel sends the active view (Include view), vision-aware system prompt, right-edge default placement

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

## Task 7: Release 0.1.0.3 via the repository + user verification handoff

**Files:**
- Modify: `modules/pi-copilot/src/module/PICopilotVersion.h` (BUILD 2 → 3)
- Modify: `modules/pi-copilot/src/module/PICopilotModule.cpp` (`GetReleaseDate`, `Description()`)
- Modify: `modules/pi-copilot/README.md`
- Regenerated by `./release.sh`: `repository/` (+ any re-signed script `.xsgn` files it touches)

**Interfaces:**
- Consumes: everything above. Produces: signed PICopilot 0.1.0.3 served from `https://raw.githubusercontent.com/scarter4work/astro-pi/main/repository/`.

- [ ] **Step 1: Version + date + description.** In `PICopilotVersion.h`: `#define PICOPILOT_MODULE_VERSION_BUILD     3`. In `PICopilotModule.cpp` `GetReleaseDate`, set year/month/day to **the day you run `release.sh`** (`date +%F`). Replace the `Description()` body's second string with `"Chat that sees the active view (auto-stretched preview + view statistics and FITS keywords)."`.

- [ ] **Step 2: README.** In `modules/pi-copilot/README.md`, add this section after "Increment 2 — Text Chat":
```markdown
## Increment 3 — Vision (the panel sees the active view)

- **Include view** (checkbox, on by default): each message you send carries the active view — an auto-stretched JPEG preview (long edge ≤ 1024 px, quality 85; display only) plus a JSON context: view id, file path, geometry, per-channel median / raw MAD / mean / min / max of the **real (usually linear) data**, and the first 60 FITS keywords (values cut at 80 characters). There is no process-history field: PCL has no API for it.
- The preview is made from a **copy** of the view (downscale → auto-STF → JPEG via a temp file that is always deleted); your image is never modified. The self-test proves it byte-identical before/after.
- **No active image** → the message is sent as text with a visible note. A context or preview failure is shown in the chat log and the text still sends.
- **Token cost**: only the latest message carries an image; older turns keep a one-line note instead of the picture.
- **Placement**: on first open after this update the panel is placed flush right, full height; after that, PixInsight remembers wherever you move it.
- **Process catalog** (`list_processes` / `describe_process` JSON from native introspection + compiled-in summaries) is built and self-tested; it becomes a model tool in increment 4.
- Self-test additions: vision smoke (ImageWindow/Bitmap/JPEG headless), ViewContext values, ViewPreview (JPEG, size, unchanged image, temp removed, red square decodes red), ProcessCatalog, request content-block shape, history stripping, and a gated **real vision call** (synthetic red square → model answers "red"). Key source: system keyring (`secret-tool lookup service anthropic account default`), then `test/.test_api_key`, else skipped.
```
Under "## Verified", add the line `**<release date>** — headless self-test PASS incl. real vision round-trip (answer: <visionAnswer from the run>). GUI: pending user verification (0.1.0.3 via repository pull).`

- [ ] **Step 3: Final self-test on the release build.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) && bash test/run-selftest.sh`
Expected: `vision check: RAN against real API, answer=…red…` and `PASS: self-test verdict all green`. **A SKIPPED vision check blocks the release.** Get a key first.

- [ ] **Step 4: Release.**

```bash
cd /home/scarter4work/projects/astro-pi
test -s /tmp/.pi_codesign_pass && stat -c '%a' /tmp/.pi_codesign_pass   # expect 600
./release.sh
```
Expected: every stage `== … ==` runs, including `== 1b/6 build PICopilot module ==`, `== 2a/6 sign PICopilot module ==`, `== 3a/6 package PICopilot module tarball ==`, and the final `== 6/6 integrity check: declared sha1 == on-disk ==` with no failure. `repository/<YYYYMMDD>-linux-x64-PICopilot.tar.gz` exists with today's date.

- [ ] **Step 5: Commit version + artifacts together, merge, push.**

```bash
cd /home/scarter4work/projects/astro-pi
git status --short        # review: version files, README, repository/, re-signed .xsgn only
git add modules/pi-copilot/src/module/PICopilotVersion.h modules/pi-copilot/src/module/PICopilotModule.cpp \
        modules/pi-copilot/README.md repository/
git add -u                # re-signed script .xsgn files release.sh touched (tracked files only)
git commit -m "release(pi-copilot): ship PICopilot 0.1.0.3 (increment-3 vision) via repository

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
git checkout main && git pull --ff-only && git merge --no-ff feat/pi-copilot-inc3 -m "Merge feat/pi-copilot-inc3: PI Copilot increment 3 (vision)

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
git push origin main
shred -u /tmp/.pi_codesign_pass
```
Expected: the push succeeds, and `/tmp/.pi_codesign_pass` no longer exists.

- [ ] **Step 6: Verify the served manifest + tarball.**

```bash
cd /home/scarter4work/projects/astro-pi
TGZ=$(grep -o 'fileName="[^"]*-linux-x64-PICopilot\.tar\.gz"' repository/updates.xri | cut -d'"' -f2)
LOCAL=$(sha1sum "repository/$TGZ" | cut -d' ' -f1)
BASE=https://raw.githubusercontent.com/scarter4work/astro-pi/main/repository
timeout 600 bash -c "until curl -fsSL '$BASE/updates.xri' | grep -q '$LOCAL'; do sleep 15; done" \
  && echo "manifest serves sha1 $LOCAL" \
  && [ "$(curl -fsSL "$BASE/$TGZ" | sha1sum | cut -d' ' -f1)" = "$LOCAL" ] && echo "tarball sha1 matches"
```
Expected: `manifest serves sha1 <40 hex>` then `tarball sha1 matches`. (The raw CDN can lag a few minutes. If this harness blocks `sleep`, poll with the Monitor tool using the same until-condition.)

- [ ] **Step 7: User verification handoff (USER, in PixInsight — repo pull only; never a local `-m=` load).** Give the user this checklist and record their answers in the README "Verified" line (replace "pending user verification"):
  1. *Resources ▸ Updates ▸ Check for Updates*, install PICopilot **0.1.0.3**, restart PixInsight. Open *Process ▸ Etc ▸ PICopilot*.
  2. **Placement:** does the panel open flush against the right edge, full height? Then move/resize it, close it and reopen it: does it keep your placement?
  3. **Docking probe:** drag the panel onto PixInsight's right dock area. Does PI dock it? (This tells us whether the core docks process interfaces at all; PCL has no docking API.)
  4. **Include view ON, image open:** ask "What object is this? Describe the image." The log shows `(attached <view>: auto-stretched preview WxH, N KiB)`, and the answer should match the image.
  5. **Include view OFF:** ask the same question. There is no attach line, and the model should say it cannot see an image.
  6. **No image open (Include view ON):** send any message. The log shows `(no active image — sending text only)`, and a reply arrives.
  7. **Follow-up turn** after (4): ask "what processing would you do next?". The reply arrives (history re-sent with the image placeholder).

---

## Self-Review

- **Spec coverage:** `get_view_context` → Task 2 (`BuildViewContext`). ViewPreview → Task 3. Grounding `list_processes` / `describe_process` + the §11-4 summaries seed → Task 4 (compiled in, native introspection, which supersedes `dump-process-catalog.js`). §7 vision data flow (image content block) → Task 5. Panel / root-thread capture (§5) → Task 6. Packaging/release (§9) → Task 7. The process-history field is explicitly omitted (no PCL API; see Corrections). The tool loop is deferred to increment 4 by ruling A.
- **Rulings:** A (capture on the UI thread at Send; no-image note; visible capture errors, text still sends) → T6 S3. B (latest image only; placeholder; FITS 60/80 constants) → T2 + T5 `StripOlderImages` + T6 S3. C (`imageJpegBase64`; arrays only with an image) → T5 S3. D (units, MAD raw and labelled, no history, byte-identical image) → T2/T3/T4. E (smoke first, fallback, BLOCKED) → T1. F (synthetic image, all asserts, gated "red", key order, never echo) → T1-T5. G (right edge, full height, remembered, API reported) → T6 S5. H (system prompt, one constant, inert modes) → T6 S2. I (bump, README, release, sha1 verify, user checklist) → T7. J → 7 tasks, with scaffolding folded in.
- **Placeholder scan:** none. The one deliberate indirection is the signing password, which is referenced to `~/.claude/CLAUDE.md` so it never lands in this committed file.
- **Type consistency:** `ViewPreviewResult{ok,error,base64,width,height,jpegBytes,tempPath}`, `BuildViewContext(const View&)`, `RenderViewPreview(const View&)`, `ComposeUserTurn(const String&, const nlohmann::json*, const IsoString&)`, `StripOlderImages(Array<AnthropicMessage>&)`, `BuildMessagesRequestBody(const IsoString&, const String&, const Array<AnthropicMessage>&)`, and `kPICopilotImageOmittedNote` (UTF-8 `const char*`, converted with `UTF8ToUTF16` at both use sites) are used identically in T5/T6. The verdict keys added to `required_true` (`visionSmokeOk`, `viewContextOk`, `previewOk`, `catalogOk`, `visionTurnOk`, `visionOk`) match the `out[...]` keys set in each section.
- **Unverified (flagged in the tasks, not hidden):** headless ImageWindow/Bitmap/JPEG (T1 decides, BLOCKED path defined); the `"quality 85"` hint for the FileFormat fallback; whether `OnShow` fires after the core's geometry restore (GUI-verified in T7 item 2); PixelMath/HT parameter ids `expression`/`H` (T4 S5 says to report, not guess).
