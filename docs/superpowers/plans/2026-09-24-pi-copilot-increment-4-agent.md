# PI Copilot — Increment 4: Agent loop + apply_process — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The copilot can act. From the chat panel, the model applies PixInsight processes, with parameters it chooses, to the active view. It uses Anthropic tool calling (`list_processes`, `describe_process`, `get_view_context`, `apply_process`), and the mode selector becomes real: **Copilot** acts directly (undoable from History), **Guided** asks before every change, and **Advisor** is read-only.

**Architecture:** There are four new headless-testable units.
- `ProcessApply` sets parameters on a *default* `ProcessInstance` (type-, range- and enum-checked, with a read-back of every value) and runs it on a `View`, with a precise error for every failure.
- `AgentTools` holds the tool schemas per mode and the root-thread dispatcher, including the Guided confirm callback.
- `SystemPrompt` builds the per-mode prompt, including the binding tone rules.
- `AgentSession` is the tool loop. It owns the history, runs every `tool_use` of a round in order, appends the `tool_use`/`tool_result` pair atomically, and enforces the 12-round cap, Stop and failure rollback. It also validates the history before any send.

The transport gains `tools`, verbatim content blocks, `stop_reason` and a testable `ParseMessagesResponse()`. The panel drives the loop from its existing Timer: the worker thread does HTTP only, and every tool runs on the UI thread.

**Tech Stack:** C++17, PCL SDK (`$HOME/PCL`, sources `/opt/PixInsight/src/pcl/`), nlohmann/json 3.11.3 (already linked), PixInsight 1.9.5 headless harness (`--automation-mode`, isolated slot 90), `release.sh`.

**Spec:** `docs/superpowers/specs/2026-09-20-pi-copilot-native-pcl-design.md`, §3 (hybrid execution: the `apply_process` row), §4 (`AgentSession`, `SystemPrompt`), §5 (threading loop), §8 (errors) and §12 item 4. `run_pjsr` is increment 5 and is **out of scope**. This plan builds on main @ `d033dc1` (PICopilot 0.1.0.4).

**API facts verified for this plan (headers + PCL sources + a headless PJSR probe, 2026-09-24):**
- `ProcessInstance( const Process& )` creates an instance with the process's **default** parameters (`ProcessInstance.cpp:46`).
- `SetParameterValue( const Variant&, const ProcessParameter&, size_type row = ~0 )` **throws** for a table parameter. An enum is sent as `api_enum( value.ToInt() )`, and `false` means rejected (`ProcessInstance.cpp:438-511`).
- `ParameterValue( p, row )` returns an invalid Variant on error. Tables use `AllocateTableRows( table, n )`, which **reallocates the whole table**, and then per-cell `SetParameterValue( v, column, row )`.
- `bool Validate( String& whyNot )` is non-const. `CanExecuteOn( const View&, String& )`. `ExecuteOn( View&, bool swapFile = true )`: with `swapFile=true` the run is written to the view's History and is undoable (`ProcessInstance.h:204-234`).
- `ProcessParameter( const Process&, const IsoString& id )` accepts aliases and **throws** for an unknown id (`ProcessParameter.h:195-216`). `EnumerationElement{ IsoString id; IsoStringList aliases; int value; }`. `EnumerationElements()` can throw `GetParameterElementIdentifier(): API function error` (the known PixelMath `newImageColorSpace` case).
- `ProcessParameter::DefaultValue()` returns the default element **INDEX** for an enumeration, not its value (`ProcessParameter.cpp:333-338`). `ProcessCatalog`'s `DefaultValueJson()` currently compares it against `e.value`, which is a latent bug that Task 2 fixes.
- `Process::CanProcessViews()` / `CanProcessGlobal()` exist (`Process.h:177-181`). A global-only process is `!CanProcessViews()`.
- Probe (headless PJSR, slot 91): SCNR `colorToRemove` elements Red=0, Green=1, Blue=2, default 1. `protectionMethod` AverageNeutral=2. HistogramTransformation `H` default is 5 rows of `[0,0.5,1,0,1]`. PixelMath `expression` is a String and `useSingleExpression` defaults to true.

## Global Constraints

- The module ID string is `"PICopilot"` and is STABLE. Never rename it. The process id, interface id and Settings prefix `PICopilot/` all stay as they are.
- C++17. Flags are exactly `-fPIC -fvisibility=hidden -fvisibility-inlines-hidden`. Defines are exactly `__PCL_LINUX __PCL_BUILDING_MODULE _REENTRANT`. Output is `PICopilot-pxm.so` (+ `PICopilot-pxm.xsgn`). Every new `.cpp` goes into `MODULE_SOURCES` in `modules/pi-copilot/src/module/CMakeLists.txt`.
- **Root-thread rules (hard):** `ImageWindow`, `View`, `Bitmap`, every `Control` (including `MessageBox`), `ProcessInstance` construction/`Validate`/`CanExecuteOn`/`ExecuteOn` and all catalog introspection are **root (UI) thread only**. Constructing a Control off-root throws `CreateControl(): API function error`. The worker `ChatThread` only `Perform()`s an already-built request. `Thread::Run()` never touches GUI, console, views or processes. Every tool runs inside the panel's Timer handler, on the UI thread.
- **Never block on a busy view.** Before any read or execute, probe with the non-waiting `View::CanRead()` / `View::CanWrite()`, exactly as `ViewCapture` does. A locked view gives a precise "is busy" error and is never passed to `LockForWrite`/`AutoViewWriteLock`/`ExecuteOn`.
- **UTF-8 on the wire:** every `pcl::String` that enters JSON goes through `U8()` (`Utf8.h`), never `String::ToUTF8()`. The body is POSTed only via `PostBytes()` (inside `AnthropicRequest`). Non-ASCII literals are written as UTF-8 byte escapes and converted with `String::UTF8ToUTF16`, because `String( const char* )` is Latin-1.
- **No masked failures:** every tool failure returns a `tool_result` with `is_error: true` and a precise, model-correctable message. There are no silent successes: a value that does not read back as set is an error. HTTP errors are shown verbatim in the log. An API-invalid history is never sent (it is reported instead).
- **Tool loop limits (named constants, exact values):** `PICopilotMaxToolRounds = 12` tool rounds per user message. `PICopilotToolLogParamChars = 120` characters of parameters in a log line. `PICopilotConfirmChangesChars = 1500` characters in the Guided dialog.
- **Image limits (unchanged, exact values):** preview long edge ≤ **1024** px (`PICopilotPreviewMaxEdge`), JPEG quality **85**, base64 ≤ **5 MiB** (`PICopilotMaxImageBase64Bytes`). **History cost rule:** only the LAST message carries images. Older images, including images inside older `tool_result` blocks, are replaced by a text note.
- Transport is unchanged except for the additions in Task 3: `PICOPILOT_DEFAULT_MODEL` (`claude-opus-4-8`), `max_tokens` 4096, non-streamed (no `"stream"` key), a 300 s per-request deadline, and cancel via `RequestCancel()`.
- **Never `make install`.** Dev tests load the module only through `test/run-selftest.sh` (`-n=90` isolated slot, `-m=` + `-r=` + `--force-exit` + `timeout`). **The user tests only by repo pull** from `https://raw.githubusercontent.com/scarter4work/astro-pi/main/repository/`. Never do a local GUI `-m=` load in the user's PixInsight.
- **The GUI cannot be tested headlessly.** This covers the Stop button, the mode combo, the Guided `MessageBox`, live log rendering and drag-resizing. They are user-verified after release (Task 8). The only headless GUI check is the Task 6 resize probe (min/max/fixed flags + programmatic `Resize`). Never claim the rest as tested from a headless run.
- **The panel is freely resizable:** explicit minimum `kMinPanelWidth` 300 × `kMinPanelHeight` 260 logical px, explicit unbounded maximum, and the chat log takes all spare space. No `SetFixedSize`/`SetFixedWidth` anywhere on the panel.
- **Signing password file:** `test/run-selftest.sh` and `./release.sh` read `/tmp/.pi_codesign_pass`. Create it before Task 1 with mode 0600, containing the password from `~/.claude/CLAUDE.md` § "Module Signing". Shred it after Task 8. **Never write the password into any committed file (including this plan).**
- **Test API key:** never echo it and never commit it. Sources, in order: the keyring (`secret-tool lookup service anthropic account default`), then `modules/pi-copilot/test/.test_api_key`, then skip. The gated live checks must RUN (not skip) before release.
- Branch `feat/pi-copilot-inc4`. Every commit message ends with `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`.

## File Structure

```
modules/pi-copilot/
  src/module/CMakeLists.txt                # MODIFY: + PICopilotAgentSelfTest.cpp, ProcessApply.cpp, AgentTools.cpp, SystemPrompt.cpp, AgentSession.cpp
  src/module/PICopilotAgentSelfTest.h/.cpp # NEW: increment-4 headless sections A0-A7 + synthetic 400x300 test window
  src/module/PICopilotSelfTest.cpp         # MODIFY: call RunAgentSelfTest(), merge its keys
  src/module/ProcessApply.h/.cpp           # NEW: ApplyProcess() (default instance -> checked params -> Validate -> CanExecuteOn -> ExecuteOn), DescribeParameterChanges()
  src/module/ProcessCatalog.cpp            # MODIFY: enum default = element at INDEX (DefaultValue() returns an index)
  src/module/AnthropicClient.h/.cpp        # MODIFY: AnthropicMessage::blocks, tools in the body, JpegImageBlock(), AnthropicResult::stopReason/contentBlocks, ParseMessagesResponse()
  src/module/ChatThread.h/.cpp             # MODIFY: forwards tools
  src/module/VisionTurn.h/.cpp             # MODIFY: StripOlderImages() also strips images inside older content blocks / tool_results
  src/module/AgentTools.h/.cpp             # NEW: AgentMode, ToolDefinitions(), ToolCall/ToolOutcome/ToolContext, ExecuteTool(), ToolResultBlock(), CollapsedViewContext()
  src/module/SystemPrompt.h/.cpp           # NEW: BuildSystemPrompt(mode), kPICopilotToneGuidance, kPICopilotToneMarkers
  src/module/AgentSession.h/.cpp           # NEW: AgentSession (tool loop state machine), AgentStep, HistoryIsApiValid(), PICopilotMaxToolRounds
  src/module/PICopilotInterface.h/.cpp     # MODIFY: modes live + persisted, Stop button, loop driving, tool log lines, Guided MessageBox, resizable window (+ test-only resize probe)
  src/module/PICopilotVersion.h            # MODIFY: 0.1.0.4 -> 0.1.1.0
  src/module/PICopilotModule.cpp           # MODIFY: release date, Description()
  test/run-selftest.sh                     # MODIFY: verdict keys; echo server "/agent" scripted path; timeout 600
  README.md                                # MODIFY: Increment 4 section + Verified line
repository/                                # REGENERATED by ./release.sh (Task 8)
```

Each unit has one job. `ProcessApply` knows PCL process parameters, `AgentTools` maps tool JSON to units, `AgentSession` enforces the API conversation rules, `SystemPrompt` is text, and the panel only orchestrates. Everything except the panel is exercised headlessly by `PICopilotAgentSelfTest.cpp`, which is kept apart from the two existing self-test files, as increment 3 did.

---

## Task 1: Platform smoke — native process execution from the module (headless)

This task proves, before any production code, that the module can build a default `ProcessInstance`, set a String, an enum and table cells, and `ExecuteOn` a view **from inside the self-test's own `ExecuteGlobal()`** (nested execution) under `--automation-mode`. It also records the facts later tasks assert: SCNR `amount` range, HT column ids and enum value semantics. **If `ExecuteOn` fails here, stop and report BLOCKED** with the `agentSmokeInfo` JSON. Do not change the production design to work around a test-only nesting limit.

**Files:**
- Create: `modules/pi-copilot/src/module/PICopilotAgentSelfTest.h`
- Create: `modules/pi-copilot/src/module/PICopilotAgentSelfTest.cpp`
- Modify: `modules/pi-copilot/src/module/PICopilotSelfTest.cpp` (end of `RunSelfTest`, the increment-3 block at lines 272-288)
- Modify: `modules/pi-copilot/src/module/CMakeLists.txt` (`MODULE_SOURCES`)
- Modify: `modules/pi-copilot/test/run-selftest.sh` (`required_true` list)

**Interfaces:**
- Consumes: `RunSelfTest( String& )`, `ThePICopilotModule->ProcessEvents()`, `U8()`.
- Produces (file-local in `PICopilotAgentSelfTest.cpp`, used by Tasks 2-7):
  - `constexpr int kAgW = 400, kAgH = 300, kAgSqX0 = 150, kAgSqY0 = 100, kAgSq = 100;` and `double AgBackground( int x )` = `0.02 + 0.03*x/(kAgW-1)`.
  - `class AgentTestWindow { explicit AgentTestWindow( const char* id ); View MainView() const; }`: a hidden 400×300 32-bit float RGB window, background R=G=B=`AgBackground(x)`, pure red square at (150,100) size 100. It is force-closed in its destructor.
  - `double ChannelMedian( View, int c )`, `float SampleAt( View, int x, int y, int c )`.
  - Public: `bool pcl::RunAgentSelfTest( nlohmann::json& out );`. Later tasks insert sections above the marker `// ---- inc4 sections end ----`.

- [ ] **Step 0: Branch + prerequisites.**

```bash
cd /home/scarter4work/projects/astro-pi
git checkout main && git pull --ff-only && git checkout -b feat/pi-copilot-inc4
test -s /tmp/.pi_codesign_pass && stat -c '%a' /tmp/.pi_codesign_pass || echo "create /tmp/.pi_codesign_pass (0600) from ~/.claude/CLAUDE.md Module Signing before continuing"
cd modules/pi-copilot && cmake -B build -DPCLDIR=$HOME/PCL -DPICOPILOT_BUILD_MODULE=ON
```
Expected: on branch `feat/pi-copilot-inc4`, the stat prints `600`, and CMake prints `PCL found at /home/scarter4work/PCL -- building PICopilot module`.

- [ ] **Step 1: Failing assertion.** In `test/run-selftest.sh`, in the python `required_true` list, add after the `'utf8BodyOk', 'twoTurnOk',` line:
```python
    # increment 4
    'agentSmokeOk',
```

- [ ] **Step 2: Run to verify RED.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) && bash test/run-selftest.sh`
Expected: `FAILED keys: agentSmokeOk` then `FAIL: self-test verdict not all green` (exit 1).

- [ ] **Step 3: Implement.** `PICopilotAgentSelfTest.h`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_AgentSelfTest_h
#define PICopilot_AgentSelfTest_h

#include <nlohmann/json.hpp>

namespace pcl
{

// Increment-4 headless self-test sections (native process execution,
// ApplyProcess, tool transport, tools + system prompt, agent loop, wire,
// gated live agent run). Root thread only (ImageWindow/View/ProcessInstance).
// Adds its keys to `out`; returns true iff every section passed.
bool RunAgentSelfTest( nlohmann::json& out );

} // namespace pcl

#endif // PICopilot_AgentSelfTest_h
```
`PICopilotAgentSelfTest.cpp`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PICopilotAgentSelfTest.h"
#include "PICopilotModule.h"
#include "Utf8.h"

#include <pcl/AutoViewLock.h>
#include <pcl/Exception.h>
#include <pcl/Image.h>
#include <pcl/ImageVariant.h>
#include <pcl/ImageWindow.h>
#include <pcl/Process.h>
#include <pcl/ProcessInstance.h>
#include <pcl/ProcessParameter.h>
#include <pcl/Variant.h>
#include <pcl/View.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pcl
{

namespace
{

// Synthetic agent test image: 400x300 32-bit float RGB, background grey
// gradient (R=G=B), pure-red 100x100 square. Small on purpose -- these
// sections test process execution, not preview downscaling (increment 3 did).
constexpr int kAgW = 400, kAgH = 300;
constexpr int kAgSqX0 = 150, kAgSqY0 = 100, kAgSq = 100;

double AgBackground( int x )
{
   return 0.02 + 0.03*x/(kAgW - 1);
}

// Owns a hidden test window and force-closes it on destruction (ImageWindow
// is only an alias handle; its destructor does not close the core window).
// Not copyable: exactly one owner per window.
class AgentTestWindow
{
public:

   explicit AgentTestWindow( const char* id )
      : m_window( kAgW, kAgH, 3, 32, true/*floatSample*/, true/*color*/,
                  false/*initialProcessing*/, IsoString( id ) )
   {
      if ( m_window.IsNull() )
         throw Error( "AgentTestWindow: ImageWindow construction returned a null window" );
      try
      {
         Fill();
      }
      catch ( ... )
      {
         Close();
         throw;
      }
   }

   ~AgentTestWindow()
   {
      Close();
   }

   AgentTestWindow( const AgentTestWindow& ) = delete;
   AgentTestWindow& operator =( const AgentTestWindow& ) = delete;

   View MainView() const
   {
      return m_window.MainView();
   }

private:

   ImageWindow m_window;

   void Fill()
   {
      View view = m_window.MainView();
      AutoViewLock lock( view );
      ImageVariant v = view.Image();
      if ( !v || !v.IsFloatSample() || v.BitsPerSample() != 32 || v.NumberOfChannels() != 3 )
         throw Error( "AgentTestWindow: not a 32-bit float RGB image" );
      Image& img = static_cast<Image&>( *v );
      for ( int y = 0; y < kAgH; ++y )
         for ( int x = 0; x < kAgW; ++x )
         {
            const bool sq = x >= kAgSqX0 && x < kAgSqX0 + kAgSq && y >= kAgSqY0 && y < kAgSqY0 + kAgSq;
            for ( int c = 0; c < 3; ++c )
               img.Pixel( x, y, c ) = float( sq ? (c == 0 ? 1.0 : 0.0) : AgBackground( x ) );
         }
   }

   void Close()
   {
      try
      {
         if ( !m_window.IsNull() )
            m_window.ForceClose();
      }
      catch ( ... )
      {
      }
   }
};

double ChannelMedian( View view, int c )
{
   AutoViewWriteLock lock( view );
   ImageVariant v = view.Image();
   return v.Median( v.Bounds(), c, c );
}

float SampleAt( View view, int x, int y, int c )
{
   AutoViewWriteLock lock( view );
   ImageVariant v = view.Image();
   if ( !v.IsFloatSample() || v.BitsPerSample() != 32 )
      throw Error( "SampleAt: not a 32-bit float image" );
   return static_cast<const Image&>( *v ).Pixel( x, y, c );
}

} // namespace

bool RunAgentSelfTest( nlohmann::json& out )
{
   bool allOk = true;

   // ---- Section A0: native process execution smoke (Task 1) ---------------
   // Proves the primitives ApplyProcess() is built on, from INSIDE this
   // self-test's own ExecuteGlobal() (nested process execution), and records
   // the parameter facts later sections assert.
   {
      bool pmExecOk = false, enumRoundTripOk = false, htTableOk = false, rangeOk = false, globalOnlyOk = false;
      nlohmann::json info = nlohmann::json::object();
      String error;
      try
      {
         AgentTestWindow tw( "PICopilotAgentSmoke" );
         View view = tw.MainView();

         // PixelMath "$T*0.5" on the view: median must halve exactly.
         const double before = ChannelMedian( view, 0 );
         Process P( IsoString( "PixelMath" ) );
         ProcessInstance pm( P );
         const bool setOk = pm.SetParameterValue( Variant( String( "$T*0.5" ) ), IsoString( "expression" ) );
         String whyNot;
         const bool valid = pm.Validate( whyNot );
         info["pmValidateWhyNot"] = U8( whyNot );
         const bool can = pm.CanExecuteOn( view, whyNot );
         info["pmCanExecuteWhyNot"] = U8( whyNot );
         const bool ran = setOk && valid && can && pm.ExecuteOn( view );
         const double after = ChannelMedian( view, 0 );
         info["pmSet"] = setOk; info["pmValid"] = valid; info["pmCan"] = can; info["pmRan"] = ran;
         info["pmMedianBefore"] = before; info["pmMedianAfter"] = after;
         pmExecOk = ran && std::fabs( after - 0.5*before ) < 1e-5;

         // SCNR enum: element list, default, set-by-value + read-back.
         Process S( IsoString( "SCNR" ) );
         ProcessParameter color( S, IsoString( "colorToRemove" ) );
         nlohmann::json elements = nlohmann::json::array();
         int redValue = -1;
         for ( const ProcessParameter::EnumerationElement& e : color.EnumerationElements() )
         {
            elements.push_back( { { "id", std::string( e.id.c_str() ) }, { "value", e.value } } );
            if ( e.id == "Red" )
               redValue = e.value;
         }
         ProcessInstance si( S );
         info["scnrColorElements"] = elements;
         info["scnrColorDefault"] = si.ParameterValue( color ).ToInt();
         const bool setRed = redValue >= 0 && si.SetParameterValue( Variant( redValue ), color );
         info["scnrColorReadBack"] = si.ParameterValue( color ).ToInt();
         enumRoundTripOk = setRed && si.ParameterValue( color ).ToInt() == redValue;

         // SCNR amount range (Task 2's out-of-range case depends on it).
         ProcessParameter amount( S, IsoString( "amount" ) );
         double lo = 0, hi = 0;
         amount.GetNumericRange( lo, hi );
         info["scnrAmountRange"] = { lo, hi };
         rangeOk = lo == 0 && hi == 1;

         // HistogramTransformation H: column ids, reallocation, cell set + read-back.
         Process H( IsoString( "HistogramTransformation" ) );
         ProcessParameter table( H, IsoString( "H" ) );
         ProcessInstance hi_( H );
         const ProcessParameter::parameter_list columns = table.TableColumns();
         nlohmann::json colIds = nlohmann::json::array();
         for ( const ProcessParameter& c : columns )
            colIds.push_back( std::string( c.Id().c_str() ) );
         info["htColumns"] = colIds;
         info["htDefaultRows"] = hi_.TableRowCount( table );
         bool cellsOk = hi_.AllocateTableRows( table, 5 );
         for ( size_type r = 0; r < 5; ++r )
            for ( size_type k = 0; k < columns.Length(); ++k )
            {
               const double v = (k == 1) ? (r == 3 ? 0.25 : 0.5) : ((k == 2 || k == 4) ? 1.0 : 0.0);
               cellsOk = cellsOk && hi_.SetParameterValue( Variant( v ), columns[k], r );
            }
         htTableOk = cellsOk && columns.Length() == 5 && hi_.TableRowCount( table ) == 5
                  && std::fabs( hi_.ParameterValue( columns[1], 3 ).ToDouble() - 0.25 ) < 1e-12;

         // Global-only detection.
         Process II( IsoString( "ImageIntegration" ) );
         info["iiCanProcessViews"] = II.CanProcessViews();
         info["iiCanProcessGlobal"] = II.CanProcessGlobal();
         info["pmCanProcessViews"] = P.CanProcessViews();
         globalOnlyOk = !II.CanProcessViews() && II.CanProcessGlobal() && P.CanProcessViews();
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }

      const bool ok = pmExecOk && enumRoundTripOk && htTableOk && rangeOk && globalOnlyOk;
      out["agentSmokeInfo"] = info;
      out["agentSmokePixelMathOk"] = pmExecOk;
      out["agentSmokeEnumOk"] = enumRoundTripOk;
      out["agentSmokeTableOk"] = htTableOk;
      out["agentSmokeRangeOk"] = rangeOk;
      out["agentSmokeGlobalOnlyOk"] = globalOnlyOk;
      out["agentSmokeError"] = U8( error );
      out["agentSmokeOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- inc4 sections end ----

   // Let the core finish the deferred teardown of the windows force-closed
   // above before control returns to --force-exit (same reasoning as the
   // drain at the end of RunVisionSelfTest). >= 250 ms between calls.
   for ( int i = 0; i < 4; ++i )
   {
      ThePICopilotModule->ProcessEvents( true/*excludeUserInputEvents*/ );
      std::this_thread::sleep_for( std::chrono::milliseconds( 250 ) );
   }

   return allOk;
}

} // namespace pcl
```
In `PICopilotSelfTest.cpp`, add `#include "PICopilotAgentSelfTest.h"` next to the vision include. Replace
```cpp
   ok = ok && visionOk;
```
with
```cpp
   // Increment 4: agent/tool sections. Same isolation as increment 3.
   bool agentOk = false;
   try
   {
      nlohmann::json agent;
      agentOk = RunAgentSelfTest( agent );
      j.update( agent );
   }
   catch ( const std::exception& x )
   {
      j["agentException"] = x.what();
   }
   catch ( ... )
   {
      j["agentException"] = "unknown exception";
   }

   ok = ok && visionOk && agentOk;
```
In `CMakeLists.txt` `MODULE_SOURCES`, add `PICopilotAgentSelfTest.cpp` after `ViewCapture.cpp`.

- [ ] **Step 4: Run to verify GREEN, and record the facts.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) && bash test/run-selftest.sh`
Expected: `PASS: self-test verdict all green`. In the printed JSON:
- `agentSmokeInfo.htColumns` is `["c0","m","c1","r0","r1"]`.
- `scnrAmountRange` is `[0.0,1.0]`.
- `scnrColorElements` lists Red/Green/Blue with values 0/1/2.
- `iiCanProcessViews` is false.

Copy these four facts into the task report. **If `pmRan` is false** (nested `ExecuteOn` refused), or any other key is false, report **BLOCKED** with the full `agentSmokeInfo`. If `htColumns` differ, Tasks 4 and 5 must use the recorded ids (Task 4's `htColumnsOk` assertion enforces this).

- [ ] **Step 5: Commit.**
```bash
cd /home/scarter4work/projects/astro-pi
git add modules/pi-copilot/src/module/PICopilotAgentSelfTest.h modules/pi-copilot/src/module/PICopilotAgentSelfTest.cpp \
        modules/pi-copilot/src/module/PICopilotSelfTest.cpp modules/pi-copilot/src/module/CMakeLists.txt modules/pi-copilot/test/run-selftest.sh
git commit -m "test(pi-copilot): headless smoke for native process execution (PixelMath/SCNR/HT, nested ExecuteOn)

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: ProcessApply — checked parameters, Validate, CanExecuteOn, ExecuteOn

**Files:**
- Create: `modules/pi-copilot/src/module/ProcessApply.h`
- Create: `modules/pi-copilot/src/module/ProcessApply.cpp`
- Modify: `modules/pi-copilot/src/module/ProcessCatalog.cpp` (`DefaultValueJson`, enumeration case)
- Modify: `modules/pi-copilot/src/module/PICopilotAgentSelfTest.cpp` (Section A1)
- Modify: `modules/pi-copilot/src/module/CMakeLists.txt`, `modules/pi-copilot/test/run-selftest.sh`

**Interfaces:**
- Consumes: Task 1 test helpers, `U8()`, `DescribeProcess()`.
- Produces:
```cpp
struct ApplyProcessResult { bool ok; String error; nlohmann::json parametersSet; double elapsedMs; String processId; String viewId; };
ApplyProcessResult ApplyProcess( const IsoString& processId, const nlohmann::json& parameters,
                                 const nlohmann::json& tableParameters, View view );   // root thread; never throws
String DescribeParameterChanges( const nlohmann::json& parameters, const nlohmann::json& tableParameters,
                                 size_type maxChars );
```

- [ ] **Step 1: Failing test.** In `PICopilotAgentSelfTest.cpp`, add `#include "ProcessApply.h"` and `#include "ProcessCatalog.h"`, and insert above `// ---- inc4 sections end ----`:
```cpp
   // ---- Section A1: ApplyProcess (Task 2) ----------------------------------
   {
      bool pmOk = false, htOk = false, scnrOk = false, errorsOk = true, busyOk = false,
           changesOk = false, enumDefaultOk = false;
      nlohmann::json detail = nlohmann::json::object();
      String error;
      try
      {
         {  // PixelMath: String parameter -> pixel values halved.
            AgentTestWindow tw( "PICopilotApplyPM" );
            View v = tw.MainView();
            const double before = ChannelMedian( v, 0 );
            const ApplyProcessResult r = ApplyProcess( "PixelMath", { { "expression", "$T*0.5" } }, nlohmann::json(), v );
            const double after = ChannelMedian( v, 0 );
            detail["pm"] = { { "ok", r.ok }, { "error", U8( r.error ) }, { "before", before }, { "after", after },
                             { "elapsedMs", r.elapsedMs } };
            pmOk = r.ok && r.processId == "PixelMath" && std::fabs( after - 0.5*before ) < 1e-5
                && r.parametersSet.value( "expression", std::string() ) == "$T*0.5" && r.error.IsEmpty();
         }
         {  // HistogramTransformation: table parameter H (row 3 = combined RGB/K, m = 0.25 brightens).
            AgentTestWindow tw( "PICopilotApplyHT" );
            View v = tw.MainView();
            const double before = ChannelMedian( v, 1 );
            nlohmann::json rows = nlohmann::json::array();
            for ( int i = 0; i < 5; ++i )
               rows.push_back( nlohmann::json::array( { 0, i == 3 ? 0.25 : 0.5, 1, 0, 1 } ) );
            nlohmann::json tables = nlohmann::json::object();
            tables["H"] = rows;
            const ApplyProcessResult r = ApplyProcess( "HistogramTransformation", nlohmann::json::object(), tables, v );
            const double after = ChannelMedian( v, 1 );
            detail["ht"] = { { "ok", r.ok }, { "error", U8( r.error ) }, { "before", before }, { "after", after } };
            htOk = r.ok && after > 2*before;
         }
         {  // SCNR: enumeration parameters by element id -> red square loses red, grey background untouched.
            AgentTestWindow tw( "PICopilotApplySCNR" );
            View v = tw.MainView();
            const float bgBefore = SampleAt( v, 10, 10, 0 );
            const nlohmann::json p = { { "colorToRemove", "Red" }, { "protectionMethod", "AverageNeutral" },
                                       { "amount", 1.0 }, { "preserveLightness", false } };
            const ApplyProcessResult r = ApplyProcess( "SCNR", p, nlohmann::json(), v );
            const float sq = SampleAt( v, kAgSqX0 + kAgSq/2, kAgSqY0 + kAgSq/2, 0 );
            const float bgAfter = SampleAt( v, 10, 10, 0 );
            detail["scnr"] = { { "ok", r.ok }, { "error", U8( r.error ) }, { "square", sq }, { "bgBefore", bgBefore }, { "bgAfter", bgAfter } };
            scnrOk = r.ok && sq < 1e-6f && std::fabs( bgAfter - bgBefore ) < 1e-6f;
         }

         // Every error path: !ok, a precise message, and the image untouched.
         struct Case { const char* name; const char* process; nlohmann::json params; nlohmann::json tables; const char* expect; };
         const Case cases[] = {
            { "unknownProcess", "NoSuchProcessXYZ", nlohmann::json::object(), nlohmann::json(),
              "unknown process id 'NoSuchProcessXYZ'" },
            { "unknownParam", "PixelMath", { { "noSuchParam", 1 } }, nlohmann::json(),
              "unknown parameter PixelMath.noSuchParam" },
            { "badEnum", "SCNR", { { "colorToRemove", "Purple" } }, nlohmann::json(),
              "SCNR.colorToRemove: 'Purple' is not a valid value; use one of: Red, Green, Blue" },
            { "wrongType", "SCNR", { { "amount", "lots" } }, nlohmann::json(),
              "SCNR.amount: expected a number" },
            { "outOfRange", "SCNR", { { "amount", 5 } }, nlohmann::json(),
              "SCNR.amount: 5 is out of range [0, 1]" },
            { "tableAsScalar", "HistogramTransformation", { { "H", 1 } }, nlohmann::json(),
              "HistogramTransformation.H is a table parameter" },
            { "badRow", "HistogramTransformation", nlohmann::json::object(),
              { { "H", nlohmann::json::array( { nlohmann::json::array( { 0, 0.5, 1 } ) } ) } },
              "HistogramTransformation.H: row 0 has 3 values; expected 5" },
            { "globalOnly", "ImageIntegration", nlohmann::json::object(), nlohmann::json(),
              "ImageIntegration can only run in the global context" },
            { "badExpression", "PixelMath", { { "expression", "$T*" } }, nlohmann::json(),
              "PixelMath" },
         };
         AgentTestWindow tw( "PICopilotApplyErr" );
         View v = tw.MainView();
         const double before = ChannelMedian( v, 0 );
         nlohmann::json caseOut = nlohmann::json::array();
         for ( const Case& c : cases )
         {
            const ApplyProcessResult r = ApplyProcess( c.process, c.params, c.tables, v );
            const bool pass = !r.ok && r.error.Contains( String::UTF8ToUTF16( c.expect ) );
            caseOut.push_back( { { "case", c.name }, { "pass", pass }, { "error", U8( r.error ) } } );
            errorsOk = errorsOk && pass;
         }
         detail["errorCases"] = caseOut;
         errorsOk = errorsOk && std::fabs( ChannelMedian( v, 0 ) - before ) < 1e-12;

         {  // Busy view: locked by "someone else" -> immediate error, never a wait.
            const auto t0 = std::chrono::steady_clock::now();
            ApplyProcessResult r;
            {
               View locked = v;
               AutoViewLock lock( locked );
               r = ApplyProcess( "PixelMath", { { "expression", "$T*0.5" } }, nlohmann::json(), v );
            }
            const double ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
            detail["busy"] = { { "error", U8( r.error ) }, { "ms", ms } };
            busyOk = !r.ok && r.error.Contains( "is busy" ) && ms < 2000
                  && std::fabs( ChannelMedian( v, 0 ) - before ) < 1e-12;
         }

         {  // Human-readable change list (Guided dialog / log).
            nlohmann::json tables = nlohmann::json::object();
            tables["H"] = nlohmann::json::array( { nlohmann::json::array( { 0, 0.5, 1, 0, 1 } ) } );
            const String s = DescribeParameterChanges( { { "expression", "$T*0.5" } }, tables, 1000 );
            const String none = DescribeParameterChanges( nlohmann::json::object(), nlohmann::json(), 1000 );
            changesOk = s.Contains( "expression = $T*0.5" ) && s.Contains( "H = [[0,0.5,1,0,1]]" )
                     && none == "(all parameters at their defaults)";
         }

         {  // describe_process default for an enum is the element at the default INDEX.
            const nlohmann::json d = DescribeProcess( "SCNR" );
            for ( const nlohmann::json& p : d.at( "parameters" ) )
               if ( p.at( "id" ) == "colorToRemove" )
                  enumDefaultOk = p.value( "default", std::string() ) == "Green";
         }
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }

      const bool ok = pmOk && htOk && scnrOk && errorsOk && busyOk && changesOk && enumDefaultOk;
      out["applyDetail"] = detail;
      out["applyPixelMathOk"] = pmOk;
      out["applyTableOk"] = htOk;
      out["applyEnumOk"] = scnrOk;
      out["applyErrorsOk"] = errorsOk;
      out["applyBusyOk"] = busyOk;
      out["applyChangesOk"] = changesOk;
      out["catalogEnumDefaultOk"] = enumDefaultOk;
      out["applyError"] = U8( error );
      out["applyProcessOk"] = ok;
      allOk = allOk && ok;
   }
```
In `run-selftest.sh` `required_true`, add `'applyProcessOk',` after `'agentSmokeOk',`.

- [ ] **Step 2: Verify RED.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) 2>&1 | grep -m3 error`
Expected: a compile error, `ProcessApply.h: No such file or directory`.

- [ ] **Step 3: Implement.** `ProcessApply.h`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ProcessApply_h
#define PICopilot_ProcessApply_h

#include <pcl/String.h>
#include <pcl/View.h>

#include <nlohmann/json.hpp>

namespace pcl
{

struct ApplyProcessResult
{
   bool           ok = false;
   String         error;                                     // precise, model-facing; empty when ok
   nlohmann::json parametersSet = nlohmann::json::object();  // id -> value exactly as applied (tables: the rows)
   double         elapsedMs = 0;                             // ExecuteOn() wall time
   String         processId;                                 // canonical id (Process::Id()), once resolved
   String         viewId;                                    // target FullId, once resolved
};

/*
 * Runs one process on one view, the apply_process tool's engine:
 *   1. resolve the process (unknown id -> error),
 *   2. refuse global-only processes (!CanProcessViews(); no ExecuteGlobal yet),
 *   3. refuse a null or BUSY view (non-blocking CanRead()/CanWrite() probe),
 *   4. start from the process's DEFAULT instance and set only the given
 *      parameters: parameters {id: value} for scalars, tableParameters
 *      {id: [[row values in TableColumns() order], ...]} replacing whole tables.
 *      Types are checked (Boolean <- bool, numbers range-checked against
 *      GetNumericRange and integral for integer types, String length limits,
 *      Enumeration <- element id / alias string or element value integer);
 *      every value is READ BACK and must match,
 *   5. Validate(whyNot), then CanExecuteOn(view, whyNot),
 *   6. ExecuteOn(view) with swap data (undoable, recorded in History).
 * Every failure is ok=false + a message naming the process/parameter and the
 * fix; nothing after the failing step runs, so a failure never touches the
 * image. Root thread only. Never throws.
 */
ApplyProcessResult ApplyProcess( const IsoString& processId, const nlohmann::json& parameters,
                                 const nlohmann::json& tableParameters, View view );

// "id = value" lines (tables as compact JSON), for the Guided confirm dialog.
// "(all parameters at their defaults)" when nothing is set. Cut to maxChars.
String DescribeParameterChanges( const nlohmann::json& parameters, const nlohmann::json& tableParameters,
                                 size_type maxChars );

} // namespace pcl

#endif // PICopilot_ProcessApply_h
```
`ProcessApply.cpp`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ProcessApply.h"
#include "Utf8.h"

#include <pcl/Exception.h>
#include <pcl/Process.h>
#include <pcl/ProcessInstance.h>
#include <pcl/ProcessParameter.h>
#include <pcl/Variant.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>

namespace pcl
{

namespace
{

// A model-correctable failure. Thrown only inside ApplyProcess() and caught there.
struct ApplyError
{
   String message;
};

String S16( const std::string& utf8 )
{
   return String::UTF8ToUTF16( utf8.c_str() );
}

String JsonShort( const nlohmann::json& v )
{
   std::string s = v.dump();
   if ( s.size() > 80 )
      s = s.substr( 0, 77 ) + "...";
   return S16( s );
}

String JoinIds( const ProcessParameter::enumeration_element_list& elements )
{
   String s;
   for ( const ProcessParameter::EnumerationElement& e : elements )
   {
      if ( !s.IsEmpty() )
         s += ", ";
      s += String( e.id );
   }
   return s;
}

String ColumnIds( const ProcessParameter::parameter_list& columns )
{
   String s;
   for ( const ProcessParameter& c : columns )
   {
      if ( !s.IsEmpty() )
         s += ", ";
      s += String( c.Id() );
   }
   return s;
}

String VariantText( const Variant& v )
{
   try
   {
      return v.IsValid() ? v.ToString() : String( "(invalid)" );
   }
   catch ( ... )
   {
      return "(unprintable)";
   }
}

Variant EnumVariant( const ProcessParameter& p, const nlohmann::json& v, const String& name )
{
   ProcessParameter::enumeration_element_list elements;
   String listError;
   try
   {
      elements = p.EnumerationElements();
   }
   catch ( const pcl::Exception& x )
   {
      listError = x.Message();
   }

   if ( !listError.IsEmpty() )
   {
      // The core cannot report this enumeration's element ids (observed for
      // PixelMath newImageColorSpace/newImageSampleFormat). Accept only the
      // raw element value, and say so precisely.
      if ( v.is_number_integer() )
         return Variant( v.get<int>() );
      throw ApplyError{ name + ": the element identifiers of this enumeration cannot be read (" + listError
                        + "); pass the integer element value instead" };
   }

   if ( v.is_string() )
   {
      const IsoString want( v.get<std::string>().c_str() );
      for ( const ProcessParameter::EnumerationElement& e : elements )
      {
         if ( e.id == want )
            return Variant( e.value );
         for ( const IsoString& a : e.aliases )
            if ( a.Trimmed() == want )
               return Variant( e.value );
      }
      throw ApplyError{ name + ": '" + S16( v.get<std::string>() ) + "' is not a valid value; use one of: "
                        + JoinIds( elements ) };
   }
   if ( v.is_number_integer() )
   {
      for ( const ProcessParameter::EnumerationElement& e : elements )
         if ( e.value == v.get<int>() )
            return Variant( e.value );
      throw ApplyError{ name + ": " + JsonShort( v ) + " is not a valid element value; use one of: " + JoinIds( elements ) };
   }
   throw ApplyError{ name + ": expected one of " + JoinIds( elements ) + " (as a string), got " + JsonShort( v ) };
}

Variant ToVariant( const ProcessParameter& p, const nlohmann::json& v, const String& name )
{
   if ( p.IsBlock() )
      throw ApplyError{ name + ": block (binary) parameters cannot be set by apply_process" };
   if ( p.IsBoolean() )
   {
      if ( !v.is_boolean() )
         throw ApplyError{ name + ": expected true or false, got " + JsonShort( v ) };
      return Variant( v.get<bool>() );
   }
   if ( p.IsEnumeration() )
      return EnumVariant( p, v, name );
   if ( p.IsString() )
   {
      if ( !v.is_string() )
         throw ApplyError{ name + ": expected a string, got " + JsonShort( v ) };
      const String s = S16( v.get<std::string>() );
      size_type minLen = 0, maxLen = 0;
      p.GetLengthLimits( minLen, maxLen );
      if ( maxLen > 0 && s.Length() > maxLen )
         throw ApplyError{ name + String().Format( ": text is %u characters; the maximum is %u",
                                                   unsigned( s.Length() ), unsigned( maxLen ) ) };
      if ( s.Length() < minLen )
         throw ApplyError{ name + String().Format( ": text is %u characters; the minimum is %u",
                                                   unsigned( s.Length() ), unsigned( minLen ) ) };
      return Variant( s );
   }
   if ( p.IsNumeric() )
   {
      if ( !v.is_number() )
         throw ApplyError{ name + ": expected a number, got " + JsonShort( v ) };
      const double d = v.get<double>();
      if ( p.IsInteger() && d != std::floor( d ) )
         throw ApplyError{ name + ": expected an integer, got " + JsonShort( v ) };
      double lo = 0, hi = 0;
      p.GetNumericRange( lo, hi );
      if ( lo < hi && (d < lo || d > hi) )
         throw ApplyError{ name + String().Format( ": %.10g is out of range [%.10g, %.10g]", d, lo, hi ) };
      return Variant( d );
   }
   throw ApplyError{ name + ": unsupported parameter type" };
}

bool SameValue( const ProcessParameter& p, const Variant& a, const Variant& b )
{
   if ( !b.IsValid() )
      return false;
   if ( p.IsString() )
      return a.ToString() == b.ToString();
   if ( p.IsBoolean() )
      return a.ToBoolean() == b.ToBoolean();
   if ( p.IsEnumeration() || p.IsInteger() )
      return a.ToInt64() == b.ToInt64();
   const double x = a.ToDouble(), y = b.ToDouble();
   return std::fabs( x - y ) <= 1e-6*std::max( 1.0, std::fabs( x ) );   // Float params store 32 bits
}

// SetParameterValue + mandatory read-back: a value that does not stick is an
// error, never a silent success (this also catches any enum value/index
// mismatch in the core API).
void SetChecked( ProcessInstance& instance, const ProcessParameter& p, const Variant& value,
                 size_type row, const String& name )
{
   bool set = false;
   try
   {
      set = instance.SetParameterValue( value, p, row );
   }
   catch ( const pcl::Exception& x )
   {
      throw ApplyError{ name + ": " + x.Message() };
   }
   if ( !set )
      throw ApplyError{ name + ": PixInsight rejected the value " + VariantText( value ) };
   const Variant back = instance.ParameterValue( p, row );
   if ( !SameValue( p, value, back ) )
      throw ApplyError{ name + ": the value did not stick (set " + VariantText( value )
                        + ", read back " + VariantText( back ) + ")" };
}

ProcessParameter FindParameter( const Process& P, const std::string& id, const String& name )
{
   try
   {
      return ProcessParameter( P, IsoString( id.c_str() ) );
   }
   catch ( const pcl::Exception& )
   {
      throw ApplyError{ "unknown parameter " + name + "; call describe_process for the valid parameter ids" };
   }
}

} // namespace

ApplyProcessResult ApplyProcess( const IsoString& processId, const nlohmann::json& parameters,
                                 const nlohmann::json& tableParameters, View view )
{
   ApplyProcessResult r;
   try
   {
      std::unique_ptr<Process> P;
      try
      {
         P.reset( new Process( processId ) );
      }
      catch ( const pcl::Exception& )
      {
         throw ApplyError{ "unknown process id '" + String( processId ) + "'; call list_processes for valid ids" };
      }
      r.processId = String( P->Id() );

      if ( !P->CanProcessViews() )
         throw ApplyError{ r.processId + " can only run in the global context (not on a view); "
                           "global execution is not supported by apply_process yet" };

      if ( view.IsNull() )
         throw ApplyError{ String( "no target view: open or select an image, or pass view_id" ) };
      r.viewId = String( view.FullId() );

      // Never block on a view a running process holds (see ViewCapture.cpp).
      bool busy = true;
      try
      {
         busy = !view.CanRead() || !view.CanWrite();
      }
      catch ( ... )
      {
      }
      if ( busy )
         throw ApplyError{ "view " + r.viewId + " is busy (locked by a running process); try again when it finishes" };

      ProcessInstance instance( *P );   // DEFAULT parameters

      if ( !parameters.is_null() && !parameters.is_object() )
         throw ApplyError{ String( "parameters must be an object {parameterId: value}" ) };
      if ( parameters.is_object() )
         for ( auto it = parameters.begin(); it != parameters.end(); ++it )
         {
            const String name = r.processId + "." + S16( it.key() );
            const ProcessParameter p = FindParameter( *P, it.key(), name );
            if ( p.IsTable() )
               throw ApplyError{ name + " is a table parameter; pass it in table_parameters as [[row values]...]" };
            if ( p.IsReadOnly() )
               throw ApplyError{ name + " is read-only" };
            SetChecked( instance, p, ToVariant( p, it.value(), name ), ~size_type( 0 ), name );
            r.parametersSet[it.key()] = it.value();
         }

      if ( !tableParameters.is_null() && !tableParameters.is_object() )
         throw ApplyError{ String( "table_parameters must be an object {tableId: [[row values]...]}" ) };
      if ( tableParameters.is_object() )
         for ( auto it = tableParameters.begin(); it != tableParameters.end(); ++it )
         {
            const String name = r.processId + "." + S16( it.key() );
            const ProcessParameter p = FindParameter( *P, it.key(), name );
            if ( !p.IsTable() )
               throw ApplyError{ name + " is not a table parameter; pass it in parameters" };
            const nlohmann::json& rows = it.value();
            if ( !rows.is_array() )
               throw ApplyError{ name + ": expected an array of rows [[...], ...]" };
            const ProcessParameter::parameter_list columns = p.TableColumns();
            for ( size_type i = 0; i < rows.size(); ++i )
               if ( !rows[i].is_array() || rows[i].size() != columns.Length() )
                  throw ApplyError{ name + String().Format( ": row %u has %u values; expected %u (columns: ",
                                                            unsigned( i ), unsigned( rows[i].is_array() ? rows[i].size() : 0 ),
                                                            unsigned( columns.Length() ) )
                                    + ColumnIds( columns ) + ")" };
            if ( !instance.AllocateTableRows( p, rows.size() ) )
               throw ApplyError{ name + String().Format( ": PixInsight refused a table of %u rows", unsigned( rows.size() ) ) };
            for ( size_type i = 0; i < rows.size(); ++i )
               for ( size_type k = 0; k < columns.Length(); ++k )
               {
                  const String cell = name + String().Format( "[%u].", unsigned( i ) ) + String( columns[k].Id() );
                  SetChecked( instance, columns[k], ToVariant( columns[k], rows[i][k], cell ), i, cell );
               }
            r.parametersSet[it.key()] = rows;
         }

      String whyNot;
      if ( !instance.Validate( whyNot ) )
         throw ApplyError{ r.processId + " rejected the parameters: "
                           + (whyNot.IsEmpty() ? String( "(no reason given)" ) : whyNot) };
      if ( !instance.CanExecuteOn( view, whyNot ) )
         throw ApplyError{ r.processId + " cannot run on " + r.viewId + ": "
                           + (whyNot.IsEmpty() ? String( "(no reason given)" ) : whyNot) };

      const auto t0 = std::chrono::steady_clock::now();
      bool ran = false;
      try
      {
         ran = instance.ExecuteOn( view );   // swapFile=true: History + undo
      }
      catch ( const pcl::Exception& x )
      {
         throw ApplyError{ r.processId + " failed while running: " + x.Message() };
      }
      r.elapsedMs = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
      if ( !ran )
         throw ApplyError{ r.processId + " did not complete on " + r.viewId
                           + " (ExecuteOn returned false; the Process Console has the details)" };
      r.ok = true;
   }
   catch ( const ApplyError& e )
   {
      r.ok = false;
      r.error = e.message;
      r.parametersSet = nlohmann::json::object();
   }
   catch ( const pcl::Exception& x )
   {
      r.ok = false;
      r.error = "apply_process internal error: " + x.Message();
   }
   catch ( const std::exception& x )
   {
      r.ok = false;
      r.error = String( "apply_process internal error: " ) + String( x.what() );
   }
   catch ( ... )
   {
      r.ok = false;
      r.error = "apply_process internal error: unknown exception";
   }
   return r;
}

String DescribeParameterChanges( const nlohmann::json& parameters, const nlohmann::json& tableParameters,
                                 size_type maxChars )
{
   String s;
   auto add = [&s]( const String& line )
   {
      if ( !s.IsEmpty() )
         s += "\n";
      s += line;
   };
   if ( parameters.is_object() )
      for ( auto it = parameters.begin(); it != parameters.end(); ++it )
         add( S16( it.key() ) + " = "
              + S16( it.value().is_string() ? it.value().get<std::string>() : it.value().dump() ) );
   if ( tableParameters.is_object() )
      for ( auto it = tableParameters.begin(); it != tableParameters.end(); ++it )
         add( S16( it.key() ) + " = " + S16( it.value().dump() ) );
   if ( s.IsEmpty() )
      s = "(all parameters at their defaults)";
   if ( maxChars > 3 && s.Length() > maxChars )
      s = s.Left( maxChars - 3 ) + "...";
   return s;
}

} // namespace pcl
```
In `ProcessCatalog.cpp` `DefaultValueJson`, replace the enumeration case with the following. `ProcessParameter::DefaultValue()` returns the default element's **index** (`GetParameterDefaultElementIndex`, `ProcessParameter.cpp:333-338`), not its value:
```cpp
   case ProcessParameterType::Enumeration:
      {
         // DefaultValue() is the default element's INDEX
         // (GetParameterDefaultElementIndex, ProcessParameter.cpp:333-338),
         // not its value -- index into the element list.
         const int index = v.ToInt();
         const ProcessParameter::enumeration_element_list elements = p.EnumerationElements();
         if ( index >= 0 && size_type( index ) < elements.Length() )
            return std::string( elements[index].id.c_str() );
         return index;
      }
```
Add `ProcessApply.cpp` to `MODULE_SOURCES` after `PICopilotAgentSelfTest.cpp`.

- [ ] **Step 4: Verify GREEN.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) && bash test/run-selftest.sh`
Expected: `PASS: self-test verdict all green`, and `applyDetail.errorCases` has `"pass":true` on all nine. If one expected message differs (for example the core's own enum id order), fix the **code** so the message is precise. Change the expected string only when the core's facts differ (e.g. real element ids), and state that in the report.

- [ ] **Step 5: Commit.**
```bash
cd /home/scarter4work/projects/astro-pi
git add modules/pi-copilot/src/module/ProcessApply.h modules/pi-copilot/src/module/ProcessApply.cpp \
        modules/pi-copilot/src/module/ProcessCatalog.cpp modules/pi-copilot/src/module/PICopilotAgentSelfTest.cpp \
        modules/pi-copilot/src/module/CMakeLists.txt modules/pi-copilot/test/run-selftest.sh
git commit -m "feat(pi-copilot): ApplyProcess -- default instance, checked+read-back params, Validate/CanExecuteOn/ExecuteOn, precise errors

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Tool transport — tools in the body, verbatim content blocks, stop_reason, tool_result image stripping

**Files:**
- Modify: `modules/pi-copilot/src/module/AnthropicClient.h`, `AnthropicClient.cpp`
- Modify: `modules/pi-copilot/src/module/ChatThread.h`, `ChatThread.cpp`
- Modify: `modules/pi-copilot/src/module/VisionTurn.h`, `VisionTurn.cpp`
- Modify: `modules/pi-copilot/src/module/PICopilotAgentSelfTest.cpp` (Section A2), `test/run-selftest.sh`

**Interfaces:**
- Consumes: existing `BuildMessagesRequestBody`, `AnthropicRequest`, `ChatThread`, `StripOlderImages`.
- Produces:
```cpp
struct AnthropicMessage { IsoString role; String content; IsoString imageJpegBase64; nlohmann::json blocks; };
nlohmann::json JpegImageBlock( const IsoString& base64 );
std::string BuildMessagesRequestBody( const IsoString& model, const String& systemPrompt,
                                      const Array<AnthropicMessage>& history, const nlohmann::json& tools = nlohmann::json() );
struct AnthropicResult { bool ok; String text; String error; int httpStatus; bool truncated; std::string stopReason; nlohmann::json contentBlocks; };
AnthropicResult ParseMessagesResponse( int httpStatus, const IsoString& body, const String& transportError );
AnthropicRequest( apiKey, model, systemPrompt, history, url = PICOPILOT_MESSAGES_URL,
                  timeoutSeconds = PICopilotRequestTimeoutSeconds, const nlohmann::json& tools = nlohmann::json() );
ChatThread( apiKey, systemPrompt, history, model = ..., url = ..., timeoutSeconds = ..., const nlohmann::json& tools = nlohmann::json() );
extern const char* const kPICopilotToolImageOmittedNote;   // VisionTurn.h, UTF-8
```

- [ ] **Step 1: Failing test.** In `PICopilotAgentSelfTest.cpp`, add `#include "AnthropicClient.h"` and `#include "VisionTurn.h"`, and insert above the marker:
```cpp
   // ---- Section A2: tool transport (Task 3, no network) --------------------
   {
      bool bodyOk = false, noToolsOk = false, parseToolUseOk = false, parseToolOnlyOk = false,
           parseNoTextOk = false, stripOk = false;
      String error;
      try
      {
         nlohmann::json tool = nlohmann::json::object();
         tool["name"] = "t1";
         tool["description"] = "test tool \xE2\x80\x94 d";
         tool["input_schema"] = { { "type", "object" } };
         nlohmann::json tools = nlohmann::json::array();
         tools.push_back( tool );

         Array<AnthropicMessage> h;
         h.Add( AnthropicMessage{ IsoString( "user" ), String( "hi" ), IsoString() } );
         AnthropicMessage a;
         a.role = "assistant";
         a.blocks = nlohmann::json::array();
         a.blocks.push_back( { { "type", "text" }, { "text", "Looking \xE2\x80\x94 one sec" } } );
         a.blocks.push_back( { { "type", "tool_use" }, { "id", "toolu_x1" }, { "name", "t1" },
                               { "input", { { "id", "PixelMath" } } } } );
         h.Add( a );
         nlohmann::json content = nlohmann::json::array();
         content.push_back( { { "type", "text" }, { "text", "{\"result\":\"ok\"}" } } );
         content.push_back( JpegImageBlock( "/9j/4AAQSkZJRgABAQ==" ) );
         AnthropicMessage u;
         u.role = "user";
         u.blocks = nlohmann::json::array();
         u.blocks.push_back( { { "type", "tool_result" }, { "tool_use_id", "toolu_x1" },
                               { "content", content }, { "is_error", false } } );
         h.Add( u );

         const nlohmann::json j = nlohmann::json::parse( BuildMessagesRequestBody( PICOPILOT_DEFAULT_MODEL, "sys", h, tools ) );
         const nlohmann::json& m = j.at( "messages" );
         bodyOk = j.at( "tools" ).size() == 1 && j["tools"][0].at( "name" ) == "t1"
               && m.at( 0 ).at( "content" ) == "hi"
               && m.at( 1 ).at( "content" ).at( 0 ).at( "text" ) == "Looking \xE2\x80\x94 one sec"
               && m.at( 1 ).at( "content" ).at( 1 ).at( "type" ) == "tool_use"
               && m.at( 2 ).at( "content" ).at( 0 ).at( "tool_use_id" ) == "toolu_x1"
               && m.at( 2 ).at( "content" ).at( 0 ).at( "content" ).at( 1 ).at( "type" ) == "image";
         noToolsOk = !nlohmann::json::parse( BuildMessagesRequestBody( PICOPILOT_DEFAULT_MODEL, "sys", h ) ).contains( "tools" );

         const AnthropicResult p1 = ParseMessagesResponse( 200, IsoString(
            "{\"content\":[{\"type\":\"text\",\"text\":\"Let me look.\"},{\"type\":\"tool_use\",\"id\":\"toolu_1\","
            "\"name\":\"describe_process\",\"input\":{\"id\":\"PixelMath\"}}],\"stop_reason\":\"tool_use\"}" ), String() );
         parseToolUseOk = p1.ok && p1.text == "Let me look." && p1.stopReason == "tool_use"
                       && p1.contentBlocks.size() == 2 && p1.contentBlocks[1].at( "input" ).at( "id" ) == "PixelMath";
         const AnthropicResult p2 = ParseMessagesResponse( 200, IsoString(
            "{\"content\":[{\"type\":\"tool_use\",\"id\":\"toolu_2\",\"name\":\"list_processes\",\"input\":{}}],"
            "\"stop_reason\":\"tool_use\"}" ), String() );
         parseToolOnlyOk = p2.ok && p2.text.IsEmpty() && p2.contentBlocks.size() == 1;
         const AnthropicResult p3 = ParseMessagesResponse( 200, IsoString( "{\"content\":[],\"stop_reason\":\"end_turn\"}" ), String() );
         parseNoTextOk = !p3.ok && p3.error == "response missing expected content/text field";

         // Older tool_result images are replaced by the note; the last message keeps its image.
         Array<AnthropicMessage> h3 = h;
         AnthropicMessage a2 = a;
         a2.blocks[1]["id"] = "toolu_x2";
         h3.Add( a2 );
         AnthropicMessage u2 = u;
         u2.blocks[0]["tool_use_id"] = "toolu_x2";
         h3.Add( u2 );
         StripOlderImages( h3 );
         const std::string once = BuildMessagesRequestBody( PICOPILOT_DEFAULT_MODEL, "sys", h3 );
         StripOlderImages( h3 );
         const std::string twice = BuildMessagesRequestBody( PICOPILOT_DEFAULT_MODEL, "sys", h3 );
         const nlohmann::json& oldC = h3[2].blocks.at( 0 ).at( "content" );
         const nlohmann::json& newC = h3[4].blocks.at( 0 ).at( "content" );
         stripOk = oldC.at( 1 ).at( "type" ) == "text" && oldC.at( 1 ).at( "text" ) == kPICopilotToolImageOmittedNote
                && newC.at( 1 ).at( "type" ) == "image" && once == twice;
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }

      const bool ok = bodyOk && noToolsOk && parseToolUseOk && parseToolOnlyOk && parseNoTextOk && stripOk;
      out["transportBodyOk"] = bodyOk;
      out["transportNoToolsOk"] = noToolsOk;
      out["transportParseToolUseOk"] = parseToolUseOk;
      out["transportParseToolOnlyOk"] = parseToolOnlyOk;
      out["transportParseNoTextOk"] = parseNoTextOk;
      out["transportStripOk"] = stripOk;
      out["transportError"] = U8( error );
      out["toolTransportOk"] = ok;
      allOk = allOk && ok;
   }
```
In `run-selftest.sh` `required_true`, add `'toolTransportOk',`.

- [ ] **Step 2: Verify RED.** Run the build command from Task 2 Step 2.
Expected: compile errors: `'JpegImageBlock' was not declared`, `'ParseMessagesResponse' was not declared`, `'struct pcl::AnthropicMessage' has no member named 'blocks'`.

- [ ] **Step 3: Implement.**

`AnthropicClient.h`: add `#include <nlohmann/json.hpp>`. Replace `struct AnthropicMessage` and `BuildMessagesRequestBody`/`AnthropicResult` declarations with:
```cpp
// One turn of chat history sent to the Anthropic Messages API.
//  - blocks non-null: the message's EXACT content-block array, sent as is
//    (an assistant tool_use turn echoed back verbatim; a user tool_result
//    turn; a user turn merged into a trailing tool_result turn). content and
//    imageJpegBase64 are then ignored. Strings inside are UTF-8.
//  - otherwise, non-empty imageJpegBase64 -> [image(base64 JPEG), text];
//    else a plain string.
struct AnthropicMessage
{
   IsoString      role;               // "user" | "assistant"
   String         content;
   IsoString      imageJpegBase64;    // optional, standard Base64, no data: prefix
   nlohmann::json blocks;             // optional, see above
};

// {"type":"image","source":{"type":"base64","media_type":"image/jpeg","data":...}}
nlohmann::json JpegImageBlock( const IsoString& base64 );

// The Messages API request body (UTF-8 JSON, non-streamed: no "stream" key).
// tools non-null -> a "tools" array. Pure function, any thread. Throws
// std::exception if JSON building fails.
std::string BuildMessagesRequestBody( const IsoString& model, const String& systemPrompt,
                                      const Array<AnthropicMessage>& history,
                                      const nlohmann::json& tools = nlohmann::json() );

struct AnthropicResult
{
   bool           ok = false;
   String         text;          // all "text" content blocks, concatenated in order (may be empty for tool_use)
   String         error;
   int            httpStatus = 0;
   bool           truncated = false; // stop_reason == "max_tokens"
   std::string    stopReason;        // "end_turn" | "tool_use" | "max_tokens" | ...
   nlohmann::json contentBlocks;     // the reply's "content" array, verbatim (echoed back in history)
};

// Parses one Messages API HTTP response. ok=true for a 2xx body with a
// content array that has text, or no text but stop_reason "tool_use".
// Non-2xx: error = the API's error.message, else transportError. Any thread.
AnthropicResult ParseMessagesResponse( int httpStatus, const IsoString& body, const String& transportError );
```
In the `AnthropicRequest` declaration, the constructor becomes:
```cpp
   AnthropicRequest( const String& apiKey, const IsoString& model,
                     const String& systemPrompt, const Array<AnthropicMessage>& history,
                     const String& url = PICOPILOT_MESSAGES_URL,
                     int timeoutSeconds = PICopilotRequestTimeoutSeconds,
                     const nlohmann::json& tools = nlohmann::json() );
```
`AnthropicClient.cpp`: replace `MessageContent` with:
```cpp
nlohmann::json MessageContent( const AnthropicMessage& msg )
{
   if ( !msg.blocks.is_null() )
      return msg.blocks;
   const std::string text = U8( msg.content );
   if ( msg.imageJpegBase64.IsEmpty() )
      return text;
   nlohmann::json blocks = nlohmann::json::array();
   blocks.push_back( JpegImageBlock( msg.imageJpegBase64 ) );   // image first, then the question
   blocks.push_back( { { "type", "text" }, { "text", text } } );
   return blocks;
}
```
Add (namespace `pcl`, outside the anonymous namespace, before `MessageContent`'s namespace block):
```cpp
nlohmann::json JpegImageBlock( const IsoString& base64 )
{
   return { { "type", "image" },
            { "source", { { "type", "base64" },
                          { "media_type", "image/jpeg" },
                          { "data", std::string( base64.c_str() ) } } } };
}
```
`BuildMessagesRequestBody` gains the `tools` parameter. After building `req`, add:
```cpp
   if ( !tools.is_null() )
      req["tools"] = tools;
```
In the `AnthropicRequest` constructor definition, add the `const nlohmann::json& tools` parameter and pass it: `BuildMessagesRequestBody( model, systemPrompt, history, tools )`.

Replace the whole `// --- Parse the response ---` part of `Perform()`, from `nlohmann::json j;` to just before `return result;`, with:
```cpp
   return ParseMessagesResponse( result.httpStatus, sink.buffer, transfer.ErrorInformation() );
```
Delete the now-unused trailing `return result;` of `Perform()`. Add the parser as a free function:
```cpp
AnthropicResult ParseMessagesResponse( int httpStatus, const IsoString& body, const String& transportError )
{
   AnthropicResult result;
   result.httpStatus = httpStatus;

   // json::parse() failing means the body isn't JSON at all -- the only case
   // that gets "unparseable response".
   nlohmann::json j;
   try
   {
      j = nlohmann::json::parse( body.c_str() );
   }
   catch ( ... )
   {
      result.error = String( "unparseable response: " ) + String::UTF8ToUTF16( body.Left( 200 ).c_str() );
      return result;
   }

   if ( httpStatus >= 200 && httpStatus < 300 )
   {
      try
      {
         const nlohmann::json& content = j.at( "content" );
         std::string joined;
         bool anyText = false;
         for ( const nlohmann::json& block : content )
            if ( block.value( "type", std::string() ) == "text" )
            {
               joined += block.at( "text" ).get<std::string>();
               anyText = true;
            }
         result.stopReason = ( j.contains( "stop_reason" ) && j["stop_reason"].is_string() )
                           ? j["stop_reason"].get<std::string>() : std::string();
         // A tool_use reply may carry no text at all; any other reply must.
         if ( !anyText && result.stopReason != "tool_use" )
            throw std::runtime_error( "no text block" );
         result.text = String::UTF8ToUTF16( joined.c_str() );
         result.truncated = result.stopReason == "max_tokens";
         result.contentBlocks = content;
         result.ok = true;
      }
      catch ( ... )
      {
         result.ok = false;
         result.text.Clear();
         result.stopReason.clear();
         result.contentBlocks = nlohmann::json();
         result.error = "response missing expected content/text field";
      }
   }
   else
   {
      // "error"/"message" present but not a string must not throw.
      std::string msg;
      try
      {
         msg = ( j.contains( "error" ) && j["error"].contains( "message" ) )
            ? j["error"]["message"].get<std::string>()
            : U8( transportError );
      }
      catch ( ... )
      {
         msg = U8( transportError );
      }
      result.error = String::UTF8ToUTF16( msg.c_str() );
      result.ok = false;
   }
   return result;
}
```
`ChatThread.h/.cpp`: add `#include <nlohmann/json.hpp>`. The constructor gains a trailing `const nlohmann::json& tools = nlohmann::json()` and initializes `m_request( apiKey, model, systemPrompt, history, url, timeoutSeconds, tools )`.

`VisionTurn.h`: add
```cpp
// Replaces an image inside an older content-block array (a tool_result preview,
// or a merged user turn) in re-sent history. UTF-8.
extern const char* const kPICopilotToolImageOmittedNote;
```
and extend the `StripOlderImages` comment: `// - a message with content blocks: every image block (top level or inside a tool_result's content) becomes a text block kPICopilotToolImageOmittedNote, and a top-level text block's leading view-context is collapsed.`

`VisionTurn.cpp`: add the constant and a helper in the anonymous namespace, and handle blocks in `StripOlderImages`:
```cpp
const char* const kPICopilotToolImageOmittedNote =
   "[A preview image was attached here when it was sent; it is omitted from the re-sent history.]";
```
```cpp
// In place: every image block (top level, or inside a tool_result's content
// array) becomes a text note. Idempotent (a note is text).
void StripBlockImages( nlohmann::json& blocks )
{
   for ( nlohmann::json& b : blocks )
   {
      if ( !b.is_object() )
         continue;
      const std::string type = b.value( "type", std::string() );
      if ( type == "image" )
         b = { { "type", "text" }, { "text", kPICopilotToolImageOmittedNote } };
      else if ( type == "tool_result" && b.contains( "content" ) && b["content"].is_array() )
         StripBlockImages( b["content"] );
   }
}
```
In `StripOlderImages`, inside the loop, right after `if ( m.role != "user" ) continue;`, add:
```cpp
      if ( m.blocks.is_array() )
      {
         StripBlockImages( m.blocks );
         for ( nlohmann::json& b : m.blocks )
            if ( b.is_object() && b.value( "type", std::string() ) == "text" && b.contains( "text" ) && b["text"].is_string() )
               b["text"] = U8( CollapseViewContext(
                  String::UTF8ToUTF16( b["text"].get<std::string>().c_str() ), String() ) );
         continue;
      }
```
(`kPICopilotToolImageOmittedNote` is defined above `StripBlockImages` in the file, so the helper can see it. Move the anonymous namespace below it if needed.)

- [ ] **Step 4: Verify GREEN (and the increment-3 regression).**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) && bash test/run-selftest.sh`
Expected: `PASS: self-test verdict all green`. `utf8BodyOk`, `twoTurnOk` and `visionTurnOk` are still true: the plain-message path is byte-identical.

- [ ] **Step 5: Commit.**
```bash
cd /home/scarter4work/projects/astro-pi
git add modules/pi-copilot/src/module/AnthropicClient.h modules/pi-copilot/src/module/AnthropicClient.cpp \
        modules/pi-copilot/src/module/ChatThread.h modules/pi-copilot/src/module/ChatThread.cpp \
        modules/pi-copilot/src/module/VisionTurn.h modules/pi-copilot/src/module/VisionTurn.cpp \
        modules/pi-copilot/src/module/PICopilotAgentSelfTest.cpp modules/pi-copilot/test/run-selftest.sh
git commit -m "feat(pi-copilot): tool transport -- tools in body, verbatim content blocks, stop_reason, ParseMessagesResponse, tool_result image stripping

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: AgentTools + SystemPrompt — schemas per mode, dispatcher, Guided confirm, tone rules

**Files:**
- Create: `modules/pi-copilot/src/module/AgentTools.h`, `AgentTools.cpp`
- Create: `modules/pi-copilot/src/module/SystemPrompt.h`, `SystemPrompt.cpp`
- Modify: `modules/pi-copilot/src/module/PICopilotAgentSelfTest.cpp` (Section A3), `CMakeLists.txt`, `test/run-selftest.sh`

**Interfaces:**
- Consumes: `ApplyProcess`, `DescribeParameterChanges` (Task 2), `JpegImageBlock` (Task 3), `ListProcesses`/`DescribeProcess`, `BuildViewContext`, `RenderViewPreview`.
- Produces:
```cpp
enum class AgentMode { Copilot = 0, Advisor = 1, Guided = 2 };        // == Mode_ComboBox item index
AgentMode AgentModeFromIndex( int index );
constexpr size_type PICopilotToolLogParamChars = 120;
constexpr size_type PICopilotConfirmChangesChars = 1500;
nlohmann::json ToolDefinitions( AgentMode mode );                    // Advisor omits apply_process
struct ToolCall { std::string id; std::string name; nlohmann::json input; };
struct ToolOutcome { bool isError = false; nlohmann::json content = array; String logLine; };
using ConfirmApplyFn = std::function<bool( const String& processId, const String& viewId, const String& changes )>;
struct ToolContext { AgentMode mode; std::function<View()> activeView; ConfirmApplyFn confirm; };
ToolOutcome ExecuteTool( const ToolCall& call, const ToolContext& ctx );   // root thread, never throws
nlohmann::json ToolResultBlock( const std::string& toolUseId, const ToolOutcome& o );
nlohmann::json CollapsedViewContext( const nlohmann::json& fullContext );  // viewId, fullId, geometry, channelStats
extern const char* const kPICopilotToneGuidance;                      // SystemPrompt.h, UTF-8
extern const char* const kPICopilotToneMarkers[7];
String BuildSystemPrompt( AgentMode mode );
```
Log-line format (plain text; the panel escapes it): `▶ <tool> <args> → ok (<s> s)`, `✖ <tool> <args> → error: <message>`, `✖ apply_process <args> → declined by user`.

- [ ] **Step 1: Failing test.** Add `#include "AgentTools.h"`, `#include "SystemPrompt.h"` and `#include <functional>`, then insert above the marker:
```cpp
   // ---- Section A3: tools + system prompt (Task 4) --------------------------
   {
      bool schemaOk = false, promptOk = true, htColumnsOk = false, dispatchOk = false, applyToolOk = false,
           declineOk = false, approveOk = false, advisorOk = false, noViewOk = false;
      nlohmann::json promptOut = nlohmann::json::object();
      String error;
      try
      {
         auto names = []( const nlohmann::json& tools )
         {
            std::vector<std::string> n;
            for ( const nlohmann::json& t : tools )
               n.push_back( t.at( "name" ).get<std::string>() );
            return n;
         };
         const std::vector<std::string> all = { "list_processes", "describe_process", "get_view_context", "apply_process" };
         const std::vector<std::string> readOnly = { "list_processes", "describe_process", "get_view_context" };
         const nlohmann::json tc = ToolDefinitions( AgentMode::Copilot );
         const nlohmann::json tg = ToolDefinitions( AgentMode::Guided );
         const nlohmann::json ta = ToolDefinitions( AgentMode::Advisor );
         bool shapes = true;
         for ( const nlohmann::json* set : { &tc, &tg, &ta } )
            for ( const nlohmann::json& t : *set )
               shapes = shapes && t.at( "input_schema" ).at( "type" ) == "object"
                     && t.at( "description" ).is_string() && !t.at( "description" ).get<std::string>().empty();
         schemaOk = shapes && names( tc ) == all && names( tg ) == all && names( ta ) == readOnly
                 && tc.at( 3 ).at( "input_schema" ).at( "required" ) == nlohmann::json::array( { "process_id" } )
                 && tc.at( 3 ).at( "input_schema" ).at( "properties" ).contains( "table_parameters" )
                 && tc.at( 1 ).at( "input_schema" ).at( "required" ) == nlohmann::json::array( { "id" } );

         for ( AgentMode m : { AgentMode::Copilot, AgentMode::Guided, AgentMode::Advisor } )
         {
            const String p = BuildSystemPrompt( m );
            bool ok = p.Contains( String::UTF8ToUTF16( kPICopilotToneGuidance ) );
            for ( const char* marker : kPICopilotToneMarkers )
               ok = ok && p.Contains( String::UTF8ToUTF16( marker ) );
            const char* modeMarker = m == AgentMode::Copilot ? "MODE: Copilot"
                                   : m == AgentMode::Guided ? "MODE: Guided" : "MODE: Advisor";
            ok = ok && p.Contains( String( modeMarker ) );
            if ( m == AgentMode::Advisor )
               ok = ok && !p.Contains( String( "apply_process {" ) );
            else
               ok = ok && p.Contains( String( "apply_process {" ) ) && p.Contains( String( "History" ) );
            promptOut[modeMarker] = ok;
            promptOk = promptOk && ok;
         }

         {  // The HT example in the prompt must name the real column ids, in order.
            Process H( IsoString( "HistogramTransformation" ) );
            ProcessParameter t( H, IsoString( "H" ) );
            String ids;
            for ( const ProcessParameter& c : t.TableColumns() )
            {
               if ( !ids.IsEmpty() )
                  ids += ", ";
               ids += String( c.Id() );
            }
            htColumnsOk = BuildSystemPrompt( AgentMode::Copilot ).Contains( "of columns " + ids + "; to set" );
         }

         AgentTestWindow tw( "PICopilotTools" );
         const View v = tw.MainView();
         ToolContext ctx;
         ctx.mode = AgentMode::Copilot;
         ctx.activeView = [v]() -> View { return v; };
         const ToolOutcome d1 = ExecuteTool( ToolCall{ "t1", "describe_process", { { "id", "PixelMath" } } }, ctx );
         const ToolOutcome d2 = ExecuteTool( ToolCall{ "t2", "describe_process", { { "id", "NoSuchProcessXYZ" } } }, ctx );
         const ToolOutcome g  = ExecuteTool( ToolCall{ "t3", "get_view_context", { { "include_preview", true } } }, ctx );
         const ToolOutcome u  = ExecuteTool( ToolCall{ "t4", "no_such_tool", nlohmann::json::object() }, ctx );
         const ToolOutcome l  = ExecuteTool( ToolCall{ "t5", "list_processes", nlohmann::json::object() }, ctx );
         dispatchOk = !d1.isError && d1.content.at( 0 ).at( "text" ).get<std::string>().find( "\"expression\"" ) != std::string::npos
                   && d2.isError
                   && !g.isError && g.content.size() == 2
                   && g.content.at( 0 ).at( "text" ).get<std::string>().find( "channelStats" ) != std::string::npos
                   && g.content.at( 1 ).at( "type" ) == "image"
                   && u.isError && u.content.at( 0 ).at( "text" ).get<std::string>().find( "unknown tool 'no_such_tool'" ) != std::string::npos
                   && !l.isError && l.logLine.StartsWith( String::UTF8ToUTF16( "\xE2\x96\xB6 list_processes" ) );

         const ToolCall halve{ "t6", "apply_process",
                               { { "process_id", "PixelMath" }, { "parameters", { { "expression", "$T*0.5" } } } } };
         {  // Copilot: applies; summary + collapsed context + fresh preview.
            AgentTestWindow tw2( "PICopilotToolApply" );
            const View v2 = tw2.MainView();
            ToolContext c2 = ctx;
            c2.activeView = [v2]() -> View { return v2; };
            const double before = ChannelMedian( v2, 0 );
            const ToolOutcome o = ExecuteTool( halve, c2 );
            const double after = ChannelMedian( v2, 0 );
            const nlohmann::json summary = o.isError ? nlohmann::json::object()
                                         : nlohmann::json::parse( o.content.at( 0 ).at( "text" ).get<std::string>() );
            applyToolOk = !o.isError && summary.value( "result", std::string() ) == "ok"
                       && summary.at( "newContext" ).contains( "channelStats" ) && !summary.at( "newContext" ).contains( "fitsKeywords" )
                       && o.content.size() == 2 && o.content.at( 1 ).at( "type" ) == "image"
                       && o.logLine.StartsWith( String::UTF8ToUTF16( "\xE2\x96\xB6 apply_process PixelMath" ) )
                       && std::fabs( after - 0.5*before ) < 1e-5;
         }
         {  // Guided: decline -> nothing changes; approve -> applied. Advisor: refused.
            AgentTestWindow tw3( "PICopilotToolGuided" );
            const View v3 = tw3.MainView();
            String gotPid, gotView, gotChanges;
            int asked = 0;
            ToolContext c3;
            c3.mode = AgentMode::Guided;
            c3.activeView = [v3]() -> View { return v3; };
            c3.confirm = [&]( const String& pid, const String& view, const String& changes )
            {
               ++asked; gotPid = pid; gotView = view; gotChanges = changes;
               return false;
            };
            const double before = ChannelMedian( v3, 0 );
            const ToolOutcome no = ExecuteTool( halve, c3 );
            declineOk = no.isError && asked == 1 && gotPid == "PixelMath" && gotView == String( v3.FullId() )
                     && gotChanges.Contains( "expression = $T*0.5" )
                     && no.content.at( 0 ).at( "text" ).get<std::string>().find( "declined" ) != std::string::npos
                     && no.logLine.EndsWith( "declined by user" )
                     && std::fabs( ChannelMedian( v3, 0 ) - before ) < 1e-12;
            c3.confirm = [&]( const String&, const String&, const String& ) { ++asked; return true; };
            const ToolOutcome yes = ExecuteTool( halve, c3 );
            approveOk = !yes.isError && asked == 2 && std::fabs( ChannelMedian( v3, 0 ) - 0.5*before ) < 1e-5;

            ToolContext c4 = c3;
            c4.mode = AgentMode::Advisor;
            const double mid = ChannelMedian( v3, 0 );
            const ToolOutcome adv = ExecuteTool( halve, c4 );
            advisorOk = adv.isError && asked == 2
                     && adv.content.at( 0 ).at( "text" ).get<std::string>().find( "not available in Advisor mode" ) != std::string::npos
                     && std::fabs( ChannelMedian( v3, 0 ) - mid ) < 1e-12;
         }
         {  // No active image: precise error from both view tools.
            ToolContext c5;
            c5.mode = AgentMode::Copilot;
            c5.activeView = []() -> View { return View::Null(); };
            const ToolOutcome nv = ExecuteTool( halve, c5 );
            const ToolOutcome ng = ExecuteTool( ToolCall{ "t8", "get_view_context", nlohmann::json::object() }, c5 );
            noViewOk = nv.isError && ng.isError
                    && ng.content.at( 0 ).at( "text" ).get<std::string>().find( "no active image" ) != std::string::npos;
         }
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }

      const bool ok = schemaOk && promptOk && htColumnsOk && dispatchOk && applyToolOk
                   && declineOk && approveOk && advisorOk && noViewOk;
      out["toolsSchemaOk"] = schemaOk;
      out["toolsPromptToneOk"] = promptOk;
      out["toolsPromptModes"] = promptOut;
      out["toolsHtColumnsOk"] = htColumnsOk;
      out["toolsDispatchOk"] = dispatchOk;
      out["toolsApplyOk"] = applyToolOk;
      out["toolsGuidedDeclineOk"] = declineOk;
      out["toolsGuidedApproveOk"] = approveOk;
      out["toolsAdvisorRefusesOk"] = advisorOk;
      out["toolsNoViewOk"] = noViewOk;
      out["toolsError"] = U8( error );
      out["agentToolsOk"] = ok;
      allOk = allOk && ok;
   }
```
In `run-selftest.sh` `required_true`, add `'agentToolsOk',`.

- [ ] **Step 2: Verify RED.** Run the Task 2 Step 2 build command.
Expected: `AgentTools.h: No such file or directory`.

- [ ] **Step 3: Implement.** `AgentTools.h`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_AgentTools_h
#define PICopilot_AgentTools_h

#include <pcl/String.h>
#include <pcl/View.h>

#include <nlohmann/json.hpp>

#include <functional>
#include <string>

namespace pcl
{

// Assistant mode. Values == the panel's Mode_ComboBox item indices.
enum class AgentMode { Copilot = 0, Advisor = 1, Guided = 2 };

// Out-of-range indices (e.g. a corrupt Setting) fall back to Copilot.
AgentMode AgentModeFromIndex( int index );

constexpr size_type PICopilotToolLogParamChars   = 120;   // parameters shown in a chat-log tool line
constexpr size_type PICopilotConfirmChangesChars = 1500;  // parameter text in the Guided dialog

// The Anthropic "tools" array for a mode: list_processes, describe_process,
// get_view_context, and -- except in Advisor -- apply_process.
nlohmann::json ToolDefinitions( AgentMode mode );

struct ToolCall
{
   std::string    id;      // tool_use id
   std::string    name;
   nlohmann::json input;   // object
};

struct ToolOutcome
{
   bool           isError = false;
   nlohmann::json content = nlohmann::json::array();  // tool_result content blocks (text / image), never empty
   String         logLine;                            // compact plain-text chat-log line
};

// Guided mode: asked before each apply_process; true = the user approved.
using ConfirmApplyFn = std::function<bool( const String& processId, const String& viewId, const String& changes )>;

struct ToolContext
{
   AgentMode             mode = AgentMode::Copilot;
   std::function<View()> activeView;   // default target, resolved at call time (may return View::Null())
   ConfirmApplyFn        confirm;      // required in Guided mode
};

// Executes one tool call. Root thread only (views, processes, previews,
// and the Guided dialog). Never throws: every failure is isError=true with a
// precise, model-correctable message in content[0].
ToolOutcome ExecuteTool( const ToolCall& call, const ToolContext& ctx );

// {"type":"tool_result","tool_use_id":..,"content":[..],"is_error":..}
nlohmann::json ToolResultBlock( const std::string& toolUseId, const ToolOutcome& outcome );

// The small, re-sendable part of a BuildViewContext() result:
// viewId, fullId, geometry, channelStats (no FITS keywords).
nlohmann::json CollapsedViewContext( const nlohmann::json& fullContext );

} // namespace pcl

#endif // PICopilot_AgentTools_h
```
`AgentTools.cpp`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "AgentTools.h"
#include "AnthropicClient.h"   // JpegImageBlock
#include "ProcessApply.h"
#include "ProcessCatalog.h"
#include "Utf8.h"
#include "ViewContext.h"
#include "ViewPreview.h"

#include <pcl/Exception.h>

#include <chrono>
#include <cmath>
#include <exception>

namespace pcl
{

namespace
{

using clock = std::chrono::steady_clock;

const char* const kOkMarkUtf8  = "\xE2\x96\xB6 ";   // "▶ "
const char* const kErrMarkUtf8 = "\xE2\x9C\x96 ";   // "✖ "
const char* const kArrowUtf8   = " \xE2\x86\x92 ";  // " → "

String S16( const std::string& s )
{
   return String::UTF8ToUTF16( s.c_str() );
}

String Shorten( const String& s, size_type n )
{
   return (s.Length() <= n || n < 4) ? s : s.Left( n - 3 ) + "...";
}

nlohmann::json TextBlock( const std::string& utf8 )
{
   return { { "type", "text" }, { "text", utf8 } };
}

std::string StringField( const nlohmann::json& in, const char* key )
{
   return (in.contains( key ) && in[key].is_string()) ? in[key].get<std::string>() : std::string();
}

bool BoolField( const nlohmann::json& in, const char* key )
{
   return in.contains( key ) && in[key].is_boolean() && in[key].get<bool>();
}

String OkLine( const String& what, clock::time_point t0 )
{
   const double s = std::chrono::duration<double>( clock::now() - t0 ).count();
   return S16( kOkMarkUtf8 ) + what + S16( kArrowUtf8 ) + String().Format( "ok (%.1f s)", s );
}

ToolOutcome Fail( const String& what, const String& error )
{
   ToolOutcome o;
   o.isError = true;
   o.content.push_back( TextBlock( U8( error ) ) );
   o.logLine = S16( kErrMarkUtf8 ) + what + S16( kArrowUtf8 ) + "error: " + Shorten( error, 200 );
   return o;
}

bool IsBusy( const View& v )
{
   bool busy = true;
   try
   {
      busy = !v.CanRead() || !v.CanWrite();
   }
   catch ( ... )
   {
   }
   return busy;
}

View DefaultView( const ToolContext& ctx )
{
   return ctx.activeView ? ctx.activeView() : View::Null();
}

ToolOutcome ApplyProcessTool( const nlohmann::json& in, const ToolContext& ctx, clock::time_point t0 )
{
   const std::string pid = StringField( in, "process_id" );
   const nlohmann::json params = in.contains( "parameters" ) ? in["parameters"] : nlohmann::json::object();
   const nlohmann::json tables = in.contains( "table_parameters" ) ? in["table_parameters"] : nlohmann::json::object();
   nlohmann::json shown = params.is_object() ? params : nlohmann::json::object();
   if ( tables.is_object() )
      for ( auto it = tables.begin(); it != tables.end(); ++it )
         shown[it.key()] = it.value();
   const String what = "apply_process " + S16( pid ) + " " + Shorten( S16( shown.dump() ), PICopilotToolLogParamChars );

   if ( ctx.mode == AgentMode::Advisor )
      return Fail( what, "apply_process is not available in Advisor mode (read-only); give the user the settings instead" );
   if ( pid.empty() )
      return Fail( "apply_process", "apply_process needs process_id; call list_processes for valid ids" );

   View target;
   const std::string viewId = StringField( in, "view_id" );
   if ( !viewId.empty() )
   {
      target = View::ViewById( IsoString( viewId.c_str() ) );
      if ( target.IsNull() )
         return Fail( what, "no view with id '" + S16( viewId ) + "'" );
   }
   else
   {
      target = DefaultView( ctx );
      if ( target.IsNull() )
         return Fail( what, "no active image: ask the user to open or select an image, or pass view_id" );
   }

   if ( ctx.mode == AgentMode::Guided )
   {
      if ( !ctx.confirm )
         return Fail( what, "internal error: Guided mode has no confirmation callback" );
      const String changes = DescribeParameterChanges( params, tables, PICopilotConfirmChangesChars );
      if ( !ctx.confirm( S16( pid ), String( target.FullId() ), changes ) )
      {
         ToolOutcome o;
         o.isError = true;
         o.content.push_back( TextBlock( "The user declined this apply_process call (" + pid + " on "
                                         + std::string( target.FullId().c_str() )
                                         + "). Nothing was changed. Do not repeat it; ask what they would like instead." ) );
         o.logLine = S16( kErrMarkUtf8 ) + what + S16( kArrowUtf8 ) + "declined by user";
         return o;
      }
   }

   const ApplyProcessResult ar = ApplyProcess( IsoString( pid.c_str() ), params, tables, target );
   if ( !ar.ok )
      return Fail( what, ar.error );

   nlohmann::json summary = {
      { "result", "ok" },
      { "process", U8( ar.processId ) },
      { "view", U8( ar.viewId ) },
      { "parametersSet", ar.parametersSet },
      { "elapsedMs", std::lround( ar.elapsedMs ) },
      { "undo", "Recorded in the view's History; the user can undo it." }
   };
   try
   {
      summary["newContext"] = CollapsedViewContext( BuildViewContext( target ) );
   }
   catch ( const pcl::Exception& x )
   {
      summary["newContextError"] = U8( x.Message() );
   }
   const ViewPreviewResult p = RenderViewPreview( target );
   if ( !p.ok )
      summary["previewError"] = U8( p.error );

   ToolOutcome o;
   o.content.push_back( TextBlock( summary.dump() ) );
   if ( p.ok )
      o.content.push_back( JpegImageBlock( p.base64 ) );
   o.logLine = OkLine( what, t0 );
   return o;
}

} // namespace

AgentMode AgentModeFromIndex( int index )
{
   return index == 1 ? AgentMode::Advisor : index == 2 ? AgentMode::Guided : AgentMode::Copilot;
}

nlohmann::json ToolDefinitions( AgentMode mode )
{
   nlohmann::json tools = nlohmann::json::array();

   nlohmann::json list = nlohmann::json::object();
   list["name"] = "list_processes";
   list["description"] = "List the processes installed in this PixInsight: id, categories, and a one-line summary when known.";
   list["input_schema"] = { { "type", "object" }, { "properties", nlohmann::json::object() } };
   tools.push_back( list );

   nlohmann::json describe = nlohmann::json::object();
   describe["name"] = "describe_process";
   describe["description"] = "Describe one process's parameters: id, type, default, numeric range, enumeration "
                             "element ids, and table columns in row order. Call it before setting parameters "
                             "you have not used yet in this conversation.";
   describe["input_schema"] = { { "type", "object" },
                                { "properties", { { "id", { { "type", "string" }, { "description", "Process id, e.g. PixelMath" } } } } },
                                { "required", nlohmann::json::array( { "id" } ) } };
   tools.push_back( describe );

   nlohmann::json context = nlohmann::json::object();
   context["name"] = "get_view_context";
   context["description"] = "Fresh facts about the active view: geometry, per-channel median/MAD/mean/min/max of the "
                            "real data, FITS keywords; with include_preview, also a new auto-stretched preview image.";
   context["input_schema"] = { { "type", "object" },
                               { "properties", { { "include_preview", { { "type", "boolean" },
                                                                        { "description", "Attach a fresh preview image (default false)." } } } } } };
   tools.push_back( context );

   if ( mode != AgentMode::Advisor )
   {
      nlohmann::json props = nlohmann::json::object();
      props["process_id"] = { { "type", "string" }, { "description", "Process id, e.g. PixelMath" } };
      props["parameters"] = { { "type", "object" },
                              { "description", "{parameterId: value} for non-table parameters. Enumerations take the element id as a string." } };
      props["table_parameters"] = { { "type", "object" },
                                    { "description", "{tableParameterId: [[row values in column order], ...]}; replaces the whole table." } };
      props["view_id"] = { { "type", "string" }, { "description", "Target view id; default: the active view." } };
      nlohmann::json apply = nlohmann::json::object();
      apply["name"] = "apply_process";
      apply["description"] = "Run a PixInsight process on the user's real image (recorded in the view's History, so "
                             "it can be undone). Starts from the process's DEFAULT settings and sets only the "
                             "parameters given. Returns ok with the new statistics and a fresh preview, or a precise error.";
      apply["input_schema"] = { { "type", "object" }, { "properties", props },
                                { "required", nlohmann::json::array( { "process_id" } ) } };
      tools.push_back( apply );
   }
   return tools;
}

nlohmann::json CollapsedViewContext( const nlohmann::json& full )
{
   nlohmann::json c = nlohmann::json::object();
   for ( const char* k : { "viewId", "fullId", "geometry", "channelStats" } )
      if ( full.contains( k ) )
         c[k] = full[k];
   return c;
}

nlohmann::json ToolResultBlock( const std::string& toolUseId, const ToolOutcome& o )
{
   nlohmann::json content = o.content;
   if ( !content.is_array() || content.empty() )
   {
      content = nlohmann::json::array();
      content.push_back( TextBlock( o.isError ? "error (no detail available)" : "ok" ) );
   }
   return { { "type", "tool_result" }, { "tool_use_id", toolUseId }, { "content", content }, { "is_error", o.isError } };
}

ToolOutcome ExecuteTool( const ToolCall& call, const ToolContext& ctx )
{
   const clock::time_point t0 = clock::now();
   const String name = S16( call.name );
   const nlohmann::json in = call.input.is_object() ? call.input : nlohmann::json::object();
   try
   {
      if ( call.name == "list_processes" )
      {
         ToolOutcome o;
         o.content.push_back( TextBlock( ListProcesses().dump() ) );
         o.logLine = OkLine( name, t0 );
         return o;
      }
      if ( call.name == "describe_process" )
      {
         const std::string id = StringField( in, "id" );
         if ( id.empty() )
            return Fail( name, "describe_process needs {\"id\": \"<process id>\"}; call list_processes for ids" );
         const String what = name + " " + S16( id );
         const nlohmann::json d = DescribeProcess( IsoString( id.c_str() ) );
         if ( d.contains( "error" ) && !d.contains( "parameters" ) )
            return Fail( what, S16( d["error"].get<std::string>() ) );
         ToolOutcome o;
         o.content.push_back( TextBlock( d.dump() ) );
         o.logLine = OkLine( what, t0 );
         return o;
      }
      if ( call.name == "get_view_context" )
      {
         const View view = DefaultView( ctx );
         if ( view.IsNull() )
            return Fail( name, "no active image: ask the user to open or select an image" );
         const String what = name + " " + String( view.FullId() );
         if ( IsBusy( view ) )
            return Fail( what, "view " + String( view.FullId() ) + " is busy (locked by a running process); try again when it finishes" );
         ToolOutcome o;
         o.content.push_back( TextBlock( BuildViewContext( view ).dump() ) );
         if ( BoolField( in, "include_preview" ) )
         {
            const ViewPreviewResult p = RenderViewPreview( view );
            if ( p.ok )
               o.content.push_back( JpegImageBlock( p.base64 ) );
            else
               o.content.push_back( TextBlock( "preview failed: " + U8( p.error ) ) );
         }
         o.logLine = OkLine( what, t0 );
         return o;
      }
      if ( call.name == "apply_process" )
         return ApplyProcessTool( in, ctx, t0 );
      return Fail( name, "unknown tool '" + name + "'; available: list_processes, describe_process, get_view_context"
                         + (ctx.mode == AgentMode::Advisor ? String() : String( ", apply_process" )) );
   }
   catch ( const pcl::Exception& x )
   {
      return Fail( name, name + " failed: " + x.Message() );
   }
   catch ( const std::exception& x )
   {
      return Fail( name, name + " failed: " + String( x.what() ) );
   }
   catch ( ... )
   {
      return Fail( name, name + " failed: unknown error" );
   }
}

} // namespace pcl
```
`SystemPrompt.h`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_SystemPrompt_h
#define PICopilot_SystemPrompt_h

#include "AgentTools.h"   // AgentMode

#include <pcl/String.h>

namespace pcl
{

// How the model treats the user -- in EVERY mode's prompt, verbatim. UTF-8.
extern const char* const kPICopilotToneGuidance;

// Phrases the self-test requires in kPICopilotToneGuidance (and so in every
// prompt), so the tone rules cannot silently regress.
extern const char* const kPICopilotToneMarkers[7];

// The full system prompt for one mode.
String BuildSystemPrompt( AgentMode mode );

} // namespace pcl

#endif // PICopilot_SystemPrompt_h
```
`SystemPrompt.cpp`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "SystemPrompt.h"

#include <string>

namespace pcl
{

const char* const kPICopilotToneGuidance =
   "How to work with the user:\n"
   "- Do what the user asks first. When apply_process is available, that means doing it, not describing how, "
   "and then saying briefly what you did.\n"
   "- Respect the user's choices about their own data, palette and workflow. If they ask for something "
   "unconventional, such as SPCC on dual-band data for an SHO palette, set it up for their goal instead of "
   "arguing about it.\n"
   "- Mention a genuine caveat at most once, briefly and constructively (what to watch for, or a tip), after "
   "the answer. Never make it a gate or a lecture, and never repeat it.\n"
   "- Be warm, encouraging and collaborative, like a friendly expert working alongside the user. No "
   "condescension, no \"you must\", no scolding, and no long lists of alternatives nobody asked for.\n"
   "- Ask a clarifying question only when the request is genuinely ambiguous AND acting on a guess would be "
   "destructive. Otherwise make a sensible choice, say what you chose, and proceed.\n"
   "- Keep replies concise.\n";

const char* const kPICopilotToneMarkers[7] =
{
   "Do what the user asks first",
   "Respect the user's choices",
   "at most once",
   "Never make it a gate or a lecture",
   "warm, encouraging and collaborative",
   "Ask a clarifying question only when",
   "Keep replies concise"
};

namespace
{

const char* const kIntro =
   "You are PI Copilot, an assistant embedded in PixInsight, the astronomical image processing application, "
   "working alongside the user on their astrophotography data.\n\n";

const char* const kCopilotMode =
   "MODE: Copilot. apply_process runs immediately on the user's real image. Every run is recorded in the view's "
   "History, so the user can undo it (Edit > Undo, or the History Explorer); mention that once, briefly, after "
   "your first change.\n\n";

const char* const kGuidedMode =
   "MODE: Guided. Each apply_process call first shows the user a confirmation dialog listing the process and its "
   "parameters. If a tool_result says the user declined, do not repeat that call; ask what they would like "
   "instead. Approved runs are recorded in the view's History and can be undone.\n\n";

const char* const kAdvisorMode =
   "MODE: Advisor. You have read-only tools and cannot change the image. When the user wants something done, "
   "give the exact process and parameter values (use describe_process for the ids) so they can apply it "
   "themselves.\n\n";

const char* const kReadTools =
   "Tools:\n"
   "- list_processes: the installed PixInsight processes (ids, categories, one-line summaries).\n"
   "- describe_process {id}: a process's parameters: ids, types, ranges, enumeration element ids, table columns "
   "and defaults.\n"
   "- get_view_context {include_preview}: fresh statistics of the active view and, if include_preview is true, a "
   "new preview.\n";

const char* const kApplyTool =
   "- apply_process {process_id, parameters, table_parameters, view_id}: runs a process on the user's REAL image "
   "(the active view unless view_id is given).\n";

const char* const kApplyIdioms =
   "\nUsing apply_process:\n"
   "- It starts from the process's DEFAULT settings and changes only the parameters you pass, so pass every "
   "non-default value you need.\n"
   "- Call describe_process before setting parameters you have not used in this conversation; never guess "
   "parameter ids.\n"
   "- Enumeration parameters take the element id as a string, e.g. {\"colorToRemove\": \"Green\"}.\n"
   "- Table parameters go in table_parameters as a list of rows, each row giving every column in describe_process "
   "order. Example: HistogramTransformation H has 5 rows (R, G, B, RGB/K, alpha) of columns c0, m, c1, r0, r1; "
   "to set midtones 0.25 on the combined channel send {\"H\": [[0,0.5,1,0,1],[0,0.5,1,0,1],[0,0.5,1,0,1],"
   "[0,0.25,1,0,1],[0,0.5,1,0,1]]}.\n"
   "- PixelMath: {\"expression\": \"$T*0.5\"} ($T is the target image; useSingleExpression is on by default).\n"
   "- If a call fails, its tool_result says exactly why (unknown parameter, value out of range, invalid "
   "enumeration id, the process cannot run on this view, the user declined). Fix the call and try again; never "
   "say a change was made unless its tool_result says \"ok\".\n"
   "- After a successful call the tool_result has the view's new statistics and a fresh preview: check the result "
   "against the goal before you report.\n"
   "- Processes that only run globally (e.g. ImageIntegration, or anything that builds new images from files) are "
   "not supported by apply_process yet: say so and give the settings instead.\n";

const char* const kVision =
   "\nA user message may begin with a [PixInsight view context] block (JSON: view identity, geometry, per-channel "
   "statistics, FITS keywords) and may include an image. Images are automatically stretched (auto-STF) JPEG "
   "previews, downscaled to at most 1024 px, for DISPLAY ONLY: the underlying data is usually still LINEAR "
   "(unstretched). Base any statement about levels, noise or clipping on the statistics, which describe the real "
   "data (mad is the raw median absolute deviation; multiply by 1.4826 for sigma). Only the latest message carries "
   "images; earlier ones are omitted from history.\n\n";

} // namespace

String BuildSystemPrompt( AgentMode mode )
{
   std::string p = kIntro;
   p += mode == AgentMode::Copilot ? kCopilotMode : mode == AgentMode::Guided ? kGuidedMode : kAdvisorMode;
   p += kReadTools;
   if ( mode != AgentMode::Advisor )
   {
      p += kApplyTool;
      p += kApplyIdioms;
   }
   p += kVision;
   p += kPICopilotToneGuidance;
   return String::UTF8ToUTF16( p.c_str() );
}

} // namespace pcl
```
Add `AgentTools.cpp` and `SystemPrompt.cpp` to `MODULE_SOURCES`.

- [ ] **Step 4: Verify GREEN.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) && bash test/run-selftest.sh`
Expected: `PASS: self-test verdict all green`. `toolsPromptModes` shows all three modes `true`, and `toolsHtColumnsOk` is true.

- [ ] **Step 5: Commit.**
```bash
cd /home/scarter4work/projects/astro-pi
git add modules/pi-copilot/src/module/AgentTools.h modules/pi-copilot/src/module/AgentTools.cpp \
        modules/pi-copilot/src/module/SystemPrompt.h modules/pi-copilot/src/module/SystemPrompt.cpp \
        modules/pi-copilot/src/module/PICopilotAgentSelfTest.cpp modules/pi-copilot/src/module/CMakeLists.txt \
        modules/pi-copilot/test/run-selftest.sh
git commit -m "feat(pi-copilot): tool schemas per mode, root-thread tool dispatcher with Guided confirm, per-mode system prompt with tone rules

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: AgentSession — the tool loop (rounds, cap, Stop, failure rollback, API-valid history) + wire proof

**Files:**
- Create: `modules/pi-copilot/src/module/AgentSession.h`, `AgentSession.cpp`
- Modify: `modules/pi-copilot/src/module/PICopilotAgentSelfTest.cpp` (Sections A4, A5), `CMakeLists.txt`
- Modify: `modules/pi-copilot/test/run-selftest.sh` (echo server `/agent` path, `PICOPILOT_SELFTEST_AGENT_URL`, verdict keys)

**Interfaces:**
- Consumes: `AnthropicMessage`/`AnthropicResult`/`AnthropicRequest` (Task 3), `ToolCall`/`ToolOutcome`/`ExecuteTool`/`ToolResultBlock`/`ToolDefinitions` (Task 4), `BuildSystemPrompt`, `StripOlderImages`, `ComposeUserTurn`.
- Produces:
```cpp
constexpr int PICopilotMaxToolRounds = 12;
struct AgentStep { enum Kind { SendAgain, Done, Failed, CapReached, Stopped }; Kind kind; String assistantText;
                   bool truncated; StringList toolLog; String error; bool restoreInput; bool toolsRan; };
class AgentSession {
public:
   using ToolRunner = std::function<ToolOutcome( const ToolCall& )>;
   const Array<AnthropicMessage>& History() const;
   void BeginUserTurn( const AnthropicMessage& userTurn );
   AgentStep OnResponse( const AnthropicResult& r, const ToolRunner& run,
                         const std::function<bool()>& stopRequested,
                         const std::function<void( const String& )>& onLog = nullptr );
   void Clear();
};
bool HistoryIsApiValid( const Array<AnthropicMessage>& history, String& why );
```
Loop rules (binding):
- A round = assistant `tool_use` turn + one user turn with ALL its `tool_result`s. The two are appended together, after every tool of the round has run, so the history never holds half a round.
- The 13th `tool_use` response (after 12 rounds) runs nothing. Each call gets `is_error` "not executed: … limit …" → `CapReached`.
- Stop is polled before each tool. The remaining calls get "not executed: the user pressed Stop" → `Stopped`. A running tool is never interrupted.
- A failure (or cancel) before any round: history rolls back to before `BeginUserTurn`, and `restoreInput`. After ≥1 round: completed rounds stay (their processes really ran), the history ends with a `tool_result` turn, and the next `BeginUserTurn` MERGES into it so roles keep alternating. `Failed` still gets `restoreInput`, plus `toolsRan` so the panel warns; a mid-loop `Stopped` does not.
- `stop_reason` other than `tool_use` → `Done`. Any stray `tool_use` blocks, e.g. a `max_tokens` cut-off mid-call, are removed from the stored assistant turn, so no unanswered `tool_use` enters the history.

- [ ] **Step 1: Failing tests (Sections A4 + A5) and the scripted wire server.**

In `run-selftest.sh`, inside the echo server's python, add this method to class `H` (after `reply`):
```python
    def agent_reply(self, req, n):
        def err(msg):
            return 400, {"type": "error", "error": {"type": "invalid_request_error", "message": "body #%d: %s" % (n, msg)}}
        msgs = req.get("messages") or []
        names = [t.get("name") for t in (req.get("tools") or [])]
        for i, m in enumerate(msgs):
            if m.get("role") != ("user" if i % 2 == 0 else "assistant"):
                return err("message %d has role %r" % (i, m.get("role")))
        if not msgs or msgs[-1].get("role") != "user":
            return err("last message is not a user message")
        last = msgs[-1].get("content")
        results = [b for b in last if b.get("type") == "tool_result"] if isinstance(last, list) else []
        if results:
            prev = msgs[-2].get("content") if len(msgs) >= 2 else []
            uses = {b.get("id") for b in prev if isinstance(prev, list) and b.get("type") == "tool_use"}
            got = {b.get("tool_use_id") for b in results}
            if uses != got:
                return err("tool_result ids %s != tool_use ids %s" % (sorted(got), sorted(uses)))
            summary = []
            for b in results:
                c = b.get("content")
                text = c if isinstance(c, str) else "".join(x.get("text", "") for x in c if x.get("type") == "text")
                summary.append({"id": b.get("tool_use_id"), "is_error": b.get("is_error", False), "text": text[:4000]})
            return 200, {"content": [{"type": "text", "text": json.dumps({"tools": names, "tool_results": summary})}],
                         "stop_reason": "end_turn"}
        return 200, {"content": [{"type": "text", "text": "Checking PixelMath — one moment \U0001F4F7"},
                                 {"type": "tool_use", "id": "toolu_wire_%02d" % n, "name": "describe_process",
                                  "input": {"id": "PixelMath"}}],
                     "stop_reason": "tool_use"}
```
In `do_POST`, right after the successful `req = json.loads(text)` block (before the final `self.reply(200, ...)`), add:
```python
        if self.path.endswith("/agent"):
            return self.reply(*self.agent_reply(req, n))
```
After the `export PICOPILOT_SELFTEST_ECHO_URL=...` line, add:
```bash
export PICOPILOT_SELFTEST_AGENT_URL="http://127.0.0.1:$(cat "$ECHO_PORT_FILE")/v1/agent"
```
In the verdict python, add `'agentLoopOk', 'agentWireOk',` to `required_true`, and after the `utf8EchoSkipped` check add:
```python
if d.get('agentWireSkipped') is not False: missing.append('agentWireSkipped==false')
```
In `PICopilotAgentSelfTest.cpp`, add `#include "AgentSession.h"` and `#include "ViewCapture.h"`. Add to the anonymous namespace:
```cpp
AnthropicResult ToolUseResult( const std::vector<std::pair<std::string, nlohmann::json>>& calls,
                               const std::string& idPrefix, const std::string& text = std::string() )
{
   AnthropicResult r;
   r.ok = true;
   r.httpStatus = 200;
   r.stopReason = "tool_use";
   r.contentBlocks = nlohmann::json::array();
   if ( !text.empty() )
   {
      r.contentBlocks.push_back( { { "type", "text" }, { "text", text } } );
      r.text = String::UTF8ToUTF16( text.c_str() );
   }
   int i = 0;
   for ( const auto& c : calls )
      r.contentBlocks.push_back( { { "type", "tool_use" }, { "id", idPrefix + std::to_string( ++i ) },
                                   { "name", c.first }, { "input", c.second } } );
   return r;
}

AnthropicResult EndTurnResult( const std::string& text )
{
   AnthropicResult r;
   r.ok = true;
   r.httpStatus = 200;
   r.stopReason = "end_turn";
   r.contentBlocks = nlohmann::json::array();
   r.contentBlocks.push_back( { { "type", "text" }, { "text", text } } );
   r.text = String::UTF8ToUTF16( text.c_str() );
   return r;
}

AnthropicResult ErrorResult( const String& error, int status )
{
   AnthropicResult r;
   r.ok = false;
   r.error = error;
   r.httpStatus = status;
   return r;
}

int CountImages( const nlohmann::json& v )
{
   int n = 0;
   if ( v.is_object() )
   {
      if ( v.value( "type", std::string() ) == "image" )
         ++n;
      for ( auto it = v.begin(); it != v.end(); ++it )
         n += CountImages( it.value() );
   }
   else if ( v.is_array() )
      for ( const nlohmann::json& e : v )
         n += CountImages( e );
   return n;
}

int CountImagesInHistory( const Array<AnthropicMessage>& h )
{
   int n = 0;
   for ( const AnthropicMessage& m : h )
      n += m.blocks.is_null() ? (m.imageJpegBase64.IsEmpty() ? 0 : 1) : CountImages( m.blocks );
   return n;
}
```
Insert above the marker:
```cpp
   // ---- Section A4: AgentSession loop (Task 5, no network) -----------------
   {
      bool loopOk = false, multiOk = false, capOk = false, stopOk = false, failFirstOk = false,
           cancelFirstOk = false, failMidOk = false, stripOk = false, invalidOk = false;
      nlohmann::json detail = nlohmann::json::object();
      String error;
      try
      {
         AgentTestWindow tw( "PICopilotLoop" );
         const View v = tw.MainView();
         ToolContext ctx;
         ctx.mode = AgentMode::Copilot;
         ctx.activeView = [v]() -> View { return v; };
         const AgentSession::ToolRunner run = [&ctx]( const ToolCall& c ) { return ExecuteTool( c, ctx ); };
         const std::function<bool()> never = []() { return false; };
         const nlohmann::json halve = { { "process_id", "PixelMath" }, { "parameters", { { "expression", "$T*0.5" } } } };
         const nlohmann::json describePM = { { "id", "PixelMath" } };
         auto userTurn = []( const char* t ) { return ComposeUserTurn( String( t ), nullptr, IsoString() ); };
         String why;

         {  // one apply round, then a final answer
            AgentSession s;
            const double before = ChannelMedian( v, 0 );
            s.BeginUserTurn( userTurn( "Halve it." ) );
            const bool valid0 = HistoryIsApiValid( s.History(), why );
            const AgentStep a = s.OnResponse( ToolUseResult( { { "apply_process", halve } }, "toolu_l", "On it." ), run, never );
            const bool valid1 = HistoryIsApiValid( s.History(), why );
            const AgentStep b = s.OnResponse( EndTurnResult( "Done \xE2\x80\x94 halved." ), run, never );
            const double after = ChannelMedian( v, 0 );
            s.BeginUserTurn( userTurn( "Thanks" ) );
            const bool valid2 = HistoryIsApiValid( s.History(), why );
            loopOk = valid0 && valid1 && valid2 && a.kind == AgentStep::SendAgain && a.toolLog.Length() == 1
                  && a.toolLog[0].StartsWith( String::UTF8ToUTF16( "\xE2\x96\xB6 apply_process PixelMath" ) )
                  && b.kind == AgentStep::Done && s.History().Length() == 5
                  && s.History()[2].blocks.at( 0 ).at( "tool_use_id" ) == "toolu_l1"
                  && s.History()[2].blocks.at( 0 ).at( "is_error" ) == false
                  && std::fabs( after - 0.5*before ) < 1e-5;
         }
         {  // two tool_use blocks in one response -> one user turn, results in order
            AgentSession s;
            s.BeginUserTurn( userTurn( "Look first." ) );
            const AgentStep a = s.OnResponse( ToolUseResult( { { "describe_process", describePM },
                                                               { "get_view_context", nlohmann::json::object() } }, "toolu_m" ), run, never );
            const nlohmann::json& results = s.History()[2].blocks;
            multiOk = a.kind == AgentStep::SendAgain && a.toolLog.Length() == 2 && results.size() == 2
                   && results.at( 0 ).at( "tool_use_id" ) == "toolu_m1" && results.at( 1 ).at( "tool_use_id" ) == "toolu_m2";
         }
         {  // cap: 12 rounds run, the 13th runs nothing; the next message merges and stays API-valid
            AgentSession s;
            s.BeginUserTurn( userTurn( "Keep describing." ) );
            int sendAgain = 0;
            AgentStep last;
            for ( int i = 1; i <= PICopilotMaxToolRounds + 1; ++i )
            {
               last = s.OnResponse( ToolUseResult( { { "describe_process", describePM } },
                                                   "toolu_c" + std::to_string( i ) + "_" ), run, never );
               if ( last.kind != AgentStep::SendAgain )
                  break;
               ++sendAgain;
            }
            s.BeginUserTurn( userTurn( "ok, continue" ) );
            const bool valid = HistoryIsApiValid( s.History(), why );
            detail["capWhy"] = U8( why );
            const AnthropicMessage& merged = s.History()[s.History().Length()-1];
            capOk = sendAgain == PICopilotMaxToolRounds && last.kind == AgentStep::CapReached
                 && last.toolLog.Length() == 1 && last.toolLog[0].Contains( "skipped (tool-round limit" )
                 && valid && merged.blocks.at( 0 ).at( "type" ) == "tool_result"
                 && merged.blocks.at( 0 ).at( "is_error" ) == true && merged.blocks.back().at( "type" ) == "text";
         }
         {  // Stop after the first tool: the second is skipped, the loop ends, history stays valid
            AgentSession s;
            s.BeginUserTurn( userTurn( "Two things." ) );
            int calls = 0;
            const AgentSession::ToolRunner counting = [&]( const ToolCall& c ) { ++calls; return ExecuteTool( c, ctx ); };
            const std::function<bool()> stopAfterFirst = [&calls]() { return calls >= 1; };
            const AgentStep a = s.OnResponse( ToolUseResult( { { "describe_process", describePM },
                                                               { "list_processes", nlohmann::json::object() } }, "toolu_s" ),
                                              counting, stopAfterFirst );
            s.BeginUserTurn( userTurn( "go on" ) );
            stopOk = a.kind == AgentStep::Stopped && calls == 1 && a.toolLog.Length() == 2
                  && a.toolLog[1].Contains( "skipped (stopped)" ) && HistoryIsApiValid( s.History(), why );
         }
         {  // failure / cancel before any round: roll back + restore input
            AgentSession s;
            s.BeginUserTurn( userTurn( "one" ) );
            s.OnResponse( EndTurnResult( "reply one" ), run, never );
            const size_type n0 = s.History().Length();
            s.BeginUserTurn( userTurn( "two" ) );
            const AgentStep f = s.OnResponse( ErrorResult( "boom", 500 ), run, never );
            failFirstOk = f.kind == AgentStep::Failed && f.restoreInput && !f.toolsRan && f.error == "boom"
                       && s.History().Length() == n0;
            s.BeginUserTurn( userTurn( "three" ) );
            const AgentStep c = s.OnResponse( ErrorResult( "request cancelled", 0 ), run, never );
            cancelFirstOk = c.kind == AgentStep::Stopped && c.restoreInput && s.History().Length() == n0;
         }
         {  // failure after a round: the round stays, next message merges, still valid
            AgentSession s;
            s.BeginUserTurn( userTurn( "describe then fail" ) );
            s.OnResponse( ToolUseResult( { { "describe_process", describePM } }, "toolu_f" ), run, never );
            const size_type n1 = s.History().Length();
            const AgentStep f = s.OnResponse( ErrorResult( "overloaded", 529 ), run, never );
            s.BeginUserTurn( userTurn( "retry" ) );
            failMidOk = f.kind == AgentStep::Failed && f.restoreInput && f.toolsRan && n1 == 3
                     && s.History().Length() == 3 && HistoryIsApiValid( s.History(), why );
         }
         {  // images: only the LAST message keeps pixels, including tool_result previews
            AgentSession s;
            s.BeginUserTurn( ComposeUserTurn( "look", nullptr, IsoString( "/9j/4AAQSkZJRgABAQ==" ) ) );
            s.OnResponse( ToolUseResult( { { "get_view_context", { { "include_preview", true } } } }, "toolu_p" ), run, never );
            const int afterRound1 = CountImagesInHistory( s.History() );
            s.OnResponse( ToolUseResult( { { "describe_process", describePM } }, "toolu_q" ), run, never );
            const int afterRound2 = CountImagesInHistory( s.History() );
            s.BeginUserTurn( ComposeUserTurn( "again", nullptr, IsoString( "/9j/4AAQSkZJRgABAQ==" ) ) );
            const int afterMerge = CountImagesInHistory( s.History() );
            detail["images"] = { afterRound1, afterRound2, afterMerge };
            stripOk = afterRound1 == 1 && afterRound2 == 0 && afterMerge == 1 && s.History()[0].imageJpegBase64.IsEmpty();
         }
         {  // the validator catches both kinds of broken pairing
            Array<AnthropicMessage> bad;
            bad.Add( AnthropicMessage{ IsoString( "user" ), String( "hi" ), IsoString() } );
            AnthropicMessage a;
            a.role = "assistant";
            a.blocks = nlohmann::json::array();
            a.blocks.push_back( { { "type", "tool_use" }, { "id", "toolu_z" }, { "name", "list_processes" },
                                  { "input", nlohmann::json::object() } } );
            bad.Add( a );
            bad.Add( AnthropicMessage{ IsoString( "user" ), String( "no result" ), IsoString() } );
            String why1, why2;
            const bool r1 = HistoryIsApiValid( bad, why1 );
            Array<AnthropicMessage> bad2;
            AnthropicMessage u;
            u.role = "user";
            u.blocks = nlohmann::json::array();
            u.blocks.push_back( { { "type", "tool_result" }, { "tool_use_id", "toolu_nope" },
                                  { "content", "x" }, { "is_error", false } } );
            bad2.Add( u );
            const bool r2 = HistoryIsApiValid( bad2, why2 );
            invalidOk = !r1 && why1.Contains( "toolu_z" ) && !r2 && why2.Contains( "toolu_nope" );
         }
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }

      const bool ok = loopOk && multiOk && capOk && stopOk && failFirstOk && cancelFirstOk && failMidOk && stripOk && invalidOk;
      out["loopDetail"] = detail;
      out["loopApplyOk"] = loopOk;
      out["loopMultiToolOk"] = multiOk;
      out["loopCapOk"] = capOk;
      out["loopStopOk"] = stopOk;
      out["loopFailFirstOk"] = failFirstOk;
      out["loopCancelFirstOk"] = cancelFirstOk;
      out["loopFailMidOk"] = failMidOk;
      out["loopStripOk"] = stripOk;
      out["loopInvalidOk"] = invalidOk;
      out["loopError"] = U8( error );
      out["agentLoopOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section A5: tool loop on the wire (Task 5; loopback scripted server) --
   // Real AnthropicRequest bytes: tools + tool_use/tool_result history, with
   // non-BMP text both in the prompt and in the echoed-back assistant blocks,
   // strict-UTF-8-decoded and pairing-checked by the harness's "/agent" server.
   {
      bool wireSkipped = true, wireOk = false;
      String error;
      int requests = 0;
      nlohmann::json statuses = nlohmann::json::array();
      const char* agentUrl = std::getenv( "PICOPILOT_SELFTEST_AGENT_URL" );
      if ( agentUrl != nullptr && *agentUrl != '\0' )
      {
         wireSkipped = false;
         try
         {
            AgentTestWindow tw( "PICopilotAgentWire" );
            const View v = tw.MainView();
            ToolContext ctx;
            ctx.mode = AgentMode::Copilot;
            ctx.activeView = [v]() -> View { return v; };
            AgentSession session;
            session.BeginUserTurn( ComposeUserTurn(
               String::UTF8ToUTF16( "Tell me about PixelMath \xE2\x80\x94 please \xF0\x9F\x93\xB7" ), nullptr, IsoString() ) );
            AgentStep s;
            do
            {
               String why;
               if ( !HistoryIsApiValid( session.History(), why ) )
               {
                  error = "history invalid before request: " + why;
                  break;
               }
               AnthropicRequest req( String( "sk-ant-invalid-selftest" ), PICOPILOT_DEFAULT_MODEL,
                                     BuildSystemPrompt( AgentMode::Copilot ), session.History(),
                                     String( agentUrl ), 30, ToolDefinitions( AgentMode::Copilot ) );
               const AnthropicResult r = req.Perform();
               ++requests;
               statuses.push_back( r.httpStatus );
               s = session.OnResponse( r, [&ctx]( const ToolCall& c ) { return ExecuteTool( c, ctx ); },
                                       []() { return false; } );
               if ( s.kind == AgentStep::Failed )
                  error = s.error;
            }
            while ( s.kind == AgentStep::SendAgain && requests < 4 );

            if ( s.kind == AgentStep::Done )
            {
               const nlohmann::json final = nlohmann::json::parse( U8( s.assistantText ) );
               bool sawApply = false;
               for ( const nlohmann::json& n : final.at( "tools" ) )
                  sawApply = sawApply || n == "apply_process";
               const nlohmann::json& tr = final.at( "tool_results" );
               wireOk = requests == 2 && sawApply && tr.size() == 1 && tr.at( 0 ).at( "is_error" ) == false
                     && tr.at( 0 ).at( "text" ).get<std::string>().find( "\"expression\"" ) != std::string::npos;
            }
         }
         catch ( const pcl::Exception& x ) { error = x.Message(); }
         catch ( const std::exception& x ) { error = String( x.what() ); }
         catch ( ... )                     { error = "unknown exception"; }
      }
      else
         error = "PICOPILOT_SELFTEST_AGENT_URL not set";
      out["agentWireSkipped"] = wireSkipped;
      out["agentWireRequests"] = requests;
      out["agentWireStatuses"] = statuses;
      out["agentWireError"] = U8( error );
      out["agentWireOk"] = wireOk;
      allOk = allOk && wireOk;
   }
```

- [ ] **Step 2: Verify RED.** Run the Task 2 Step 2 build command.
Expected: `AgentSession.h: No such file or directory`.

- [ ] **Step 3: Implement.** `AgentSession.h`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_AgentSession_h
#define PICopilot_AgentSession_h

#include "AgentTools.h"
#include "AnthropicClient.h"

#include <pcl/StringList.h>

#include <functional>

namespace pcl
{

// Tool rounds (one assistant tool_use turn + its tool_results) allowed per
// user message. The next tool_use response runs nothing and ends the loop.
constexpr int PICopilotMaxToolRounds = 12;

struct AgentStep
{
   enum Kind { SendAgain, Done, Failed, CapReached, Stopped };

   Kind       kind = Failed;
   String     assistantText;         // the model's text in this response (may be empty)
   bool       truncated = false;     // stop_reason == max_tokens
   StringList toolLog;               // one compact line per tool call (run, declined, failed or skipped)
   String     error;                 // Failed/Stopped-by-cancel: the request error
   bool       restoreInput = false;  // give the prompt back to the input line
   bool       toolsRan = false;      // some tool of this user message already ran (processes may have changed the image)
};

/*
 * The tool-calling loop over one conversation. UI thread only: OnResponse()
 * runs tools (views, processes, dialogs). The caller performs each HTTP
 * request (ChatThread) from History() and feeds the result back in.
 *
 * History invariants (checked by HistoryIsApiValid()): roles alternate
 * starting with user; every assistant tool_use is answered, in the NEXT
 * message, by a tool_result with its id; only the last message carries images.
 */
class AgentSession
{
public:

   using ToolRunner = std::function<ToolOutcome( const ToolCall& )>;

   const Array<AnthropicMessage>& History() const
   {
      return m_history;
   }

   // Appends the user's turn -- or, when the history ends with a user
   // tool_result turn (loop stopped, capped or failed mid-way), merges it
   // into that turn as trailing blocks so roles keep alternating -- then
   // strips older images. Resets the per-message round count.
   void BeginUserTurn( const AnthropicMessage& userTurn );

   // Feeds the result of the request built from History(). For stop_reason
   // tool_use, runs every tool_use in order through `run` (polling
   // stopRequested before each), then appends the assistant turn and the
   // tool_result turn together. onLog (optional) gets each tool line as
   // soon as it exists. Never throws.
   AgentStep OnResponse( const AnthropicResult& r, const ToolRunner& run,
                         const std::function<bool()>& stopRequested,
                         const std::function<void( const String& )>& onLog = nullptr );

   void Clear();

private:

   Array<AnthropicMessage> m_history;
   Array<AnthropicMessage> m_snapshot;   // history before the current BeginUserTurn()
   int                     m_rounds = 0;
   bool                    m_anyToolRan = false;
};

// Structural Messages-API validity of a history about to be sent (see the
// invariants above, and the last message must be a user message). why = the
// first violation, naming the message index / tool_use id.
bool HistoryIsApiValid( const Array<AnthropicMessage>& history, String& why );

} // namespace pcl

#endif // PICopilot_AgentSession_h
```
`AgentSession.cpp`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "AgentSession.h"
#include "Utf8.h"
#include "VisionTurn.h"

#include <pcl/Exception.h>

#include <set>
#include <string>
#include <vector>

namespace pcl
{

namespace
{

String S16( const std::string& s )
{
   return String::UTF8ToUTF16( s.c_str() );
}

// "✖ <tool> → <suffix>"
String CallLine( const ToolCall& c, const String& suffix )
{
   return String::UTF8ToUTF16( "\xE2\x9C\x96 " ) + S16( c.name ) + String::UTF8ToUTF16( " \xE2\x86\x92 " ) + suffix;
}

ToolOutcome NotExecuted( const std::string& why )
{
   ToolOutcome o;
   o.isError = true;
   o.content.push_back( { { "type", "text" }, { "text", why } } );
   return o;
}

nlohmann::json ToBlocks( const AnthropicMessage& m )
{
   if ( m.blocks.is_array() )
      return m.blocks;
   nlohmann::json b = nlohmann::json::array();
   if ( !m.imageJpegBase64.IsEmpty() )
      b.push_back( JpegImageBlock( m.imageJpegBase64 ) );
   b.push_back( { { "type", "text" }, { "text", U8( m.content ) } } );
   return b;
}

} // namespace

void AgentSession::Clear()
{
   m_history.Clear();
   m_snapshot.Clear();
   m_rounds = 0;
   m_anyToolRan = false;
}

void AgentSession::BeginUserTurn( const AnthropicMessage& userTurn )
{
   m_snapshot = m_history;
   m_rounds = 0;
   m_anyToolRan = false;
   if ( !m_history.IsEmpty() && m_history[m_history.Length()-1].role == "user" )
   {
      AnthropicMessage& last = m_history[m_history.Length()-1];
      nlohmann::json merged = ToBlocks( last );          // tool_results first (API rule)
      for ( const nlohmann::json& b : ToBlocks( userTurn ) )
         merged.push_back( b );
      last.blocks = std::move( merged );
      last.content.Clear();
      last.imageJpegBase64.Clear();
   }
   else
      m_history.Add( userTurn );
   StripOlderImages( m_history );
}

AgentStep AgentSession::OnResponse( const AnthropicResult& r, const ToolRunner& run,
                                    const std::function<bool()>& stopRequested,
                                    const std::function<void( const String& )>& onLog )
{
   AgentStep s;
   s.toolsRan = m_anyToolRan;
   auto log = [&]( const String& line )
   {
      s.toolLog.Add( line );
      if ( onLog )
         onLog( line );
   };
   auto stopNow = [&]() { return stopRequested && stopRequested(); };

   if ( !r.ok )
   {
      s.kind = r.error == "request cancelled" ? AgentStep::Stopped : AgentStep::Failed;
      s.error = r.error;
      if ( m_rounds == 0 )
      {
         m_history = m_snapshot;        // nothing ran: as if never sent
         s.restoreInput = true;
      }
      else
         s.restoreInput = s.kind == AgentStep::Failed;
      return s;
   }

   s.assistantText = r.text;
   s.truncated = r.truncated;

   std::vector<ToolCall> calls;
   if ( r.contentBlocks.is_array() )
      for ( const nlohmann::json& b : r.contentBlocks )
         if ( b.is_object() && b.value( "type", std::string() ) == "tool_use" )
            calls.push_back( ToolCall{ b.value( "id", std::string() ), b.value( "name", std::string() ),
                                       b.contains( "input" ) ? b["input"] : nlohmann::json::object() } );

   AnthropicMessage assistant;
   assistant.role = "assistant";
   assistant.content = r.text;
   assistant.blocks = r.contentBlocks;   // verbatim (null -> plain text)

   if ( r.stopReason != "tool_use" )
   {
      if ( !calls.empty() && assistant.blocks.is_array() )
      {
         // e.g. max_tokens cut a tool call: never store an unanswered tool_use.
         nlohmann::json kept = nlohmann::json::array();
         for ( const nlohmann::json& b : assistant.blocks )
            if ( !(b.is_object() && b.value( "type", std::string() ) == "tool_use") )
               kept.push_back( b );
         if ( kept.empty() )
            kept.push_back( { { "type", "text" }, { "text", "[reply cut off before a tool call completed]" } } );
         assistant.blocks = kept;
      }
      m_history.Add( assistant );
      s.kind = AgentStep::Done;
      return s;
   }

   if ( calls.empty() )
   {
      s.kind = AgentStep::Failed;
      s.error = "the model asked to use a tool (stop_reason tool_use) but sent no tool_use block";
      if ( m_rounds == 0 )
         m_history = m_snapshot;
      s.restoreInput = true;
      return s;
   }

   const bool capped = m_rounds >= PICopilotMaxToolRounds;
   bool stopped = false;
   nlohmann::json results = nlohmann::json::array();
   for ( const ToolCall& call : calls )
   {
      if ( capped )
      {
         results.push_back( ToolResultBlock( call.id, NotExecuted(
            "not executed: the limit of " + std::to_string( PICopilotMaxToolRounds )
            + " tool rounds for one user message was reached; summarize your progress for the user" ) ) );
         log( CallLine( call, "skipped (tool-round limit reached)" ) );
         continue;
      }
      if ( stopped || stopNow() )
      {
         stopped = true;
         results.push_back( ToolResultBlock( call.id, NotExecuted( "not executed: the user pressed Stop" ) ) );
         log( CallLine( call, "skipped (stopped)" ) );
         continue;
      }
      ToolOutcome o;
      try
      {
         o = run( call );
      }
      catch ( const pcl::Exception& x )
      {
         o = NotExecuted( "tool failed: " + U8( x.Message() ) );
         o.logLine = CallLine( call, "error: tool threw an exception" );
      }
      catch ( ... )
      {
         o = NotExecuted( "tool failed: unknown error" );
         o.logLine = CallLine( call, "error: tool threw an exception" );
      }
      m_anyToolRan = true;
      results.push_back( ToolResultBlock( call.id, o ) );
      log( o.logLine );
   }

   // The round is appended as a unit: never half a tool_use/tool_result pair.
   m_history.Add( assistant );
   AnthropicMessage user;
   user.role = "user";
   user.blocks = std::move( results );
   m_history.Add( user );
   ++m_rounds;
   StripOlderImages( m_history );

   s.toolsRan = m_anyToolRan;
   s.kind = capped ? AgentStep::CapReached
          : (stopped || stopNow()) ? AgentStep::Stopped
          : AgentStep::SendAgain;
   return s;
}

bool HistoryIsApiValid( const Array<AnthropicMessage>& h, String& why )
{
   why.Clear();
   if ( h.IsEmpty() )
   {
      why = "empty history";
      return false;
   }
   std::set<std::string> pending;   // tool_use ids the next (user) message must answer
   for ( size_type i = 0; i < h.Length(); ++i )
   {
      const AnthropicMessage& m = h[i];
      const char* expected = (i % 2 == 0) ? "user" : "assistant";
      if ( m.role != expected )
      {
         why = String().Format( "message %u should be ", unsigned( i ) ) + expected + " but is " + String( m.role );
         return false;
      }
      std::set<std::string> uses, answers;
      if ( m.blocks.is_array() )
         for ( const nlohmann::json& b : m.blocks )
         {
            const std::string type = b.is_object() ? b.value( "type", std::string() ) : std::string();
            if ( type == "tool_use" )
            {
               if ( m.role != "assistant" )
               {
                  why = String().Format( "message %u: tool_use in a user message", unsigned( i ) );
                  return false;
               }
               uses.insert( b.value( "id", std::string() ) );
            }
            else if ( type == "tool_result" )
            {
               const std::string id = b.value( "tool_use_id", std::string() );
               if ( m.role != "user" || pending.count( id ) == 0 )
               {
                  why = "tool_result " + S16( id ) + String().Format( " (message %u) has no matching tool_use in the previous message", unsigned( i ) );
                  return false;
               }
               answers.insert( id );
            }
         }
      if ( m.role == "user" )
      {
         for ( const std::string& id : pending )
            if ( answers.count( id ) == 0 )
            {
               why = "tool_use " + S16( id ) + String().Format( " is not answered by a tool_result in message %u", unsigned( i ) );
               return false;
            }
         pending.clear();
      }
      else
         pending = uses;
   }
   if ( h[h.Length()-1].role != "user" )
   {
      why = "the last message must be a user message";
      return false;
   }
   return true;
}

} // namespace pcl
```
Add `AgentSession.cpp` to `MODULE_SOURCES`.

- [ ] **Step 4: Verify GREEN.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && PICOPILOT_ECHO_KEEP=$(mktemp -d) bash -c 'cmake --build build -j$(nproc) && bash test/run-selftest.sh; ls -la "$PICOPILOT_ECHO_KEEP"'`
Expected: `PASS: self-test verdict all green`. `agentWireStatuses` is `[200,200]`. The kept echo bodies include the two `/agent` request bodies, both of which decoded as strict UTF-8.

- [ ] **Step 5: Commit.**
```bash
cd /home/scarter4work/projects/astro-pi
git add modules/pi-copilot/src/module/AgentSession.h modules/pi-copilot/src/module/AgentSession.cpp \
        modules/pi-copilot/src/module/PICopilotAgentSelfTest.cpp modules/pi-copilot/src/module/CMakeLists.txt \
        modules/pi-copilot/test/run-selftest.sh
git commit -m "feat(pi-copilot): AgentSession tool loop -- atomic rounds, 12-round cap, Stop, rollback, API-valid history; wire proof via scripted loopback

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: Panel — real modes (persisted), Stop button, loop driving, tool log lines, Guided dialog, freely resizable window

**Files:**
- Modify: `modules/pi-copilot/src/module/PICopilotInterface.h`
- Modify: `modules/pi-copilot/src/module/PICopilotInterface.cpp`
- Modify: `modules/pi-copilot/src/module/PICopilotAgentSelfTest.cpp` (Section A7: resize probe), `modules/pi-copilot/test/run-selftest.sh`

**Interfaces:**
- Consumes: `AgentSession`, `AgentStep`, `HistoryIsApiValid`, `PICopilotMaxToolRounds` (Task 5), `AgentMode`, `AgentModeFromIndex`, `ToolContext`, `ExecuteTool` (Task 4), `BuildSystemPrompt` (Task 4), `ChatThread` with tools (Task 3).
- Produces:
  - Panel-private: `void StartRequest()`, `void FinishTurn()`, `ToolContext MakeToolContext() const`, `static bool ConfirmApply( const String&, const String&, const String& )`, `void e_Stop_Click( Button&, bool )`, `void e_Mode_ItemSelected( ComboBox&, int )`, `PushButton Stop_Button`. The Setting `PICopilot/Mode` (int 0-2).
  - Public, test-only: `nlohmann::json PICopilotInterface::ProbeResizeForSelfTest();`.

**Resizable window (binding ruling).** The user reports that the panel cannot be resized. Nothing in our code calls `SetFixedSize`: `GUIData` ends with `SetSizer` → `EnsureLayoutUpdated()` → `AdjustToContents()` and sets no explicit min or max size. PCL's own resizable windows use the idiom `AdjustToContents(); SetMinSize();` (`/opt/PixInsight/src/pcl/MultiViewSelectionDialog.cpp:184-185`, `OnlineObjectSearchDialog.cpp:139-140`). `Control::IsFixedWidth()` is defined as `MinWidth() == MaxWidth()` (`Control.h:401-403`). `ProcessInterface::SaveGeometry()` stores Width/Height **only if the interface is resizable** (`ProcessInterface.h:2487-2490`).

What exactly pins the size today is **not yet observed**, so this task measures before it fixes. Step 2 adds a headless probe that builds the panel GUI, reads `IsFixedWidth/Height`, min and max, resizes by +300/+300 and down to the minimum, and checks that the chat log follows.

The fix makes the constraint explicit and verifiable:
- an explicit minimum size (`kMinPanelWidth` 300 × `kMinPanelHeight` 260 logical px, below the 420-px default placement width);
- an explicit unbounded maximum (`kUnboundedPanelSize = 16777215`, Qt's `QWIDGETSIZE_MAX`; PCL has no named constant for it);
- a smaller chat-log minimum (240×120) so the window can shrink;
- the chat log keeps sizer stretch 100 and has expansion enabled on both axes.

The one-time right-edge placement is untouched: it calls `Resize`/`Move`/`SaveGeometry`, and with a resizable window `SaveGeometry` now persists Width/Height too. PI's auto-save geometry keeps remembering the user's own size.

- [ ] **Step 1: Header.** In `PICopilotInterface.h`, add `#include "AgentSession.h"`, `#include "AgentTools.h"` and `#include <nlohmann/json.hpp>`. In the `public:` section, after `static String PlainText( const String& text );`, add:
```cpp
   // Test-only (self-test Section A7): builds the GUI if it does not exist
   // yet, then measures whether the panel resizes both ways and the chat log
   // follows; restores the original size. Root thread only.
   nlohmann::json ProbeResizeForSelfTest();
```
Replace the chat-state block (`Array<AnthropicMessage> m_history;` through `void SetBusy( bool busy );`) with:
```cpp
   // The conversation + tool loop (UI thread only). Roles alternate and
   // every tool_use is answered; see AgentSession.
   AgentSession m_session;

   // The in-flight HTTP request, if any. Constructed and destroyed on the UI
   // thread only (see ChatThread), and never destroyed while still active.
   AutoPointer<ChatThread> m_thread;

   // Per user message: the prompt (restored on failure), the key and the mode
   // it was sent with (a mode change mid-loop applies to the NEXT message).
   String    m_pendingPrompt;
   String    m_apiKey;
   AgentMode m_turnMode = AgentMode::Copilot;

   // Stop pressed: cancel the request in flight, run no further tool.
   bool m_stopRequested = false;

   // True while tools run inside e_Poll_Timer. Processes pump events, so
   // Send/Stop/Timer can fire re-entrantly; this blocks a second turn.
   bool m_handlingResult = false;

   void SendCurrentInput();
   void StartRequest();
   void FinishTurn();
   void AppendToLog( const String& richText );
   void StopWorker();
   void SetBusy( bool busy );
   ToolContext MakeToolContext() const;

   // Guided-mode confirmation (modal MessageBox, root thread).
   static bool ConfirmApply( const String& processId, const String& viewId, const String& changes );
```
In `GUIData`, add `PushButton Stop_Button;` after `PushButton Send_Button;`. In the event handlers, add:
```cpp
   void e_Stop_Click( Button& sender, bool checked );
   void e_Mode_ItemSelected( ComboBox& sender, int itemIndex );
```

- [ ] **Step 2: Resize probe, then RED.** In `run-selftest.sh` `required_true`, add `'panelResizableOk',`. In `PICopilotAgentSelfTest.cpp`, add `#include "PICopilotInterface.h"` and insert above the marker:
```cpp
   // ---- Section A7: panel resizability probe (Task 6) ----------------------
   {
      bool ok = false;
      nlohmann::json probe;
      String error;
      try
      {
         if ( ThePICopilotInterface == nullptr )
            throw Error( "ThePICopilotInterface is null" );
         probe = ThePICopilotInterface->ProbeResizeForSelfTest();
         ok = probe.value( "resizableOk", false );
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }
      out["panelResizeProbe"] = probe;
      out["panelResizeError"] = U8( error );
      out["panelResizableOk"] = ok;
      allOk = allOk && ok;
   }
```
Add the probe to `PICopilotInterface.cpp` now, **before** any layout change, so its first run measures the current code:
```cpp
nlohmann::json PICopilotInterface::ProbeResizeForSelfTest()
{
   nlohmann::json j;
   if ( GUI == nullptr )
   {
      bool dynamic = false;
      unsigned flags = 0;
      Launch( *ThePICopilotProcess, nullptr, dynamic, flags );
   }
   EnsureLayoutUpdated();
   const int w0 = Width(), h0 = Height(), log0 = GUI->ChatLog.Height();
   j["fixedWidth"] = IsFixedWidth();
   j["fixedHeight"] = IsFixedHeight();
   j["min"] = { MinWidth(), MinHeight() };
   j["max"] = { MaxWidth(), MaxHeight() };
   j["before"] = { w0, h0, log0 };

   Resize( w0 + 300, h0 + 300 );
   EnsureLayoutUpdated();
   const int w1 = Width(), h1 = Height(), log1 = GUI->ChatLog.Height();
   j["grown"] = { w1, h1, log1 };

   Resize( MinWidth(), MinHeight() );
   EnsureLayoutUpdated();
   const int w2 = Width(), h2 = Height();
   j["shrunk"] = { w2, h2, GUI->ChatLog.Height() };

   Resize( w0, h0 );
   EnsureLayoutUpdated();
   j["restored"] = { Width(), Height() };

   j["resizableOk"] = !IsFixedWidth() && !IsFixedHeight()
                   && w1 == w0 + 300 && h1 == h0 + 300 && log1 > log0          // grows, chat log follows
                   && w2 < w0 && h2 < h0                                        // shrinks
                   && MinWidth() <= LogicalPixelsToPhysical( kMinPanelWidth )  // sensible minimum
                   && MinHeight() <= LogicalPixelsToPhysical( kMinPanelHeight );
   return j;
}
```
Also add the three constants to the anonymous namespace now. The probe references them, and Step 3 uses them for the fix:
```cpp
// Resizable panel: explicit minimum (logical px) and an explicit unbounded
// maximum (Qt's QWIDGETSIZE_MAX; PCL has no named constant for it).
constexpr int kMinPanelWidth      = 300;
constexpr int kMinPanelHeight     = 260;
constexpr int kUnboundedPanelSize = 16777215;
```
Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) && bash test/run-selftest.sh`
Expected RED: `FAILED keys: panelResizableOk`. Copy `panelResizeProbe` into the report: it is the evidence of what pins the size (`fixedWidth`/`fixedHeight`, `min == max`, or the chat log not following). **If the probe is already green headlessly**, the headless run cannot reproduce the user's report. Say so explicitly, keep the Step 3 fix (explicit min/max is still the documented idiom), and rely on checklist item 10 as the verification. Do not claim the bug is reproduced.

- [ ] **Step 3: Implementation (`PICopilotInterface.cpp`).** Add these includes: `"AgentSession.h"`, `"AgentTools.h"`, `"SystemPrompt.h"`, `<pcl/MessageBox.h>`. **Delete `kSystemPrompt`**: prompts now come from `BuildSystemPrompt()`. Add to the anonymous namespace:
```cpp
// Persisted assistant mode: Mode_ComboBox index == AgentMode value.
const char* const kModeKey = "PICopilot/Mode";

String EscapeHtml( const String& s )
{
   String out;
   for ( size_type i = 0; i < s.Length(); ++i )
   {
      const char16_type c = s[i];
      if ( c == '&' )
         out += "&amp;";
      else if ( c == '<' )
         out += "&lt;";
      else if ( c == '>' )
         out += "&gt;";
      else if ( c == '\n' )
         out += "<br/>";
      else
         out += c;
   }
   return out;
}
```
Replace `SendCurrentInput()` with:
```cpp
void PICopilotInterface::SendCurrentInput()
{
   // One user message in flight at a time -- including while tools run
   // (processes pump events, so this can be reached re-entrantly).
   if ( m_thread || m_handlingResult )
      return;

   String prompt = GUI->ChatInput.Text().Trimmed();
   if ( prompt.IsEmpty() )
      return;

   String key = KeyStore::Load();
   if ( key.IsEmpty() )
   {
      AppendToLog( PlainText(
         String::UTF8ToUTF16( "Set your Anthropic API key via the \xE2\x9A\x99 button." ) ) + "\n\n" );
      return;
   }

   AppendToLog( "<b>You:</b> " + PlainText( prompt ) + "\n\n" );
   // Root thread, before any request: capture the active view (increment 3).
   m_session.BeginUserTurn( ComposeTurnWithActiveView( prompt ) );
   m_pendingPrompt = prompt;
   m_apiKey = key;
   m_turnMode = AgentModeFromIndex( GUI->Mode_ComboBox.CurrentItem() );
   m_stopRequested = false;
   GUI->ChatInput.Clear();
   SetBusy( true );
   StartRequest();
}

void PICopilotInterface::StartRequest()
{
   String why;
   if ( !HistoryIsApiValid( m_session.History(), why ) )
   {
      // Never send a body the API would reject with an opaque 400.
      AppendToLog( PlainText( "Internal error: the conversation history is not valid for the API ("
                              + why + "). Nothing was sent." ) + "\n\n" );
      FinishTurn();
      return;
   }
   // ChatThread serializes key, prompt, history snapshot and tools HERE (UI thread).
   m_thread = new ChatThread( m_apiKey, BuildSystemPrompt( m_turnMode ), m_session.History(),
                              PICOPILOT_DEFAULT_MODEL, PICOPILOT_MESSAGES_URL,
                              PICopilotRequestTimeoutSeconds, ToolDefinitions( m_turnMode ) );
   m_thread->Start();
   if ( !GUI->Poll_Timer.IsRunning() )
      GUI->Poll_Timer.Start();
}

void PICopilotInterface::FinishTurn()
{
   m_pendingPrompt.Clear();
   m_stopRequested = false;
   GUI->Poll_Timer.Stop();
   SetBusy( false );
}

ToolContext PICopilotInterface::MakeToolContext() const
{
   ToolContext ctx;
   ctx.mode = m_turnMode;
   ctx.activeView = []() -> View
   {
      ImageWindow w = ImageWindow::ActiveWindow();
      return w.IsNull() ? View::Null() : w.CurrentView();
   };
   ctx.confirm = &PICopilotInterface::ConfirmApply;
   return ctx;
}

bool PICopilotInterface::ConfirmApply( const String& processId, const String& viewId, const String& changes )
{
   const String text = "<p>Apply <b>" + EscapeHtml( processId ) + "</b> to <b>" + EscapeHtml( viewId ) + "</b>?</p>"
                     + "<p>" + EscapeHtml( changes ) + "</p>"
                     + "<p>You can undo it afterwards from the view's History.</p>";
   return MessageBox( text, String::UTF8ToUTF16( "PI Copilot \xE2\x80\x94 Guided mode" ), StdIcon::Question,
                      StdButton::Yes, StdButton::No, StdButton::NoButton, 0/*default: Yes*/, 1/*Esc: No*/ ).Execute()
          == StdButton::Yes;
}
```
Replace `SetBusy()` with:
```cpp
void PICopilotInterface::SetBusy( bool busy )
{
   GUI->Send_Button.SetText( busy ? String::UTF8ToUTF16( kBusyTextUtf8 ) : String( kSendText ) );
   GUI->Send_Button.SetToolTip( busy
      ? String( "<p>Working (each request gives up after " ) + String( PICopilotRequestTimeoutSeconds ) + " s).</p>"
      : String( "<p>Send the message (or press Return).</p>" ) );
   GUI->Send_Button.Enable( !busy );
   // The mode is fixed per message; the combo is locked while one runs.
   GUI->Mode_ComboBox.Enable( !busy );
   if ( busy )
   {
      GUI->Stop_Button.Enable();
      GUI->Stop_Button.Show();
   }
   else
      GUI->Stop_Button.Hide();
}
```
Replace `e_Poll_Timer()` with:
```cpp
void PICopilotInterface::e_Poll_Timer( Timer& )
{
   if ( !m_thread )
   {
      GUI->Poll_Timer.Stop();
      return;
   }
   if ( m_thread->IsActive() )
      return;

   AnthropicResult r;
   if ( !m_thread->TryTakeResult( r ) )
   {
      r = AnthropicResult();
      r.error = "worker thread ended without a result";
   }
   m_thread.Destroy();
   // Tools may run for seconds and pump events: never re-enter this handler.
   GUI->Poll_Timer.Stop();

   if ( r.ok && !r.text.IsEmpty() )
      AppendToLog( "<b>Copilot:</b> " + PlainText( r.truncated ? r.text + " [truncated: max_tokens]" : r.text ) + "\n\n" );

   m_handlingResult = true;
   const ToolContext ctx = MakeToolContext();
   const AgentStep s = m_session.OnResponse( r,
      [&ctx]( const ToolCall& call ) { return ExecuteTool( call, ctx ); },
      [this]() { return m_stopRequested; },
      [this]( const String& line ) { AppendToLog( PlainText( line ) + "\n" ); } );
   m_handlingResult = false;

   switch ( s.kind )
   {
   case AgentStep::SendAgain:
      if ( !s.toolLog.IsEmpty() )
         AppendToLog( "\n" );
      StartRequest();
      return;
   case AgentStep::Done:
      break;
   case AgentStep::CapReached:
      AppendToLog( PlainText( String().Format( "(stopped: reached the limit of %d tool rounds for one message; "
                                               "send another message to let it continue)", PICopilotMaxToolRounds ) ) + "\n\n" );
      break;
   case AgentStep::Stopped:
      AppendToLog( PlainText( "(stopped)" ) + "\n\n" );
      break;
   case AgentStep::Failed:
      AppendToLog( PlainText( "Error " + String( r.httpStatus ) + ": " + s.error ) + "\n\n" );
      if ( s.toolsRan )
         AppendToLog( PlainText( String::UTF8ToUTF16(
            "(the actions above were already applied \xE2\x80\x94 resending the prompt runs them again)" ) ) + "\n\n" );
      break;
   }
   if ( s.restoreInput && GUI->ChatInput.Text().IsEmpty() )
      GUI->ChatInput.SetText( m_pendingPrompt );
   FinishTurn();
}

void PICopilotInterface::e_Stop_Click( Button&, bool )
{
   if ( !m_thread && !m_handlingResult )
      return;
   // A running process is never interrupted: the loop checks this flag
   // between tools; the HTTP request in flight (if any) is cancelled.
   m_stopRequested = true;
   if ( m_thread )
      m_thread->RequestCancel();
   GUI->Stop_Button.Disable();
}

void PICopilotInterface::e_Mode_ItemSelected( ComboBox&, int itemIndex )
{
   Settings::Write( kModeKey, itemIndex );
}
```
In `GUIData::GUIData`, replace the `Mode_ComboBox` tooltip line with:
```cpp
   Mode_ComboBox.SetToolTip( "<p><b>Copilot</b>: applies processes to the active image directly "
                             "(every change is in the view's History; undo as usual).</p>"
                             "<p><b>Advisor</b>: read-only; looks and advises, never changes the image.</p>"
                             "<p><b>Guided</b>: proposes each process and asks you before it runs.</p>" );
   {
      int mode = 0;
      Settings::Read( kModeKey, mode );
      Mode_ComboBox.SetCurrentItem( int( AgentModeFromIndex( mode ) ) );
   }
   Mode_ComboBox.OnItemSelected( (ComboBox::item_event_handler)&PICopilotInterface::e_Mode_ItemSelected, w );
```
After the `Send_Button` setup, add:
```cpp
   Stop_Button.SetText( "Stop" );
   Stop_Button.SetToolTip( "<p>Stop after the current step. A process that is already running always "
                           "finishes; the request in flight is cancelled.</p>" );
   Stop_Button.OnClick( (Button::click_event_handler)&PICopilotInterface::e_Stop_Click, w );
   Stop_Button.Hide();
```
and in the `Input_Sizer` block add `Input_Sizer.Add( Stop_Button );` after `Input_Sizer.Add( Send_Button );`.

**Resizable layout.** In `GUIData::GUIData`, replace `ChatLog.SetScaledMinSize( 360, 200 );` with:
```cpp
   // Small minimum so the panel can shrink; the log takes all spare space
   // (stretch 100 in Global_Sizer) and expands on both axes.
   ChatLog.SetScaledMinSize( 240, 120 );
   ChatLog.EnableExpansion( true/*horz*/, true/*vert*/ );
```
Keep `Global_Sizer.Add( ChatLog, 100 );` and `Input_Sizer.Add( ChatInput, 100 );` as they are. Replace the last three lines of the constructor (`w.SetSizer( Global_Sizer ); w.EnsureLayoutUpdated(); w.AdjustToContents();`) with:
```cpp
   w.SetSizer( Global_Sizer );
   w.EnsureLayoutUpdated();
   w.AdjustToContents();
   // Freely resizable (the PCL idiom for resizable windows is
   // AdjustToContents() + an explicit minimum, cf. MultiViewSelectionDialog.cpp:184-185),
   // plus an explicit unbounded maximum so no fixed-size constraint
   // (MinWidth()==MaxWidth(), Control.h:401) can survive. With the window
   // resizable, SaveGeometry()/auto-save also persist Width/Height
   // (ProcessInterface.h:2487-2490), so the size is remembered.
   w.SetScaledMinSize( kMinPanelWidth, kMinPanelHeight );
   w.SetMaxSize( kUnboundedPanelSize, kUnboundedPanelSize );
```
`ApplyDefaultPlacement()` and `e_Show()` stay unchanged: the one-time right-edge placement resizes to 420 logical px (≥ `kMinPanelWidth`) × screen height and saves the geometry. After that, PI's auto-save geometry remembers the user's size.

- [ ] **Step 4: Build + full regression (GREEN).**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) 2>&1 | grep -E "warning|error" ; bash test/run-selftest.sh`
Expected: no warnings or errors from `PICopilotInterface.cpp`, then `PASS: self-test verdict all green`. `panelResizeProbe.resizableOk` is true: grown = before + 300 on both axes, the chat log is taller, and it shrinks below the start size. `plainTextOk` still passes. **Headless coverage of this task is only the resize probe.** Stop, the mode combo and persistence, the Guided dialog, live log rendering, and resizing by dragging the window edges are verified by the user in Task 8. Say so in the report.

- [ ] **Step 5: Commit.**
```bash
cd /home/scarter4work/projects/astro-pi
git add modules/pi-copilot/src/module/PICopilotInterface.h modules/pi-copilot/src/module/PICopilotInterface.cpp \
        modules/pi-copilot/src/module/PICopilotAgentSelfTest.cpp modules/pi-copilot/test/run-selftest.sh
git commit -m "feat(pi-copilot): panel runs the agent loop -- live Copilot/Guided/Advisor modes (persisted), Stop, tool log lines, Guided confirm; freely resizable window

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

## Task 7: Gated LIVE end-to-end — "Halve the brightness … using PixelMath" in Copilot mode

**Files:**
- Modify: `modules/pi-copilot/src/module/PICopilotAgentSelfTest.cpp` (Section A6)
- Modify: `modules/pi-copilot/test/run-selftest.sh` (timeout 600, verdict line)

**Interfaces:**
- Consumes: everything in Tasks 2-5, plus `CaptureViewTurn` (increment 3).
- Produces: verdict keys `liveAgentSkipped`, `liveAgentOk`, `liveAgentLog`, `liveAgentText`, `liveAgentRatio`.

- [ ] **Step 1: Failing check.** In `run-selftest.sh`:
- Change `timeout 300` to `timeout 600` and the failure text to `(600s)`. The live loop adds 2-4 real requests to the existing live checks.
- Add `'liveAgentOk',` to `required_true`.
- Add after the `vision check` print:
```python
print('live agent check: %s' % ('SKIPPED (no key)' if d.get('liveAgentSkipped') else 'RAN against real API, ratio=%r log=%r' % (d.get('liveAgentRatio'), d.get('liveAgentLog'))))
```
Run `bash test/run-selftest.sh`. Expected RED: `FAILED keys: liveAgentOk`.

- [ ] **Step 2: Implement Section A6** (insert above the marker):
```cpp
   // ---- Section A6: gated LIVE agent run (Task 7) --------------------------
   // Real model, Copilot tools, the panel's own turn composition: the model
   // must call apply_process(PixelMath) and the synthetic image's median must
   // be ~halved (0.45..0.55 -- also catches a double application).
   {
      bool liveSkipped = true, liveOk = true;
      String error, finalText;
      double ratio = -1;
      int requests = 0;
      nlohmann::json log = nlohmann::json::array();
      if ( const char* key = std::getenv( "PICOPILOT_TEST_API_KEY" ) )
      {
         liveSkipped = false;
         liveOk = false;
         try
         {
            AgentTestWindow tw( "PICopilotAgentLive" );
            View v = tw.MainView();
            const double before = ChannelMedian( v, 0 );
            ToolContext ctx;
            ctx.mode = AgentMode::Copilot;
            ctx.activeView = [v]() -> View { return v; };
            AgentSession session;
            StringList notes;
            session.BeginUserTurn( CaptureViewTurn( "Halve the brightness of this image using PixelMath.", &v, notes ) );
            AgentStep s;
            do
            {
               AnthropicRequest req( String( key ), PICOPILOT_DEFAULT_MODEL, BuildSystemPrompt( AgentMode::Copilot ),
                                     session.History(), PICOPILOT_MESSAGES_URL, PICopilotRequestTimeoutSeconds,
                                     ToolDefinitions( AgentMode::Copilot ) );
               const AnthropicResult r = req.Perform();
               ++requests;
               if ( !r.text.IsEmpty() )
                  finalText = r.text;
               s = session.OnResponse( r, [&ctx]( const ToolCall& c ) { return ExecuteTool( c, ctx ); },
                                       []() { return false; },
                                       [&log]( const String& line ) { log.push_back( U8( line ) ); } );
               if ( s.kind == AgentStep::Failed )
                  error = s.error;
            }
            while ( s.kind == AgentStep::SendAgain && requests <= PICopilotMaxToolRounds );
            ratio = ChannelMedian( v, 0 )/before;
            bool appliedPM = false;
            for ( const nlohmann::json& line : log )
               appliedPM = appliedPM || line.get<std::string>().rfind( "\xE2\x96\xB6 apply_process PixelMath", 0 ) == 0;
            liveOk = s.kind == AgentStep::Done && appliedPM && ratio > 0.45 && ratio < 0.55;
         }
         catch ( const pcl::Exception& x ) { error = x.Message(); }
         catch ( const std::exception& x ) { error = String( x.what() ); }
         catch ( ... )                     { error = "unknown exception"; }
      }
      out["liveAgentSkipped"] = liveSkipped;
      out["liveAgentRequests"] = requests;
      out["liveAgentLog"] = log;
      out["liveAgentText"] = U8( finalText );
      out["liveAgentRatio"] = ratio;
      out["liveAgentError"] = U8( error );
      out["liveAgentOk"] = liveOk;
      allOk = allOk && liveOk;
   }
```

- [ ] **Step 3: Verify GREEN with the live model.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) && bash test/run-selftest.sh`
Expected: `live agent check: RAN against real API, ratio=0.5… log=['▶ apply_process PixelMath {"expression":…} → ok (…)']` and `PASS: self-test verdict all green`. **A SKIPPED live check does not count.** Get the key first (keyring or `test/.test_api_key`). A failing live run is a real finding. Read `liveAgentLog`/`liveAgentText`/`liveAgentError` and fix the prompt, schema or tool, not the assertion. Run twice to check stability, and report both ratios.

- [ ] **Step 4: Commit.**
```bash
cd /home/scarter4work/projects/astro-pi
git add modules/pi-copilot/src/module/PICopilotAgentSelfTest.cpp modules/pi-copilot/test/run-selftest.sh
git commit -m "test(pi-copilot): gated live agent run -- model halves a synthetic image via apply_process(PixelMath)

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

## Task 8: Release 0.1.1.0 via the repository + user verification handoff

**Files:**
- Modify: `modules/pi-copilot/src/module/PICopilotVersion.h` (REVISION 0 → 1, BUILD 4 → 0)
- Modify: `modules/pi-copilot/src/module/PICopilotModule.cpp` (`GetReleaseDate`, `Description()`)
- Modify: `modules/pi-copilot/README.md`
- Regenerated by `./release.sh`: `repository/` (+ any re-signed script `.xsgn`)

**Interfaces:**
- Consumes: everything above. Produces: signed PICopilot **0.1.1.0** (minor capability bump, as ruled) served from `https://raw.githubusercontent.com/scarter4work/astro-pi/main/repository/`.

- [ ] **Step 1: Version, date, description.** In `PICopilotVersion.h`, set `#define PICOPILOT_MODULE_VERSION_REVISION  1` and `#define PICOPILOT_MODULE_VERSION_BUILD     0` (MAJOR 0 and MINOR 1 unchanged → `0.1.1.0`). In `PICopilotModule.cpp` `GetReleaseDate`, set year/month/day to the day you run `release.sh` (`date +%F`). Replace the second string of `Description()` with `"Chat that sees the active view and applies processes to it: Copilot (acts, undoable), Guided (asks first), Advisor (read-only)."`.

- [ ] **Step 2: README.** Insert after the `## 0.1.0.4 — UTF-8 wire fix` section:
```markdown
## Increment 4 — The copilot acts (0.1.1.0)

- **Tools:** the model can call `list_processes`, `describe_process`, `get_view_context` (fresh statistics, optional new preview) and `apply_process` (process id + parameters + table parameters + optional view id). Enumerations take element ids ("Green"), tables take whole rows in column order.
- **Modes (persisted):** **Copilot** applies directly — every run is a normal PixInsight process execution, so it is in the view's History and undoable. **Guided** shows a Yes/No dialog with the process and parameters before each run; No tells the model you declined. **Advisor** offers read-only tools only.
- **apply_process** starts from the process's default settings, sets only the given parameters, checks each one (type, range, enumeration id, table shape) and reads it back, then runs `Validate` → `CanExecuteOn` → `ExecuteOn` on the UI thread. Every failure (unknown process/parameter, bad value, busy view, global-only process such as ImageIntegration, execution failure) goes back to the model as a precise error it can correct — never a silent success. After a success the model gets the new statistics and a fresh preview.
- **Loop:** up to 12 tool rounds per message (then a visible note); **Stop** cancels the request in flight and runs no further tool (a running process always finishes); the log shows one line per tool action (`▶ … → ok (1.2 s)` / `✖ … → error: …`).
- **Resizable panel:** drag the edges; the chat log grows with the window, and the size is remembered.
- **Tone:** every mode's system prompt tells the model to do what you ask first, respect your choices about your data and palette, mention a caveat at most once and constructively, and stay concise (self-tested).
- **Self-test** additions: native nested execution smoke; ApplyProcess on PixelMath (pixels halved), HistogramTransformation (table H), SCNR (enums) + nine error paths + busy view; tool schemas per mode; per-mode tone markers; Guided decline/approve; Advisor refusal; the loop (multi-tool rounds, 12-round cap, Stop, rollback, image stripping, history validator); the real request bytes of a two-round tool loop through a strict loopback server; and a gated **live** run where the model halves a synthetic image with PixelMath.
```
Under `## Verified`, add: `**<release date>** — headless self-test PASS incl. live agent run (PixelMath, median ratio <liveAgentRatio>). GUI: pending user verification (0.1.1.0 via repository pull).`

- [ ] **Step 3: Final self-test on the release build.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) && bash test/run-selftest.sh`
Expected: `anthropic check`, `two-turn check`, `vision check` and `live agent check` all `RAN`, then `PASS: self-test verdict all green`. Any SKIPPED blocks the release.

- [ ] **Step 4: Release.**
```bash
cd /home/scarter4work/projects/astro-pi
test -s /tmp/.pi_codesign_pass && stat -c '%a' /tmp/.pi_codesign_pass   # expect 600
./release.sh
```
Expected: all stages run, including `== 3a/6 package PICopilot module tarball ==`, and the final `== 6/6 integrity check … ==` passes. `repository/<YYYYMMDD>-linux-x64-PICopilot-0.1.1.0.tar.gz` exists, and `updates.xri` names it.

- [ ] **Step 5: Commit version + artifacts, merge, push, shred.**
```bash
cd /home/scarter4work/projects/astro-pi
git status --short        # review: version files, README, repository/, re-signed .xsgn only
git add modules/pi-copilot/src/module/PICopilotVersion.h modules/pi-copilot/src/module/PICopilotModule.cpp \
        modules/pi-copilot/README.md repository/
git add -u
git commit -m "release(pi-copilot): ship PICopilot 0.1.1.0 (increment 4: agent loop + apply_process, live modes) via repository

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
git checkout main && git pull --ff-only && git merge --no-ff feat/pi-copilot-inc4 -m "Merge feat/pi-copilot-inc4: PI Copilot increment 4 (agent + apply_process)

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
git push origin main
shred -u /tmp/.pi_codesign_pass
```
Expected: the push succeeds, and `/tmp/.pi_codesign_pass` no longer exists.

- [ ] **Step 6: Verify the served manifest + tarball.**
```bash
cd /home/scarter4work/projects/astro-pi
TGZ=$(grep -o 'fileName="[^"]*-linux-x64-PICopilot-[0-9.]*\.tar\.gz"' repository/updates.xri | cut -d'"' -f2)
LOCAL=$(sha1sum "repository/$TGZ" | cut -d' ' -f1)
BASE=https://raw.githubusercontent.com/scarter4work/astro-pi/main/repository
timeout 600 bash -c "until curl -fsSL '$BASE/updates.xri' | grep -q '$LOCAL'; do sleep 15; done" \
  && echo "manifest serves sha1 $LOCAL" \
  && [ "$(curl -fsSL "$BASE/$TGZ" | sha1sum | cut -d' ' -f1)" = "$LOCAL" ] && echo "tarball sha1 matches"
```
Expected: `manifest serves sha1 <40 hex>` then `tarball sha1 matches`. If the harness blocks `sleep`, poll with the Monitor tool using the same until-condition.

- [ ] **Step 7: User verification handoff (USER, in PixInsight, repo pull only; never a local `-m=` load).** Give the user this checklist and record the answers in the README "Verified" line:
  1. *Resources ▸ Updates ▸ Check for Updates*: install PICopilot **0.1.1.0** and restart. Open *Process ▸ Etc ▸ PICopilot*.
  2. **Copilot applies:** open an image and choose mode **Copilot**. Ask "Halve the brightness of this image using PixelMath". The log shows `▶ apply_process PixelMath {"expression":…} → ok (…)`, and the image gets darker.
  3. **History/undo:** the History Explorer shows the PixelMath step, and *Edit ▸ Undo* restores the image.
  4. **Guided asks first:** switch to **Guided** and ask for a stretch. A dialog lists the process and parameters. **No** leaves the image unchanged, and the model acknowledges it. **Yes** applies it.
  5. **Advisor refuses to act:** switch to **Advisor** and ask it to apply something. Nothing changes, and it gives the settings instead.
  6. **Mode persists:** restart PixInsight. The last selected mode is still selected.
  7. **Stop works:** ask for a multi-step job (e.g. "try three different stretches and compare"), then press **Stop** mid-way. The log shows `(stopped)`, and any process already running finishes normally.
  8. **SPCC-style multi-parameter request:** e.g. "Run SPCC on this image for an SHO palette". It either runs, or ends with a clear, specific error (for example that the image needs an astrometric solution). It never claims success without an `→ ok` line.
  9. **Tone:** replies feel helpful, not critical. The SHO request is carried out (or set up) without pushback, and at most one brief caveat is given.
  10. **Resizable panel:** drag the window edges. It resizes both ways, and the chat log grows and shrinks with it. Close and reopen the panel (and restart PixInsight): the size is remembered.

---

## Self-Review

- **Spec coverage:** §3 `apply_process` via `ProcessInstance` → Task 2 (+ Task 4 tool). `list_processes`/`describe_process`/`get_view_context` as tools → Task 4. §4 AgentSession → Task 5, SystemPrompt → Task 4. §5 threading (worker HTTP, root-thread tools, Timer marshal) → Tasks 5/6. §8 errors (verbatim HTTP, no masked failures) → Tasks 2/4/5/6. §12-4 → all. `run_pjsr` / `ExecuteGlobal` deliberately excluded: global-only processes get the ruled error.
- **Rulings:** A (4 tools, inputs) → T4 `ToolDefinitions`/`ExecuteTool`. B (real modes, Guided confirm, Advisor omits apply, persisted) → T4 + T6. C (default instance, enum id/alias, range, whole tables, Validate → CanExecuteOn, busy first, root thread, precise errors, global-only error) → T2. D (summary + collapsed context + preview) → T4 `ApplyProcessTool`. E (all tool_use in order, UI thread, 12-round cap, valid pairs, stripping inside tool_results, mid-loop failure) → T3 strip + T5 + T6. F (Stop) → T5 + T6. G (log lines) → T4 format + T6. H + tone ruling (per-mode prompt, idioms, verbatim tone text, headless marker test, checklist item 9) → T4 + T8. I (tests) → A0-A6. J (0.1.1.0, release, sha1, checklist) → T8. K → 8 tasks, with scaffolding folded in. Resizable-panel ruling → T6 (probe first, explicit min/max, chat log stretch, geometry persistence reasoning) + checklist item 10.
- **Placeholder scan:** none. The signing password is referenced by location only. Where a core fact could differ (HT column ids, SCNR range, enum ids), Task 1 records it and the later assertion enforces it. Nothing is left to guess.
- **Type consistency:** `ApplyProcess(const IsoString&, const nlohmann::json&, const nlohmann::json&, View)` is used identically in T2/T4. `ToolCall{id,name,input}`, `ToolOutcome{isError,content,logLine}`, `ToolContext{mode,activeView,confirm}` and `ConfirmApplyFn(processId, viewId, changes)` are used identically in T4/T5/T6/T7. `AgentSession::OnResponse(r, run, stopRequested, onLog)` is the same in T5/T6/T7. `AgentStep::Kind{SendAgain,Done,Failed,CapReached,Stopped}`, `ChatThread(..., tools)` / `AnthropicRequest(..., url, timeout, tools)` argument order, and `AgentMode` values == combo indices (Copilot 0, Advisor 1, Guided 2) are all consistent. Every verdict key in `required_true` (`agentSmokeOk`, `applyProcessOk`, `toolTransportOk`, `agentToolsOk`, `agentLoopOk`, `agentWireOk`, `panelResizableOk`, `liveAgentOk`) is set by exactly one section.
- **Unverified, de-risked in-plan:**
  1. Nested `ExecuteOn` inside the self-test's own `ExecuteGlobal` (headless). Task 1 proves it first, and the BLOCKED path is defined.
  2. Whether the core's enum `SetParameterValue` expects the element VALUE or INDEX. SCNR has value == index, so the probe cannot tell. Every set is read back and a mismatch is an error, so a wrong assumption fails loudly, never silently.
  3. Whether `MessageBox` renders the rich text, and how Stop behaves when clicked while a process runs (event pumping). Both are GUI-only, covered by checklist items 4 and 7.
  4. Whether History/undo appear for a panel-driven `ExecuteOn`. `swapFile=true` is documented to do so, and checklist item 3 confirms it.
  5. Live-model behaviour (Task 7). Run twice, with the ratio bounds catching a double application.
  6. What currently pins the panel's size is not observed yet. Task 6 Step 2 measures it headlessly before the fix. If the headless probe cannot reproduce the problem, that is stated, and checklist item 10 is the verification.
