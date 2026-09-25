# PI Copilot — Increment 5: run_pjsr, streaming, conversation management, keyring, global processes — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** PI Copilot 0.1.2.0. Replies stream in live. The conversation stays within a token budget and uses prompt caching. The API key lives in the system keyring. The model can be chosen in ⚙. The model can run global processes, including ImageIntegration over files on disk. A process safety policy denies or gates processes with side effects outside the image. An off-by-default `run_pjsr` escape hatch runs model-written JavaScript only after the user has read and approved every script.

**Architecture:** Five new headless-testable units and four panel/dialog changes on top of the increment-4 `AgentSession` design. The worker thread still does HTTP only, and every tool still runs on the UI thread.
- `SseStream` parses the Server-Sent Events stream on the worker thread and rebuilds a final message with exactly the non-streamed shape. `ParseMessagesResponse()`, `AgentSession` and the history validator are therefore unchanged. Text deltas go to the UI through a mutex-guarded queue that the panel's Timer drains.
- `HistoryBudget` trims the oldest whole exchanges once the estimated history size passes a budget, always cutting at a fresh user turn so every `tool_use`/`tool_result` pair stays valid. The request body gains `cache_control` breakpoints, and a per-model profile (`ModelCatalog`) adds the documented thinking-binding controls for models that enforce preserved thinking.
- `Keyring` wraps `secret-tool` through `pcl::ExternalProcess` (`/usr/bin/env -u LD_LIBRARY_PATH`), and `KeyStore` migrates a plaintext Settings key into the keyring, removing the plaintext copy only after a verified read-back.
- `ProcessSafety` holds a compiled deny/confirm policy with a coverage self-test. `ProcessApply` gains `RunGlobalProcess()`: file paths are checked first, then `ExecuteGlobal`, then new windows are found by a before/after window diff. `PjsrRunner` does a parse-only syntax check (`new Function(<string literal>)`, which cannot execute) and then runs the script through `EvaluateScript` with console capture.

**Tech Stack:** C++17, PCL SDK (`$HOME/PCL`, sources `/opt/PixInsight/src/pcl/`), nlohmann/json 3.11.3, PixInsight 1.9.5 headless harness (`xvfb-run`, `--automation-mode`, isolated slot 90), `secret-tool` (libsecret CLI, runtime only), Python 3 loopback servers (tests only), `release.sh`.

**Spec:** `docs/superpowers/specs/2026-09-20-pi-copilot-native-pcl-design.md`, specifically §3 (`run_pjsr`: off by default, pre-flight validate → confirm → execute on the root thread, and `apply_process` via `ExecuteGlobal()`), §5 (streamed text marshalled to the UI), §6 (key handling, amended by scope item 4: keyring first), §8 (errors) and §12 item 5. This plan builds on main @ `8972d10` (PICopilot 0.1.1.0). The binding scope is `.superpowers/sdd/inc5-scope.md` plus Scott's 7-item brief. The increment-4 ledger's deferred items folded in here are: the process deny-list review (Task 7, before `run_pjsr`), prompt caching + history cap (Task 4), the vacuous Advisor assertion (Task 9) and hard-gating the live checks (Task 3, `PICOPILOT_REQUIRE_LIVE=1`).

**API facts verified for this plan (headers, PJSR sources, the claude-api skill; 2026-09-24):**
- `MetaModule::EvaluateScript( const String&, const IsoString& = "JavaScript" )` runs on the root thread only. It returns the value of the last expression statement and **throws `Error`** on a syntax error or an uncaught exception (`MetaModule.h:620-657`). An endless loop cannot be interrupted.
- PJSR `console.beginLog()` / `console.endLog()` returns a `ByteArray`, and `ByteArray.toString()` returns a String (pjsr_class_info). PI's own scripts use exactly this pair (`MureDenoiseDetectorSettings/MainViewController.js:589`, `WavefrontEstimator/OutputDirectoryController.js:1624`). Undo flags are `UndoFlag_PixelData` etc. (pjsr_constants).
- `pjsr_validate` is a **Node** tool in `tools/pjsr_parser`. It is not shipped and not present on users' machines, so the shipped module cannot use it. The pre-flight instead uses the core parser: `new Function( "targetViewId", <JSON string literal of the code> )` inside `EvaluateScript` parses the body completely and never runs it. Because the code is passed as a **string literal** (ASCII-escaped JSON), text such as `}); evil(); (function(){` cannot break out and execute during validation. The "uncalled function-expression wrapper" suggested in the scope *can* be broken out of that way, so this plan replaces it. Task 1 proves both properties.
- `ProcessInstance::CanExecuteGlobal( String& whyNot ) const` and `ExecuteGlobal()` (`ProcessInstance.h:253-262`). `Process::Parameters()`, `CanProcessGlobal()`, `AllProcesses()` (`Process.h:181,268,292`). `ImageWindow::AllWindows()`, `WindowById()` (`ImageWindow.h:3017-3050`). `FileFormat( ext, toRead, toWrite )` throws when no installed format matches (`FileFormat.h:98-134`). `FileInfo::Exists()/IsFile()/IsReadable()`.
- ImageIntegration `images` rows are `[enabled, path, drizzlePath, localNormalizationDataPath]` (`AutoIntegrate/AutoIntegrateEngine.js:1834`). The outputs are the read-only ids `integrationImageId`, `lowRejectionMapImageId`, `highRejectionMapImageId` and `slopeMapImageId` (`AutoIntegrateCalibrate.js:58-65`). II has `generateDrizzleData` (writes `.xdrz` files next to the frames) and `closePreviousImages` (closes windows). **It has not been run live on synthetic files yet, so Task 1 does that first.**
- `pcl::ExternalProcess`: `Start( program, args )`, `WaitForStarted`, `Write`, `CloseStandardInput`, `IsStarting/IsRunning`, `ExitCode`, `HasCrashed`, `StandardOutput/StandardError` (`ExternalProcess.h`). Repo memory `pi-externalprocess-gotchas`: PI pollutes the child's `LD_LIBRARY_PATH` (so run through `/usr/bin/env -u LD_LIBRARY_PATH`), and `waitForFinished` can return early (so spin on `IsStarting()||IsRunning()` with `ProcessEvents`). `secret-tool` is at `/usr/bin/secret-tool`, and `store` reads the secret from stdin.
- Streaming (NetworkTransfer): `OnDownloadDataAvailable` fires incrementally **during** a POST (research probe: chunks at 800/1600/2400/3200 ms matched the server). The SSE events are `message_start`, `content_block_start`, `content_block_delta` (`text_delta`, `input_json_delta`, `thinking_delta`, `signature_delta`, `citations_delta`), `content_block_stop`, `message_delta` (stop_reason + usage), `message_stop`, `ping` and `error` (claude-api skill `curl/examples.md` + `shared/tool-use-concepts.md`).
- Models (claude-api skill, cached 2026-06-24). The ids are `claude-opus-4-8`, `claude-opus-5-5`, `claude-fable-5-1`, `claude-sonnet-5` and `claude-haiku-4-5`. The skill says to use the table ids exactly, with **no date suffix**, so Haiku is `claude-haiku-4-5`, not the `-20251001` form in the brief. Opus 5.5 and Fable 5.1 **always think** and enforce *preserved thinking* for accounts created on or after 2026-08-31. Under that rule, editing earlier turns (our `StripOlderImages`, history trimming, a system/tools change) invalidates later thinking blocks and causes a 400, unless the request sends `anthropic-beta: thinking-binding-controls-2026-08-01` with `thinking: {type: "adaptive", block_binding: {prefix_mismatch_behavior: "drop_block"}}`. Forced `tool_choice` is a 400 on both, and PI Copilot never uses it.
- Prompt caching (skill `shared/prompt-caching.md`). A request has at most 4 breakpoints, and the render order is tools → system → messages. A top-level `cache_control: {type: "ephemeral"}` is automatic caching: it places a rolling breakpoint on the last cacheable block. The minimum cacheable prefix is 1024 tokens on Opus 4.8 and 4096 on Haiku 4.5 (below that it silently does not cache). The effect shows as `usage.cache_read_input_tokens`.

## Global Constraints

- The module ID string is `"PICopilot"` and is STABLE: never rename it. The process id, interface id and Settings prefix `PICopilot/` stay as they are.
- C++17. Flags are exactly `-fPIC -fvisibility=hidden -fvisibility-inlines-hidden`. Defines are exactly `__PCL_LINUX __PCL_BUILDING_MODULE _REENTRANT`. Output is `PICopilot-pxm.so` (+ `PICopilot-pxm.xsgn`). Every new `.cpp` goes into `MODULE_SOURCES` in `modules/pi-copilot/src/module/CMakeLists.txt`.
- **Root-thread rules (hard):** the following are **root (UI) thread only**:
  - `ImageWindow`, `View`, `Bitmap`, and every `Control` (including `MessageBox` and `Dialog`).
  - `ProcessInstance` construction/`Validate`/`CanExecuteOn`/`CanExecuteGlobal`/`ExecuteOn`/`ExecuteGlobal`.
  - All catalog introspection, `MetaModule::EvaluateScript` and `pcl::ExternalProcess`.

  Constructing a Control off-root throws `CreateControl(): API function error`. The worker `ChatThread` only `Perform()`s an already-built request. `Thread::Run()` never touches GUI, console, views, processes or scripts. Every tool runs inside the panel's Timer handler.
- **Never block on a busy view.** Probe with the non-waiting `View::CanRead()` / `View::CanWrite()` before any read or execute (unchanged from inc 4).
- **Pre-validate everything the core would reject with a modal dialog.** Core rejections can pop uncatchable modals (inc-4 Task 1). Parameters, table shapes and file paths are checked before the core sees them. Tests run under `xvfb-run`, so a modal only fails the run by timeout, loudly.
- **UTF-8 on the wire:** every `pcl::String` that enters JSON goes through `U8()` (`Utf8.h`), never `String::ToUTF8()`. The body is POSTed only via `PostBytes()`. Non-ASCII literals are written as UTF-8 byte escapes plus `String::UTF8ToUTF16`, because `String( const char* )` is Latin-1.
- **No masked failures:** every tool failure is a `tool_result` with `is_error: true` and a precise, model-correctable message. A value that does not read back as set is an error. An unknown stream *delta* type is an error, not ignored. HTTP and API errors are shown verbatim in the log. An API-invalid history is never sent.
- **Tool loop limits (unchanged):** `PICopilotMaxToolRounds = 12`, `PICopilotMaxToolCallsPerStep = 8`, `PICopilotToolLogParamChars = 120`, `PICopilotConfirmChangesChars = 1500`.
- **Image limits (unchanged):** preview long edge ≤ 1024 px, JPEG quality 85, base64 ≤ 5 MiB. Only the last message carries images.
- **Transport (changed by this plan, exact values):**
  - Default model `PICOPILOT_DEFAULT_MODEL` = `claude-opus-4-8`.
  - Production requests stream (`"stream": true`) with `max_tokens` = `PICopilotStreamMaxTokens` = **16000**. The default `RequestShape` (tests) stays non-streamed with **4096**.
  - Overall deadline `PICopilotRequestTimeoutSeconds` = **600**, and stream idle limit `PICopilotStreamIdleSeconds` = **120** (no bytes for 120 s ends the request as stalled).
  - Cancel via `RequestCancel()`.
- **Conversation limits (exact):** history budget `PICopilotHistoryTokenBudget` = **100000** estimated tokens, trimmed down to `PICopilotHistoryTrimTarget` = **70000**. Estimate = UTF-8 text bytes / 3, plus **1600** per image, plus 8 per message.
- **Prompt caching:** `cache_control: {type: "ephemeral"}` goes on the last tool and on the (array-form) system block, plus a top-level automatic `cache_control` for the rolling conversation tail. That is 3 of 4 breakpoints.
- **Preserved thinking:** for models with `thinkingBinding` (`claude-opus-5-5`, `claude-fable-5-1`), every request sends header `anthropic-beta: thinking-binding-controls-2026-08-01` and `"thinking": {"type": "adaptive", "block_binding": {"prefix_mismatch_behavior": "drop_block"}}`. Other models get no `thinking` key. Thinking blocks are stored and re-sent verbatim (signature included).
- **run_pjsr safety:**
  - It is **off by default**, and ⚙ "Allow scripts" is persisted as `PICopilot/RunPjsrEnabled`.
  - It is never offered in Advisor.
  - Every script is shown in full in a confirm dialog whose default is **Don't run**. This happens EVERY time, in every mode, and nothing bypasses it.
  - Script limits (exact values):

    | Constant | Value |
    |---|---|
    | `PICopilotMaxScriptChars` | 20000 |
    | `PICopilotMaxScriptConsoleChars` | 8000 (tail kept) |
    | `PICopilotMaxScriptValueChars` | 4000 |

  - A syntax error is returned to the model **without** showing the dialog.
- **Global processes:** only offered in Copilot/Guided (`run_global_process`). Guided always confirms. Copilot confirms only when the safety policy says so (ruling below). Declared file-table paths must be absolute, existing, readable files that an installed format can read. ImageIntegration needs ≥ **3** enabled frames. At most **4** created windows are described in detail in a tool_result.
- **Process safety policy** (`data/process-safety.json`, compiled in) applies to both `apply_process` and `run_global_process`. A `deny` entry is never run. `confirmAlways`/`confirmWhen` asks the user in **every** mode. The coverage self-test fails if any installed process with a file/disk/window-looking parameter is unclassified.
- **Keyring:**
  - Never link libsecret. Use `secret-tool` via `pcl::ExternalProcess`, program `/usr/bin/env`, args `-u LD_LIBRARY_PATH secret-tool …`.
  - Production attributes are `service picopilot account anthropic-api-key`. The self-test switches to `service picopilot-selftest` **before any KeyStore use** and never touches the production attributes or the harness's `service anthropic account default`.
  - The plaintext Settings copy is removed only after a keyring write has been read back equal.
  - The key never appears in a note, log line, error or test output.
- **Never `make install`.** Dev tests load the module only through `test/run-selftest.sh` (`xvfb-run -a`, `-n=90` isolated slot + guard, `-m=` + `-r=` + `--force-exit` + `timeout 600`). **The user tests only by repo pull** from `https://raw.githubusercontent.com/scarter4work/astro-pi/main/repository/`. Never launch the PixInsight GUI with `-m=`.
- **The GUI cannot be tested headlessly.** This covers:
  - live log rendering of streamed text
  - the ⚙ dialog
  - the Guided/safety confirm and script-confirm dialogs
  - the "Capturing view…" caption
  - New chat
  - left/right placement

  The headless self-test covers every unit behind them. The GUI parts are user-verified after release (Task 10 checklist). Never claim them as tested from a headless run.
- **Signing password file:** `/tmp/.pi_codesign_pass`, mode 0600, containing the password from `~/.claude/CLAUDE.md` § "Module Signing". Create it before Task 1 and shred it after Task 10. **Never write the password into any committed file (including this plan).**
- **Test API key:** never echo it and never commit it. Sources, in order: the keyring (`secret-tool lookup service anthropic account default`), then `modules/pi-copilot/test/.test_api_key`, then skip. Fixture capture sends it only via `curl -H @-` (stdin, not argv) and asserts it is absent from every fixture. The release run uses `PICOPILOT_REQUIRE_LIVE=1`, which turns every skipped live check into a failure.
- Branch `feat/pi-copilot-inc5`. Every commit message ends with `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`.

## Rulings made in this plan

1. **Global runs and confirmation.** In Copilot, a `run_global_process` call does **not** ask by default. Guided always asks, and any mode asks when the safety policy says `confirmAlways`/`confirmWhen`.

   Why: a global run reads files and creates **new** windows. It never modifies an open image, so it is strictly less destructive than `apply_process`, which changes the user's real image (undoably) and which Copilot already runs without asking. What does warrant a human gate is a side effect *outside* new windows, such as writing files next to the frames (`generateDrizzleData`), closing windows (`closePreviousImages`), or output directories and overwrites (ImageCalibration & co.). The policy targets exactly those, in every mode, so a Copilot user is asked precisely when something beyond "new windows appear" will happen.

   Cost: a long ImageIntegration can start without a dialog in Copilot. That is the same posture as a long apply_process, and Stop does not interrupt a running process in either case.
2. **Pre-flight validation** uses `new Function(<ASCII JSON string literal>)` in the core engine, not `pjsr_validate` (Node, not shipped) and not an inline wrapper (breakout risk, see API facts). Task 1 proves that parsing does not execute and that a breakout text is only a SyntaxError.
3. **Haiku id** is `claude-haiku-4-5` (the skill's canonical id; dated suffixes are forbidden there).
4. **Thinking binding with `drop_block`** is the documented way to keep an editing harness (image stripping, trimming, a mode change) working on Opus 5.5 / Fable 5.1. The alternative, never editing history, would drop the cost rule and the budget. A gated live check (Task 4) proves a 200 on an edited history. If the API rejects the header or field, that is **BLOCKED** (report the response body) and nothing is improvised.

## File Structure

```
modules/pi-copilot/
  data/process-safety.json                 # NEW (T7, T8): deny / confirmAlways / confirmWhen / reviewedSafe / fileTables
  src/module/CMakeLists.txt                # MODIFY: new sources; configure ProcessSafetyData.h
  src/module/PICopilotInc5SelfTest.h/.cpp  # NEW (T1..T9): headless sections B0-B8 + gated live B3L; own tiny test-image helpers
  src/module/PICopilotSelfTest.cpp         # MODIFY (T1, T5): call RunInc5SelfTest(); keyring test attributes set FIRST
  src/module/SseStream.h/.cpp              # NEW (T2): SseMessageAssembler (SSE parse + final-message rebuild)
  src/module/AnthropicClient.h/.cpp        # MODIFY (T3, T4, T6): RequestShape, errorKind, usage, streaming sink, text-delta queue, idle deadline, caching, thinking binding, refusal wording
  src/module/ChatThread.h/.cpp             # MODIFY (T3): shape pass-through, TakeStreamedText()
  src/module/ModelCatalog.h/.cpp           # NEW (T4): the model list + thinkingBinding flag
  src/module/HistoryBudget.h/.cpp          # NEW (T4): EstimateHistoryTokens(), TrimHistoryToBudget()
  src/module/AgentSession.h/.cpp           # MODIFY (T4, T6): trim on append, TakeTrimmedMessages(); errorKind into AgentStep
  src/module/Keyring.h/.cpp                # NEW (T5): secret-tool lookup/store/clear via ExternalProcess
  src/module/KeyStore.h/.cpp               # REWRITE (T5): keyring-first State API, migration, Settings fallback + note
  src/module/CopilotSettings.h/.cpp        # NEW (T6): persisted model / run_pjsr toggle / panel side
  src/module/PanelPlacement.h              # MODIFY (T6): PanelSide (left/right)
  src/module/ConfigDialog.h/.cpp           # REWRITE (T6): key + storage line, model, Allow scripts, side
  src/module/TurnEndNotes.h/.cpp           # MODIFY (T6): wording by error kind (no "Error 0")
  src/module/ProcessSafety.h/.cpp          # NEW (T7, T8): policy verdicts, coverage candidates, file-path validation
  src/module/ProcessSafetyData.h.in        # NEW (T7): compiled-in JSON
  src/module/ProcessApply.h/.cpp           # MODIFY (T8): SetParameters() extracted; PrecheckGlobalRun(), RunGlobalProcess()
  src/module/PjsrRunner.h/.cpp             # NEW (T9): CheckPjsrSyntax(), RunPjsr(), ScriptLiteral()
  src/module/ScriptConfirmDialog.h/.cpp    # NEW (T9): full-script approval dialog (default Don't run)
  src/module/AgentTools.h/.cpp             # MODIFY (T7, T8, T9): safety gate, run_global_process, run_pjsr, ToolOptions
  src/module/SystemPrompt.h/.cpp           # MODIFY (T8, T9): global + script guidance, BuildSystemPrompt(mode, opts)
  src/module/PICopilotInterface.h/.cpp     # MODIFY (T3-T9): live streaming, New chat, trim note, key note, model per message, capturing caption, side, script confirm
  src/module/PICopilotAgentSelfTest.cpp    # MODIFY (T3, T8, T9): A6 live run streams; A3 tool-name lists; Advisor literal
  src/module/PICopilotVersion.h, PICopilotModule.cpp  # MODIFY (T10): 0.1.2.0, date, description
  test/run-selftest.sh                     # MODIFY: verdict keys; fixtures env; SSE loopback paths; echo shape; REQUIRE_LIVE
  test/capture-sse-fixtures.sh             # NEW (T2): records real SSE streams (key via stdin, never printed)
  test/fixtures/sse/*.sse                  # NEW (T2): recorded streams (no key inside, asserted)
  README.md                                # MODIFY (T7, T10): safety policy table, Increment 5 section, Verified line
repository/                                # REGENERATED by ./release.sh (T10)
```

Each unit has one job. `SseStream` knows SSE and nothing about HTTP. `HistoryBudget` knows message sizes and pair rules. `Keyring` knows secret-tool, and `KeyStore` knows where the key lives. `ProcessSafety` is policy, and `PjsrRunner` is the script engine. Everything except the dialogs and the panel is exercised headlessly by `PICopilotInc5SelfTest.cpp`, which is kept separate from the increment-3/4 self-test files, as increments 3 and 4 did.

---

## Task 1: Platform smoke — ImageIntegration ExecuteGlobal on synthetic FITS; PJSR parse-without-execute; console capture (headless)

This task proves the three unverified platform behaviours before any production code:
1. ImageIntegration's `ExecuteGlobal()` runs from inside the self-test's own `ExecuteGlobal()` on three tiny FITS files written to a temp dir, and the result window is found by id and by window diff.
2. `new Function( "targetViewId", "<literal>" )` parses without executing, and a breakout text is only a SyntaxError.
3. `console.beginLog()/endLog()` captures `console.writeln` inside one `EvaluateScript`.

It also records the facts Tasks 8 and 9 use: the II column ids, the enumeration ids of `weightMode`/`rejection`/`combination`/`normalization`, the output ids, and the PJSR line numbering of `new Function` bodies. **If II does not run (`iiRan` false) or a PJSR property fails, stop and report BLOCKED with `inc5SmokeInfo`. Do not change the production design to work around it.**

**Files:**
- Create: `modules/pi-copilot/src/module/PICopilotInc5SelfTest.h`
- Create: `modules/pi-copilot/src/module/PICopilotInc5SelfTest.cpp`
- Modify: `modules/pi-copilot/src/module/PICopilotSelfTest.cpp` (after the increment-4 block, before `ok = ok && visionOk && agentOk;`)
- Modify: `modules/pi-copilot/src/module/CMakeLists.txt` (`MODULE_SOURCES`)
- Modify: `modules/pi-copilot/test/run-selftest.sh` (`required_true`)

**Interfaces:**
- Consumes: `EnumerationInfoOf()` (`ProcessCatalog.h`), `U8()`, `ThePICopilotModule->EvaluateScript/ProcessEvents`.
- Produces (file-local in `PICopilotInc5SelfTest.cpp`, used by Tasks 2-9):
  - `class SyntheticFrames { explicit SyntheticFrames( int n = 3 ); const StringList& Paths() const; const String& Dir() const; double MeanOfMeans() const; }`: n deterministic 64×64 mono float FITS frames in a fresh temp dir, removed in the destructor.
  - `std::set<std::string> OpenMainViewIds()`, `void ForceCloseWindows( const std::vector<std::string>& ids )`.
  - `String EvalJs( const String& src )`: `EvaluateScript(...).ToString()`.
  - `class Inc5TestWindow { Inc5TestWindow( const char* id, int w, int h, int channels, double value ); View MainView() const; }`: a hidden float window filled with a constant, force-closed on destruction.
  - `double Inc5Median( View v, int channel )`.
  - Public: `bool pcl::RunInc5SelfTest( nlohmann::json& out );`. Later tasks insert sections above the marker `// ---- inc5 sections end ----`.

- [ ] **Step 0: Branch + prerequisites.**

```bash
cd /home/scarter4work/projects/astro-pi
git checkout main && git pull --ff-only && git checkout -b feat/pi-copilot-inc5
test -s /tmp/.pi_codesign_pass && stat -c '%a' /tmp/.pi_codesign_pass || echo "create /tmp/.pi_codesign_pass (0600) from ~/.claude/CLAUDE.md Module Signing before continuing"
command -v xvfb-run secret-tool curl
cd modules/pi-copilot && cmake -B build -DPCLDIR=$HOME/PCL -DPICOPILOT_BUILD_MODULE=ON
```
Expected: branch `feat/pi-copilot-inc5`, `600`, three paths printed, and `PCL found at /home/scarter4work/PCL -- building PICopilot module`.

- [ ] **Step 1: Failing assertion.** In `test/run-selftest.sh`, in the python `required_true` list, insert before `'ok',`:
```python
    # increment 5
    'inc5SmokeOk',
```

- [ ] **Step 2: Verify RED.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) && bash test/run-selftest.sh`
Expected: `FAILED keys: inc5SmokeOk` then `FAIL: self-test verdict not all green`.

- [ ] **Step 3: Implement.** `PICopilotInc5SelfTest.h`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_Inc5SelfTest_h
#define PICopilot_Inc5SelfTest_h

#include <nlohmann/json.hpp>

namespace pcl
{

// Increment-5 headless self-test sections (platform smoke, SSE, streaming,
// conversation, keyring, config, safety policy, global processes, run_pjsr,
// gated live checks). Root thread only. Adds its keys to `out`; returns true
// iff every section passed.
bool RunInc5SelfTest( nlohmann::json& out );

} // namespace pcl

#endif // PICopilot_Inc5SelfTest_h
```
`PICopilotInc5SelfTest.cpp`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PICopilotInc5SelfTest.h"
#include "PICopilotModule.h"
#include "ProcessCatalog.h"
#include "Utf8.h"

#include <pcl/AutoViewLock.h>
#include <pcl/Exception.h>
#include <pcl/File.h>
#include <pcl/FileFormat.h>
#include <pcl/FileFormatInstance.h>
#include <pcl/Image.h>
#include <pcl/ImageVariant.h>
#include <pcl/ImageWindow.h>
#include <pcl/Process.h>
#include <pcl/ProcessInstance.h>
#include <pcl/ProcessParameter.h>
#include <pcl/Variant.h>
#include <pcl/View.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace pcl
{

namespace
{

// ---- shared inc5 test helpers --------------------------------------------

constexpr int kIiW = 64, kIiH = 64;

// Deterministic synthetic light frame: gradient + xorshift noise + 3 Gaussian
// "stars", so noise estimation has something real to measure.
void FillFrame( Image& img, unsigned seed )
{
   img.AllocateData( kIiW, kIiH, 1, ColorSpace::Gray );
   uint32_t s = 2463534242u ^ (seed*2654435761u);
   auto rnd = [&s]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (s & 0xFFFFFFu)/double( 0x1000000 ); };
   const double stars[3][2] = { { 16, 20 }, { 40, 44 }, { 50, 12 } };
   for ( int y = 0; y < kIiH; ++y )
      for ( int x = 0; x < kIiW; ++x )
      {
         double v = 0.1 + 0.05*x/(kIiW - 1) + 0.01*(rnd() - 0.5);
         for ( const auto& st : stars )
         {
            const double dx = x - st[0], dy = y - st[1];
            v += 0.5*std::exp( -(dx*dx + dy*dy)/(2*1.5*1.5) );
         }
         img.Pixel( x, y ) = float( v );
      }
}

void WriteFits( const String& path, const Image& img )
{
   FileFormat fits( ".fits", false/*toRead*/, true/*toWrite*/ );
   FileFormatInstance f( fits );
   if ( !f.Create( path ) )
      throw Error( "SyntheticFrames: cannot create " + path );
   if ( !f.WriteImage( img ) )
      throw Error( "SyntheticFrames: cannot write " + path );
   f.Close();
}

// n synthetic FITS frames in a fresh temp directory; files + directory are
// removed in the destructor (best effort: a leftover is reported, not thrown).
class SyntheticFrames
{
public:

   explicit SyntheticFrames( int n = 3 )
   {
      m_dir = File::UniqueFileName( File::SystemTempDirectory(), 12, "picopilot-inc5-" );
      if ( !m_dir.StartsWith( '/' ) )   // a bare name: anchor it in the temp directory
         m_dir = File::SystemTempDirectory() + '/' + m_dir;
      File::CreateDirectory( m_dir );
      double sum = 0;
      for ( int i = 0; i < n; ++i )
      {
         Image img;
         FillFrame( img, unsigned( i + 1 ) );
         sum += img.Mean();
         const String path = m_dir + String().Format( "/light_%02d.fits", i + 1 );
         WriteFits( path, img );
         m_paths << path;
      }
      m_meanOfMeans = n > 0 ? sum/n : 0;
   }

   ~SyntheticFrames()
   {
      try
      {
         for ( const String& p : m_extra )
            if ( File::Exists( p ) )
               File::Remove( p );
         for ( const String& p : m_paths )
            if ( File::Exists( p ) )
               File::Remove( p );
         if ( File::DirectoryExists( m_dir ) )
            File::RemoveDirectory( m_dir );
      }
      catch ( ... )
      {
      }
   }

   SyntheticFrames( const SyntheticFrames& ) = delete;
   SyntheticFrames& operator =( const SyntheticFrames& ) = delete;

   const StringList& Paths() const { return m_paths; }
   const String& Dir() const { return m_dir; }
   double MeanOfMeans() const { return m_meanOfMeans; }

   // Another file in the directory (e.g. a non-image), removed with it.
   String AddFile( const String& name, const IsoString& contents )
   {
      const String p = m_dir + '/' + name;
      File::WriteFile( p, contents.Begin(), contents.Length() );
      m_extra << p;
      return p;
   }

private:

   String     m_dir;
   StringList m_paths;
   StringList m_extra;
   double     m_meanOfMeans = 0;
};

std::set<std::string> OpenMainViewIds()
{
   std::set<std::string> ids;
   for ( const ImageWindow& w : ImageWindow::AllWindows() )
      ids.insert( std::string( w.MainView().Id().c_str() ) );
   return ids;
}

void ForceCloseWindows( const std::vector<std::string>& ids )
{
   for ( const std::string& id : ids )
      try
      {
         ImageWindow w = ImageWindow::WindowById( IsoString( id.c_str() ) );
         if ( !w.IsNull() )
            w.ForceClose();
      }
      catch ( ... )
      {
      }
}

String EvalJs( const String& src )
{
   return ThePICopilotModule->EvaluateScript( src, "JavaScript" ).ToString();
}

// Hidden float window filled with one constant; force-closed on destruction.
class Inc5TestWindow
{
public:

   Inc5TestWindow( const char* id, int w, int h, int channels, double value )
      : m_window( w, h, channels, 32, true/*float*/, channels >= 3/*color*/, false, IsoString( id ) )
   {
      if ( m_window.IsNull() )
         throw Error( "Inc5TestWindow: null window" );
      View v = m_window.MainView();
      AutoViewLock lock( v );
      ImageVariant iv = v.Image();
      static_cast<Image&>( *iv ).Fill( float( value ) );
   }

   ~Inc5TestWindow()
   {
      try { if ( !m_window.IsNull() ) m_window.ForceClose(); } catch ( ... ) {}
   }

   Inc5TestWindow( const Inc5TestWindow& ) = delete;
   Inc5TestWindow& operator =( const Inc5TestWindow& ) = delete;

   View MainView() const { return m_window.MainView(); }

private:

   ImageWindow m_window;
};

// [[maybe_unused]]: first used by Section B6 (Task 7).
[[maybe_unused]] double Inc5Median( View v, int channel )
{
   if ( !v.CanRead() || !v.CanWrite() )
      throw Error( "Inc5Median: view is busy" );
   AutoViewWriteLock lock( v );
   ImageVariant iv = v.Image();
   return iv.Median( iv.Bounds(), channel, channel );
}

// Sets an enumeration parameter by element id (via the same EnumerationInfoOf
// the tools use). Records a miss in info and returns false.
bool SetEnumById( ProcessInstance& instance, const Process& P, const char* param, const char* id,
                  nlohmann::json& info )
{
   const ProcessParameter p( P, IsoString( param ) );
   for ( const ProcessParameter::EnumerationElement& e : EnumerationInfoOf( p ).elements )
      if ( e.id == id )
         return instance.SetParameterValue( Variant( e.value ), p, 0 ) && instance.ParameterValue( p, 0 ).ToInt() == e.value;
   info[std::string( "missingEnum_" ) + param] = id;
   return false;
}

} // namespace

bool RunInc5SelfTest( nlohmann::json& out )
{
   bool allOk = true;

   // ---- Section B0: platform smoke (Task 1) --------------------------------
   {
      bool iiOk = false, parseNoExecOk = false, breakoutOk = false, syntaxLineOk = false,
           runtimeLineOk = false, consoleOk = false, throwOk = false;
      nlohmann::json info = nlohmann::json::object();
      std::vector<std::string> created;
      String error;
      try
      {
         // (a) ImageIntegration in the global context over 3 synthetic FITS frames.
         SyntheticFrames frames( 3 );
         Process II( IsoString( "ImageIntegration" ) );
         const ProcessParameter images( II, IsoString( "images" ) );
         const ProcessParameter::parameter_list cols = images.TableColumns();
         nlohmann::json colIds = nlohmann::json::array();
         for ( const ProcessParameter& c : cols )
            colIds.push_back( std::string( c.Id().c_str() ) );
         info["iiImagesColumns"] = colIds;
         if ( cols.Length() != 4 )
            throw Error( String().Format( "ImageIntegration.images has %u columns, expected 4", unsigned( cols.Length() ) ) );
         for ( const char* e : { "weightMode", "rejection", "combination", "normalization" } )
         {
            const EnumerationInfo& ei = EnumerationInfoOf( ProcessParameter( II, IsoString( e ) ) );
            nlohmann::json ids = nlohmann::json::array();
            for ( const ProcessParameter::EnumerationElement& el : ei.elements )
               ids.push_back( { { "id", std::string( el.id.c_str() ) }, { "value", el.value } } );
            info[std::string( "iiEnum_" ) + e] = { { "elements", ids }, { "default", std::string( ei.defaultId.c_str() ) } };
         }
         for ( const char* b : { "generateDrizzleData", "closePreviousImages", "generateRejectionMaps", "generateIntegratedImage" } )
            try
            {
               ProcessInstance d( II );
               info[std::string( "iiDefault_" ) + b] = d.ParameterValue( ProcessParameter( II, IsoString( b ) ), 0 ).ToBoolean();
            }
            catch ( ... )
            {
               info[std::string( "iiDefault_" ) + b] = "absent";
            }

         ProcessInstance ii( II );
         bool setOk = ii.AllocateTableRows( images, frames.Paths().Length() );
         for ( size_type r = 0; r < frames.Paths().Length(); ++r )
            setOk = setOk && ii.SetParameterValue( Variant( true ), cols[0], r )
                          && ii.SetParameterValue( Variant( frames.Paths()[r] ), cols[1], r )
                          && ii.SetParameterValue( Variant( String() ), cols[2], r )
                          && ii.SetParameterValue( Variant( String() ), cols[3], r );
         // Equal weights: PSF-signal weighting (the PI 1.9 default) is not what this smoke tests.
         setOk = setOk && SetEnumById( ii, II, "weightMode", "DontCare", info );
         info["iiSetOk"] = setOk;

         String whyNot;
         const bool valid = ii.Validate( whyNot );
         info["iiValidate"] = valid; info["iiValidateWhyNot"] = U8( whyNot );
         whyNot.Clear();
         const bool can = ii.CanExecuteGlobal( whyNot );
         info["iiCanExecuteGlobal"] = can; info["iiCanWhyNot"] = U8( whyNot );

         const std::set<std::string> before = OpenMainViewIds();
         const auto t0 = std::chrono::steady_clock::now();
         const bool ran = setOk && valid && can && ii.ExecuteGlobal();
         info["iiRan"] = ran;
         info["iiSeconds"] = std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count();
         for ( const std::string& id : OpenMainViewIds() )
            if ( before.count( id ) == 0 )
               created.push_back( id );
         info["iiCreatedWindows"] = created;

         nlohmann::json outIds = nlohmann::json::object();
         for ( const char* o : { "integrationImageId", "lowRejectionMapImageId", "highRejectionMapImageId", "slopeMapImageId" } )
            try
            {
               outIds[o] = U8( ii.ParameterValue( ProcessParameter( II, IsoString( o ) ), 0 ).ToString() );
            }
            catch ( ... )
            {
               outIds[o] = "absent";
            }
         info["iiOutputIds"] = outIds;

         const std::string integ = outIds.value( "integrationImageId", std::string() );
         ImageWindow w = integ.empty() ? ImageWindow::Null() : ImageWindow::WindowById( IsoString( integ.c_str() ) );
         if ( !w.IsNull() )
         {
            View v = w.MainView();
            AutoViewWriteLock lock( v );
            ImageVariant iv = v.Image();
            info["iiResultGeometry"] = { iv.Width(), iv.Height(), iv.NumberOfChannels() };
            const double mean = iv.Mean();
            info["iiResultMean"] = mean;
            info["iiInputMeanOfMeans"] = frames.MeanOfMeans();
            iiOk = ran && iv.Width() == kIiW && iv.Height() == kIiH && iv.NumberOfChannels() == 1
                && std::fabs( mean - frames.MeanOfMeans() ) < 0.01*frames.MeanOfMeans()
                && std::find( created.begin(), created.end(), integ ) != created.end();
         }
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }
      ForceCloseWindows( created );

      // (b) PJSR: parse without executing; breakout stays a SyntaxError; line numbers.
      try
      {
         const String parsed = EvalJs(
            "(function(){ var m = { v: 0 }; var f;"
            " try { f = new Function( \"m\", \"m.v = 1;\" ); } catch ( e ) { return \"error: \" + e; }"
            " var before = m.v; f( m ); return \"parsed:\" + before + \":\" + m.v; })()" );
         info["pjsrParse"] = U8( parsed );
         parseNoExecOk = parsed == "parsed:0:1";   // not run by parsing; runs when called

         const String breakout = EvalJs(
            "(function(){ var m = { v: 0 };"
            " try { new Function( \"m\", \"}); m.v = 2; (function(){\" ); return \"parsed:\" + m.v; }"
            " catch ( e ) { return \"syntax:\" + m.v + \":\" + (e instanceof SyntaxError); } })()" );
         info["pjsrBreakout"] = U8( breakout );
         breakoutOk = breakout == "syntax:0:true";

         const String syn = EvalJs(
            "(function(){ try { new Function( \"targetViewId\", \"var a = 1;\\nvar b = 2;\\nvar c = ;\\n\" ); return \"parsed\"; }"
            " catch ( e ) { return JSON.stringify( { name: e.name, message: String( e.message ), line: e.lineNumber } ); } })()" );
         info["pjsrSyntaxForBodyLine3"] = U8( syn );
         const nlohmann::json sj = nlohmann::json::parse( U8( syn ) );
         syntaxLineOk = sj.value( "name", std::string() ) == "SyntaxError" && sj.contains( "line" ) && sj["line"].is_number();

         const String run = EvalJs(
            "(function(){ try { var f = new Function( \"targetViewId\", \"var a = 1;\\nnull.x = 2;\\n\" ); f( \"\" ); return \"ran\"; }"
            " catch ( e ) { return JSON.stringify( { name: e.name, message: String( e.message ), line: e.lineNumber } ); } })()" );
         info["pjsrRuntimeForBodyLine2"] = U8( run );
         const nlohmann::json rj = nlohmann::json::parse( U8( run ) );
         runtimeLineOk = rj.value( "name", std::string() ) == "TypeError" && rj.contains( "line" ) && rj["line"].is_number();
      }
      catch ( const pcl::Exception& x ) { info["pjsrError"] = U8( x.Message() ); }
      catch ( const std::exception& x ) { info["pjsrError"] = x.what(); }

      // (c) console capture inside ONE EvaluateScript; (d) what an uncaught throw looks like.
      try
      {
         const String log = EvalJs(
            "(function(){ var out = {}; console.beginLog();"
            " try { console.writeln( \"pc-smoke-line-1\" ); console.writeln( \"pc-smoke-\\u00e9\" ); }"
            " finally { var b = console.endLog(); out.type = typeof b; out.text = b.toString(); }"
            " return JSON.stringify( out ); })()" );
         info["consoleCapture"] = U8( log );
         const std::string text = nlohmann::json::parse( U8( log ) ).value( "text", std::string() );
         consoleOk = text.find( "pc-smoke-line-1" ) != std::string::npos
                  && text.find( "pc-smoke-\xC3\xA9" ) != std::string::npos;
      }
      catch ( const pcl::Exception& x ) { info["consoleError"] = U8( x.Message() ); }
      catch ( const std::exception& x ) { info["consoleError"] = x.what(); }
      try
      {
         EvalJs( "throw new Error( \"pc-boom\" );" );
         info["evalThrow"] = "no exception";
      }
      catch ( const pcl::Exception& x )
      {
         info["evalThrow"] = U8( x.Message() );
         throwOk = x.Message().Contains( "pc-boom" );
      }

      const bool ok = iiOk && parseNoExecOk && breakoutOk && syntaxLineOk && runtimeLineOk && consoleOk;
      out["inc5SmokeInfo"] = info;
      out["inc5SmokeIiOk"] = iiOk;
      out["inc5SmokePjsrOk"] = parseNoExecOk && breakoutOk && syntaxLineOk && runtimeLineOk;
      out["inc5SmokeConsoleOk"] = consoleOk;
      out["inc5SmokeEvalThrowNamesMessage"] = throwOk;   // informational (RunPjsr catches inside JS anyway)
      out["inc5SmokeError"] = U8( error );
      out["inc5SmokeOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- inc5 sections end ----

   // Let the core finish deferred window teardown before --force-exit (same
   // reasoning as the drains in the vision and agent self-tests).
   for ( int i = 0; i < 4; ++i )
   {
      ThePICopilotModule->ProcessEvents( true/*excludeUserInputEvents*/ );
      std::this_thread::sleep_for( std::chrono::milliseconds( 250 ) );
   }
   return allOk;
}

} // namespace pcl
```
In `PICopilotSelfTest.cpp`, add `#include "PICopilotInc5SelfTest.h"` next to the agent include, then insert after the increment-4 `try`/`catch` block:
```cpp
   // Increment 5. Same isolation as increments 3 and 4.
   bool inc5Ok = false;
   try
   {
      nlohmann::json inc5;
      inc5Ok = RunInc5SelfTest( inc5 );
      j.update( inc5 );
   }
   catch ( const std::exception& x )
   {
      j["inc5Exception"] = x.what();
   }
   catch ( ... )
   {
      j["inc5Exception"] = "unknown exception";
   }
```
and change `ok = ok && visionOk && agentOk;` to `ok = ok && visionOk && agentOk && inc5Ok;`. In `CMakeLists.txt` `MODULE_SOURCES`, add `PICopilotInc5SelfTest.cpp` after `TurnEndNotes.cpp`.

- [ ] **Step 4: Verify GREEN and record the facts.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) && bash test/run-selftest.sh`
Expected: `PASS: self-test verdict all green`. Copy these from `inc5SmokeInfo` into the task report, because Tasks 8 and 9 rely on them:
- `iiImagesColumns`, expected `["enabled","path","drizzlePath","localNormalizationDataPath"]`
- `iiEnum_weightMode.elements` (must contain `DontCare`)
- `iiDefault_generateDrizzleData` and `iiDefault_closePreviousImages`
- `iiOutputIds`
- `pjsrSyntaxForBodyLine3.line` and `pjsrRuntimeForBodyLine2.line` (their offsets from 3 and 2 should be the same number; Task 9 calibrates it at runtime)
- `consoleCapture`

**BLOCKED** (report `inc5SmokeInfo` + `inc5SmokeError` and stop) if any of the following happens:
- `iiRan` is false
- `iiResultGeometry` is not 64×64×1
- `pjsrParse` ≠ `parsed:0:1`
- `pjsrBreakout` ≠ `syntax:0:true`
- the console text lacks the two lines

If the run hangs until the 600 s timeout, a core modal came up under Xvfb. Record the last `inc5SmokeInfo` you can obtain by commenting sections out, and report BLOCKED with that finding.

- [ ] **Step 5: Commit.**
```bash
cd /home/scarter4work/projects/astro-pi
git add modules/pi-copilot/src/module/PICopilotInc5SelfTest.h modules/pi-copilot/src/module/PICopilotInc5SelfTest.cpp \
        modules/pi-copilot/src/module/PICopilotSelfTest.cpp modules/pi-copilot/src/module/CMakeLists.txt modules/pi-copilot/test/run-selftest.sh
git commit -m "test(pi-copilot): inc5 platform smoke -- ImageIntegration ExecuteGlobal on synthetic FITS, PJSR parse-without-execute, console capture

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---
## Task 2: SSE stream parser + final-message assembler (pure, on recorded and synthetic bytes)

This task de-risks streaming before any transport change. `SseMessageAssembler` takes raw bytes in any chunking and rebuilds the final message in the **non-streamed** shape, so `ParseMessagesResponse()` and everything after it stay unchanged. It is proven on three real recorded streams (text, tool_use, and Opus 5.5 with thinking) and on hand-written streams with exact expected output, fed in 1/2/7/64-byte and whole chunks.

**Files:**
- Create: `modules/pi-copilot/src/module/SseStream.h`
- Create: `modules/pi-copilot/src/module/SseStream.cpp`
- Create: `modules/pi-copilot/test/capture-sse-fixtures.sh`
- Create (recorded by the script): `modules/pi-copilot/test/fixtures/sse/text-opus-4-8.sse`, `tool-opus-4-8.sse`, `thinking-tool-opus-5-5.sse`
- Modify: `modules/pi-copilot/src/module/PICopilotInc5SelfTest.cpp` (Section B1)
- Modify: `modules/pi-copilot/src/module/CMakeLists.txt`, `modules/pi-copilot/test/run-selftest.sh`

**Interfaces:**
- Consumes: `ParseMessagesResponse( int, const IsoString&, const String& )` (unchanged).
- Produces:
```cpp
class SseMessageAssembler
{
public:
   std::string Feed( const char* data, size_t size );   // returns the text_delta text completed by these bytes (UTF-8)
   bool Finished() const;            // message_stop seen (and the message is complete)
   bool Failed() const;              // an "error" event or a protocol violation
   const std::string& Error() const; // "overloaded_error: Overloaded", or the protocol problem
   const std::string& ErrorType() const;  // the API error type for an "error" event, else ""
   bool SawAnyEvent() const;
   nlohmann::json FinalMessage() const;   // {"id","type":"message","role","model","content":[...],"stop_reason","stop_sequence","usage",...}
};
```

- [ ] **Step 1: Record the fixtures (real API, key never printed).** Create `test/capture-sse-fixtures.sh`:
```bash
#!/usr/bin/env bash
# Records real Messages API SSE streams as self-test fixtures (Task 2).
# The key comes from the keyring and reaches curl on STDIN (-H @-), never argv;
# every fixture is checked for message_stop and for NOT containing the key.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/fixtures/sse"
mkdir -p "$OUT"
KEY="$(secret-tool lookup service anthropic account default)" || { echo "FAIL: no test key in the keyring"; exit 1; }
[ -n "$KEY" ] || { echo "FAIL: empty test key"; exit 1; }
post() { # $1 = output file, $2 = JSON body
   printf 'x-api-key: %s\nanthropic-version: 2023-06-01\ncontent-type: application/json\n' "$KEY" \
      | curl -sSN --fail-with-body -H @- --data-binary "$2" https://api.anthropic.com/v1/messages > "$1"
}
TOOL='{"name":"describe_process","description":"Describe a PixInsight process by id.","input_schema":{"type":"object","properties":{"id":{"type":"string"}},"required":["id"]}}'
post "$OUT/text-opus-4-8.sse" \
   '{"model":"claude-opus-4-8","max_tokens":200,"stream":true,"messages":[{"role":"user","content":"Reply with exactly: café ok — done"}]}'
post "$OUT/tool-opus-4-8.sse" \
   '{"model":"claude-opus-4-8","max_tokens":400,"stream":true,"tools":['"$TOOL"'],"messages":[{"role":"user","content":"Call describe_process for PixelMath."}]}'
post "$OUT/thinking-tool-opus-5-5.sse" \
   '{"model":"claude-opus-5-5","max_tokens":4000,"stream":true,"tools":['"$TOOL"'],"messages":[{"role":"user","content":"Think about which PixInsight process removes a green colour cast, then call describe_process on it."}]}'
for f in "$OUT"/*.sse; do
   grep -q '^event: message_stop' "$f" || { echo "FAIL: $f has no message_stop"; exit 1; }
   if grep -qF -- "$KEY" "$f"; then echo "FAIL: the key appears in $f"; rm -f "$f"; exit 1; fi
   echo "ok: $f ($(wc -c < "$f") bytes)"
done
```
Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && chmod +x test/capture-sse-fixtures.sh && test/capture-sse-fixtures.sh`
Expected: three `ok:` lines. If `claude-opus-5-5` returns an HTTP error (the body is in the file and `curl` exits non-zero), report the body and **stop (BLOCKED)**. The model list in Task 4 depends on it.

- [ ] **Step 2: Failing test.** In `run-selftest.sh`, after the `export PICOPILOT_SELFTEST_AGENT_URL=…` line add
```bash
export PICOPILOT_SELFTEST_FIXTURES="$HERE/fixtures"
```
and add `'sseParserOk',` after `'inc5SmokeOk',` in `required_true`. In `PICopilotInc5SelfTest.cpp`, add `#include "AnthropicClient.h"`, `#include "SseStream.h"` and `#include <cstdlib>`. Add these helpers to the anonymous namespace:
```cpp
std::string SseEv( const char* name, const std::string& data )
{
   return std::string( "event: " ) + name + "\ndata: " + data + "\n\n";
}

struct Assembled
{
   nlohmann::json message;
   std::string    text;
   bool           finished = false;
   bool           failed = false;
   std::string    error;
   std::string    errorType;
};

Assembled AssembleInChunks( const std::string& s, size_t chunk )
{
   SseMessageAssembler a;
   Assembled r;
   for ( size_t i = 0; i < s.size(); i += chunk )
      r.text += a.Feed( s.data() + i, std::min( chunk, s.size() - i ) );
   r.finished = a.Finished();
   r.failed = a.Failed();
   r.error = a.Error();
   r.errorType = a.ErrorType();
   if ( r.finished )
      r.message = a.FinalMessage();
   return r;
}

std::string MessageStart( const char* id )
{
   return SseEv( "message_start", std::string( "{\"type\":\"message_start\",\"message\":{\"id\":\"" ) + id
      + "\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"m\",\"content\":[],\"stop_reason\":null,"
        "\"stop_sequence\":null,\"usage\":{\"input_tokens\":12,\"cache_read_input_tokens\":0,\"output_tokens\":1}}}" );
}

std::string BlockStart( int index, const std::string& block )
{
   return SseEv( "content_block_start", "{\"type\":\"content_block_start\",\"index\":" + std::to_string( index )
                                        + ",\"content_block\":" + block + "}" );
}

std::string Delta( int index, const std::string& delta )
{
   return SseEv( "content_block_delta", "{\"type\":\"content_block_delta\",\"index\":" + std::to_string( index )
                                        + ",\"delta\":" + delta + "}" );
}

std::string BlockStop( int index )
{
   return SseEv( "content_block_stop", "{\"type\":\"content_block_stop\",\"index\":" + std::to_string( index ) + "}" );
}

std::string MessageEnd( const char* stopReason, int outputTokens )
{
   return SseEv( "message_delta", std::string( "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"" ) + stopReason
                 + "\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":" + std::to_string( outputTokens ) + "}}" )
        + SseEv( "message_stop", "{\"type\":\"message_stop\"}" );
}

// The synthetic tool_use stream (S1) without its final message_stop tail.
std::string SyntheticToolStreamBody()
{
   return std::string( ": keep-alive comment\n\n" )
        + MessageStart( "msg_t1" )
        + "event: ping\ndata: {\"type\":\n" + "data: \"ping\"}\n\n"            // one event, two data lines
        + BlockStart( 0, "{\"type\":\"text\",\"text\":\"\"}" )
        + Delta( 0, "{\"type\":\"text_delta\",\"text\":\"Caf\"}" )
        + Delta( 0, "{\"type\":\"text_delta\",\"text\":\"\xC3\xA9 \xF0\x9F\x93\xB7\"}" )   // raw UTF-8, split by small chunks
        + BlockStop( 0 )
        + SseEv( "some_future_event", "{\"type\":\"some_future_event\",\"x\":1}" )
        + BlockStart( 1, "{\"type\":\"tool_use\",\"id\":\"toolu_t1\",\"name\":\"describe_process\",\"input\":{}}" )
        + Delta( 1, "{\"type\":\"input_json_delta\",\"partial_json\":\"\"}" )
        + Delta( 1, "{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"id\\\": \\\"Pix\"}" )
        + Delta( 1, "{\"type\":\"input_json_delta\",\"partial_json\":\"elMath\\\"}\"}" )
        + BlockStop( 1 );
}

std::string ReadFixture( const char* name )
{
   const char* dir = std::getenv( "PICOPILOT_SELFTEST_FIXTURES" );
   if ( dir == nullptr )
      throw Error( "PICOPILOT_SELFTEST_FIXTURES is not set" );
   const ByteArray b = File::ReadFile( String( dir ) + "/sse/" + name );
   return std::string( reinterpret_cast<const char*>( b.Begin() ), b.Length() );
}
```
Insert above `// ---- inc5 sections end ----`:
```cpp
   // ---- Section B1: SSE parser + assembler (Task 2) --------------------------
   {
      bool s1Ok = true, crlfOk = false, errorOk = false, unknownDeltaOk = false, truncOk = false,
           thinkingOk = false, badJsonOk = false, orderOk = false, fixturesOk = true;
      nlohmann::json detail = nlohmann::json::object();
      String error;
      try
      {
         const std::string s1 = SyntheticToolStreamBody() + MessageEnd( "tool_use", 40 );
         const nlohmann::json expected = nlohmann::json::parse(
            "{\"id\":\"msg_t1\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"m\","
            "\"content\":[{\"type\":\"text\",\"text\":\"Caf\xC3\xA9 \xF0\x9F\x93\xB7\"},"
            "{\"type\":\"tool_use\",\"id\":\"toolu_t1\",\"name\":\"describe_process\",\"input\":{\"id\":\"PixelMath\"}}],"
            "\"stop_reason\":\"tool_use\",\"stop_sequence\":null,"
            "\"usage\":{\"input_tokens\":12,\"cache_read_input_tokens\":0,\"output_tokens\":40}}" );
         for ( size_t chunk : { size_t( 1 ), size_t( 2 ), size_t( 7 ), size_t( 64 ), s1.size() } )
         {
            const Assembled a = AssembleInChunks( s1, chunk );
            const bool pass = a.finished && !a.failed && a.message == expected && a.text == "Caf\xC3\xA9 \xF0\x9F\x93\xB7";
            if ( !pass )
               detail["s1Fail_" + std::to_string( chunk )] = { { "message", a.message }, { "text", a.text }, { "error", a.error } };
            s1Ok = s1Ok && pass;
         }
         {  // The rebuilt body parses exactly like a non-streamed reply.
            const Assembled a = AssembleInChunks( s1, 5 );
            const AnthropicResult r = ParseMessagesResponse( 200, IsoString( a.message.dump().c_str() ), String() );
            s1Ok = s1Ok && r.ok && r.stopReason == "tool_use" && r.contentBlocks.size() == 2
                && r.contentBlocks[1].at( "input" ) == nlohmann::json( { { "id", "PixelMath" } } );
         }
         {  // CRLF line endings, split anywhere.
            std::string crlf;
            for ( char c : s1 )
               crlf += (c == '\n') ? std::string( "\r\n" ) : std::string( 1, c );
            const Assembled a = AssembleInChunks( crlf, 3 );
            crlfOk = a.finished && a.message == expected;
         }
         {  // API "error" event mid-stream.
            const std::string s = MessageStart( "msg_e" ) + BlockStart( 0, "{\"type\":\"text\",\"text\":\"\"}" )
               + Delta( 0, "{\"type\":\"text_delta\",\"text\":\"Partial\"}" )
               + SseEv( "error", "{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}" );
            const Assembled a = AssembleInChunks( s, 4 );
            detail["errorCase"] = { { "error", a.error }, { "text", a.text } };
            errorOk = a.failed && !a.finished && a.error == "overloaded_error: Overloaded"
                   && a.errorType == "overloaded_error" && a.text == "Partial";
         }
         {  // An unknown DELTA type fails loudly (the block could not be echoed back correctly).
            const std::string s = MessageStart( "msg_u" ) + BlockStart( 0, "{\"type\":\"text\",\"text\":\"\"}" )
               + Delta( 0, "{\"type\":\"mystery_delta\",\"x\":1}" ) + BlockStop( 0 ) + MessageEnd( "end_turn", 3 );
            const Assembled a = AssembleInChunks( s, 9 );
            detail["unknownDelta"] = a.error;
            unknownDeltaOk = a.failed && a.error == "unsupported stream delta type 'mystery_delta' (content[0])";
         }
         {  // Cut before message_stop: neither finished nor failed (the transport reports it).
            const std::string s = SyntheticToolStreamBody();
            const Assembled a = AssembleInChunks( s, 11 );
            truncOk = !a.finished && !a.failed;
         }
         {  // Thinking block: thinking_delta + signature_delta rebuilt verbatim.
            const std::string s = MessageStart( "msg_th" )
               + BlockStart( 0, "{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"\"}" )
               + Delta( 0, "{\"type\":\"thinking_delta\",\"thinking\":\"Let me\"}" )
               + Delta( 0, "{\"type\":\"thinking_delta\",\"thinking\":\" think\"}" )
               + Delta( 0, "{\"type\":\"signature_delta\",\"signature\":\"c2ln\"}" )
               + BlockStop( 0 )
               + BlockStart( 1, "{\"type\":\"text\",\"text\":\"\"}" )
               + Delta( 1, "{\"type\":\"text_delta\",\"text\":\"Done.\"}" ) + BlockStop( 1 )
               + MessageEnd( "end_turn", 9 );
            const Assembled a = AssembleInChunks( s, 6 );
            const AnthropicResult r = a.finished
               ? ParseMessagesResponse( 200, IsoString( a.message.dump().c_str() ), String() ) : AnthropicResult();
            thinkingOk = a.finished && r.ok && r.text == "Done." && a.text == "Done."
                      && r.contentBlocks.at( 0 ) == nlohmann::json::parse(
                            "{\"type\":\"thinking\",\"thinking\":\"Let me think\",\"signature\":\"c2ln\"}" );
         }
         {  // Invalid tool input JSON under stop_reason tool_use.
            const std::string s = MessageStart( "msg_b" )
               + BlockStart( 0, "{\"type\":\"tool_use\",\"id\":\"toolu_b\",\"name\":\"describe_process\",\"input\":{}}" )
               + Delta( 0, "{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"id\\\": \"}" ) + BlockStop( 0 )
               + MessageEnd( "tool_use", 5 );
            const Assembled a = AssembleInChunks( s, 8 );
            detail["badJson"] = a.error;
            badJsonOk = a.failed && a.error == "tool_use input (content[0]) was not valid JSON";
         }
         {  // Protocol order.
            const Assembled a = AssembleInChunks( BlockStart( 0, "{\"type\":\"text\",\"text\":\"\"}" ), 100 );
            orderOk = a.failed && a.error == "content_block_start before message_start";
         }

         // Recorded real streams: chunking-invariant, complete, and parse like non-streamed replies.
         struct Fixture { const char* file; const char* stop; };
         const Fixture fixtures[] = { { "text-opus-4-8.sse", "end_turn" }, { "tool-opus-4-8.sse", "tool_use" },
                                      { "thinking-tool-opus-5-5.sse", "tool_use" } };
         for ( const Fixture& f : fixtures )
         {
            const std::string bytes = ReadFixture( f.file );
            const Assembled one = AssembleInChunks( bytes, 1 ), big = AssembleInChunks( bytes, 4096 );
            const AnthropicResult r = big.finished
               ? ParseMessagesResponse( 200, IsoString( big.message.dump().c_str() ), String() ) : AnthropicResult();
            int toolUses = 0, thinking = 0;
            bool signaturesOk = true;
            for ( const nlohmann::json& b : r.contentBlocks )
            {
               const std::string t = b.value( "type", std::string() );
               if ( t == "tool_use" && b.value( "name", std::string() ) == "describe_process"
                    && b.at( "input" ).is_object() && b.at( "input" ).value( "id", nlohmann::json() ).is_string() )
                  ++toolUses;
               if ( t == "thinking" )
               {
                  ++thinking;
                  signaturesOk = signaturesOk && !b.value( "signature", std::string() ).empty();
               }
            }
            const bool pass = one.finished && big.finished && !one.failed && one.message == big.message
                           && one.text == big.text && r.ok && r.stopReason == f.stop
                           && (std::string( f.stop ) != "tool_use" || toolUses == 1)
                           && (std::string( f.stop ) != "end_turn" || !r.text.IsEmpty())
                           && signaturesOk;
            detail[f.file] = { { "pass", pass }, { "stop", r.stopReason }, { "toolUses", toolUses },
                               { "thinkingBlocks", thinking }, { "error", big.error } };
            fixturesOk = fixturesOk && pass;
         }
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); fixturesOk = false; }
      catch ( const std::exception& x ) { error = String( x.what() ); fixturesOk = false; }

      const bool ok = s1Ok && crlfOk && errorOk && unknownDeltaOk && truncOk && thinkingOk && badJsonOk && orderOk && fixturesOk;
      out["sseDetail"] = detail;
      out["sseError"] = U8( error );
      out["sseParserOk"] = ok;
      allOk = allOk && ok;
   }
```

- [ ] **Step 3: Verify RED.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) 2>&1 | grep -m3 error`
Expected: `SseStream.h: No such file or directory`.

- [ ] **Step 4: Implement.** `SseStream.h`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_SseStream_h
#define PICopilot_SseStream_h

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace pcl
{

/*
 * Incremental parser for the Messages API's Server-Sent Events stream
 * ("stream": true) that rebuilds the final message in the NON-streamed shape,
 * so ParseMessagesResponse() -- and everything after it -- is unchanged.
 *
 * Bytes may arrive in any chunking (lines, UTF-8 sequences and CRLF pairs can
 * be split across Feed() calls). Events handled: message_start,
 * content_block_start/delta/stop, message_delta (every delta key + usage
 * merged into the message), message_stop, ping, error. Deltas: text_delta,
 * input_json_delta (tool input rebuilt and parsed at content_block_stop),
 * thinking_delta, signature_delta, citations_delta. An unknown DELTA type is a
 * failure (the block could not be echoed back faithfully); unknown EVENT types
 * are ignored (the API may add events). Pure: no PCL, no GUI; any thread.
 */
class SseMessageAssembler
{
public:

   // Returns the text of every text_delta completed by these bytes (UTF-8),
   // for live display. Ignores input after a failure or after message_stop.
   std::string Feed( const char* data, size_t size );

   bool Finished() const { return m_finished; }
   bool Failed() const { return m_failed; }
   const std::string& Error() const { return m_error; }
   const std::string& ErrorType() const { return m_errorType; }
   bool SawAnyEvent() const { return m_sawEvent; }

   // Valid when Finished() && !Failed().
   nlohmann::json FinalMessage() const;

private:

   std::string              m_line;
   bool                     m_lastWasCR = false;
   std::string              m_event;
   std::string              m_data;
   bool                     m_hasData = false;
   nlohmann::json           m_message;
   bool                     m_started = false;
   std::vector<std::string> m_partialJson;    // per content index (tool_use input)
   std::vector<size_t>      m_badToolInput;   // content indices whose input JSON did not parse
   bool                     m_finished = false;
   bool                     m_failed = false;
   bool                     m_sawEvent = false;
   std::string              m_error;
   std::string              m_errorType;

   void OnLine( std::string& text );
   void Dispatch( std::string& text );
   void OnEvent( const nlohmann::json& ev, const std::string& name, std::string& text );
   void Fail( const std::string& why );
};

} // namespace pcl

#endif // PICopilot_SseStream_h
```
`SseStream.cpp`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "SseStream.h"

#include <exception>
#include <utility>

namespace pcl
{

void SseMessageAssembler::Fail( const std::string& why )
{
   if ( !m_failed )
   {
      m_failed = true;
      m_error = why;
   }
}

std::string SseMessageAssembler::Feed( const char* data, size_t size )
{
   std::string text;
   for ( size_t i = 0; i < size && !m_failed && !m_finished; ++i )
   {
      const char c = data[i];
      if ( c == '\n' || c == '\r' )
      {
         if ( c == '\n' && m_lastWasCR )   // the CR of this CRLF already ended the line
         {
            m_lastWasCR = false;
            continue;
         }
         m_lastWasCR = c == '\r';
         OnLine( text );
         m_line.clear();
      }
      else
      {
         m_lastWasCR = false;
         m_line.push_back( c );
      }
   }
   return text;
}

void SseMessageAssembler::OnLine( std::string& text )
{
   if ( m_line.empty() )
   {
      Dispatch( text );
      return;
   }
   if ( m_line[0] == ':' )   // comment
      return;
   const size_t colon = m_line.find( ':' );
   const std::string field = m_line.substr( 0, colon );
   std::string value = (colon == std::string::npos) ? std::string() : m_line.substr( colon + 1 );
   if ( !value.empty() && value[0] == ' ' )
      value.erase( 0, 1 );
   if ( field == "event" )
      m_event = value;
   else if ( field == "data" )
   {
      if ( m_hasData )
         m_data += '\n';
      m_data += value;
      m_hasData = true;
   }
}

void SseMessageAssembler::Dispatch( std::string& text )
{
   if ( !m_hasData )
   {
      m_event.clear();
      return;
   }
   const std::string data = std::move( m_data );
   const std::string name = std::move( m_event );
   m_data.clear();
   m_event.clear();
   m_hasData = false;
   nlohmann::json ev;
   try
   {
      ev = nlohmann::json::parse( data );
   }
   catch ( const std::exception& )
   {
      Fail( "stream event '" + name + "' is not JSON: " + data.substr( 0, 120 ) );
      return;
   }
   m_sawEvent = true;
   try
   {
      OnEvent( ev, name, text );
   }
   catch ( const std::exception& x )
   {
      Fail( "malformed stream event '" + name + "': " + x.what() );
   }
}

void SseMessageAssembler::OnEvent( const nlohmann::json& ev, const std::string& name, std::string& text )
{
   const std::string type = (ev.is_object() && ev.contains( "type" ) && ev["type"].is_string())
                          ? ev["type"].get<std::string>() : name;
   if ( type == "ping" )
      return;
   if ( type == "error" )
   {
      const nlohmann::json& e = ev.at( "error" );
      m_errorType = e.value( "type", std::string( "error" ) );
      Fail( m_errorType + ": " + e.value( "message", std::string( "(no message)" ) ) );
      return;
   }
   if ( type == "message_start" )
   {
      m_message = ev.at( "message" );
      if ( !m_message.is_object() )
      {
         Fail( "message_start without a message object" );
         return;
      }
      m_message["content"] = nlohmann::json::array();
      m_partialJson.clear();
      m_badToolInput.clear();
      m_started = true;
      return;
   }
   if ( type != "content_block_start" && type != "content_block_delta" && type != "content_block_stop"
     && type != "message_delta" && type != "message_stop" )
      return;   // a future event type: ignored
   if ( !m_started )
   {
      Fail( type + " before message_start" );
      return;
   }

   nlohmann::json& content = m_message["content"];
   if ( type == "content_block_start" )
   {
      const size_t index = ev.at( "index" ).get<size_t>();
      if ( index != content.size() )
      {
         Fail( "content_block_start index " + std::to_string( index ) + " out of order (expected "
               + std::to_string( content.size() ) + ")" );
         return;
      }
      content.push_back( ev.at( "content_block" ) );
      m_partialJson.push_back( std::string() );
      return;
   }
   if ( type == "content_block_delta" || type == "content_block_stop" )
   {
      const size_t index = ev.at( "index" ).get<size_t>();
      if ( index >= content.size() )
      {
         Fail( type + " for unknown content index " + std::to_string( index ) );
         return;
      }
      nlohmann::json& block = content[index];
      const std::string at = " (content[" + std::to_string( index ) + "])";
      if ( type == "content_block_stop" )
      {
         const std::string btype = block.value( "type", std::string() );
         if ( btype == "tool_use" || btype == "server_tool_use" )
         {
            if ( !m_partialJson[index].empty() )
            {
               try
               {
                  block["input"] = nlohmann::json::parse( m_partialJson[index] );
               }
               catch ( const std::exception& )
               {
                  block["input"] = nlohmann::json::object();
                  m_badToolInput.push_back( index );
               }
            }
            else if ( !block.contains( "input" ) || !block["input"].is_object() )
               block["input"] = nlohmann::json::object();
         }
         return;
      }
      const nlohmann::json& delta = ev.at( "delta" );
      const std::string dtype = delta.value( "type", std::string() );
      if ( dtype == "text_delta" )
      {
         const std::string t = delta.at( "text" ).get<std::string>();
         block["text"] = block.value( "text", std::string() ) + t;
         text += t;
      }
      else if ( dtype == "input_json_delta" )
         m_partialJson[index] += delta.at( "partial_json" ).get<std::string>();
      else if ( dtype == "thinking_delta" )
         block["thinking"] = block.value( "thinking", std::string() ) + delta.at( "thinking" ).get<std::string>();
      else if ( dtype == "signature_delta" )
         block["signature"] = delta.at( "signature" ).get<std::string>();
      else if ( dtype == "citations_delta" )
      {
         if ( !block.contains( "citations" ) || !block["citations"].is_array() )
            block["citations"] = nlohmann::json::array();
         block["citations"].push_back( delta.at( "citation" ) );
      }
      else
         Fail( "unsupported stream delta type '" + dtype + "'" + at );
      return;
   }
   if ( type == "message_delta" )
   {
      if ( ev.contains( "delta" ) && ev["delta"].is_object() )
         for ( auto it = ev["delta"].begin(); it != ev["delta"].end(); ++it )
            m_message[it.key()] = it.value();
      if ( ev.contains( "usage" ) && ev["usage"].is_object() )
      {
         if ( !m_message.contains( "usage" ) || !m_message["usage"].is_object() )
            m_message["usage"] = nlohmann::json::object();
         for ( auto it = ev["usage"].begin(); it != ev["usage"].end(); ++it )
            m_message["usage"][it.key()] = it.value();
      }
      for ( auto it = ev.begin(); it != ev.end(); ++it )
         if ( it.key() != "type" && it.key() != "delta" && it.key() != "usage" )
            m_message[it.key()] = it.value();   // e.g. input_transformations after a fallback
      return;
   }
   // message_stop
   const bool toolStop = m_message.contains( "stop_reason" ) && m_message["stop_reason"].is_string()
                      && m_message["stop_reason"].get<std::string>() == "tool_use";
   if ( toolStop && !m_badToolInput.empty() )
   {
      Fail( "tool_use input (content[" + std::to_string( m_badToolInput.front() ) + "]) was not valid JSON" );
      return;
   }
   m_finished = true;
}

nlohmann::json SseMessageAssembler::FinalMessage() const
{
   nlohmann::json m = m_message;
   if ( m.is_object() && !m.contains( "type" ) )
      m["type"] = "message";
   return m;
}

} // namespace pcl
```
In `CMakeLists.txt` `MODULE_SOURCES`, add `SseStream.cpp` after `PICopilotInc5SelfTest.cpp`.

- [ ] **Step 5: Verify GREEN.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) && bash test/run-selftest.sh`
Expected: `PASS: self-test verdict all green`. `sseDetail` shows `pass: true` for all three fixtures. Record `thinkingBlocks` for the Opus 5.5 fixture in the report (0 is acceptable: adaptive thinking may skip, and the synthetic thinking case is the binding test).

- [ ] **Step 6: Commit.**
```bash
cd /home/scarter4work/projects/astro-pi
git add modules/pi-copilot/src/module/SseStream.h modules/pi-copilot/src/module/SseStream.cpp \
        modules/pi-copilot/src/module/PICopilotInc5SelfTest.cpp modules/pi-copilot/src/module/CMakeLists.txt \
        modules/pi-copilot/test/run-selftest.sh modules/pi-copilot/test/capture-sse-fixtures.sh modules/pi-copilot/test/fixtures/sse
git commit -m "feat(pi-copilot): SSE stream assembler rebuilding the non-streamed message shape; recorded + synthetic fixtures

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Streaming transport — `"stream": true`, live text deltas to the UI, idle deadline, error kinds

This task wires Task 2 into `AnthropicRequest`. The worker thread feeds every received chunk to the assembler. Text deltas go into a mutex-guarded queue that the panel's Timer drains, and the final result is the rebuilt message passed through the unchanged `ParseMessagesResponse()`. An `error` event mid-stream, a stream that ends without `message_stop`, a stall (no bytes for `streamIdleSeconds`), a cancel and the overall deadline each get their own `RequestErrorKind` and a precise message. A strict loopback SSE server proves that the deltas reach the UI thread **while the request is still running**, and it runs the unchanged `AgentSession` tool loop over two streamed requests. The gated live agent run (A6) switches to the production (streamed) shape.

**Files:**
- Modify: `modules/pi-copilot/src/module/AnthropicClient.h`, `AnthropicClient.cpp`
- Modify: `modules/pi-copilot/src/module/ChatThread.h`, `ChatThread.cpp`
- Modify: `modules/pi-copilot/src/module/PICopilotInterface.h`, `PICopilotInterface.cpp` (live rendering)
- Modify: `modules/pi-copilot/src/module/PICopilotAgentSelfTest.cpp` (A6 uses the production shape)
- Modify: `modules/pi-copilot/src/module/PICopilotInc5SelfTest.cpp` (Section B2)
- Modify: `modules/pi-copilot/test/run-selftest.sh` (SSE loopback paths, `PICOPILOT_REQUIRE_LIVE`)

**Interfaces:**
- Consumes: `SseMessageAssembler` (Task 2).
- Produces:
```cpp
constexpr int PICopilotRequestTimeoutSeconds = 600;   // was 300
constexpr int PICopilotStreamIdleSeconds = 120;
constexpr int PICopilotStreamMaxTokens = 16000;
enum class RequestErrorKind { None, Build, Cancelled, TimedOut, Stalled, Network, Http, Stream, BadReply };
struct RequestShape { bool stream = false; int maxTokens = 4096; int streamIdleSeconds = PICopilotStreamIdleSeconds; };
RequestShape ProductionRequestShape( const IsoString& model );          // Task 4 extends it
std::string BuildMessagesRequestBody( model, systemPrompt, history, tools = json(), const RequestShape& shape = RequestShape() );
// AnthropicResult gains: RequestErrorKind errorKind; nlohmann::json usage; nlohmann::json inputTransformations;
AnthropicRequest( apiKey, model, systemPrompt, history, url, timeoutSeconds, tools, const RequestShape& shape = RequestShape() );
String AnthropicRequest::TakeStreamedText();                            // thread-safe
ChatThread( apiKey, systemPrompt, history, model, url, timeoutSeconds, tools, const RequestShape& shape = RequestShape() );
String ChatThread::TakeStreamedText();                                  // thread-safe
```

- [ ] **Step 1: Loopback SSE server paths (test infrastructure).** In `run-selftest.sh`'s echo-server python, add `import time` to the imports, set `srv.daemon_threads = True` before `srv.serve_forever()`, and add these methods to class `H`:
```python
    def sse(self, events, delay=0.3, stall=False):
        self.send_response(200)
        self.send_header("content-type", "text/event-stream")
        self.send_header("connection", "close")
        self.end_headers()
        self.close_connection = True
        try:
            for name, data in events:
                self.wfile.write(("event: %s\ndata: %s\n\n" % (name, json.dumps(data, ensure_ascii=False))).encode("utf-8"))
                self.wfile.flush()
                time.sleep(delay)
            while stall:          # say nothing more: the client's idle deadline must end it
                time.sleep(0.5)
        except (BrokenPipeError, ConnectionResetError):
            pass
    def stream_events(self, req, n, kind):
        msgs = req.get("messages") or []
        last = msgs[-1].get("content") if msgs else None
        results = [b for b in last if b.get("type") == "tool_result"] if isinstance(last, list) else []
        ev = [("message_start", {"type": "message_start", "message": {"id": "msg_s%02d" % n, "type": "message",
               "role": "assistant", "model": req.get("model"), "content": [], "stop_reason": None,
               "stop_sequence": None, "usage": {"input_tokens": 10, "output_tokens": 1}}}),
              ("ping", {"type": "ping"})]
        def text_block(index, parts):
            out = [("content_block_start", {"type": "content_block_start", "index": index,
                                            "content_block": {"type": "text", "text": ""}})]
            out += [("content_block_delta", {"type": "content_block_delta", "index": index,
                                             "delta": {"type": "text_delta", "text": p}}) for p in parts]
            return out + [("content_block_stop", {"type": "content_block_stop", "index": index})]
        def end(stop, tokens):
            return [("message_delta", {"type": "message_delta", "delta": {"stop_reason": stop, "stop_sequence": None},
                                       "usage": {"output_tokens": tokens}}),
                    ("message_stop", {"type": "message_stop"})]
        if kind == "stall":
            return ev
        if kind == "error":
            return ev + text_block(0, ["Partial"])[:2] + [("error", {"type": "error",
                    "error": {"type": "overloaded_error", "message": "Overloaded"}})]
        if results:
            return ev + text_block(0, ["Got %d tool_" % len(results), "result(s) — done."]) + end("end_turn", 12)
        return (ev + text_block(0, ["Hello, ", "streamed ", "world é"])
                + [("content_block_start", {"type": "content_block_start", "index": 1, "content_block":
                        {"type": "tool_use", "id": "toolu_s%02d" % n, "name": "describe_process", "input": {}}}),
                   ("content_block_delta", {"type": "content_block_delta", "index": 1,
                        "delta": {"type": "input_json_delta", "partial_json": "{\"id\": \"Pixel"}}),
                   ("content_block_delta", {"type": "content_block_delta", "index": 1,
                        "delta": {"type": "input_json_delta", "partial_json": "Math\"}"}}),
                   ("content_block_stop", {"type": "content_block_stop", "index": 1})]
                + end("tool_use", 30))
```
In `do_POST`, before `if self.path.endswith("/agent"):`, add:
```python
        for suffix, kind in (("/stream-error", "error"), ("/stream-stall", "stall"), ("/stream", "ok")):
            if self.path.endswith(suffix):
                if req.get("stream") is not True:
                    return self.reply(400, {"type": "error", "error": {"type": "invalid_request_error",
                                            "message": "body #%d: \"stream\" is not true" % n}})
                return self.sse(self.stream_events(req, n, kind), stall=(kind == "stall"))
```
After the `export PICOPILOT_SELFTEST_AGENT_URL=…` line, add:
```bash
export PICOPILOT_SELFTEST_STREAM_BASE="http://127.0.0.1:$(cat "$ECHO_PORT_FILE")/v1"
```
In the verdict python, add `'streamTransportOk',` after `'sseParserOk',` and, after the `agentWireSkipped` line:
```python
if d.get('streamLoopbackSkipped') is not False: missing.append('streamLoopbackSkipped==false')
import os
if os.environ.get('PICOPILOT_REQUIRE_LIVE') == '1':
    for k in ('anthropicSkipped', 'twoTurnSkipped', 'visionSkipped', 'liveAgentSkipped'):
        if d.get(k) is not False: missing.append(k + '==false (PICOPILOT_REQUIRE_LIVE=1)')
```

- [ ] **Step 2: Failing test (Section B2).** In `PICopilotInc5SelfTest.cpp`, add `#include "AgentSession.h"`, `#include "AgentTools.h"` and `#include "ChatThread.h"`. Then insert above the marker:
```cpp
   // ---- Section B2: streaming transport (Task 3) ------------------------------
   {
      using clock = std::chrono::steady_clock;
      auto secondsSince = []( clock::time_point t0 ) { return std::chrono::duration<double>( clock::now() - t0 ).count(); };
      bool bodyOk = false, liveDeltasOk = false, loopOk = false, errorOk = false, stallOk = false, cancelOk = false;
      bool skipped = true;
      nlohmann::json detail = nlohmann::json::object();
      String error;
      try
      {
         Array<AnthropicMessage> hist;
         AnthropicMessage hi;
         hi.role = "user";
         hi.content = "hi";
         hist.Add( hi );
         const nlohmann::json streamed = nlohmann::json::parse(
            BuildMessagesRequestBody( "m", "sys", hist, nlohmann::json(), ProductionRequestShape( PICOPILOT_DEFAULT_MODEL ) ) );
         const nlohmann::json plain = nlohmann::json::parse( BuildMessagesRequestBody( "m", "sys", hist ) );
         bodyOk = streamed.value( "stream", false ) && streamed.at( "max_tokens" ) == PICopilotStreamMaxTokens
               && !plain.contains( "stream" ) && plain.at( "max_tokens" ) == 4096;

         if ( const char* base = std::getenv( "PICOPILOT_SELFTEST_STREAM_BASE" ) )
         {
            skipped = false;
            RequestShape shape;
            shape.stream = true;
            shape.maxTokens = 1000;
            shape.streamIdleSeconds = 5;

            // (1) Deltas reach this (UI) thread while the request is still running.
            AgentSession session;
            AnthropicMessage u;
            u.role = "user";
            u.content = "stream please";
            session.BeginUserTurn( u );
            AnthropicResult r;
            std::string shown;
            int takesWhileActive = 0;
            double firstDelta = -1, total = 0;
            {
               ChatThread t( "sk-ant-invalid-selftest", "sys", session.History(), PICOPILOT_DEFAULT_MODEL,
                             String( base ) + "/stream", 30, ToolDefinitions( AgentMode::Advisor ), shape );
               const clock::time_point t0 = clock::now();
               t.Start();
               while ( t.IsActive() )
               {
                  const String d = t.TakeStreamedText();
                  if ( !d.IsEmpty() )
                  {
                     ++takesWhileActive;
                     shown += U8( d );
                     if ( firstDelta < 0 )
                        firstDelta = secondsSince( t0 );
                  }
                  ThePICopilotModule->ProcessEvents( true );
                  std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
               }
               t.Wait();
               total = secondsSince( t0 );
               shown += U8( t.TakeStreamedText() );
               t.TryTakeResult( r );
            }
            detail["stream1"] = { { "ok", r.ok }, { "error", U8( r.error ) }, { "stop", r.stopReason }, { "shown", shown },
                                  { "takesWhileActive", takesWhileActive }, { "firstDelta", firstDelta }, { "total", total },
                                  { "usage", r.usage } };
            liveDeltasOk = r.ok && r.stopReason == "tool_use" && takesWhileActive >= 2 && firstDelta >= 0
                        && firstDelta < total - 0.5 && shown == "Hello, streamed world \xC3\xA9" && U8( r.text ) == shown
                        && r.usage.value( "output_tokens", 0 ) == 30;

            // (2) The unchanged AgentSession loop over two streamed requests.
            ToolContext ctx;
            ctx.mode = AgentMode::Advisor;
            const AgentStep s = session.OnResponse( r, [&ctx]( const ToolCall& c ) { return ExecuteTool( c, ctx ); },
                                                    []() { return false; } );
            if ( s.kind == AgentStep::SendAgain )
            {
               AnthropicResult r2;
               {
                  ChatThread t2( "sk-ant-invalid-selftest", "sys", session.History(), PICOPILOT_DEFAULT_MODEL,
                                 String( base ) + "/stream", 30, ToolDefinitions( AgentMode::Advisor ), shape );
                  t2.Start();
                  t2.Wait();
                  t2.TryTakeResult( r2 );
               }
               const AgentStep s2 = session.OnResponse( r2, [&ctx]( const ToolCall& c ) { return ExecuteTool( c, ctx ); },
                                                        []() { return false; } );
               String why;
               detail["loop"] = { { "text", U8( r2.text ) }, { "error", U8( r2.error ) } };
               loopOk = s2.kind == AgentStep::Done && U8( r2.text ) == "Got 1 tool_result(s) \xE2\x80\x94 done."
                     && HistoryPrefixIsApiValid( session.History(), why ) && session.History().Length() == 4;
            }

            // (3) API error event mid-stream.
            {
               AnthropicResult e;
               std::string partial;
               {
                  ChatThread t( "sk-ant-invalid-selftest", "sys", hist, PICOPILOT_DEFAULT_MODEL,
                                String( base ) + "/stream-error", 30, nlohmann::json(), shape );
                  t.Start();
                  t.Wait();
                  partial = U8( t.TakeStreamedText() );
                  t.TryTakeResult( e );
               }
               detail["streamError"] = { { "error", U8( e.error ) }, { "partial", partial } };
               errorOk = !e.ok && e.errorKind == RequestErrorKind::Stream && !e.cancelled
                      && e.error == "the reply stream failed: overloaded_error: Overloaded" && partial == "Partial";
            }

            // (4) Stall: message_start, then silence -> the idle deadline ends it.
            {
               RequestShape idle = shape;
               idle.streamIdleSeconds = 3;
               AnthropicResult e;
               const clock::time_point t0 = clock::now();
               {
                  ChatThread t( "sk-ant-invalid-selftest", "sys", hist, PICOPILOT_DEFAULT_MODEL,
                                String( base ) + "/stream-stall", 30, nlohmann::json(), idle );
                  t.Start();
                  t.Wait();
                  t.TryTakeResult( e );
               }
               const double took = secondsSince( t0 );
               detail["stall"] = { { "error", U8( e.error ) }, { "seconds", took } };
               stallOk = !e.ok && e.errorKind == RequestErrorKind::Stalled
                      && e.error == "the reply stalled: no data from the API for 3 s" && took < 10;
            }

            // (5) Cancel mid-stream.
            {
               AnthropicResult e;
               const clock::time_point t0 = clock::now();
               {
                  ChatThread t( "sk-ant-invalid-selftest", "sys", hist, PICOPILOT_DEFAULT_MODEL,
                                String( base ) + "/stream-stall", 30, nlohmann::json(), shape );
                  t.Start();
                  std::this_thread::sleep_for( std::chrono::milliseconds( 1500 ) );
                  t.RequestCancel();
                  t.Wait();
                  t.TryTakeResult( e );
               }
               const double took = secondsSince( t0 );
               detail["cancel"] = { { "error", U8( e.error ) }, { "seconds", took } };
               cancelOk = !e.ok && e.cancelled && e.errorKind == RequestErrorKind::Cancelled
                       && e.error == "request cancelled" && took < 5;
            }
         }
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }

      const bool ok = bodyOk && (skipped || (liveDeltasOk && loopOk && errorOk && stallOk && cancelOk));
      out["streamDetail"] = detail;
      out["streamError"] = U8( error );
      out["streamLoopbackSkipped"] = skipped;
      out["streamTransportOk"] = ok;
      allOk = allOk && ok;
   }
```

- [ ] **Step 3: Verify RED.** Run the Task 2 Step 3 build command.
Expected: compile errors naming `ProductionRequestShape`, `RequestShape`, `TakeStreamedText` and `RequestErrorKind`.

- [ ] **Step 4: Implement the transport.** In `AnthropicClient.h`:
  - Change `constexpr int PICopilotRequestTimeoutSeconds = 300;` to `600`.
  - Add after it:
```cpp
// Streamed requests only: no byte from the API for this long ends the request
// ("stalled"). The API sends ping events while it works, so a live stream is
// never silent this long.
constexpr int PICopilotStreamIdleSeconds = 120;

// max_tokens of a production (streamed) request. Streaming removes the HTTP
// timeout concern that kept non-streamed requests at 4096; thinking models
// (Opus 5.5, Fable 5.1) spend part of it thinking.
constexpr int PICopilotStreamMaxTokens = 16000;

// Why a request failed; the panel words its note by this (Task 6).
enum class RequestErrorKind { None, Build, Cancelled, TimedOut, Stalled, Network, Http, Stream, BadReply };

// How a request is shaped on the wire. The default is the non-streamed
// increment-4 shape the older self-tests pin; production uses
// ProductionRequestShape().
struct RequestShape
{
   bool stream = false;                                 // "stream": true (Server-Sent Events)
   int  maxTokens = 4096;
   int  streamIdleSeconds = PICopilotStreamIdleSeconds; // streamed requests only
};

// The shape the panel sends for `model`: streamed, PICopilotStreamMaxTokens.
RequestShape ProductionRequestShape( const IsoString& model );
```
  - Change the `BuildMessagesRequestBody` declaration to add a trailing `const RequestShape& shape = RequestShape()` parameter, and document that it adds `"stream": true` when `shape.stream`.
  - In `struct AnthropicResult`, add after `contentBlocks`:
```cpp
   RequestErrorKind errorKind = RequestErrorKind::None;   // set whenever ok == false
   nlohmann::json   usage;                 // the reply's "usage" object (incl. cache_read_input_tokens), when present
   nlohmann::json   inputTransformations;  // the reply's "input_transformations" array, when present
```
  - Add a trailing `const RequestShape& shape = RequestShape()` parameter to the `AnthropicRequest` constructor, and add a public member:
```cpp
   // Thread-safe. The text of the text deltas received since the last call
   // (streamed requests; empty otherwise). The UI thread polls it.
   String TakeStreamedText();
```

In `AnthropicClient.cpp`:
  - Add the includes `"SseStream.h"`, `<pcl/AutoLock.h>`, `<pcl/Mutex.h>`, `<memory>` and `<string>`.
  - Replace the `ResponseSink` members and methods from `std::atomic<bool> cancelRequested` through the end of `OnData` with:
```cpp
   // Set from any thread by AnthropicRequest::Cancel().
   std::atomic<bool> cancelRequested{ false };

   // Written on the performing thread only (inside the callbacks below),
   // read back on that same thread after POST() returns.
   bool              cancelled = false;
   bool              timedOut = false;
   bool              stalled = false;
   clock::time_point deadline = clock::time_point::max();

   // Streamed requests: the SSE assembler (fed on the performing thread) and
   // the idle limit, measured from the last received byte.
   std::unique_ptr<SseMessageAssembler> sse;
   clock::duration   idleLimit = clock::duration::max();
   clock::time_point lastData;

   // Text deltas not yet taken by the UI thread (UTF-8). Guarded by deltaMutex.
   Mutex             deltaMutex;
   std::string       pendingDelta;

   // Returns false (abort) when the request was cancelled, its overall
   // deadline has passed, or a stream went silent; records which one.
   bool ShouldContinue()
   {
      if ( cancelRequested.load() )
      {
         cancelled = true;
         return false;
      }
      const clock::time_point now = clock::now();
      if ( now >= deadline )
      {
         timedOut = true;
         return false;
      }
      if ( sse && now - lastData >= idleLimit )
      {
         stalled = true;
         return false;
      }
      return true;
   }

   bool OnData( NetworkTransfer& /*sender*/, const void* data, fsize_type size )
   {
      if ( !ShouldContinue() )
         return false;
      buffer.Append( reinterpret_cast<const char*>( data ), size_type( size ) );
      lastData = clock::now();
      if ( sse )
      {
         const std::string d = sse->Feed( reinterpret_cast<const char*>( data ), size_t( size ) );
         if ( !d.empty() )
         {
            volatile AutoLock lock( deltaMutex );
            pendingDelta += d;
         }
      }
      return true;
   }
```
  - In `BuildMessagesRequestBody`, add the `shape` parameter, change `{ "max_tokens", 4096 }` to `{ "max_tokens", shape.maxTokens }`, and before `return req.dump();` add:
```cpp
   if ( shape.stream )
      req["stream"] = true;
```
  - Add after `BuildMessagesRequestBody`:
```cpp
RequestShape ProductionRequestShape( const IsoString& /*model*/ )
{
   RequestShape s;
   s.stream = true;
   s.maxTokens = PICopilotStreamMaxTokens;
   return s;
}
```
  - In `struct AnthropicRequest::Impl`, add `RequestShape shape;`. In the constructor, add the `shape` parameter, and after `m->timeoutSeconds = timeoutSeconds;` add:
```cpp
   m->shape = shape;
   if ( shape.stream )
   {
      m->sink.sse.reset( new SseMessageAssembler );
      m->sink.idleLimit = std::chrono::seconds( shape.streamIdleSeconds );
   }
```
     Then pass `shape` to `BuildMessagesRequestBody( model, systemPrompt, history, tools, shape )`.
  - Add:
```cpp
String AnthropicRequest::TakeStreamedText()
{
   std::string d;
   {
      volatile AutoLock lock( m->sink.deltaMutex );
      d.swap( m->sink.pendingDelta );
   }
   return d.empty() ? String() : String::UTF8ToUTF16( d.c_str() );
}
```
  - In `Perform()`:
     - On the build-error return, set `result.errorKind = RequestErrorKind::Build;`.
     - In the early-cancel return, set `result.errorKind = RequestErrorKind::Cancelled;`.
     - Right after `sink.deadline = …`, add `sink.lastData = ResponseSink::clock::now();`.
     - Replace the aborted-branch body with:
```cpp
      if ( transfer.WasAborted() || sink.cancelled || sink.timedOut || sink.stalled )
      {
         result.ok = false;
         result.httpStatus = 0;
         if ( sink.cancelled )
         {
            result.cancelled = true;
            result.errorKind = RequestErrorKind::Cancelled;
            result.error = "request cancelled";
         }
         else if ( sink.timedOut )
         {
            result.errorKind = RequestErrorKind::TimedOut;
            result.error = timedOutError;
         }
         else if ( sink.stalled )
         {
            result.errorKind = RequestErrorKind::Stalled;
            result.error = String().Format( "the reply stalled: no data from the API for %d s", m->shape.streamIdleSeconds );
         }
         else
         {
            result.errorKind = RequestErrorKind::Network;
            result.error = "request aborted: " + transfer.ErrorInformation();
         }
         return result;
      }
```
     - Set `result.errorKind = RequestErrorKind::Network;` in the `!okHttp && result.httpStatus == 0` branch and in the three `catch` returns.
     - Replace the final `return ParseMessagesResponse( result.httpStatus, sink.buffer, transfer.ErrorInformation() );` with:
```cpp
   // Streamed 2xx: the assembled message (or why there is none). A non-2xx
   // reply is a plain JSON error body even for a streamed request.
   if ( sink.sse && result.httpStatus >= 200 && result.httpStatus < 300 )
   {
      AnthropicResult s;
      s.httpStatus = result.httpStatus;
      s.errorKind = RequestErrorKind::Stream;
      if ( sink.sse->Failed() )
      {
         s.error = "the reply stream failed: " + String::UTF8ToUTF16( sink.sse->Error().c_str() );
         return s;
      }
      if ( !sink.sse->Finished() )
      {
         s.error = sink.sse->SawAnyEvent()
            ? String( "the reply stream ended before it was complete (no message_stop)" )
            : "the API sent no stream events: " + String::UTF8ToUTF16( sink.buffer.Left( 200 ).c_str() );
         return s;
      }
      const std::string body = sink.sse->FinalMessage().dump();
      return ParseMessagesResponse( result.httpStatus, IsoString( body.c_str() ), String() );
   }
   return ParseMessagesResponse( result.httpStatus, sink.buffer, transfer.ErrorInformation() );
```
  - In `ParseMessagesResponse`:
     - In the 2xx success block, right before `result.ok = true;`, add:
```cpp
               if ( j.contains( "usage" ) && j["usage"].is_object() )
                  result.usage = j["usage"];
               if ( j.contains( "input_transformations" ) )
                  result.inputTransformations = j["input_transformations"];
```
     - In the 2xx `if ( !error.IsEmpty() )` block, add `result.errorKind = RequestErrorKind::BadReply;`.
     - In the non-2xx `else` block, add `result.errorKind = RequestErrorKind::Http;`.
     - In the not-JSON early return, set `result.errorKind = (httpStatus >= 200 && httpStatus < 300) ? RequestErrorKind::BadReply : RequestErrorKind::Http;`.

In `ChatThread.h/.cpp`, add the trailing `const RequestShape& shape = RequestShape()` constructor parameter and forward it: `m_request( apiKey, model, systemPrompt, history, url, timeoutSeconds, tools, shape )`. Add:
```cpp
   // Thread-safe: streamed text received since the last call (see AnthropicRequest).
   String TakeStreamedText()
   {
      return m_request.TakeStreamedText();
   }
```

- [ ] **Step 5: Live rendering in the panel.** In `PICopilotInterface.h` (private), add after `bool m_handlingResult = false;`:
```cpp
   // The current request's reply has started rendering live (streamed text).
   bool m_replyShown = false;

   // Appends streamed text received since the last call (UI thread).
   void DrainStreamedText();
```
In `PICopilotInterface.cpp`:
  - In `StartRequest()`, add `m_replyShown = false;` before the `try`, and pass `ProductionRequestShape( PICOPILOT_DEFAULT_MODEL )` as the new last `ChatThread` argument.
  - In `GUIData::GUIData`, change `Poll_Timer.SetInterval( 0.2 );` to `Poll_Timer.SetInterval( 0.1 );`.
  - Add:
```cpp
void PICopilotInterface::DrainStreamedText()
{
   if ( !m_thread )
      return;
   const String d = m_thread->TakeStreamedText();
   if ( d.IsEmpty() )
      return;
   if ( !m_replyShown )
   {
      AppendToLog( "<b>Copilot:</b> " );
      m_replyShown = true;
   }
   AppendToLog( PlainText( d ) );
}
```
  - In `e_Poll_Timer`, insert `DrainStreamedText();` before `if ( m_thread->IsActive() )`, and insert another `DrainStreamedText();` right before `m_thread.Destroy();`. Replace
```cpp
   if ( r.ok && !r.text.IsEmpty() )
      AppendToLog( "<b>Copilot:</b> " + PlainText( r.truncated ? r.text + " [truncated: max_tokens]" : r.text ) + "\n\n" );
```
with
```cpp
   if ( m_replyShown )
   {
      // The reply was rendered live; close it (and say so when it broke off).
      AppendToLog( (r.ok && r.truncated ? PlainText( " [truncated: max_tokens]" ) : String()) + "\n\n" );
      if ( !r.ok && !r.cancelled )
         AppendToLog( PlainText( "(the partial reply above was interrupted; it is not kept in the conversation)" ) + "\n\n" );
      m_replyShown = false;
   }
   else if ( r.ok && !r.text.IsEmpty() )   // not streamed, or no delta arrived before the end
      AppendToLog( "<b>Copilot:</b> " + PlainText( r.truncated ? r.text + " [truncated: max_tokens]" : r.text ) + "\n\n" );
```
  - In `SetBusy`, the tooltip keeps `PICopilotRequestTimeoutSeconds`. Its text now reads 600 s automatically.

In `PICopilotAgentSelfTest.cpp` Section A6, add `ProductionRequestShape( PICOPILOT_DEFAULT_MODEL )` as the last `AnthropicRequest` argument, so the gated live agent run streams. Add `out["liveAgentStreamed"] = true;` next to `out["liveAgentSkipped"]`.

- [ ] **Step 6: Verify GREEN, including the increment-2/3/4 regressions.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) && bash test/run-selftest.sh`
Expected: `PASS: self-test verdict all green`, with `live agent check: RAN against real API, ratio=0.5…` (now streamed). `streamDetail.stream1.takesWhileActive` ≥ 2 and `firstDelta` well below `total`. The existing `cancelOk`/`deadlineOk`/`agentWireOk` keys stay green (non-streamed default shape).

- [ ] **Step 7: Commit.**
```bash
cd /home/scarter4work/projects/astro-pi
git add modules/pi-copilot/src/module/AnthropicClient.h modules/pi-copilot/src/module/AnthropicClient.cpp \
        modules/pi-copilot/src/module/ChatThread.h modules/pi-copilot/src/module/ChatThread.cpp \
        modules/pi-copilot/src/module/PICopilotInterface.h modules/pi-copilot/src/module/PICopilotInterface.cpp \
        modules/pi-copilot/src/module/PICopilotAgentSelfTest.cpp modules/pi-copilot/src/module/PICopilotInc5SelfTest.cpp \
        modules/pi-copilot/test/run-selftest.sh
git commit -m "feat(pi-copilot): streamed replies (SSE) with live rendering, idle deadline and error kinds; live agent run streams

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Conversation management — model catalog, prompt caching, thinking binding, history budget, New chat

This task adds three things.
- **Request shaping per model.** Every production request gets three prompt-cache breakpoints (the last tool, the system block, and a top-level automatic breakpoint for the conversation tail). Models that enforce preserved thinking get the documented `drop_block` binding control, so the harness's history edits (image stripping, trimming, mode switches) degrade gracefully instead of causing a 400.
- **A history budget.** Once the estimated history size exceeds 100k tokens, the oldest whole exchanges are removed, cutting only at a fresh user turn (never inside a tool round). The new first message is marked with a note, and the panel says how many messages were dropped.
- **"Clear" becomes "New chat".**

Gated live checks prove a real cache read on the second request and a 200 on an edited Opus 5.5 history.

**Files:**
- Create: `modules/pi-copilot/src/module/ModelCatalog.h`, `ModelCatalog.cpp`
- Create: `modules/pi-copilot/src/module/HistoryBudget.h`, `HistoryBudget.cpp`
- Modify: `modules/pi-copilot/src/module/AnthropicClient.h/.cpp` (`RequestShape`, body, header)
- Modify: `modules/pi-copilot/src/module/AgentSession.h/.cpp` (trim on append, `TakeTrimmedMessages()`)
- Modify: `modules/pi-copilot/src/module/PICopilotInterface.h/.cpp` (New chat, trim note)
- Modify: `modules/pi-copilot/src/module/PICopilotInc5SelfTest.cpp` (Sections B3, B3L)
- Modify: `modules/pi-copilot/src/module/CMakeLists.txt`, `modules/pi-copilot/test/run-selftest.sh`

**Interfaces:**
- Consumes: `RequestShape`, `ProductionRequestShape()` (Task 3), `StripOlderImages()`, `HistoryIsApiValid()`.
- Produces:
```cpp
struct ModelInfo { const char* id; const char* label; bool thinkingBinding; };
constexpr size_type PICopilotModelCount = 5;
extern const ModelInfo kPICopilotModels[PICopilotModelCount];     // [0] is PICOPILOT_DEFAULT_MODEL
const ModelInfo* FindModel( const IsoString& id );                // nullptr if unknown
int ModelIndex( const IsoString& id );                            // -1 if unknown
#define PICOPILOT_THINKING_BINDING_BETA "thinking-binding-controls-2026-08-01"
// RequestShape gains: bool promptCaching = false; bool thinkingBinding = false;
// ProductionRequestShape( model ): stream, 16000, promptCaching, thinkingBinding = FindModel(model)->thinkingBinding
constexpr size_type PICopilotHistoryTokenBudget = 100000;
constexpr size_type PICopilotHistoryTrimTarget  = 70000;
constexpr size_type PICopilotImageTokenEstimate = 1600;
extern const char* const kPICopilotTrimNote;
size_type EstimateMessageTokens( const AnthropicMessage& m );
size_type EstimateHistoryTokens( const Array<AnthropicMessage>& h, size_type from = 0 );
bool IsFreshUserTurn( const AnthropicMessage& m );
size_type TrimHistoryToBudget( Array<AnthropicMessage>& h, size_type budget, size_type target );  // returns messages removed
size_type AgentSession::TakeTrimmedMessages();                    // messages trimmed since the last call
```

- [ ] **Step 1: Failing tests (Sections B3 + B3L).** In `run-selftest.sh`:
  - Add `'conversationOk', 'liveConversationOk',` after `'streamTransportOk',` in `required_true`.
  - Add `'liveConversationSkipped'` to the `PICOPILOT_REQUIRE_LIVE` tuple.
  - After the `live agent check` print, add:
```python
print('live conversation check: %s' % ('SKIPPED (no key)' if d.get('liveConversationSkipped') else 'RAN against real API, cacheRead=%r binding=%r' % (d.get('liveCacheRead'), d.get('liveBindingTransformations'))))
```
  - In the echo server's default reply, replace the reply text's `json.dumps({"messages": req.get("messages"), "tools": req.get("tools")})` with:
```python
json.dumps({"messages": req.get("messages"), "tools": req.get("tools"), "system": req.get("system"),
            "cache_control": req.get("cache_control"), "thinking": req.get("thinking"),
            "anthropic_beta": self.headers.get("anthropic-beta")})
```

In `PICopilotInc5SelfTest.cpp`, add the includes `"HistoryBudget.h"`, `"ModelCatalog.h"`, `"SystemPrompt.h"` and `"ViewCapture.h"`. Add to the anonymous namespace:
```cpp
int CountKey( const nlohmann::json& v, const char* key )
{
   int n = 0;
   if ( v.is_object() )
      for ( auto it = v.begin(); it != v.end(); ++it )
         n += (it.key() == key ? 1 : 0) + CountKey( it.value(), key );
   else if ( v.is_array() )
      for ( const nlohmann::json& e : v )
         n += CountKey( e, key );
   return n;
}

AnthropicMessage TextMsg( const char* role, const std::string& text )
{
   AnthropicMessage m;
   m.role = role;
   m.content = String::UTF8ToUTF16( text.c_str() );
   return m;
}

AnthropicMessage BlocksMsg( const char* role, const nlohmann::json& blocks )
{
   AnthropicMessage m;
   m.role = role;
   m.blocks = blocks;
   return m;
}

// k exchanges of: fresh user text, assistant text+tool_use, user tool_result+merged text, assistant text.
Array<AnthropicMessage> LongHistory( int k, size_t chars )
{
   Array<AnthropicMessage> h;
   const std::string big( chars, 'a' );
   for ( int i = 0; i < k; ++i )
   {
      const std::string id = "toolu_h" + std::to_string( i );
      h.Add( TextMsg( "user", "question " + std::to_string( i ) + " " + big ) );
      nlohmann::json a = nlohmann::json::array();
      a.push_back( { { "type", "text" }, { "text", "checking" } } );
      a.push_back( { { "type", "tool_use" }, { "id", id }, { "name", "describe_process" }, { "input", { { "id", "PixelMath" } } } } );
      h.Add( BlocksMsg( "assistant", a ) );
      nlohmann::json u = nlohmann::json::array();
      u.push_back( { { "type", "tool_result" }, { "tool_use_id", id },
                     { "content", nlohmann::json::array( { { { "type", "text" }, { "text", big } } } ) }, { "is_error", false } } );
      u.push_back( { { "type", "text" }, { "text", "and also this" } } );
      h.Add( BlocksMsg( "user", u ) );
      h.Add( TextMsg( "assistant", "answer " + std::to_string( i ) + " " + big ) );
   }
   h.Add( TextMsg( "user", "the current question" ) );
   return h;
}
```
Insert above the marker:
```cpp
   // ---- Section B3: models, caching/binding shape, history budget (Task 4) ----
   {
      bool modelsOk = false, shapeOk = false, wireOk = false, trimOk = false, sessionTrimOk = true, noTrimOk = false;
      bool wireSkipped = true;
      nlohmann::json detail = nlohmann::json::object();
      String error;
      try
      {
         std::set<std::string> ids;
         for ( const ModelInfo& m : kPICopilotModels )
            ids.insert( m.id );
         modelsOk = ids.size() == PICopilotModelCount && std::string( kPICopilotModels[0].id ) == PICOPILOT_DEFAULT_MODEL
                 && FindModel( "claude-opus-5-5" ) && FindModel( "claude-opus-5-5" )->thinkingBinding
                 && FindModel( "claude-fable-5-1" ) && FindModel( "claude-fable-5-1" )->thinkingBinding
                 && FindModel( "claude-sonnet-5" ) && !FindModel( "claude-sonnet-5" )->thinkingBinding
                 && FindModel( "claude-haiku-4-5" ) && !FindModel( "claude-haiku-4-5" )->thinkingBinding
                 && FindModel( "claude-opus-4-8" ) && !FindModel( "claude-opus-4-8" )->thinkingBinding
                 && FindModel( "claude-nope" ) == nullptr && ModelIndex( "claude-fable-5-1" ) == 2 && ModelIndex( "x" ) == -1;

         Array<AnthropicMessage> hist;
         hist.Add( TextMsg( "user", "hi" ) );
         const nlohmann::json tools = ToolDefinitions( AgentMode::Copilot );
         const nlohmann::json b55 = nlohmann::json::parse(
            BuildMessagesRequestBody( "claude-opus-5-5", "sys", hist, tools, ProductionRequestShape( "claude-opus-5-5" ) ) );
         const nlohmann::json b48 = nlohmann::json::parse(
            BuildMessagesRequestBody( "claude-opus-4-8", "sys", hist, tools, ProductionRequestShape( "claude-opus-4-8" ) ) );
         const nlohmann::json plain = nlohmann::json::parse( BuildMessagesRequestBody( "m", "sys", hist, tools ) );
         const nlohmann::json eph = { { "type", "ephemeral" } };
         shapeOk = b55.at( "system" ).is_array() && b55["system"].size() == 1
                && b55["system"][0].at( "text" ) == "sys" && b55["system"][0].at( "cache_control" ) == eph
                && b55.at( "tools" ).back().at( "cache_control" ) == eph && b55.at( "cache_control" ) == eph
                && CountKey( b55, "cache_control" ) == 3
                && b55.at( "thinking" ) == nlohmann::json::parse(
                      "{\"type\":\"adaptive\",\"block_binding\":{\"prefix_mismatch_behavior\":\"drop_block\"}}" )
                && !b48.contains( "thinking" ) && CountKey( b48, "cache_control" ) == 3
                && plain.at( "system" ).is_string() && CountKey( plain, "cache_control" ) == 0 && !plain.contains( "thinking" );
         detail["b55"] = b55;

         if ( const char* echoUrl = std::getenv( "PICOPILOT_SELFTEST_ECHO_URL" ) )
         {
            wireSkipped = false;
            auto echo = [&]( const char* model ) {
               RequestShape s = ProductionRequestShape( model );
               s.stream = false;   // the echo path answers non-streamed
               AnthropicRequest req( "sk-ant-invalid-selftest", model, "sys", hist, String( echoUrl ), 30, tools, s );
               const AnthropicResult r = req.Perform();
               return r.ok ? nlohmann::json::parse( U8( r.text ) ) : nlohmann::json( { { "error", U8( r.error ) } } );
            };
            const nlohmann::json e55 = echo( "claude-opus-5-5" ), e48 = echo( "claude-opus-4-8" );
            detail["echo55"] = { { "anthropic_beta", e55.value( "anthropic_beta", nlohmann::json() ) }, { "thinking", e55.value( "thinking", nlohmann::json() ) } };
            wireOk = e55.value( "anthropic_beta", nlohmann::json() ) == PICOPILOT_THINKING_BINDING_BETA
                  && e55.at( "thinking" ).at( "block_binding" ).at( "prefix_mismatch_behavior" ) == "drop_block"
                  && e55.at( "system" ).at( 0 ).at( "cache_control" ) == eph
                  && e48.value( "anthropic_beta", nlohmann::json() ).is_null() && e48.value( "thinking", nlohmann::json() ).is_null();
         }

         {  // Direct trim: 30 long exchanges with tool rounds -> within the target, API-valid, current turn kept.
            Array<AnthropicMessage> h = LongHistory( 30, 6000 );
            const size_type before = EstimateHistoryTokens( h );
            const AnthropicMessage lastBefore = h[h.Length()-1];
            const size_type removed = TrimHistoryToBudget( h, PICopilotHistoryTokenBudget, PICopilotHistoryTrimTarget );
            String why;
            const bool valid = HistoryIsApiValid( h, why );
            const size_type after = EstimateHistoryTokens( h );
            detail["trim"] = { { "before", before }, { "after", after }, { "removed", removed }, { "why", U8( why ) },
                               { "first", h.IsEmpty() ? nlohmann::json() : h[0].blocks } };
            trimOk = before > PICopilotHistoryTokenBudget && removed > 0 && valid
                  && after <= PICopilotHistoryTrimTarget + 100   // + the trim note itself
                  && IsFreshUserTurn( h[0] ) && h[0].blocks.is_array()
                  && h[0].blocks.at( 0 ).at( "text" ) == kPICopilotTrimNote
                  && h[h.Length()-1].content == lastBefore.content;
         }
         {  // Under budget: untouched. Only the current turn over budget: nothing to cut.
            Array<AnthropicMessage> small = LongHistory( 2, 100 );
            const size_type n0 = small.Length();
            Array<AnthropicMessage> huge;
            huge.Add( TextMsg( "user", std::string( 400000, 'b' ) ) );
            noTrimOk = TrimHistoryToBudget( small, PICopilotHistoryTokenBudget, PICopilotHistoryTrimTarget ) == 0
                    && small.Length() == n0
                    && TrimHistoryToBudget( huge, PICopilotHistoryTokenBudget, PICopilotHistoryTrimTarget ) == 0 && huge.Length() == 1;
         }
         {  // Through AgentSession: 40 long plain turns; the history never exceeds the budget and stays valid.
            AgentSession s;
            size_type trimmedTotal = 0;
            const std::string big( 24000, 'c' );
            for ( int i = 0; i < 40 && sessionTrimOk; ++i )
            {
               s.BeginUserTurn( TextMsg( "user", "turn " + std::to_string( i ) + " " + big ) );
               trimmedTotal += s.TakeTrimmedMessages();
               String why;
               sessionTrimOk = HistoryIsApiValid( s.History(), why )
                            && EstimateHistoryTokens( s.History() ) <= PICopilotHistoryTokenBudget;
               AnthropicResult r;
               r.ok = true;
               r.httpStatus = 200;
               r.stopReason = "end_turn";
               r.text = String::UTF8ToUTF16( ("reply " + big).c_str() );
               r.contentBlocks = nlohmann::json::array( { { { "type", "text" }, { "text", "reply " + big } } } );
               s.OnResponse( r, []( const ToolCall& ) { return ToolOutcome(); }, []() { return false; } );
               trimmedTotal += s.TakeTrimmedMessages();
            }
            detail["sessionTrimmed"] = trimmedTotal;
            sessionTrimOk = sessionTrimOk && trimmedTotal > 0;
         }
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }

      const bool ok = modelsOk && shapeOk && (wireSkipped || wireOk) && trimOk && noTrimOk && sessionTrimOk;
      out["conversationDetail"] = detail;
      out["conversationError"] = U8( error );
      out["conversationWireSkipped"] = wireSkipped;
      out["conversationOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section B3L: gated LIVE caching + thinking binding (Task 4) ------------
   {
      bool skipped = true, ok = true;
      nlohmann::json detail = nlohmann::json::object();
      String error;
      if ( const char* key = std::getenv( "PICOPILOT_TEST_API_KEY" ) )
      {
         skipped = false;
         ok = false;
         try
         {
            // (1) Prompt cache: the second identical-prefix request reads the cache.
            Array<AnthropicMessage> h;
            h.Add( TextMsg( "user", "Reply with the single word: ok." ) );
            nlohmann::json usage[2];
            bool bothOk = true;
            for ( int i = 0; i < 2; ++i )
            {
               AnthropicRequest req( String( key ), PICOPILOT_DEFAULT_MODEL, BuildSystemPrompt( AgentMode::Copilot ), h,
                                     PICOPILOT_MESSAGES_URL, PICopilotRequestTimeoutSeconds,
                                     ToolDefinitions( AgentMode::Copilot ), ProductionRequestShape( PICOPILOT_DEFAULT_MODEL ) );
               const AnthropicResult r = req.Perform();
               bothOk = bothOk && r.ok;
               usage[i] = r.usage;
               if ( !r.ok )
                  detail["cacheError"] = U8( r.error );
            }
            const int cacheRead = usage[1].value( "cache_read_input_tokens", 0 );
            detail["cacheUsage"] = { usage[0], usage[1] };
            out["liveCacheRead"] = cacheRead;
            const bool cacheOk = bothOk && cacheRead > 0;

            // (2) Opus 5.5, an EDITED history (the first turn's image is stripped when
            //     the tool round is appended): the drop_block binding keeps it a 200.
            Inc5TestWindow tw( "PCBindLive", 256, 192, 3, 0.2 );
            View v = tw.MainView();
            ToolContext ctx;
            ctx.mode = AgentMode::Advisor;
            ctx.turnViewId = v.FullId();
            AgentSession session;
            StringList notes;
            session.BeginUserTurn( CaptureViewTurn( "Call describe_process for PixelMath, then answer in one short sentence.", &v, notes ) );
            AgentStep s;
            int requests = 0;
            bool allArrays = true;
            nlohmann::json transformations = nlohmann::json::array();
            do
            {
               AnthropicRequest req( String( key ), "claude-opus-5-5", BuildSystemPrompt( AgentMode::Advisor ), session.History(),
                                     PICOPILOT_MESSAGES_URL, PICopilotRequestTimeoutSeconds,
                                     ToolDefinitions( AgentMode::Advisor ), ProductionRequestShape( "claude-opus-5-5" ) );
               const AnthropicResult r = req.Perform();
               ++requests;
               if ( !r.ok )
                  detail["bindingError"] = { { "status", r.httpStatus }, { "error", U8( r.error ) } };
               allArrays = allArrays && r.inputTransformations.is_array();
               transformations.push_back( r.inputTransformations );
               s = session.OnResponse( r, [&ctx]( const ToolCall& c ) { return ExecuteTool( c, ctx ); }, []() { return false; } );
            }
            while ( s.kind == AgentStep::SendAgain && requests < 5 );
            out["liveBindingTransformations"] = transformations;
            detail["bindingRequests"] = requests;
            const bool bindingOk = s.kind == AgentStep::Done && requests >= 2 && allArrays;
            ok = cacheOk && bindingOk;
         }
         catch ( const pcl::Exception& x ) { error = x.Message(); }
         catch ( const std::exception& x ) { error = String( x.what() ); }
      }
      out["liveConversationDetail"] = detail;
      out["liveConversationError"] = U8( error );
      out["liveConversationSkipped"] = skipped;
      out["liveConversationOk"] = ok;
      allOk = allOk && ok;
   }
```

- [ ] **Step 2: Verify RED.** Run the Task 2 Step 3 build command.
Expected: `ModelCatalog.h: No such file or directory`.

- [ ] **Step 3: Implement.** `ModelCatalog.h`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ModelCatalog_h
#define PICopilot_ModelCatalog_h

#include <pcl/String.h>

namespace pcl
{

// Header value of the thinking-binding controls beta (preserved thinking).
#define PICOPILOT_THINKING_BINDING_BETA "thinking-binding-controls-2026-08-01"

// The models offered in the ⚙ dialog. Ids exactly as the Anthropic API names
// them (no date suffixes). thinkingBinding: the model always thinks and binds
// thinking blocks to the conversation prefix (Opus 5.5, Fable 5.1): requests
// carry the drop_block binding control so the harness's history edits (image
// stripping, trimming, a mode switch) drop stale blocks instead of a 400.
struct ModelInfo
{
   const char* id;
   const char* label;
   bool        thinkingBinding;
};

constexpr size_type PICopilotModelCount = 5;

// [0] is PICOPILOT_DEFAULT_MODEL.
extern const ModelInfo kPICopilotModels[PICopilotModelCount];

const ModelInfo* FindModel( const IsoString& id );   // nullptr if unknown
int ModelIndex( const IsoString& id );               // -1 if unknown

} // namespace pcl

#endif // PICopilot_ModelCatalog_h
```
`ModelCatalog.cpp`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ModelCatalog.h"

namespace pcl
{

const ModelInfo kPICopilotModels[PICopilotModelCount] =
{
   { "claude-opus-4-8",  "Claude Opus 4.8 (default)", false },
   { "claude-opus-5-5",  "Claude Opus 5.5",           true  },
   { "claude-fable-5-1", "Claude Fable 5.1",          true  },
   { "claude-sonnet-5",  "Claude Sonnet 5",           false },
   { "claude-haiku-4-5", "Claude Haiku 4.5",          false }
};

int ModelIndex( const IsoString& id )
{
   for ( size_type i = 0; i < PICopilotModelCount; ++i )
      if ( id == kPICopilotModels[i].id )
         return int( i );
   return -1;
}

const ModelInfo* FindModel( const IsoString& id )
{
   const int i = ModelIndex( id );
   return i < 0 ? nullptr : &kPICopilotModels[i];
}

} // namespace pcl
```
`HistoryBudget.h`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_HistoryBudget_h
#define PICopilot_HistoryBudget_h

#include "AnthropicClient.h"

namespace pcl
{

// History budget (estimated tokens) and where a trim brings it back to
// (hysteresis: a trim is a prompt-cache miss, so it should be rare).
constexpr size_type PICopilotHistoryTokenBudget = 100000;
constexpr size_type PICopilotHistoryTrimTarget  = 70000;
constexpr size_type PICopilotImageTokenEstimate = 1600;   // a <=1024 px preview

// Put in front of the new first message after a trim.
extern const char* const kPICopilotTrimNote;

// UTF-8 text bytes / 3 (+ PICopilotImageTokenEstimate per image, + 8 per
// message). A deliberate over-estimate: no network call, never under-counts
// Latin text by much.
size_type EstimateMessageTokens( const AnthropicMessage& m );
size_type EstimateHistoryTokens( const Array<AnthropicMessage>& h, size_type from = 0 );

// A user message that starts a new exchange: role user and not starting with
// a tool_result (a merged "tool_results + your next message" turn is not fresh).
bool IsFreshUserTurn( const AnthropicMessage& m );

// If h is over `budget`, removes its oldest messages up to the earliest fresh
// user turn from which the rest fits `target` -- or, if none does, up to the
// LAST fresh user turn (the current exchange is never cut). Cutting only in
// front of a fresh user turn keeps roles alternating and every
// tool_use/tool_result pair intact. The new first message gets
// kPICopilotTrimNote as its first text block. Returns the number removed.
size_type TrimHistoryToBudget( Array<AnthropicMessage>& h, size_type budget, size_type target );

} // namespace pcl

#endif // PICopilot_HistoryBudget_h
```
`HistoryBudget.cpp`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "HistoryBudget.h"
#include "Utf8.h"

#include <string>

namespace pcl
{

const char* const kPICopilotTrimNote =
   "[Earlier parts of this conversation were removed to keep it within PI Copilot's history budget.]";

namespace
{

bool TypeIs( const nlohmann::json& v, const char* type )
{
   if ( !v.is_object() )
      return false;
   const auto t = v.find( "type" );
   return t != v.end() && t->is_string() && t->get_ref<const std::string&>() == type;
}

size_type TextBytes( const nlohmann::json& v, size_type& images )
{
   if ( v.is_string() )
      return v.get_ref<const std::string&>().size();
   if ( v.is_object() )
   {
      if ( TypeIs( v, "image" ) )
      {
         ++images;
         return 0;
      }
      size_type n = 0;
      for ( auto it = v.begin(); it != v.end(); ++it )
         n += it.key().size() + TextBytes( it.value(), images );
      return n;
   }
   if ( v.is_array() )
   {
      size_type n = 0;
      for ( const nlohmann::json& e : v )
         n += TextBytes( e, images );
      return n;
   }
   return 8;   // number / bool / null
}

} // namespace

size_type EstimateMessageTokens( const AnthropicMessage& m )
{
   size_type images = 0, bytes = 0;
   if ( !m.blocks.is_null() )
      bytes = TextBytes( m.blocks, images );
   else
   {
      bytes = U8( m.content ).size();
      if ( !m.imageJpegBase64.IsEmpty() )
         ++images;
   }
   return bytes/3 + images*PICopilotImageTokenEstimate + 8;
}

size_type EstimateHistoryTokens( const Array<AnthropicMessage>& h, size_type from )
{
   size_type n = 0;
   for ( size_type i = from; i < h.Length(); ++i )
      n += EstimateMessageTokens( h[i] );
   return n;
}

bool IsFreshUserTurn( const AnthropicMessage& m )
{
   if ( m.role != "user" )
      return false;
   if ( !m.blocks.is_array() || m.blocks.empty() )
      return true;
   return !TypeIs( m.blocks[0], "tool_result" );
}

size_type TrimHistoryToBudget( Array<AnthropicMessage>& h, size_type budget, size_type target )
{
   if ( h.Length() < 3 || EstimateHistoryTokens( h ) <= budget )
      return 0;
   size_type lastFresh = 0;
   for ( size_type i = h.Length(); i-- > 0; )
      if ( IsFreshUserTurn( h[i] ) )
      {
         lastFresh = i;
         break;
      }
   if ( lastFresh == 0 )
      return 0;   // the whole history is the current exchange: nothing may be cut
   size_type cut = lastFresh;
   for ( size_type i = 2; i < lastFresh; ++i )
      if ( IsFreshUserTurn( h[i] ) && EstimateHistoryTokens( h, i ) <= target )
      {
         cut = i;
         break;
      }
   h.Remove( h.Begin(), h.At( cut ) );

   AnthropicMessage& first = h[0];
   nlohmann::json blocks = nlohmann::json::array();
   blocks.push_back( { { "type", "text" }, { "text", kPICopilotTrimNote } } );
   if ( first.blocks.is_array() )
      for ( const nlohmann::json& b : first.blocks )
         blocks.push_back( b );
   else
   {
      if ( !first.imageJpegBase64.IsEmpty() )
         blocks.push_back( JpegImageBlock( first.imageJpegBase64 ) );
      if ( !first.content.IsEmpty() )
         blocks.push_back( { { "type", "text" }, { "text", U8( first.content ) } } );
   }
   first.blocks = std::move( blocks );
   first.content.Clear();
   first.imageJpegBase64.Clear();
   return cut;
}

} // namespace pcl
```
In `AnthropicClient.h`, add `bool promptCaching = false;` and `bool thinkingBinding = false;` to `RequestShape`, with comments ("three `cache_control` breakpoints: last tool, system, top-level automatic" and "`thinking` adaptive + `block_binding` drop_block + the `anthropic-beta` header"). In `AnthropicClient.cpp`, add `#include "ModelCatalog.h"` and extend `ProductionRequestShape`:
```cpp
RequestShape ProductionRequestShape( const IsoString& model )
{
   RequestShape s;
   s.stream = true;
   s.maxTokens = PICopilotStreamMaxTokens;
   s.promptCaching = true;
   const ModelInfo* info = FindModel( model );
   s.thinkingBinding = info != nullptr && info->thinkingBinding;
   return s;
}
```
In `BuildMessagesRequestBody`, replace the `req` construction and the tools/stream tail with:
```cpp
   nlohmann::json req = {
      { "model", model.c_str() },
      { "max_tokens", shape.maxTokens },
      { "messages", messages }
   };
   if ( shape.promptCaching )
   {
      // Breakpoint 1 (system; it covers the tools too, render order tools ->
      // system), and a top-level automatic breakpoint that rolls forward over
      // the conversation tail.
      nlohmann::json sys = nlohmann::json::object();
      sys["type"] = "text";
      sys["text"] = U8( systemPrompt );
      sys["cache_control"] = { { "type", "ephemeral" } };
      req["system"] = nlohmann::json::array();
      req["system"].push_back( sys );
      req["cache_control"] = { { "type", "ephemeral" } };
   }
   else
      req["system"] = U8( systemPrompt );
   if ( !tools.is_null() )
   {
      req["tools"] = tools;
      // Breakpoint 2: the tool list alone (it outlives a system-prompt change).
      if ( shape.promptCaching && tools.is_array() && !tools.empty() )
         req["tools"].back()["cache_control"] = { { "type", "ephemeral" } };
   }
   if ( shape.thinkingBinding )
      req["thinking"] = { { "type", "adaptive" },
                          { "block_binding", { { "prefix_mismatch_behavior", "drop_block" } } } };
   if ( shape.stream )
      req["stream"] = true;
   return req.dump();
```
In the `AnthropicRequest` constructor, replace the `SetCustomHTTPHeaders(...)` call with:
```cpp
      String headers = String( "x-api-key: " ) + apiKey
                     + "\nanthropic-version: 2023-06-01\ncontent-type: application/json";
      if ( shape.thinkingBinding )
         headers += "\nanthropic-beta: " PICOPILOT_THINKING_BINDING_BETA;
      m->transfer.SetCustomHTTPHeaders( headers );
```
In `AgentSession.h`, add `#include "HistoryBudget.h"`, the public member
```cpp
   // Messages TrimHistoryToBudget() removed since the last call (the panel
   // tells the user).
   size_type TakeTrimmedMessages()
   {
      const size_type n = m_trimmed;
      m_trimmed = 0;
      return n;
   }
```
and the private member `size_type m_trimmed = 0;`. In `AgentSession.cpp`:
  - In `Clear()`, add `m_trimmed = 0;`.
  - In `BeginUserTurn()`, after `StripOlderImages( m_history );`, add `m_trimmed += TrimHistoryToBudget( m_history, PICopilotHistoryTokenBudget, PICopilotHistoryTrimTarget );`.
  - In `OnResponse()`, inside the round-append `try`, after `StripOlderImages( m_history );`, add the same line.
  - In the `stopReason != "tool_use"` path, after `m_history.Add( assistant );`, add the same line (a long reply can push the history over the budget).

In `CMakeLists.txt` `MODULE_SOURCES`, add `ModelCatalog.cpp` and `HistoryBudget.cpp` after `SseStream.cpp`.

Panel (`PICopilotInterface.h/.cpp`):
  - Declare `void NoteTrimmed();` (private) and add:
```cpp
void PICopilotInterface::NoteTrimmed()
{
   const size_type n = m_session.TakeTrimmedMessages();
   if ( n > 0 )
      AppendToLog( PlainText( String().Format( "(%u older messages are no longer sent to the model, to keep this "
                                               "conversation within its history budget. Your images are unchanged; "
                                               "press New chat to start fresh.)", unsigned( n ) ) ) + "\n\n" );
}
```
  - Call `NoteTrimmed();` right after `m_session.BeginUserTurn( … );` in `SendCurrentInput()`, and right after the `m_session.OnResponse( … )` block in `e_Poll_Timer` (after `m_handlingResult = false;`).
  - In `ProductionRequestShape( PICOPILOT_DEFAULT_MODEL )`, the model stays the default until Task 6.
  - In `GUIData::GUIData`, change `Clear_Button.SetText( "Clear" );` to `Clear_Button.SetText( "New chat" );` and its tooltip to `"<p>Start a new chat: the model forgets this conversation and the log is cleared. Your images and their History are not touched.</p>"`.
  - In `e_Clear_Click`, change the note to `"(new chat started: the model no longer sees the earlier conversation; your images are unchanged)"`.
  - In `TurnEndNotes.cpp`, change `press Clear to start a new chat` to `press New chat to start fresh`. In `PICopilotAgentSelfTest.cpp` Section A7b case `abortInvalid`, change `all.Contains( "Clear" )` to `all.Contains( "New chat" )` (same meaning, new button name).

- [ ] **Step 4: Verify GREEN (with the live key).**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) && bash test/run-selftest.sh`
Expected: `PASS: self-test verdict all green`, and `live conversation check: RAN against real API, cacheRead=<n > 0> binding=[[…],[…]]`.
- If the Opus 5.5 request returns 400 naming `block_binding`, the beta header, or `thinking`, it is **BLOCKED**. Report `liveConversationDetail.bindingError` verbatim (Ruling 4). Do not remove the binding.
- If `cacheRead` is 0 on both of two consecutive runs, report `cacheUsage`. Check first that the system+tools prefix is ≥ 1024 tokens (`usage[0].cache_creation_input_tokens`).

- [ ] **Step 5: Commit.**
```bash
cd /home/scarter4work/projects/astro-pi
git add modules/pi-copilot/src/module/ModelCatalog.h modules/pi-copilot/src/module/ModelCatalog.cpp \
        modules/pi-copilot/src/module/HistoryBudget.h modules/pi-copilot/src/module/HistoryBudget.cpp \
        modules/pi-copilot/src/module/AnthropicClient.h modules/pi-copilot/src/module/AnthropicClient.cpp \
        modules/pi-copilot/src/module/AgentSession.h modules/pi-copilot/src/module/AgentSession.cpp \
        modules/pi-copilot/src/module/PICopilotInterface.h modules/pi-copilot/src/module/PICopilotInterface.cpp \
        modules/pi-copilot/src/module/TurnEndNotes.cpp modules/pi-copilot/src/module/PICopilotInc5SelfTest.cpp \
        modules/pi-copilot/src/module/PICopilotAgentSelfTest.cpp \
        modules/pi-copilot/src/module/CMakeLists.txt modules/pi-copilot/test/run-selftest.sh
git commit -m "feat(pi-copilot): prompt caching, thinking-binding for Opus 5.5/Fable 5.1, history token budget with pair-safe trimming, New chat

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: API key in the system keyring (secret-tool), with a verified migration and a visible Settings fallback

The key moves out of plaintext PI Settings and into the desktop's Secret Service. PI Copilot does this through the `secret-tool` CLI, run by `pcl::ExternalProcess` through `/usr/bin/env -u LD_LIBRARY_PATH` (PI pollutes the child's library path), with no libsecret link. Every keyring write is **read back and compared** before the plaintext copy is removed. When the keyring is unusable (no `secret-tool`, no Secret Service, a locked keyring), the key goes to Settings, and the user is told why in words.

**Files:**
- Create: `modules/pi-copilot/src/module/Keyring.h`, `Keyring.cpp`
- Rewrite: `modules/pi-copilot/src/module/KeyStore.h`, `KeyStore.cpp`
- Modify: `modules/pi-copilot/src/module/ConfigDialog.cpp` (the new KeyStore API; Task 6 rewrites the dialog)
- Modify: `modules/pi-copilot/src/module/PICopilotInterface.h/.cpp` (`SendCurrentInput`, `e_Config_Click`)
- Modify: `modules/pi-copilot/src/module/PICopilotSelfTest.cpp` (test keyring attributes FIRST)
- Modify: `modules/pi-copilot/src/module/PICopilotInc5SelfTest.cpp` (Section B4)
- Modify: `modules/pi-copilot/src/module/CMakeLists.txt`, `modules/pi-copilot/test/run-selftest.sh`

**Interfaces:**
- Consumes: `ThePICopilotModule->ProcessEvents()`, `U8()`.
- Produces:
```cpp
struct KeyringId { String program = "secret-tool"; String service = "picopilot"; String account = "anthropic-api-key"; };
struct KeyringResult { bool ok = false; bool found = false; IsoString secret; String error; };
constexpr int PICopilotKeyringTimeoutMs = 60000;
KeyringResult KeyringLookup( const KeyringId& );
KeyringResult KeyringStore( const KeyringId&, const String& label, const IsoString& secret );
KeyringResult KeyringClear( const KeyringId& );
namespace KeyStore {
   enum class Where { None, Keyring, Settings };
   struct State { String key; Where where = Where::None; String note; };
   State  Load();                    // cached once a key is known; migrates a Settings key into the keyring
   State  Save( const String& key ); // keyring (verified) else Settings + note
   String Clear();                   // "" or why the keyring entry could not be removed
   String DescribeWhere( const State& );
   void   SetKeyringForSelfTest( const KeyringId&, const IsoString& settingsKey );
}
```

- [ ] **Step 1: Failing test (Section B4) + self-test isolation.** In `PICopilotSelfTest.cpp`, add `#include "KeyStore.h"`, `#include "Keyring.h"` and `#include <chrono>`. At the very top of `RunSelfTest()`, before path 1, add:
```cpp
   // Before ANY KeyStore use: the self-test must never read, write or delete
   // the user's real key (keyring service "picopilot", Settings key
   // "PICopilot/AnthropicApiKey").
   {
      KeyringId testId;
      testId.service = "picopilot-selftest";
      testId.account = String().Format( "selftest-%u",
         unsigned( std::chrono::steady_clock::now().time_since_epoch().count() & 0xFFFFFF ) );
      KeyStore::SetKeyringForSelfTest( testId, "PICopilot/SelfTestApiKey" );
   }
```
In `run-selftest.sh`, add `'keyStoreKeyringOk',` after `'liveConversationOk',`. In `PICopilotInc5SelfTest.cpp`, add `#include "KeyStore.h"`, `#include "Keyring.h"` and `#include <pcl/Settings.h>`, and insert above the marker:
```cpp
   // ---- Section B4: keyring-first key storage (Task 5) ------------------------
   {
      bool missOk = false, saveOk = false, loadOk = false, migrateOk = false, fallbackOk = false,
           clearOk = false, noLeakOk = true;
      nlohmann::json detail = nlohmann::json::object();
      String error;
      const IsoString sk = "PICopilot/SelfTestApiKey";
      KeyringId id;
      id.service = "picopilot-selftest";
      id.account = String().Format( "b4-%u", unsigned( std::chrono::steady_clock::now().time_since_epoch().count() & 0xFFFFFF ) );
      auto noLeak = [&]( const String& s ) { noLeakOk = noLeakOk && !s.Contains( "sk-ant-selftest" ); };
      try
      {
         KeyStore::SetKeyringForSelfTest( id, sk );
         Settings::Remove( sk );

         const KeyringResult miss = KeyringLookup( id );
         noLeak( miss.error );
         detail["miss"] = { { "ok", miss.ok }, { "found", miss.found }, { "error", U8( miss.error ) } };
         missOk = miss.ok && !miss.found;

         const KeyStore::State saved = KeyStore::Save( "sk-ant-selftest-AAAA" );
         noLeak( saved.note );
         const KeyringResult back = KeyringLookup( id );
         String settingsCopy;
         Settings::Read( sk, settingsCopy );
         detail["save"] = { { "where", int( saved.where ) }, { "note", U8( saved.note ) } };
         saveOk = saved.where == KeyStore::Where::Keyring && back.found && back.secret == "sk-ant-selftest-AAAA"
               && settingsCopy.IsEmpty() && KeyStore::DescribeWhere( saved ) == "stored in the system keyring";

         KeyStore::SetKeyringForSelfTest( id, sk );   // drop the cache: a fresh Load() reads the keyring
         const KeyStore::State loaded = KeyStore::Load();
         loadOk = loaded.where == KeyStore::Where::Keyring && loaded.key == "sk-ant-selftest-AAAA" && loaded.note.IsEmpty();

         // Migration: a plaintext Settings key moves into the keyring, verified, and the plaintext is removed.
         clearOk = KeyStore::Clear().IsEmpty() && !KeyringLookup( id ).found;
         Settings::Write( sk, String( "sk-ant-selftest-BBBB" ) );
         KeyStore::SetKeyringForSelfTest( id, sk );
         const KeyStore::State migrated = KeyStore::Load();
         noLeak( migrated.note );
         String left;
         Settings::Read( sk, left );
         const KeyringResult mback = KeyringLookup( id );
         detail["migrate"] = { { "where", int( migrated.where ) }, { "note", U8( migrated.note ) } };
         migrateOk = migrated.where == KeyStore::Where::Keyring && migrated.key == "sk-ant-selftest-BBBB"
                  && migrated.note.Contains( "moved" ) && left.IsEmpty() && mback.found && mback.secret == "sk-ant-selftest-BBBB";

         // Keyring unusable -> Settings, with a visible note; the plaintext copy is kept.
         KeyringId bad = id;
         bad.program = "/nonexistent/secret-tool";
         KeyStore::SetKeyringForSelfTest( bad, sk );
         const KeyStore::State fb = KeyStore::Save( "sk-ant-selftest-CCCC" );
         noLeak( fb.note );
         String plain;
         Settings::Read( sk, plain );
         KeyStore::SetKeyringForSelfTest( bad, sk );
         const KeyStore::State fbLoad = KeyStore::Load();
         noLeak( fbLoad.note );
         const String badClear = KeyStore::Clear();
         noLeak( badClear );
         String afterClear;
         Settings::Read( sk, afterClear );
         detail["fallback"] = { { "note", U8( fb.note ) }, { "loadNote", U8( fbLoad.note ) }, { "clear", U8( badClear ) } };
         fallbackOk = fb.where == KeyStore::Where::Settings && plain == "sk-ant-selftest-CCCC"
                   && fb.note.Contains( "keyring could not be used" ) && fb.note.Contains( "not installed" )
                   && KeyStore::DescribeWhere( fb ) == "stored in PixInsight's settings (plain text)"
                   && fbLoad.where == KeyStore::Where::Settings && fbLoad.key == "sk-ant-selftest-CCCC"
                   && !badClear.IsEmpty() && afterClear.IsEmpty();

         KeyStore::SetKeyringForSelfTest( id, sk );
         clearOk = clearOk && KeyStore::Clear().IsEmpty() && !KeyringLookup( id ).found
                && KeyStore::Load().where == KeyStore::Where::None;
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      KeyringClear( id );
      Settings::Remove( sk );

      const bool ok = missOk && saveOk && loadOk && migrateOk && fallbackOk && clearOk && noLeakOk;
      out["keyStoreDetail"] = detail;
      out["keyStoreError"] = U8( error );
      out["keyStoreKeyringOk"] = ok;
      allOk = allOk && ok;
   }
```

- [ ] **Step 2: Verify RED.** Run the Task 2 Step 3 build command.
Expected: `Keyring.h: No such file or directory`.

- [ ] **Step 3: Implement.** `Keyring.h`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_Keyring_h
#define PICopilot_Keyring_h

#include <pcl/String.h>

namespace pcl
{

// Which keyring item holds the key: secret-tool attributes
// "service <service> account <account>". program is looked up on PATH.
struct KeyringId
{
   String program = "secret-tool";
   String service = "picopilot";
   String account = "anthropic-api-key";
};

// ok: secret-tool ran and answered (found says whether an item exists).
// error: why it could not be used -- never contains the secret.
struct KeyringResult
{
   bool      ok = false;
   bool      found = false;
   IsoString secret;
   String    error;
};

// How long a keyring call may take, including the desktop's unlock prompt.
constexpr int PICopilotKeyringTimeoutMs = 60000;

// Root thread only (pcl::ExternalProcess). Each call runs
// /usr/bin/env -u LD_LIBRARY_PATH <program> ... (PixInsight's own library
// path would otherwise be inherited and can break system binaries), pumps
// events (user input excluded) while waiting, and never throws.
KeyringResult KeyringLookup( const KeyringId& id );
KeyringResult KeyringStore( const KeyringId& id, const String& label, const IsoString& secret );
KeyringResult KeyringClear( const KeyringId& id );

} // namespace pcl

#endif // PICopilot_Keyring_h
```
`Keyring.cpp`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "Keyring.h"
#include "PICopilotModule.h"

#include <pcl/Exception.h>
#include <pcl/ExternalProcess.h>
#include <pcl/StringList.h>

#include <chrono>
#include <thread>

namespace pcl
{

namespace
{

struct ToolRun
{
   bool      finished = false;
   int       exitCode = -1;
   bool      crashed = false;
   IsoString out;
   IsoString err;
   String    error;   // set when it did not finish
};

IsoString Bytes( const ByteArray& b )
{
   IsoString s;
   if ( !b.IsEmpty() )
      s.Append( reinterpret_cast<const char*>( b.Begin() ), b.Length() );
   return s;
}

ToolRun RunSecretTool( const KeyringId& id, const StringList& args, const IsoString* input )
{
   ToolRun r;
   try
   {
      ExternalProcess p;
      StringList a;
      a << String( "-u" ) << String( "LD_LIBRARY_PATH" ) << id.program;
      for ( const String& s : args )
         a << s;
      p.Start( "/usr/bin/env", a );
      if ( !p.WaitForStarted( 10000 ) )
      {
         r.error = "could not start /usr/bin/env for " + id.program;
         return r;
      }
      if ( input != nullptr )
         p.Write( *input );
      p.CloseStandardInput();
      // Not WaitForFinished(): it can return before a slow child exits
      // (repo memory pi-externalprocess-gotchas). Spin, pumping events.
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( PICopilotKeyringTimeoutMs );
      while ( p.IsStarting() || p.IsRunning() )
      {
         if ( std::chrono::steady_clock::now() >= deadline )
         {
            p.Kill();
            r.error = id.program + String().Format( " did not finish within %d s (is the keyring locked, with its "
                                                    "unlock prompt hidden?)", PICopilotKeyringTimeoutMs/1000 );
            return r;
         }
         ThePICopilotModule->ProcessEvents( true/*excludeUserInputEvents*/ );
         std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
      }
      r.finished = true;
      r.exitCode = p.ExitCode();
      r.crashed = p.HasCrashed();
      r.out = Bytes( p.StandardOutput() );
      r.err = Bytes( p.StandardError() );
   }
   catch ( const pcl::Exception& x )
   {
      r.error = "could not run " + id.program + ": " + x.Message();
   }
   catch ( ... )
   {
      r.error = "could not run " + id.program;
   }
   return r;
}

String Detail( const ToolRun& r )
{
   const IsoString e = r.err.Trimmed();
   return e.IsEmpty() ? String( "(no message)" ) : String( e.Left( 200 ) );
}

// env exits 127 when the program cannot be found.
String NotInstalled( const KeyringId& id )
{
   return "'" + id.program + "' is not installed or not on PATH (on Fedora/Nobara: dnf install libsecret; "
          "on Debian/Ubuntu: apt install libsecret-tools)";
}

StringList Attributes( const KeyringId& id )
{
   StringList a;
   a << String( "service" ) << id.service << String( "account" ) << id.account;
   return a;
}

} // namespace

KeyringResult KeyringLookup( const KeyringId& id )
{
   KeyringResult k;
   StringList args;
   args << String( "lookup" );
   args.Add( Attributes( id ) );
   const ToolRun r = RunSecretTool( id, args, nullptr );
   if ( !r.finished )
      k.error = r.error;
   else if ( r.exitCode == 127 )
      k.error = NotInstalled( id );
   else if ( r.exitCode == 0 && !r.crashed )
   {
      k.ok = true;
      k.secret = r.out.Trimmed();
      k.found = !k.secret.IsEmpty();
   }
   else if ( r.exitCode == 1 && r.out.IsEmpty() && r.err.Trimmed().IsEmpty() )
      k.ok = true;   // no such item
   else
      k.error = String().Format( "secret-tool lookup failed (exit %d): ", r.exitCode ) + Detail( r );
   return k;
}

KeyringResult KeyringStore( const KeyringId& id, const String& label, const IsoString& secret )
{
   KeyringResult k;
   StringList args;
   args << String( "store" ) << ("--label=" + label);
   args.Add( Attributes( id ) );
   const ToolRun r = RunSecretTool( id, args, &secret );   // secret-tool reads the secret from stdin
   if ( !r.finished )
      k.error = r.error;
   else if ( r.exitCode == 127 )
      k.error = NotInstalled( id );
   else if ( r.exitCode == 0 && !r.crashed )
      k.ok = true;
   else
      k.error = String().Format( "secret-tool store failed (exit %d): ", r.exitCode ) + Detail( r );
   return k;
}

KeyringResult KeyringClear( const KeyringId& id )
{
   KeyringResult k;
   StringList args;
   args << String( "clear" );
   args.Add( Attributes( id ) );
   const ToolRun r = RunSecretTool( id, args, nullptr );
   if ( !r.finished )
      k.error = r.error;
   else if ( r.exitCode == 127 )
      k.error = NotInstalled( id );
   else if ( (r.exitCode == 0 || r.exitCode == 1) && !r.crashed && r.err.Trimmed().IsEmpty() )
      k.ok = true;   // exit 1 without a message: nothing to clear
   else
      k.error = String().Format( "secret-tool clear failed (exit %d): ", r.exitCode ) + Detail( r );
   return k;
}

} // namespace pcl
```
`KeyStore.h`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_KeyStore_h
#define PICopilot_KeyStore_h

#include "Keyring.h"

#include <pcl/String.h>

namespace pcl { namespace KeyStore {

enum class Where { None, Keyring, Settings };

// The key and where it lives. note: a plain-language sentence for the user
// (a migration that happened, or why the keyring could not be used); never
// contains the key.
struct State
{
   String key;
   Where  where = Where::None;
   String note;
};

// Keyring first. A key found in PixInsight Settings (0.1.1.x and earlier, or a
// fallback) is written to the keyring, READ BACK and compared, and only then
// removed from Settings. Cached once a key is known (the next Save()/Clear()
// or SetKeyringForSelfTest() refreshes it). Root thread only.
State Load();

// Keyring (verified) and the Settings copy removed; if the keyring cannot be
// used, Settings + a note saying why. The caller validates the key.
State Save( const String& key );

// Removes the key from both places. Returns "" or why the keyring entry could
// not be removed (the Settings copy is always removed).
String Clear();

// "stored in the system keyring" / "stored in PixInsight's settings (plain
// text)" / "not set".
String DescribeWhere( const State& state );

// Self-test only: another keyring item and Settings key, and a dropped cache.
void SetKeyringForSelfTest( const KeyringId& id, const IsoString& settingsKey );

} } // namespace pcl::KeyStore

#endif // PICopilot_KeyStore_h
```
`KeyStore.cpp`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "KeyStore.h"
#include "Utf8.h"

#include <pcl/Settings.h>

namespace pcl { namespace KeyStore {

namespace
{

KeyringId  g_id;
IsoString  g_settingsKey = "PICopilot/AnthropicApiKey";
bool       g_cached = false;
State      g_cache;

const char* const kLabel = "PI Copilot: Anthropic API key";

bool IsPrintableAscii( const IsoString& s )
{
   if ( s.IsEmpty() )
      return false;
   for ( const char c : s )
      if ( static_cast<unsigned char>( c ) < 0x21 || static_cast<unsigned char>( c ) > 0x7E )
         return false;
   return true;
}

// Writes the key to the keyring and reads it back. "" when verified, else why not.
String StoreVerified( const String& key )
{
   const IsoString bytes( U8( key ).c_str() );   // printable ASCII (ConfigDialog validates)
   const KeyringResult w = KeyringStore( g_id, kLabel, bytes );
   if ( !w.ok )
      return w.error;
   const KeyringResult r = KeyringLookup( g_id );
   if ( !r.ok )
      return "the key was written but could not be read back: " + r.error;
   if ( !r.found || r.secret != bytes )
      return "the key read back from the keyring did not match what was written";
   return String();
}

State Remember( const State& s )
{
   g_cache = s;
   g_cached = !s.key.IsEmpty();   // retry next time when nothing was found (e.g. a locked keyring)
   return s;
}

} // namespace

State Load()
{
   if ( g_cached )
      return g_cache;
   State st;
   String plain;
   Settings::Read( g_settingsKey, plain );
   if ( !plain.IsEmpty() )
   {
      st.key = plain;
      const String why = StoreVerified( plain );
      if ( why.IsEmpty() )
      {
         Settings::Remove( g_settingsKey );
         st.where = Where::Keyring;
         st.note = "Your API key was moved from PixInsight's settings (plain text) into the system keyring.";
      }
      else
      {
         st.where = Where::Settings;
         st.note = "The system keyring could not be used (" + why + "), so your API key stays in PixInsight's "
                   "settings in plain text.";
      }
      return Remember( st );
   }
   const KeyringResult r = KeyringLookup( g_id );
   if ( r.ok && r.found )
   {
      if ( IsPrintableAscii( r.secret ) )
      {
         st.key = String( r.secret );
         st.where = Where::Keyring;
      }
      else
         st.note = "The API key entry in the system keyring is not a valid key; enter it again in PI Copilot's settings.";
   }
   else if ( !r.ok )
      st.note = "Could not read the system keyring (" + r.error + ").";
   return Remember( st );
}

State Save( const String& key )
{
   State st;
   st.key = key;
   const String why = StoreVerified( key );
   if ( why.IsEmpty() )
   {
      Settings::Remove( g_settingsKey );
      st.where = Where::Keyring;
   }
   else
   {
      Settings::Write( g_settingsKey, key );
      st.where = Where::Settings;
      st.note = "The system keyring could not be used (" + why + "), so your API key is stored in PixInsight's "
                "settings in plain text.";
   }
   return Remember( st );
}

String Clear()
{
   Settings::Remove( g_settingsKey );
   g_cache = State();
   g_cached = false;
   const KeyringResult r = KeyringClear( g_id );
   return r.ok ? String() : "Could not remove the key from the system keyring (" + r.error + ").";
}

String DescribeWhere( const State& s )
{
   switch ( s.where )
   {
   case Where::Keyring:  return "stored in the system keyring";
   case Where::Settings: return "stored in PixInsight's settings (plain text)";
   default:              return "not set";
   }
}

void SetKeyringForSelfTest( const KeyringId& id, const IsoString& settingsKey )
{
   g_id = id;
   g_settingsKey = settingsKey;
   g_cache = State();
   g_cached = false;
}

} } // namespace pcl::KeyStore
```
In `CMakeLists.txt`, add `Keyring.cpp` after `HistoryBudget.cpp` (`KeyStore.cpp` is already listed).

In `ConfigDialog.cpp`:
  - In `OK_Button_Click`, replace `KeyStore::Clear();` (empty field) with
```cpp
      const String why = KeyStore::Clear();
      if ( !why.IsEmpty() )
         MessageBox( "<p>" + why + "</p>", "PI Copilot", StdIcon::Warning, StdButton::Ok ).Execute();
```
  - Replace `KeyStore::Save( result_ );` with
```cpp
   const KeyStore::State st = KeyStore::Save( result_ );
   if ( st.where == KeyStore::Where::Settings )
      MessageBox( "<p>" + st.note + "</p>", "PI Copilot", StdIcon::Warning, StdButton::Ok ).Execute();
```
In `PICopilotInterface.cpp`:
  - In `e_Config_Click`, use `d.Run( KeyStore::Load().key );`.
  - In `SendCurrentInput()`, replace `String key = KeyStore::Load();` with
```cpp
   const KeyStore::State ks = KeyStore::Load();
   if ( !ks.note.IsEmpty() && ks.note != m_lastKeyNote )
   {
      AppendToLog( PlainText( "(" + ks.note + ")" ) + "\n\n" );   // once per distinct note
      m_lastKeyNote = ks.note;
   }
   const String key = ks.key;
```
  - Add the private member `String m_lastKeyNote;` to `PICopilotInterface.h`.

- [ ] **Step 4: Verify GREEN.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) && bash test/run-selftest.sh`
Expected: `PASS: self-test verdict all green`, `keyStoreKeyringOk: true`. Then prove that the production item was never touched: `secret-tool search service picopilot account anthropic-api-key 2>&1 | head -3` prints the same thing it printed before the run (nothing, on this dev box). Also, `secret-tool search service picopilot-selftest 2>&1 | grep -c '^\[/'` prints `0` (the tests cleaned up).

- [ ] **Step 5: Commit.**
```bash
cd /home/scarter4work/projects/astro-pi
git add modules/pi-copilot/src/module/Keyring.h modules/pi-copilot/src/module/Keyring.cpp \
        modules/pi-copilot/src/module/KeyStore.h modules/pi-copilot/src/module/KeyStore.cpp \
        modules/pi-copilot/src/module/ConfigDialog.cpp modules/pi-copilot/src/module/PICopilotInterface.h \
        modules/pi-copilot/src/module/PICopilotInterface.cpp modules/pi-copilot/src/module/PICopilotSelfTest.cpp \
        modules/pi-copilot/src/module/PICopilotInc5SelfTest.cpp modules/pi-copilot/src/module/CMakeLists.txt \
        modules/pi-copilot/test/run-selftest.sh
git commit -m "feat(pi-copilot): API key in the system keyring via secret-tool; verified migration off plaintext Settings; visible fallback

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: ⚙ settings and polish — model choice, Allow scripts toggle, left/right placement, "Capturing view…", precise failure wording

The ⚙ dialog grows from "API key" to four persisted settings:
- the key, with a line saying where it is stored
- the model (Task 4's catalog; default `claude-opus-4-8`)
- **Allow scripts** (the `run_pjsr` switch, off by default; Task 9 reads it)
- the default panel side (right/left)

The panel reads the model per message, shows "Capturing view…" on the Send button while a large view is captured (painted before the capture blocks), and words every failure by its `RequestErrorKind`. The result is plain sentences with the cause and the next step, never "Error 0". A refusal gets its own sentence.

**Files:**
- Create: `modules/pi-copilot/src/module/CopilotSettings.h`, `CopilotSettings.cpp`
- Modify: `modules/pi-copilot/src/module/PanelPlacement.h` (`PanelSide`)
- Rewrite: `modules/pi-copilot/src/module/ConfigDialog.h`, `ConfigDialog.cpp`
- Modify: `modules/pi-copilot/src/module/AgentSession.h/.cpp` (`AgentStep::errorKind`)
- Modify: `modules/pi-copilot/src/module/TurnEndNotes.cpp`
- Modify: `modules/pi-copilot/src/module/AnthropicClient.cpp` (refusal wording)
- Modify: `modules/pi-copilot/src/module/PICopilotInterface.h/.cpp`
- Modify: `modules/pi-copilot/src/module/PICopilotInc5SelfTest.cpp` (Section B5)
- Modify: `modules/pi-copilot/src/module/CMakeLists.txt`, `modules/pi-copilot/test/run-selftest.sh`

**Interfaces:**
- Consumes: `kPICopilotModels`, `FindModel()`, `ModelIndex()` (Task 4), `KeyStore::State/Load/Save/Clear/DescribeWhere` (Task 5), `RequestErrorKind` (Task 3).
- Produces:
```cpp
enum class PanelSide { Right = 0, Left = 1 };                       // PanelPlacement.h
PanelPlacement ComputeDefaultPanelPlacement( int cx, int cy, int width, int topMargin, int bottomMargin,
                                             int sideMargin, PanelSide side = PanelSide::Right );
namespace CopilotSettings {
   IsoString LoadModel();              // "PICopilot/Model"; unknown/empty -> PICOPILOT_DEFAULT_MODEL
   void      SaveModel( const IsoString& id );
   bool      LoadRunPjsrEnabled();     // "PICopilot/RunPjsrEnabled"; default false
   void      SaveRunPjsrEnabled( bool );
   PanelSide LoadPanelSide();          // "PICopilot/PanelSide"; default Right
   void      SavePanelSide( PanelSide );
}
struct ConfigOutcome { bool accepted = false; bool sideChanged = false; };
ConfigOutcome ConfigDialog::Run();
// AgentStep gains: RequestErrorKind errorKind = RequestErrorKind::None;
```

- [ ] **Step 1: Failing test (Section B5).** Add `'configPolishOk',` after `'keyStoreKeyringOk',` in `required_true`. In `PICopilotInc5SelfTest.cpp`, add `#include "CopilotSettings.h"`, `#include "PanelPlacement.h"` and `#include "TurnEndNotes.h"`, then insert above the marker:
```cpp
   // ---- Section B5: settings, placement side, failure wording (Task 6) ---------
   {
      bool settingsOk = false, placementOk = false, wordingOk = true, kindOk = false, refusalOk = false;
      nlohmann::json detail = nlohmann::json::object();
      String error;
      try
      {
         for ( const char* k : { "PICopilot/Model", "PICopilot/RunPjsrEnabled", "PICopilot/PanelSide" } )
            Settings::Remove( IsoString( k ) );
         const bool defaults = CopilotSettings::LoadModel() == PICOPILOT_DEFAULT_MODEL
                            && !CopilotSettings::LoadRunPjsrEnabled()
                            && CopilotSettings::LoadPanelSide() == PanelSide::Right;
         CopilotSettings::SaveModel( "claude-opus-5-5" );
         CopilotSettings::SaveRunPjsrEnabled( true );
         CopilotSettings::SavePanelSide( PanelSide::Left );
         const bool stored = CopilotSettings::LoadModel() == "claude-opus-5-5" && CopilotSettings::LoadRunPjsrEnabled()
                          && CopilotSettings::LoadPanelSide() == PanelSide::Left;
         Settings::Write( "PICopilot/Model", String( "claude-bogus-9" ) );
         const bool unknownFallsBack = CopilotSettings::LoadModel() == PICOPILOT_DEFAULT_MODEL;
         for ( const char* k : { "PICopilot/Model", "PICopilot/RunPjsrEnabled", "PICopilot/PanelSide" } )
            Settings::Remove( IsoString( k ) );
         settingsOk = defaults && stored && unknownFallsBack;

         const PanelPlacement r = ComputeDefaultPanelPlacement( 1280, 720, 420, 40, 60, 8 );
         const PanelPlacement l = ComputeDefaultPanelPlacement( 1280, 720, 420, 40, 60, 8, PanelSide::Left );
         const PanelPlacement ml = ComputeDefaultPanelPlacement( 3200, 720, 420, 40, 60, 8, PanelSide::Left );
         placementOk = r.ok && r.x == 2560 - 420 - 8 && l.ok && l.x == 8 && l.y == 40 && l.width == 420
                    && l.height == 1440 - 100 && ml.ok && ml.x == 1920 + 8;

         auto note = []( RequestErrorKind k, const char* err, int status ) {
            AgentStep s;
            s.kind = AgentStep::Failed;
            s.error = err;
            s.errorKind = k;
            const TurnEndView v = DescribeTurnEnd( s, status );
            return v.notes.IsEmpty() ? String() : v.notes[0];
         };
         struct Case { RequestErrorKind kind; const char* err; int status; const char* expect; };
         const Case cases[] = {
            { RequestErrorKind::TimedOut, "request timed out after 600 s", 0,
              "The request took too long and was stopped (request timed out after 600 s). Send the message again, or ask for something smaller." },
            { RequestErrorKind::Stalled, "the reply stalled: no data from the API for 120 s", 0,
              "The reply stalled and was stopped (the reply stalled: no data from the API for 120 s). Send the message again." },
            { RequestErrorKind::Network, "network request failed: Could not resolve host", 0,
              "Could not reach the Anthropic API (network request failed: Could not resolve host). Check the internet connection, then send the message again." },
            { RequestErrorKind::Stream, "the reply stream failed: overloaded_error: Overloaded", 200,
              "The reply was cut off by the Anthropic API (the reply stream failed: overloaded_error: Overloaded). Send the message again." },
            { RequestErrorKind::Http, "invalid x-api-key", 401,
              "Anthropic API error 401: invalid x-api-key (check your API key in PI Copilot's settings)" },
            { RequestErrorKind::Http, "Overloaded", 529,
              "Anthropic API error 529: Overloaded (the service is busy; wait a moment, then send again)" },
            { RequestErrorKind::BadReply, "no text in reply (stop_reason=pause_turn)", 200,
              "Unexpected reply from the Anthropic API: no text in reply (stop_reason=pause_turn)" },
         };
         nlohmann::json w = nlohmann::json::array();
         for ( const Case& c : cases )
         {
            const String n = note( c.kind, c.err, c.status );
            const bool pass = n == c.expect && !n.Contains( "Error 0" );
            w.push_back( { { "note", U8( n ) }, { "pass", pass } } );
            wordingOk = wordingOk && pass;
         }
         detail["wording"] = w;

         AgentSession s;
         AnthropicMessage u;
         u.role = "user";
         u.content = "x";
         s.BeginUserTurn( u );
         AnthropicResult stalled;
         stalled.errorKind = RequestErrorKind::Stalled;
         stalled.error = "the reply stalled: no data from the API for 120 s";
         const AgentStep st = s.OnResponse( stalled, []( const ToolCall& ) { return ToolOutcome(); }, []() { return false; } );
         kindOk = st.kind == AgentStep::Failed && st.errorKind == RequestErrorKind::Stalled;

         const AnthropicResult ref = ParseMessagesResponse( 200, IsoString( "{\"content\":[],\"stop_reason\":\"refusal\"}" ), String() );
         detail["refusal"] = U8( ref.error );
         refusalOk = !ref.ok && ref.errorKind == RequestErrorKind::BadReply
                  && ref.error == "the model declined this request (stop_reason refusal); rephrase it, or choose "
                                  "another model in PI Copilot's settings";
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }

      const bool ok = settingsOk && placementOk && wordingOk && kindOk && refusalOk;
      out["configDetail"] = detail;
      out["configError"] = U8( error );
      out["configPolishOk"] = ok;
      allOk = allOk && ok;
   }
```

- [ ] **Step 2: Verify RED.** Run the Task 2 Step 3 build command.
Expected: `CopilotSettings.h: No such file or directory`.

- [ ] **Step 3: Implement the units.** In `PanelPlacement.h`, add before `struct PanelPlacement`:
```cpp
// Which edge of the primary screen the panel's default placement hugs.
enum class PanelSide { Right = 0, Left = 1 };
```
Rename the last parameter `rightMargin` to `sideMargin`, add `PanelSide side = PanelSide::Right`, and replace `p.x = int( cx + halfW - w - rightMargin );` with
```cpp
   p.x = (side == PanelSide::Left) ? int( cx - halfW + sideMargin )
                                   : int( cx + halfW - w - sideMargin );
```
Update the derivation comment's inequalities to name `sideMargin`, and add one line: "Left: `x == Cx - halfW + sideMargin`." (`maxW = 2*halfW - sideMargin` holds for both sides.)

`CopilotSettings.h`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_CopilotSettings_h
#define PICopilot_CopilotSettings_h

#include "PanelPlacement.h"

#include <pcl/String.h>

namespace pcl { namespace CopilotSettings {

// Persisted ⚙ settings (PixInsight local Settings, prefix PICopilot/).
IsoString LoadModel();                 // unknown or unset -> PICOPILOT_DEFAULT_MODEL
void      SaveModel( const IsoString& id );
bool      LoadRunPjsrEnabled();        // default false: run_pjsr is off until the user allows scripts
void      SaveRunPjsrEnabled( bool enabled );
PanelSide LoadPanelSide();             // default Right
void      SavePanelSide( PanelSide side );

} } // namespace pcl::CopilotSettings

#endif // PICopilot_CopilotSettings_h
```
`CopilotSettings.cpp`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "CopilotSettings.h"
#include "AnthropicClient.h"   // PICOPILOT_DEFAULT_MODEL
#include "ModelCatalog.h"
#include "Utf8.h"

#include <pcl/Settings.h>

namespace pcl { namespace CopilotSettings {

namespace
{
const char* const kModelKey   = "PICopilot/Model";
const char* const kRunPjsrKey = "PICopilot/RunPjsrEnabled";
const char* const kSideKey    = "PICopilot/PanelSide";
}

IsoString LoadModel()
{
   String s;
   Settings::Read( kModelKey, s );
   const IsoString id( U8( s ).c_str() );
   return FindModel( id ) != nullptr ? id : IsoString( PICOPILOT_DEFAULT_MODEL );
}

void SaveModel( const IsoString& id )
{
   Settings::Write( kModelKey, String( id ) );
}

bool LoadRunPjsrEnabled()
{
   bool enabled = false;
   Settings::Read( kRunPjsrKey, enabled );
   return enabled;
}

void SaveRunPjsrEnabled( bool enabled )
{
   Settings::Write( kRunPjsrKey, enabled );
}

PanelSide LoadPanelSide()
{
   int side = 0;
   Settings::Read( kSideKey, side );
   return side == 1 ? PanelSide::Left : PanelSide::Right;
}

void SavePanelSide( PanelSide side )
{
   Settings::Write( kSideKey, int( side ) );
}

} } // namespace pcl::CopilotSettings
```
In `AgentSession.h`, add to `AgentStep` after `String error;`:
```cpp
   RequestErrorKind errorKind = RequestErrorKind::None;   // Failed/Stopped from a request: why (TurnEndNotes words it)
```
In `AgentSession.h/.cpp`, change `Fail( AgentStep::Kind kind, const String& error )` to `Fail( AgentStep::Kind kind, const String& error, RequestErrorKind errorKind = RequestErrorKind::None )`, set `s.errorKind = errorKind;` in its body, and in `OnResponse` change `return Fail( r.cancelled ? AgentStep::Stopped : AgentStep::Failed, r.error );` to `return Fail( r.cancelled ? AgentStep::Stopped : AgentStep::Failed, r.error, r.errorKind );`.

In `TurnEndNotes.cpp`, add to an anonymous namespace:
```cpp
String FailureNote( const AgentStep& s, int httpStatus )
{
   switch ( s.errorKind )
   {
   case RequestErrorKind::TimedOut:
      return "The request took too long and was stopped (" + s.error + "). Send the message again, or ask for something smaller.";
   case RequestErrorKind::Stalled:
      return "The reply stalled and was stopped (" + s.error + "). Send the message again.";
   case RequestErrorKind::Network:
      return "Could not reach the Anthropic API (" + s.error + "). Check the internet connection, then send the message again.";
   case RequestErrorKind::Stream:
      return "The reply was cut off by the Anthropic API (" + s.error + "). Send the message again.";
   case RequestErrorKind::BadReply:
      return "Unexpected reply from the Anthropic API: " + s.error;
   case RequestErrorKind::Http:
      {
         String n = String( "Anthropic API error" ) + (httpStatus > 0 ? " " + String( httpStatus ) : String()) + ": " + s.error;
         if ( httpStatus == 401 )
            n += " (check your API key in PI Copilot's settings)";
         else if ( httpStatus == 429 || httpStatus == 529 )
            n += " (the service is busy; wait a moment, then send again)";
         return n;
      }
   default:   // not from a request (history invalid, internal): the increment-4 form
      return (httpStatus > 0 ? "Error " + String( httpStatus ) + ": " : String( "Error: " )) + s.error;
   }
}
```
Replace the `case AgentStep::Failed:` line that adds the note with `v.notes.Add( FailureNote( step, httpStatus ) );`.

In `AnthropicClient.cpp` `ParseMessagesResponse`, change the `else if ( !anyText )` branch to:
```cpp
               else if ( !anyText )
                  error = result.stopReason == "refusal"
                     ? String( "the model declined this request (stop_reason refusal); rephrase it, or choose "
                               "another model in PI Copilot's settings" )
                     // pause_turn, model_context_window_exceeded, a max_tokens cut inside a tool call, ...
                     : String::UTF8ToUTF16( ( "no text in reply (stop_reason=" + stopShown + ")" ).c_str() );
```
In `CMakeLists.txt`, add `CopilotSettings.cpp` after `Keyring.cpp`.

- [ ] **Step 4: Rewrite the ⚙ dialog.** `ConfigDialog.h`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ConfigDialog_h
#define PICopilot_ConfigDialog_h

#include "PanelPlacement.h"

#include <pcl/CheckBox.h>
#include <pcl/ComboBox.h>
#include <pcl/Dialog.h>
#include <pcl/Edit.h>
#include <pcl/Label.h>
#include <pcl/PushButton.h>
#include <pcl/Sizer.h>

namespace pcl
{

struct ConfigOutcome
{
   bool accepted = false;
   bool sideChanged = false;   // the caller re-applies the default placement
};

// PI Copilot settings: API key (masked; its storage shown), model, Allow
// scripts (run_pjsr; off by default), default panel side. On OK:
//  - the key is saved only if changed: emptied -> KeyStore::Clear(); any
//    character outside printable ASCII (0x21-0x7E; CR/LF would inject HTTP
//    header lines) -> error box, the dialog stays open, nothing is saved;
//    otherwise KeyStore::Save() (keyring, or Settings with a warning box);
//  - model, scripts and side are persisted (CopilotSettings).
// Cancel saves nothing.
class ConfigDialog : public Dialog
{
public:

   ConfigDialog();

   ConfigOutcome Run();

private:

   VerticalSizer   Global_Sizer;
   Label           ApiKey_Label;
   Edit            ApiKey_Edit;
   Label           KeyWhere_Label;
   HorizontalSizer Model_Sizer;
   Label           Model_Label;
   ComboBox        Model_ComboBox;
   CheckBox        RunPjsr_CheckBox;
   Label           RunPjsrInfo_Label;
   HorizontalSizer Side_Sizer;
   Label           Side_Label;
   ComboBox        Side_ComboBox;
   HorizontalSizer Buttons_Sizer;
   PushButton      OK_PushButton;
   PushButton      Cancel_PushButton;

   String        m_initialKey;
   PanelSide     m_initialSide = PanelSide::Right;
   ConfigOutcome m_outcome;

   void OK_Button_Click( Button& sender, bool checked );
   void Cancel_Button_Click( Button& sender, bool checked );
};

} // namespace pcl

#endif // PICopilot_ConfigDialog_h
```
`ConfigDialog.cpp`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ConfigDialog.h"
#include "CopilotSettings.h"
#include "KeyStore.h"
#include "ModelCatalog.h"

#include <pcl/MessageBox.h>

namespace pcl
{

namespace
{

// Anthropic keys are printable ASCII. Anything else -- in particular CR/LF,
// which would split the x-api-key header line -- is rejected.
bool IsValidApiKey( const String& key )
{
   for ( String::const_iterator i = key.Begin(); i != key.End(); ++i )
      if ( *i < 0x21 || *i > 0x7E )
         return false;
   return true;
}

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
      else
         out += c;
   }
   return out;
}

void Warn( const String& text )
{
   MessageBox( "<p>" + EscapeHtml( text ) + "</p>", "PI Copilot", StdIcon::Warning, StdButton::Ok ).Execute();
}

} // namespace

ConfigDialog::ConfigDialog()
{
   ApiKey_Label.SetText( "Anthropic API key:" );
   ApiKey_Edit.EnablePasswordMode();
   ApiKey_Edit.SetMinWidth( 400 );

   Model_Label.SetText( "Model:" );
   for ( const ModelInfo& m : kPICopilotModels )
      Model_ComboBox.AddItem( m.label );
   Model_ComboBox.SetToolTip( "<p>The Claude model PI Copilot uses from your next message on.</p>" );
   Model_Sizer.SetSpacing( 6 );
   Model_Sizer.Add( Model_Label );
   Model_Sizer.Add( Model_ComboBox, 100 );

   RunPjsr_CheckBox.SetText( "Allow scripts (run_pjsr)" );
   RunPjsrInfo_Label.EnableWordWrapping();
   RunPjsrInfo_Label.SetText( "When allowed, PI Copilot may propose JavaScript to run inside PixInsight for jobs no process "
                              "can do. Every script is shown to you in full and runs only if you click Run script. "
                              "Scripts have full access to PixInsight and your files, and cannot be interrupted once running." );

   Side_Label.SetText( "Default side:" );
   Side_ComboBox.AddItem( "Right" );
   Side_ComboBox.AddItem( "Left" );
   Side_ComboBox.SetToolTip( "<p>Changing it moves the panel to that edge of the primary screen now; after that, "
                             "PixInsight remembers where you put it.</p>" );
   Side_Sizer.SetSpacing( 6 );
   Side_Sizer.Add( Side_Label );
   Side_Sizer.Add( Side_ComboBox, 100 );

   OK_PushButton.SetText( "OK" );
   OK_PushButton.SetDefault();
   OK_PushButton.OnClick( (Button::click_event_handler)&ConfigDialog::OK_Button_Click, *this );
   Cancel_PushButton.SetText( "Cancel" );
   Cancel_PushButton.OnClick( (Button::click_event_handler)&ConfigDialog::Cancel_Button_Click, *this );
   Buttons_Sizer.SetSpacing( 8 );
   Buttons_Sizer.AddStretch();
   Buttons_Sizer.Add( OK_PushButton );
   Buttons_Sizer.Add( Cancel_PushButton );

   Global_Sizer.SetMargin( 8 );
   Global_Sizer.SetSpacing( 6 );
   Global_Sizer.Add( ApiKey_Label );
   Global_Sizer.Add( ApiKey_Edit );
   Global_Sizer.Add( KeyWhere_Label );
   Global_Sizer.AddSpacing( 6 );
   Global_Sizer.Add( Model_Sizer );
   Global_Sizer.AddSpacing( 6 );
   Global_Sizer.Add( RunPjsr_CheckBox );
   Global_Sizer.Add( RunPjsrInfo_Label );
   Global_Sizer.AddSpacing( 6 );
   Global_Sizer.Add( Side_Sizer );
   Global_Sizer.AddSpacing( 8 );
   Global_Sizer.Add( Buttons_Sizer );

   SetWindowTitle( String::UTF8ToUTF16( "PI Copilot \xE2\x80\x94 Settings" ) );
   SetSizer( Global_Sizer );
   AdjustToContents();
   SetFixedSize();
}

ConfigOutcome ConfigDialog::Run()
{
   const KeyStore::State ks = KeyStore::Load();
   m_initialKey = ks.key;
   ApiKey_Edit.SetText( ks.key );
   KeyWhere_Label.SetText( "Key: " + KeyStore::DescribeWhere( ks ) + (ks.note.IsEmpty() ? String() : ". " + ks.note) );
   KeyWhere_Label.EnableWordWrapping();
   const int mi = ModelIndex( CopilotSettings::LoadModel() );
   Model_ComboBox.SetCurrentItem( mi < 0 ? 0 : mi );
   RunPjsr_CheckBox.SetChecked( CopilotSettings::LoadRunPjsrEnabled() );
   m_initialSide = CopilotSettings::LoadPanelSide();
   Side_ComboBox.SetCurrentItem( int( m_initialSide ) );
   m_outcome = ConfigOutcome();
   Execute();
   return m_outcome;
}

void ConfigDialog::OK_Button_Click( Button&, bool )
{
   const String key = ApiKey_Edit.Text().Trimmed();
   if ( !key.IsEmpty() && !IsValidApiKey( key ) )
   {
      // Keep the dialog open so the user can correct the paste.
      MessageBox( "<p>The API key contains spaces, line breaks or other invalid characters.</p>"
                  "<p>Paste the key exactly as shown in the Anthropic Console. Nothing was saved.</p>",
                  "PI Copilot", StdIcon::Error, StdButton::Ok ).Execute();
      return;
   }
   if ( key != m_initialKey )
   {
      if ( key.IsEmpty() )
      {
         const String why = KeyStore::Clear();
         if ( !why.IsEmpty() )
            Warn( why );
      }
      else
      {
         const KeyStore::State st = KeyStore::Save( key );
         if ( st.where == KeyStore::Where::Settings )
            Warn( st.note );
      }
   }
   const int mi = Model_ComboBox.CurrentItem();
   if ( mi >= 0 && size_type( mi ) < PICopilotModelCount )
      CopilotSettings::SaveModel( kPICopilotModels[mi].id );
   CopilotSettings::SaveRunPjsrEnabled( RunPjsr_CheckBox.IsChecked() );
   const PanelSide side = Side_ComboBox.CurrentItem() == 1 ? PanelSide::Left : PanelSide::Right;
   CopilotSettings::SavePanelSide( side );
   m_outcome.accepted = true;
   m_outcome.sideChanged = side != m_initialSide;
   Ok();
}

void ConfigDialog::Cancel_Button_Click( Button&, bool )
{
   m_outcome = ConfigOutcome();
   Cancel();
}

} // namespace pcl
```

- [ ] **Step 5: Panel wiring.** In `PICopilotInterface.h`, add `#include "CopilotSettings.h"`, change `void SetBusy( bool busy );` to `void SetBusy( bool busy, const String& caption = String() );`, and add the private members `IsoString m_turnModel;` and `IsoString m_lastModel;`. In `PICopilotInterface.cpp`:
  - Add the includes `"ModelCatalog.h"` and `"PICopilotModule.h"`, and next to `kBusyTextUtf8` add `const char* const kCapturingTextUtf8 = "Capturing view\xE2\x80\xA6";`.
  - In `SetBusy`, replace the first `SetText` with
```cpp
   GUI->Send_Button.SetText( busy ? (caption.IsEmpty() ? String::UTF8ToUTF16( kBusyTextUtf8 ) : caption)
                                  : String( kSendText ) );
```
  - In `SendCurrentInput()`, replace the block from `BeginTurnTarget();` through `SetBusy( true );` with:
```cpp
   BeginTurnTarget();
   if ( GUI->IncludeView_CheckBox.IsChecked() && !m_turnViewId.IsEmpty() )
   {
      // Capturing a large view takes a moment: say so, and paint it before
      // the capture blocks the UI thread (user input stays excluded).
      SetBusy( true, String::UTF8ToUTF16( kCapturingTextUtf8 ) );
      ThePICopilotModule->ProcessEvents( true/*excludeUserInputEvents*/ );
   }
   m_session.BeginUserTurn( ComposeTurnWithActiveView( prompt ) );
   NoteTrimmed();
   m_pendingPrompt = prompt;
   m_apiKey = key;
   m_turnMode = AgentModeFromIndex( GUI->Mode_ComboBox.CurrentItem() );
   m_turnModel = CopilotSettings::LoadModel();
   if ( m_turnModel != m_lastModel )
   {
      const ModelInfo* info = FindModel( m_turnModel );
      AppendToLog( PlainText( "(model: " + String( info != nullptr ? info->label : m_turnModel.c_str() ) + ")" ) + "\n\n" );
      m_lastModel = m_turnModel;
   }
   m_stopRequested = false;
   GUI->ChatInput.Clear();
   SetBusy( true );
```
  - In `StartRequest()`, pass `m_turnModel` instead of `PICOPILOT_DEFAULT_MODEL` as the model, and `ProductionRequestShape( m_turnModel )` as the shape.
  - In `ApplyDefaultPlacement()`, pass `CopilotSettings::LoadPanelSide()` as the new last argument of `ComputeDefaultPanelPlacement`. Update the two warning texts from "right-side placement" to "side placement".
  - Replace `e_Config_Click`'s body with:
```cpp
   ConfigDialog d;
   const ConfigOutcome o = d.Run();
   if ( o.accepted && o.sideChanged )
      ApplyDefaultPlacement();   // move to the newly chosen edge now; PI's geometry auto-save keeps it
```
  - Change the gear tooltip to `"<p>Settings: API key, model, scripts (run_pjsr), default panel side.</p>"`.
  - In `e_Clear_Click`, add `m_lastModel.Clear();` so the model is named again in the new chat.

- [ ] **Step 6: Verify GREEN.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) && bash test/run-selftest.sh`
Expected: `PASS: self-test verdict all green`, including the unchanged increment-3 `placementOk` cases (right side is the default) and increment-4 `turnEndNotesOk` (the default wording branch is unchanged).

- [ ] **Step 7: Commit.**
```bash
cd /home/scarter4work/projects/astro-pi
git add modules/pi-copilot/src/module/CopilotSettings.h modules/pi-copilot/src/module/CopilotSettings.cpp \
        modules/pi-copilot/src/module/PanelPlacement.h modules/pi-copilot/src/module/ConfigDialog.h \
        modules/pi-copilot/src/module/ConfigDialog.cpp modules/pi-copilot/src/module/AgentSession.h \
        modules/pi-copilot/src/module/AgentSession.cpp modules/pi-copilot/src/module/TurnEndNotes.cpp \
        modules/pi-copilot/src/module/AnthropicClient.cpp modules/pi-copilot/src/module/PICopilotInterface.h \
        modules/pi-copilot/src/module/PICopilotInterface.cpp modules/pi-copilot/src/module/PICopilotInc5SelfTest.cpp \
        modules/pi-copilot/src/module/CMakeLists.txt modules/pi-copilot/test/run-selftest.sh
git commit -m "feat(pi-copilot): settings dialog (model, Allow scripts, panel side), Capturing view caption, failure wording by cause

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

## Task 7: Process safety policy — deny/confirm list with a coverage gate (the deferred deny-list review)

This task comes before global runs and `run_pjsr`. It classifies every installed process whose parameters suggest effects **outside the image**: writing, moving or deleting files, output directories, overwrites, closing windows, caches, or running scripts or commands. The policy is compiled in. `deny` entries are never run. `confirmAlways`/`confirmWhen` ask the user in **every** mode, Copilot included. A self-test enumerates `Process::AllProcesses()` with a parameter-id heuristic and **fails if anything it flags is unclassified**, so a PixInsight update that adds such a process cannot slip through silently. `apply_process` uses the policy now, and `run_global_process` will use it in Task 8. The final list is documented in the README.

**Files:**
- Create: `modules/pi-copilot/data/process-safety.json`
- Create: `modules/pi-copilot/src/module/ProcessSafetyData.h.in`
- Create: `modules/pi-copilot/src/module/ProcessSafety.h`, `ProcessSafety.cpp`
- Modify: `modules/pi-copilot/src/module/CMakeLists.txt` (source + `configure_file`)
- Modify: `modules/pi-copilot/src/module/AgentTools.cpp` (`ApplyProcessTool` safety gate)
- Modify: `modules/pi-copilot/src/module/PICopilotInc5SelfTest.cpp` (Section B6)
- Modify: `modules/pi-copilot/test/run-selftest.sh`, `modules/pi-copilot/README.md`

**Interfaces:**
- Consumes: `EnumerationInfoOf()`, `ExecuteTool()`, `ToolContext::confirm`, `DescribeParameterChanges()`.
- Produces:
```cpp
struct SafetyVerdict { enum Kind { Allow, Confirm, Deny }; Kind kind = Allow; String reason; };
const nlohmann::json& CompiledProcessSafety();
SafetyVerdict CheckProcessSafety( const IsoString& processId, const nlohmann::json& parameters,
                                  const nlohmann::json& tableParameters );              // root thread
nlohmann::json UnclassifiedSideEffectCandidates();   // [{process, parameters[], canProcessViews, canProcessGlobal}]
nlohmann::json UnknownPolicyProcessIds();            // policy ids not installed (typos)
void SetProcessSafetyPolicyForSelfTest( const nlohmann::json* policy );   // nullptr = compiled policy
```

- [ ] **Step 1: Policy seed.** Create `data/process-safety.json`:
```json
{
  "_doc": "PI Copilot process safety policy. deny: never run from PI Copilot. confirmAlways: ask the user before every run, in every mode. confirmWhen: ask when the effective value of `parameter` (as given, else the process default) equals `equals`. reviewedSafe: flagged by the file/disk/window heuristic but reviewed: no effect beyond the target image or new windows. fileTables: path rules for global runs (Task 8).",
  "deny": {
    "ProcessContainer": "it runs a list of other processes that PI Copilot cannot check one by one",
    "PICopilot": "it is PI Copilot itself (running it from the chat would recurse)"
  },
  "confirmAlways": {
    "ImageCalibration": "it writes calibrated copies of the input frames to its output directory and can overwrite existing files",
    "Debayer": "it writes debayered copies of the input frames to its output directory",
    "CosmeticCorrection": "it writes corrected copies of the input frames to its output directory",
    "StarAlignment": "in the global context it writes registered copies of the input frames to its output directory",
    "LocalNormalization": "it writes normalization data files (.xnml) to disk",
    "SubframeSelector": "it can copy or move approved and rejected frames to output directories",
    "DrizzleIntegration": "it reads drizzle data files from disk and can write output files"
  },
  "confirmWhen": {
    "ImageIntegration": [
      { "parameter": "generateDrizzleData", "equals": true, "reason": "it updates the frames' .xdrz drizzle files on disk" },
      { "parameter": "closePreviousImages", "equals": true, "reason": "it closes the images created by a previous ImageIntegration run, and unsaved results are lost" }
    ]
  },
  "reviewedSafe": {},
  "fileTables": {}
}
```
`ProcessSafetyData.h.in`:
```cpp
// GENERATED at CMake configure time from modules/pi-copilot/data/process-safety.json.
// Do not edit the generated copy; edit the JSON and re-run CMake.
#ifndef PICopilot_ProcessSafetyData_h
#define PICopilot_ProcessSafetyData_h

namespace pcl
{
static const char kProcessSafetyJson[] = R"PCSAFETY(@PICOPILOT_PROCESS_SAFETY_JSON@)PCSAFETY";
} // namespace pcl

#endif
```
In `CMakeLists.txt`, after the `configure_file(ProcessSummaries.h.in …)` line, add:
```cmake
set(PICOPILOT_SAFETY_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../data/process-safety.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PICOPILOT_SAFETY_SRC}")
file(READ "${PICOPILOT_SAFETY_SRC}" PICOPILOT_PROCESS_SAFETY_JSON)
configure_file(ProcessSafetyData.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/ProcessSafetyData.h" @ONLY)
```
and add `ProcessSafety.cpp` to `MODULE_SOURCES` after `CopilotSettings.cpp`.

- [ ] **Step 2: Failing test (Section B6).** Add `'processSafetyOk',` after `'configPolishOk',` in `required_true`. In `PICopilotInc5SelfTest.cpp`, add `#include "ProcessSafety.h"` and insert above the marker:
```cpp
   // ---- Section B6: process safety policy (Task 7) -----------------------------
   {
      bool coverageOk = false, idsOk = false, verdictOk = false, denyToolOk = false, confirmToolOk = false;
      nlohmann::json detail = nlohmann::json::object();
      String error;
      try
      {
         const nlohmann::json unclassified = UnclassifiedSideEffectCandidates();
         const nlohmann::json unknown = UnknownPolicyProcessIds();
         detail["unclassified"] = unclassified;   // the review worklist (Step 5)
         detail["unknownPolicyIds"] = unknown;
         coverageOk = unclassified.is_array() && unclassified.empty();
         idsOk = unknown.is_array() && unknown.empty();

         const SafetyVerdict pc = CheckProcessSafety( "ProcessContainer", nlohmann::json::object(), nlohmann::json() );
         const SafetyVerdict iiPlain = CheckProcessSafety( "ImageIntegration",
            { { "generateDrizzleData", false }, { "closePreviousImages", false } }, nlohmann::json() );
         const SafetyVerdict iiDrz = CheckProcessSafety( "ImageIntegration", { { "generateDrizzleData", true } }, nlohmann::json() );
         const SafetyVerdict pm = CheckProcessSafety( "PixelMath", { { "expression", "$T" } }, nlohmann::json() );
         verdictOk = pc.kind == SafetyVerdict::Deny && pc.reason.Contains( "cannot check" )
                  && CheckProcessSafety( "PICopilot", nlohmann::json::object(), nlohmann::json() ).kind == SafetyVerdict::Deny
                  && iiPlain.kind == SafetyVerdict::Allow
                  && iiDrz.kind == SafetyVerdict::Confirm && iiDrz.reason.Contains( ".xdrz" )
                  && pm.kind == SafetyVerdict::Allow;

         Inc5TestWindow tw( "PCSafetyT", 64, 48, 1, 0.4 );
         View v = tw.MainView();
         int confirmCalls = 0;
         bool answer = false;
         String lastChanges;
         ToolContext ctx;
         ctx.mode = AgentMode::Copilot;
         ctx.turnViewId = v.FullId();
         ctx.confirm = [&]( const String&, const String&, const String& changes ) { ++confirmCalls; lastChanges = changes; return answer; };

         const ToolOutcome denied = ExecuteTool( ToolCall{ "s1", "apply_process", { { "process_id", "ProcessContainer" } } }, ctx );
         detail["deny"] = denied.content;
         denyToolOk = denied.isError && confirmCalls == 0
                   && denied.content.at( 0 ).at( "text" ).get<std::string>().find( "is not allowed from PI Copilot" ) != std::string::npos;

         // A confirm rule, in Copilot: asked once; No -> unchanged; Yes -> applied.
         const nlohmann::json testPolicy = nlohmann::json::parse(
            "{\"deny\":{},\"confirmAlways\":{},\"reviewedSafe\":{},\"fileTables\":{},\"confirmWhen\":{\"PixelMath\":"
            "[{\"parameter\":\"expression\",\"equals\":\"$T*0.5\",\"reason\":\"self-test confirm rule\"}]}}" );
         SetProcessSafetyPolicyForSelfTest( &testPolicy );
         const double before = Inc5Median( v, 0 );
         const ToolCall halve{ "s2", "apply_process", { { "process_id", "PixelMath" }, { "parameters", { { "expression", "$T*0.5" } } } } };
         answer = false;
         const ToolOutcome no = ExecuteTool( halve, ctx );
         const double afterNo = Inc5Median( v, 0 );
         const int callsNo = confirmCalls;
         const String changesNo = lastChanges;
         answer = true;
         const ToolOutcome yes = ExecuteTool( halve, ctx );
         const double afterYes = Inc5Median( v, 0 );
         ctx.mode = AgentMode::Guided;   // Guided + a confirm rule: still ONE dialog
         const ToolOutcome guided = ExecuteTool( halve, ctx );
         SetProcessSafetyPolicyForSelfTest( nullptr );
         detail["confirm"] = { { "callsNo", callsNo }, { "calls", confirmCalls }, { "changes", U8( changesNo ) },
                               { "before", before }, { "afterNo", afterNo }, { "afterYes", afterYes } };
         confirmToolOk = no.isError && callsNo == 1 && changesNo.Contains( "self-test confirm rule" )
                      && std::fabs( afterNo - before ) < 1e-6
                      && !yes.isError && std::fabs( afterYes - 0.5*before ) < 1e-5
                      && !guided.isError && confirmCalls == 3;
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      SetProcessSafetyPolicyForSelfTest( nullptr );

      const bool ok = coverageOk && idsOk && verdictOk && denyToolOk && confirmToolOk;
      out["processSafetyDetail"] = detail;
      out["processSafetyError"] = U8( error );
      out["processSafetyOk"] = ok;
      allOk = allOk && ok;
   }
```

- [ ] **Step 3: Verify RED.** Run the Task 2 Step 3 build command.
Expected: `ProcessSafety.h: No such file or directory`.

- [ ] **Step 4: Implement.** `ProcessSafety.h`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ProcessSafety_h
#define PICopilot_ProcessSafety_h

#include <pcl/String.h>

#include <nlohmann/json.hpp>

namespace pcl
{

// What PI Copilot may do with one process run (data/process-safety.json).
struct SafetyVerdict
{
   enum Kind { Allow, Confirm, Deny };
   Kind   kind = Allow;
   String reason;   // Confirm/Deny: plain words, shown to the user and the model
};

// The compiled policy (or the self-test override).
const nlohmann::json& CompiledProcessSafety();

// deny -> Deny; confirmAlways -> Confirm; confirmWhen -> Confirm when a
// rule's parameter has the rule's value -- the value given in `parameters`,
// else the process default (enumerations compare by element id, also when the
// model passed the element's integer value); several matching rules join
// their reasons with "; ". An unknown process id is Allow here (ApplyProcess /
// RunGlobalProcess then fail with their precise "unknown process" error).
// Root thread only. Never throws.
SafetyVerdict CheckProcessSafety( const IsoString& processId, const nlohmann::json& parameters,
                                  const nlohmann::json& tableParameters );

// Installed processes with a parameter (or table column) id matching the
// side-effect heuristic (output|directory|dir$|file|path|overwrite|write|save|
// log|cache|delete|remove|close|script|command|url|exec, case-insensitive) that
// are in none of deny / confirmAlways / confirmWhen / reviewedSafe.
// [{process, parameters, canProcessViews, canProcessGlobal}]. Root thread.
nlohmann::json UnclassifiedSideEffectCandidates();

// Policy ids that are not installed processes (catches typos).
nlohmann::json UnknownPolicyProcessIds();

// Self-test only: evaluate against `policy` instead (nullptr restores).
void SetProcessSafetyPolicyForSelfTest( const nlohmann::json* policy );

} // namespace pcl

#endif // PICopilot_ProcessSafety_h
```
`ProcessSafety.cpp`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ProcessSafety.h"
#include "ProcessCatalog.h"
#include "ProcessSafetyData.h"
#include "Utf8.h"

#include <pcl/Exception.h>
#include <pcl/Process.h>
#include <pcl/ProcessInstance.h>
#include <pcl/ProcessParameter.h>
#include <pcl/StringList.h>
#include <pcl/Variant.h>

#include <regex>
#include <string>

namespace pcl
{

namespace
{

const nlohmann::json* g_override = nullptr;

const char* const kSections[] = { "deny", "confirmAlways", "confirmWhen", "reviewedSafe" };

String S16( const std::string& s )
{
   return String::UTF8ToUTF16( s.c_str() );
}

const nlohmann::json& Section( const nlohmann::json& policy, const char* name )
{
   static const nlohmann::json empty = nlohmann::json::object();
   const auto it = policy.find( name );
   return (it != policy.end() && it->is_object()) ? *it : empty;
}

// The value a run will use: as given, else the process default. Throws for an unknown parameter.
nlohmann::json EffectiveValue( const Process& P, const std::string& param, const nlohmann::json& parameters )
{
   if ( parameters.is_object() && parameters.contains( param ) )
      return parameters[param];
   const ProcessParameter p( P, IsoString( param.c_str() ) );
   ProcessInstance d( P );
   const Variant v = d.ParameterValue( p, 0 );   // row 0, never ~0 (inc-4 Task 1 modal)
   if ( p.IsBoolean() )
      return v.ToBoolean();
   if ( p.IsEnumeration() )
   {
      const int x = v.ToInt();
      for ( const ProcessParameter::EnumerationElement& e : EnumerationInfoOf( p ).elements )
         if ( e.value == x )
            return std::string( e.id.c_str() );
      return x;
   }
   if ( p.IsNumeric() )
      return v.ToDouble();
   if ( p.IsString() )
      return U8( v.ToString() );
   return nullptr;
}

bool Matches( const Process& P, const std::string& param, const nlohmann::json& got, const nlohmann::json& equals )
{
   if ( got == equals )
      return true;
   if ( got.is_number() && equals.is_number() )
      return got.get<double>() == equals.get<double>();
   if ( equals.is_string() && got.is_number_integer() )   // an enumeration passed as its value
   {
      const ProcessParameter p( P, IsoString( param.c_str() ) );
      if ( p.IsEnumeration() )
         for ( const ProcessParameter::EnumerationElement& e : EnumerationInfoOf( p ).elements )
            if ( e.value == got.get<int>() )
               return equals.get<std::string>() == e.id.c_str();
   }
   return false;
}

} // namespace

const nlohmann::json& CompiledProcessSafety()
{
   static const nlohmann::json policy = nlohmann::json::parse( kProcessSafetyJson );
   return g_override != nullptr ? *g_override : policy;
}

void SetProcessSafetyPolicyForSelfTest( const nlohmann::json* policy )
{
   g_override = policy;
}

SafetyVerdict CheckProcessSafety( const IsoString& processId, const nlohmann::json& parameters,
                                  const nlohmann::json& /*tableParameters*/ )
{
   SafetyVerdict v;
   try
   {
      const Process P( processId );
      const std::string id( P.Id().c_str() );
      const nlohmann::json& policy = CompiledProcessSafety();
      const nlohmann::json& deny = Section( policy, "deny" );
      if ( deny.contains( id ) )
      {
         v.kind = SafetyVerdict::Deny;
         v.reason = S16( deny[id].get<std::string>() );
         return v;
      }
      const nlohmann::json& always = Section( policy, "confirmAlways" );
      if ( always.contains( id ) )
      {
         v.kind = SafetyVerdict::Confirm;
         v.reason = S16( always[id].get<std::string>() );
         return v;
      }
      const nlohmann::json& when = Section( policy, "confirmWhen" );
      if ( when.contains( id ) && when[id].is_array() )
      {
         StringList reasons;
         for ( const nlohmann::json& rule : when[id] )
         {
            const std::string param = rule.value( "parameter", std::string() );
            nlohmann::json got;
            try
            {
               got = EffectiveValue( P, param, parameters );
            }
            catch ( ... )
            {
               continue;   // unknown parameter: ApplyProcess reports it precisely
            }
            if ( Matches( P, param, got, rule.value( "equals", nlohmann::json() ) ) )
               reasons << S16( rule.value( "reason", std::string( "a parameter with side effects is set" ) ) );
         }
         if ( !reasons.IsEmpty() )
         {
            v.kind = SafetyVerdict::Confirm;
            for ( size_type i = 0; i < reasons.Length(); ++i )
               v.reason += (i > 0 ? String( "; " ) : String()) + reasons[i];
         }
      }
   }
   catch ( ... )
   {
      // Unknown process id: Allow here; the executor's own error names it.
   }
   return v;
}

nlohmann::json UnclassifiedSideEffectCandidates()
{
   const std::regex re( "output|directory|dir$|file|path|overwrite|write|save|log|cache|delete|remove|close|script|command|url|exec",
                        std::regex::icase );
   const nlohmann::json& policy = CompiledProcessSafety();
   nlohmann::json out = nlohmann::json::array();
   for ( const Process& P : Process::AllProcesses() )
   {
      const std::string id( P.Id().c_str() );
      bool classified = false;
      for ( const char* s : kSections )
         classified = classified || Section( policy, s ).contains( id );
      if ( classified )
         continue;
      nlohmann::json hits = nlohmann::json::array();
      for ( const ProcessParameter& p : P.Parameters() )
      {
         const std::string pid( p.Id().c_str() );
         if ( std::regex_search( pid, re ) )
            hits.push_back( pid );
         if ( p.IsTable() )
            for ( const ProcessParameter& c : p.TableColumns() )
               if ( std::regex_search( std::string( c.Id().c_str() ), re ) )
                  hits.push_back( pid + "." + c.Id().c_str() );
      }
      if ( !hits.empty() )
         out.push_back( { { "process", id }, { "parameters", hits },
                          { "canProcessViews", P.CanProcessViews() }, { "canProcessGlobal", P.CanProcessGlobal() } } );
   }
   return out;
}

nlohmann::json UnknownPolicyProcessIds()
{
   nlohmann::json out = nlohmann::json::array();
   const nlohmann::json& policy = CompiledProcessSafety();
   for ( const char* s : { "deny", "confirmAlways", "confirmWhen", "reviewedSafe", "fileTables" } )
      for ( auto it = Section( policy, s ).begin(); it != Section( policy, s ).end(); ++it )
         try
         {
            const Process P( IsoString( it.key().c_str() ) );
         }
         catch ( ... )
         {
            out.push_back( std::string( s ) + ":" + it.key() );
         }
   return out;
}

} // namespace pcl
```
In `AgentTools.cpp`, add `#include "ProcessSafety.h"`. In `ApplyProcessTool`, right after `what += " on " + String( targetId );`, replace the whole `if ( ctx.mode == AgentMode::Guided ) { … }` block with:
```cpp
   const SafetyVerdict safety = CheckProcessSafety( IsoString( pid.c_str() ), params, tables );
   if ( safety.kind == SafetyVerdict::Deny )
      return Fail( what, S16( pid ) + " is not allowed from PI Copilot: " + safety.reason
                         + ". Give the user the settings so they can run it themselves." );
   // Guided asks before every run; a confirm rule asks in EVERY mode (one dialog either way).
   if ( ctx.mode == AgentMode::Guided || safety.kind == SafetyVerdict::Confirm )
   {
      if ( !ctx.confirm )
         return Fail( what, "internal error: no confirmation callback" );
      String changes = DescribeParameterChanges( params, tables, PICopilotConfirmChangesChars );
      if ( safety.kind == SafetyVerdict::Confirm )
         changes = "Why you are asked: " + safety.reason + ".\n\n" + changes;
      if ( !ctx.confirm( S16( pid ), String( targetId ), changes ) )
      {
         ToolOutcome o;
         o.isError = true;
         o.content.push_back( TextBlock( "The user declined this apply_process call (" + pid + " on "
                                         + std::string( targetId.c_str() )
                                         + "). Nothing was changed. Do not repeat it; ask what they would like instead." ) );
         o.logLine = S16( kErrMarkUtf8 ) + what + S16( kArrowUtf8 ) + "declined by user";
         return o;
      }
      // The dialog pumps events: the user may have closed (or replaced) the
      // image meanwhile. Never run on a stale handle.
      target = ViewByFullId( targetId );
      if ( target.IsNull() )
         return Fail( what, "view " + String( targetId ) + " is no longer open (closed while the confirmation "
                            "dialog was up); nothing was changed" );
   }
```
The inc-4 A3 Guided test uses a context with `confirm` set, so it is unaffected.

- [ ] **Step 5: Run the coverage gate, review every flagged process, classify it.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) && bash test/run-selftest.sh | grep -o '"processSafetyDetail":{.*}' | head -c 20000`
Expected: `FAILED keys: processSafetyOk`. `processSafetyDetail.unclassified` lists every flagged process with its matching parameter ids, and `unknownPolicyIds` lists any seed id not installed on this PI.

Then, for **each** `unclassified` entry:
1. Look at its parameters. Use a scratch headless run of `DescribeProcess("<id>")`, or `tools/pcl-query.sh pjsr_class_info '{"className":"<id>"}'` where available.
2. Put it in exactly one section of `data/process-safety.json`, with a one-line reason a user understands:
   - `deny`: it can do something PI Copilot must never trigger (arbitrary nested processes, running commands).
   - `confirmAlways`: it writes, moves or deletes files, or closes windows, in any normal use.
   - `confirmWhen`: only a specific parameter value has that effect. Give `parameter`, `equals` and `reason`.
   - `reviewedSafe`: the flagged ids are view ids, display flags, log-scale options, in-memory caches, or similar. Give the reason (e.g. `"'outputData' is an in-memory result table"`).
3. Remove any `unknownPolicyIds` seed that is not installed. Note the removal in the task report, and never invent ids.

Re-run until `unclassified` and `unknownPolicyIds` are both `[]` and the verdict is `PASS: self-test verdict all green`. Report the counts per section.

- [ ] **Step 6: Document the policy.** Run:
```bash
cd /home/scarter4work/projects/astro-pi && python3 - <<'PY'
import json
p = json.load(open('modules/pi-copilot/data/process-safety.json'))
print('| Process | PI Copilot will | Why |')
print('|---|---|---|')
for k, v in sorted(p['deny'].items()):
    print('| %s | never run it | %s |' % (k, v))
for k, v in sorted(p['confirmAlways'].items()):
    print('| %s | always ask you first | %s |' % (k, v))
for k, rules in sorted(p['confirmWhen'].items()):
    for r in rules:
        print('| %s | ask you first when `%s` is `%s` | %s |' % (k, r['parameter'], json.dumps(r['equals']), r['reason']))
print()
print('Reviewed and allowed without asking (their file/path-like parameters have no effect beyond the image or new windows): '
      + ', '.join(sorted(p['reviewedSafe'])) + '.')
PY
```
Paste its output into `modules/pi-copilot/README.md` under a new heading `## Process safety policy`, placed after the `## Increment 4` section. Start with this sentence: `PI Copilot checks every process it runs (apply_process, run_global_process) against a compiled policy (data/process-safety.json). The self-test fails if an installed process with file, directory, overwrite or window-closing parameters is not classified.`

- [ ] **Step 7: Commit.**
```bash
cd /home/scarter4work/projects/astro-pi
git add modules/pi-copilot/data/process-safety.json modules/pi-copilot/src/module/ProcessSafetyData.h.in \
        modules/pi-copilot/src/module/ProcessSafety.h modules/pi-copilot/src/module/ProcessSafety.cpp \
        modules/pi-copilot/src/module/CMakeLists.txt modules/pi-copilot/src/module/AgentTools.cpp \
        modules/pi-copilot/src/module/PICopilotInc5SelfTest.cpp modules/pi-copilot/test/run-selftest.sh \
        modules/pi-copilot/README.md
git commit -m "feat(pi-copilot): process safety policy (deny / confirm in every mode) with an installed-process coverage gate; README list

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

## Task 8: Global processes — `run_global_process` (ImageIntegration over files on disk)

The model can now run a process in the global context. The flow is:
1. Parameters are checked exactly as for `apply_process` (the shared `SetParameters()`).
2. File paths are validated first: declared file-table columns must be absolute, existing, readable files that an installed format can read, ImageIntegration needs ≥ 3 enabled frames, and any other absolute path in the parameters must exist.
3. The safety policy is applied (Task 7).
4. `Validate` → `CanExecuteGlobal` → `ExecuteGlobal`.
5. The created windows are found by a before/after diff of open windows, plus the process's own read-only `…ImageId` outputs. They go back to the model with statistics and a preview of the main result.

The tool is offered only in Copilot/Guided. Guided always asks. Copilot asks only on a policy rule (Ruling 1).

Two platform facts from increment 4 shape this task:
- ImageIntegration reports `CanProcessViews() == true` in PI 1.9.5. `apply_process` therefore now redirects every process with a declared file table to `run_global_process` with a precise message.
- The only global-only processes are `MARSGen`, `NukeX` and `PICopilot`. `PICopilot` itself is denied by the policy (Task 7).

**Files:**
- Modify: `modules/pi-copilot/src/module/ProcessApply.h`, `ProcessApply.cpp` (`SetParameters()` extracted; `PrecheckGlobalRun()`, `RunGlobalProcess()`)
- Modify: `modules/pi-copilot/src/module/ProcessSafety.h`, `ProcessSafety.cpp` (`ValidateProcessFilePaths()`, `HasFileTables()`)
- Modify: `modules/pi-copilot/data/process-safety.json` (`fileTables`)
- Modify: `modules/pi-copilot/src/module/AgentTools.cpp` (tool definition + `RunGlobalTool`, the apply_process redirect)
- Modify: `modules/pi-copilot/src/module/SystemPrompt.cpp`
- Modify: `modules/pi-copilot/src/module/PICopilotAgentSelfTest.cpp` (A3 tool-name lists)
- Modify: `modules/pi-copilot/src/module/PICopilotInc5SelfTest.cpp` (Section B7)
- Modify: `modules/pi-copilot/test/run-selftest.sh`

**Interfaces:**
- Consumes: `CheckProcessSafety()`, `SafetyVerdict`, `CompiledProcessSafety()` (Task 7), `BuildViewContext()`, `CollapsedViewContext()`, `RenderViewPreview()`, `JpegImageBlock()`, `DescribeParameterChanges()`. Also the Task 1 facts: II columns `enabled, path, drizzlePath, localNormalizationDataPath`, and `weightMode` id `DontCare`.
- Produces:
```cpp
struct GlobalRunResult
{
   bool ok = false; String error; nlohmann::json parametersSet = nlohmann::json::object(); double elapsedMs = 0;
   String processId; std::vector<std::string> createdWindows; nlohmann::json outputIds = nlohmann::json::object();
};
String PrecheckGlobalRun( const IsoString& processId, const nlohmann::json& parameters, const nlohmann::json& tableParameters );
GlobalRunResult RunGlobalProcess( const IsoString& processId, const nlohmann::json& parameters, const nlohmann::json& tableParameters );
String ValidateProcessFilePaths( const IsoString& processId, const nlohmann::json& parameters, const nlohmann::json& tableParameters );
bool HasFileTables( const IsoString& processId );
constexpr size_type PICopilotMaxDescribedWindows = 4;
constexpr size_type PICopilotMinIntegrationFrames = 3;   // documentation of the policy value; the JSON is authoritative
// ToolDefinitions(mode): + "run_global_process" after "apply_process" (Copilot/Guided only)
```

- [ ] **Step 1: File-table policy.** In `data/process-safety.json`, replace `"fileTables": {}` with:
```json
  "fileTables": {
    "ImageIntegration": {
      "images": {
        "enabledColumn": "enabled",
        "minEnabledRows": 3,
        "columns": { "path": "image", "drizzlePath": "optionalFile", "localNormalizationDataPath": "optionalFile" }
      }
    }
  }
```

- [ ] **Step 2: Failing test (Section B7) and the A3 name lists.** In `PICopilotAgentSelfTest.cpp` Section A3, change `all` to `{ "list_processes", "describe_process", "get_view_context", "apply_process", "run_global_process" }`. Add `'globalProcessOk',` after `'processSafetyOk',` in `required_true`. In `PICopilotInc5SelfTest.cpp`, add `#include "ProcessApply.h"` and insert above the marker:
```cpp
   // ---- Section B7: global processes / run_global_process (Task 8) --------------
   {
      bool precheckOk = true, runOk = false, guidedOk = false, safetyOk = false, advisorOk = false,
           schemaOk = false, redirectOk = false;
      nlohmann::json detail = nlohmann::json::object();
      std::vector<std::string> created;
      String error;
      try
      {
         SyntheticFrames frames( 3 );
         auto rows = [&]( int enabled ) {
            nlohmann::json r = nlohmann::json::array();
            for ( size_type i = 0; i < frames.Paths().Length(); ++i )
               r.push_back( nlohmann::json::array( { int( i ) < enabled, U8( frames.Paths()[i] ), "", "" } ) );
            return r;
         };
         const String notImage = frames.AddFile( "notes.txt", "hello" );
         struct Case { const char* name; IsoString process; nlohmann::json params; nlohmann::json tables; const char* expect; };
         const std::vector<Case> cases = {
            { "noTable", "ImageIntegration", nlohmann::json::object(), nlohmann::json::object(),
              "ImageIntegration.images is required" },
            { "relative", "ImageIntegration", nlohmann::json::object(),
              { { "images", { { true, "light_01.fits", "", "" }, { true, "b.fits", "", "" }, { true, "c.fits", "", "" } } } },
              "ImageIntegration.images[0].path: 'light_01.fits' is not an absolute path" },
            { "tilde", "ImageIntegration", nlohmann::json::object(),
              { { "images", { { true, "~/a.fits", "", "" } } } },
              "use an absolute path (PixInsight does not expand ~)" },
            { "missing", "ImageIntegration", nlohmann::json::object(),
              { { "images", { { true, "/nonexistent/picopilot/a.fits", "", "" } } } },
              "'/nonexistent/picopilot/a.fits' does not exist" },
            { "notImage", "ImageIntegration", nlohmann::json::object(),
              { { "images", { { true, U8( notImage ), "", "" } } } },
              "is not an image file PixInsight can read (.txt)" },
            { "tooFew", "ImageIntegration", nlohmann::json::object(), { { "images", rows( 2 ) } },
              "ImageIntegration.images: 2 enabled rows; at least 3 are required" },
            { "unknown", "NoSuchProcessXYZ", nlohmann::json::object(), nlohmann::json::object(),
              "unknown process id 'NoSuchProcessXYZ'" },
         };
         nlohmann::json pc = nlohmann::json::array();
         for ( const Case& c : cases )
         {
            const String e = PrecheckGlobalRun( c.process, c.params, c.tables );
            const bool pass = e.Contains( String::UTF8ToUTF16( c.expect ) );
            pc.push_back( { { "case", c.name }, { "pass", pass }, { "error", U8( e ) } } );
            precheckOk = precheckOk && pass;
         }
         precheckOk = precheckOk && PrecheckGlobalRun( "ImageIntegration", nlohmann::json::object(),
                                                       { { "images", rows( 3 ) } } ).IsEmpty();
         // A view-only process, if this PI has one (most processes default to global-capable).
         for ( const Process& P : Process::AllProcesses() )
            if ( !P.CanProcessGlobal() )
            {
               const String e = PrecheckGlobalRun( P.Id(), nlohmann::json::object(), nlohmann::json::object() );
               detail["viewOnly"] = { { "process", std::string( P.Id().c_str() ) }, { "error", U8( e ) } };
               precheckOk = precheckOk && e.Contains( "cannot run in the global context" );
               break;
            }
         detail["precheck"] = pc;

         int confirmCalls = 0;
         bool answer = false;
         ToolContext ctx;
         ctx.mode = AgentMode::Copilot;
         ctx.confirm = [&]( const String&, const String&, const String& ) { ++confirmCalls; return answer; };
         const nlohmann::json input = { { "process_id", "ImageIntegration" },
                                        { "parameters", { { "weightMode", "DontCare" } } },
                                        { "table_parameters", { { "images", rows( 3 ) } } } };

         // Copilot: runs without asking; the result windows come back.
         const std::set<std::string> before = OpenMainViewIds();
         const ToolOutcome o = ExecuteTool( ToolCall{ "g1", "run_global_process", input }, ctx );
         for ( const std::string& id : OpenMainViewIds() )
            if ( before.count( id ) == 0 )
               created.push_back( id );
         nlohmann::json summary;
         if ( !o.isError )
            summary = nlohmann::json::parse( o.content.at( 0 ).at( "text" ).get<std::string>() );
         int images = 0;
         for ( const nlohmann::json& b : o.content )
            images += b.value( "type", std::string() ) == "image" ? 1 : 0;
         detail["run"] = { { "isError", o.isError }, { "log", U8( o.logLine ) }, { "summary", summary },
                           { "created", created }, { "images", images },
                           { "error", o.isError ? o.content.at( 0 ).at( "text" ) : nlohmann::json() } };
         const std::string integ = summary.value( "outputIds", nlohmann::json::object() ).value( "integrationImageId", std::string() );
         runOk = !o.isError && confirmCalls == 0 && !o.mutated && images == 1 && !integ.empty()
              && std::find( created.begin(), created.end(), integ ) != created.end()
              && summary.at( "createdWindows" ).at( 0 ).at( "context" ).at( "geometry" ).at( "width" ) == kIiW
              && U8( o.logLine ).rfind( "\xE2\x96\xB6 run_global_process ImageIntegration", 0 ) == 0;

         // Guided: asks; No -> nothing opens.
         ctx.mode = AgentMode::Guided;
         const std::set<std::string> b2 = OpenMainViewIds();
         const ToolOutcome declined = ExecuteTool( ToolCall{ "g2", "run_global_process", input }, ctx );
         guidedOk = declined.isError && confirmCalls == 1 && OpenMainViewIds() == b2;

         // Copilot + a policy rule (generateDrizzleData would write .xdrz files): asks; No -> nothing runs.
         ctx.mode = AgentMode::Copilot;
         nlohmann::json drz = input;
         drz["parameters"]["generateDrizzleData"] = true;
         const ToolOutcome drzOut = ExecuteTool( ToolCall{ "g3", "run_global_process", drz }, ctx );
         safetyOk = drzOut.isError && confirmCalls == 2 && OpenMainViewIds() == b2;

         // Advisor: not offered, refused if called anyway.
         ctx.mode = AgentMode::Advisor;
         const ToolOutcome adv = ExecuteTool( ToolCall{ "g4", "run_global_process", input }, ctx );
         advisorOk = adv.isError && adv.content.at( 0 ).at( "text" ).get<std::string>().find( "not available in Advisor" ) != std::string::npos;

         bool inCopilot = false, inAdvisor = false;
         for ( const nlohmann::json& t : ToolDefinitions( AgentMode::Copilot ) )
            inCopilot = inCopilot || t.at( "name" ) == "run_global_process";
         for ( const nlohmann::json& t : ToolDefinitions( AgentMode::Advisor ) )
            inAdvisor = inAdvisor || t.at( "name" ) == "run_global_process";
         schemaOk = inCopilot && !inAdvisor && BuildSystemPrompt( AgentMode::Copilot ).Contains( "run_global_process" )
                 && !BuildSystemPrompt( AgentMode::Advisor ).Contains( "run_global_process" );

         // apply_process on a file-list process points to run_global_process.
         Inc5TestWindow tw( "PCRedirectT", 32, 32, 1, 0.3 );
         ctx.mode = AgentMode::Copilot;
         ctx.turnViewId = tw.MainView().FullId();
         const ToolOutcome red = ExecuteTool( ToolCall{ "g5", "apply_process", { { "process_id", "ImageIntegration" } } }, ctx );
         redirectOk = red.isError && red.content.at( 0 ).at( "text" ).get<std::string>().find( "use run_global_process" ) != std::string::npos;
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      ForceCloseWindows( created );

      const bool ok = precheckOk && runOk && guidedOk && safetyOk && advisorOk && schemaOk && redirectOk;
      out["globalDetail"] = detail;
      out["globalError"] = U8( error );
      out["globalProcessOk"] = ok;
      allOk = allOk && ok;
   }
```

- [ ] **Step 3: Verify RED.** Run the Task 2 Step 3 build command.
Expected: compile errors naming `PrecheckGlobalRun`.

- [ ] **Step 4: Implement the engine.** In `ProcessSafety.h`, add:
```cpp
// File-path rules for global runs (policy "fileTables", plus a general rule).
// Declared table columns: "image" = absolute, existing, readable file that an
// installed format can read; "optionalFile" = empty, or an absolute existing
// readable file; only enabled rows (enabledColumn) are checked, and
// minEnabledRows is enforced; a declared table missing from tableParameters is
// an error. Any other string value (parameter or table cell) that starts with
// '/' must exist (file or directory); one starting with '~' is refused. Row
// SHAPE problems are left to SetParameters()'s precise messages. Returns "" or
// the first problem, naming <Process>.<table>[row].<column>. Root thread.
String ValidateProcessFilePaths( const IsoString& processId, const nlohmann::json& parameters,
                                 const nlohmann::json& tableParameters );

// True when the policy declares file tables for the process (it integrates
// files from disk; apply_process refers the model to run_global_process).
bool HasFileTables( const IsoString& processId );
```
In `ProcessSafety.cpp`, add `#include <pcl/File.h>`, `#include <pcl/FileFormat.h>`, `#include <pcl/FileInfo.h>`, `#include <map>` and `#include <set>`. Add to the anonymous namespace:
```cpp
String CheckInputFile( const String& path, bool image )
{
   if ( path.StartsWith( '~' ) )
      return "'" + path + "': use an absolute path (PixInsight does not expand ~)";
   if ( !path.StartsWith( '/' ) )
      return "'" + path + "' is not an absolute path";
   const FileInfo fi( path );
   if ( !fi.Exists() )
      return "'" + path + "' does not exist";
   if ( !fi.IsFile() )
      return "'" + path + "' is not a file";
   if ( !fi.IsReadable() )
      return "'" + path + "' is not readable";
   if ( image )
   {
      const String ext = File::ExtractExtension( path );
      try
      {
         const FileFormat f( ext, true/*toRead*/, false/*toWrite*/ );
      }
      catch ( ... )
      {
         return "'" + path + "' is not an image file PixInsight can read ("
                + (ext.IsEmpty() ? String( "no extension" ) : ext) + ")";
      }
   }
   return String();
}

// '/'-rooted strings must exist; '~' is refused; anything else is not a path.
String CheckLoosePath( const String& where, const nlohmann::json& v )
{
   if ( !v.is_string() )
      return String();
   const String s = S16( v.get<std::string>() );
   if ( s.StartsWith( '~' ) )
      return where + ": '" + s + "': use an absolute path (PixInsight does not expand ~)";
   if ( s.StartsWith( '/' ) && !File::Exists( s ) && !File::DirectoryExists( s ) )
      return where + ": '" + s + "' does not exist";
   return String();
}
```
and after `UnknownPolicyProcessIds()`:
```cpp
bool HasFileTables( const IsoString& processId )
{
   try
   {
      const Process P( processId );
      return Section( CompiledProcessSafety(), "fileTables" ).contains( std::string( P.Id().c_str() ) );
   }
   catch ( ... )
   {
      return false;
   }
}

String ValidateProcessFilePaths( const IsoString& processId, const nlohmann::json& parameters,
                                 const nlohmann::json& tableParameters )
{
   try
   {
      const Process P( processId );
      const std::string id( P.Id().c_str() );
      const String pname( P.Id() );
      std::set<std::string> declared;
      const nlohmann::json& files = Section( CompiledProcessSafety(), "fileTables" );
      if ( files.contains( id ) && files[id].is_object() )
         for ( auto t = files[id].begin(); t != files[id].end(); ++t )
         {
            const std::string table = t.key();
            const nlohmann::json& rule = t.value();
            declared.insert( table );
            const String tname = pname + "." + S16( table );
            const ProcessParameter tp( P, IsoString( table.c_str() ) );
            const ProcessParameter::parameter_list cols = tp.TableColumns();
            std::map<std::string, int> index;
            String columnList;
            for ( size_type k = 0; k < cols.Length(); ++k )
            {
               index[std::string( cols[k].Id().c_str() )] = int( k );
               columnList += (k > 0 ? String( ", " ) : String()) + String( cols[k].Id() );
            }
            if ( !tableParameters.is_object() || !tableParameters.contains( table ) )
               return tname + " is required: pass table_parameters." + S16( table ) + " as rows [" + columnList + "]";
            const nlohmann::json& rows = tableParameters[table];
            if ( !rows.is_array() )
               return String();   // SetParameters() reports the shape
            const std::string enabledCol = rule.value( "enabledColumn", std::string() );
            const int enabledIdx = index.count( enabledCol ) ? index[enabledCol] : -1;
            size_type enabled = 0;
            for ( size_type r = 0; r < rows.size(); ++r )
            {
               const nlohmann::json& row = rows[r];
               if ( !row.is_array() || row.size() != cols.Length() )
                  return String();   // SetParameters() reports the shape
               if ( enabledIdx >= 0 && !(row[enabledIdx].is_boolean() && row[enabledIdx].get<bool>()) )
                  continue;
               ++enabled;
               for ( auto c = rule["columns"].begin(); c != rule["columns"].end(); ++c )
               {
                  if ( !index.count( c.key() ) )
                     return "internal: policy column " + S16( c.key() ) + " is not a column of " + tname;
                  const nlohmann::json& cell = row[index[c.key()]];
                  const bool optional = c.value() == "optionalFile";
                  const String where = tname + String().Format( "[%u].", unsigned( r ) ) + S16( c.key() );
                  if ( cell.is_null() && optional )
                     continue;
                  if ( !cell.is_string() )
                     return where + ": expected a file path string";
                  const String path = S16( cell.get<std::string>() );
                  if ( path.IsEmpty() )
                  {
                     if ( optional )
                        continue;
                     return where + ": a file path is required";
                  }
                  const String e = CheckInputFile( path, c.value() == "image" );
                  if ( !e.IsEmpty() )
                     return where + ": " + e;
               }
            }
            const size_type minRows = rule.value( "minEnabledRows", 0 );
            if ( enabled < minRows )
               return tname + String().Format( ": %u enabled rows; at least %u are required", unsigned( enabled ), unsigned( minRows ) );
         }
      if ( parameters.is_object() )
         for ( auto it = parameters.begin(); it != parameters.end(); ++it )
         {
            const String e = CheckLoosePath( pname + "." + S16( it.key() ), it.value() );
            if ( !e.IsEmpty() )
               return e;
         }
      if ( tableParameters.is_object() )
         for ( auto it = tableParameters.begin(); it != tableParameters.end(); ++it )
            if ( !declared.count( it.key() ) && it.value().is_array() )
               for ( size_type r = 0; r < it.value().size(); ++r )
                  if ( it.value()[r].is_array() )
                     for ( size_type k = 0; k < it.value()[r].size(); ++k )
                     {
                        const String e = CheckLoosePath( pname + "." + S16( it.key() )
                                                         + String().Format( "[%u][%u]", unsigned( r ), unsigned( k ) ),
                                                         it.value()[r][k] );
                        if ( !e.IsEmpty() )
                           return e;
                     }
   }
   catch ( ... )
   {
      // Unknown process / table: the executor reports it precisely.
   }
   return String();
}
```
In `ProcessApply.h`, add `#include <string>` and `#include <vector>`, and declare (after `ApplyProcess`):
```cpp
constexpr size_type PICopilotMaxDescribedWindows = 4;    // windows described in detail in one tool_result
constexpr size_type PICopilotMinIntegrationFrames = 3;   // data/process-safety.json fileTables is authoritative

struct GlobalRunResult
{
   bool                     ok = false;
   String                   error;                                     // precise, model-facing
   nlohmann::json           parametersSet = nlohmann::json::object();
   double                   elapsedMs = 0;
   String                   processId;
   std::vector<std::string> createdWindows;                            // main-view ids of windows the run opened
   nlohmann::json           outputIds = nlohmann::json::object();      // read-only "...ImageId" outputs, non-empty only
};

// Cheap checks before anything is asked or run: known id, global-capable,
// file paths (ValidateProcessFilePaths). "" when fine. Root thread.
String PrecheckGlobalRun( const IsoString& processId, const nlohmann::json& parameters,
                          const nlohmann::json& tableParameters );

// PrecheckGlobalRun, then a DEFAULT instance with the checked parameters
// (SetParameters, as ApplyProcess), Validate, CanExecuteGlobal, ExecuteGlobal.
// The windows it opened are found by diffing the open main views before and
// after (even on failure, so the model can mention them). Never modifies an
// open image; never throws. Root thread only.
GlobalRunResult RunGlobalProcess( const IsoString& processId, const nlohmann::json& parameters,
                                  const nlohmann::json& tableParameters );
```
Update the step-2 comment of `ApplyProcess` to read: `2. refuse processes that cannot run on views (use RunGlobalProcess),`.

In `ProcessApply.cpp`, add `#include "ProcessSafety.h"`, `#include <pcl/ImageWindow.h>` and `#include <set>`. In the anonymous namespace, add `SetParameters`. Its body is **moved verbatim** from `ApplyProcess` (the two blocks from `if ( !parameters.is_null() && !parameters.is_object() )` through the end of the `tableParameters` loop), with `r.processId` → `processId` and `r.parametersSet` → `parametersSet`:
```cpp
// Sets `parameters` (scalars) and `tableParameters` (whole tables) on a
// DEFAULT instance, checking everything the core would reject with a modal
// and reading every value back. Throws ApplyError with a precise message.
void SetParameters( const Process& P, ProcessInstance& instance, const String& processId,
                    const nlohmann::json& parameters, const nlohmann::json& tableParameters,
                    nlohmann::json& parametersSet )
{
   if ( !parameters.is_null() && !parameters.is_object() )
      throw ApplyError{ String( "parameters must be an object {parameterId: value}" ) };
   if ( parameters.is_object() )
      for ( auto it = parameters.begin(); it != parameters.end(); ++it )
      {
         const String name = processId + "." + S16( it.key() );
         const ProcessParameter p = FindParameter( P, it.key(), name );
         if ( p.IsTable() )
            throw ApplyError{ name + " is a table parameter; pass it in table_parameters as [[row values]...]" };
         if ( p.IsReadOnly() )
            throw ApplyError{ name + " is read-only" };
         SetChecked( instance, p, ToVariant( p, it.value(), name ), kScalarRow, name );
         parametersSet[it.key()] = it.value();
      }

   if ( !tableParameters.is_null() && !tableParameters.is_object() )
      throw ApplyError{ String( "table_parameters must be an object {tableId: [[row values]...]}" ) };
   if ( tableParameters.is_object() )
      for ( auto it = tableParameters.begin(); it != tableParameters.end(); ++it )
      {
         const String name = processId + "." + S16( it.key() );
         const ProcessParameter p = FindParameter( P, it.key(), name );
         if ( !p.IsTable() )
            throw ApplyError{ name + " is not a table parameter; pass it in parameters" };
         if ( p.IsReadOnly() )
            throw ApplyError{ name + " is read-only (an output of the process); it cannot be set" };
         const ProcessParameter::parameter_list columns = p.TableColumns();
         for ( const ProcessParameter& c : columns )
            if ( c.IsReadOnly() )
               throw ApplyError{ name + "." + String( c.Id() )
                                 + " is a read-only column (an output of the process); this table cannot be set" };
         const nlohmann::json& rows = it.value();
         if ( !rows.is_array() )
            throw ApplyError{ name + ": expected an array of rows [[...], ...]" };
         for ( size_type i = 0; i < rows.size(); ++i )
            if ( !rows[i].is_array() || rows[i].size() != columns.Length() )
               throw ApplyError{ name + String().Format( ": row %u has %u values; expected %u (columns: ",
                                                         unsigned( i ), unsigned( rows[i].is_array() ? rows[i].size() : 0 ),
                                                         unsigned( columns.Length() ) )
                                 + ColumnIds( columns ) + ")" };
         size_type minRows = 0, maxRows = 0;
         p.GetLengthLimits( minRows, maxRows );
         const bool unlimited = maxRows == 0 || maxRows == ~size_type( 0 );
         if ( rows.size() < minRows || (!unlimited && rows.size() > maxRows) )
         {
            String need;
            if ( unlimited )
               need = "at least " + RowCount( minRows );
            else if ( minRows == maxRows )
               need = "exactly " + RowCount( minRows );
            else
               need = String().Format( "between %u and %u rows", unsigned( minRows ), unsigned( maxRows ) );
            throw ApplyError{ name + ": " + RowCount( rows.size() ) + " given; this table needs "
                              + need + " (columns: " + ColumnIds( columns ) + ")" };
         }
         if ( !instance.AllocateTableRows( p, rows.size() ) )
            throw ApplyError{ name + String().Format( ": PixInsight refused a table of %u rows", unsigned( rows.size() ) ) };
         for ( size_type i = 0; i < rows.size(); ++i )
            for ( size_type k = 0; k < columns.Length(); ++k )
            {
               const String cell = name + String().Format( "[%u].", unsigned( i ) ) + String( columns[k].Id() );
               SetChecked( instance, columns[k], ToVariant( columns[k], rows[i][k], cell ), i, cell );
            }
         parametersSet[it.key()] = rows;
      }
}

std::set<std::string> OpenMainViewIds()
{
   std::set<std::string> ids;
   for ( const ImageWindow& w : ImageWindow::AllWindows() )
      ids.insert( std::string( w.MainView().Id().c_str() ) );
   return ids;
}
```
Keep the long explanatory comments from the original blocks (the modal/row-count and unlimited-length notes) above the matching lines. In `ApplyProcess`, replace the two moved blocks with `SetParameters( *P, instance, r.processId, parameters, tableParameters, r.parametersSet );`, and change the global-only message to `r.processId + " can only run in the global context (not on a view); use run_global_process"`. The inc-4 A1 case still matches its `"PICopilot can only run in the global context"` prefix. Then add:
```cpp
String PrecheckGlobalRun( const IsoString& processId, const nlohmann::json& parameters,
                          const nlohmann::json& tableParameters )
{
   try
   {
      const Process P( processId );
      if ( !P.CanProcessGlobal() )
         return String( P.Id() ) + " cannot run in the global context; use apply_process on a view";
   }
   catch ( ... )
   {
      return "unknown process id '" + String( processId ) + "'; call list_processes for valid ids";
   }
   return ValidateProcessFilePaths( processId, parameters, tableParameters );
}

GlobalRunResult RunGlobalProcess( const IsoString& processId, const nlohmann::json& parameters,
                                  const nlohmann::json& tableParameters )
{
   GlobalRunResult r;
   std::set<std::string> before;
   bool started = false;
   try
   {
      const String pre = PrecheckGlobalRun( processId, parameters, tableParameters );
      if ( !pre.IsEmpty() )
         throw ApplyError{ pre };
      const Process P( processId );
      r.processId = String( P.Id() );
      ProcessInstance instance( P );   // DEFAULT parameters
      SetParameters( P, instance, r.processId, parameters, tableParameters, r.parametersSet );

      String whyNot;
      if ( !instance.Validate( whyNot ) )
         throw ApplyError{ r.processId + " rejected the parameters: " + (whyNot.IsEmpty() ? String( "(no reason given)" ) : whyNot) };
      whyNot.Clear();
      if ( !instance.CanExecuteGlobal( whyNot ) )
         throw ApplyError{ r.processId + " cannot run globally with these parameters: "
                           + (whyNot.IsEmpty() ? String( "(no reason given)" ) : whyNot) };

      before = OpenMainViewIds();
      started = true;
      const auto t0 = std::chrono::steady_clock::now();
      bool ran = false;
      try
      {
         ran = instance.ExecuteGlobal();
      }
      catch ( const pcl::Exception& x )
      {
         throw ApplyError{ r.processId + " failed while running: " + x.Message() };
      }
      r.elapsedMs = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
      for ( const ProcessParameter& p : P.Parameters() )
         if ( p.IsString() && p.IsReadOnly() && p.Id().EndsWith( "ImageId" ) )
         {
            const String v = instance.ParameterValue( p, kScalarRow ).ToString();
            if ( !v.IsEmpty() )
               r.outputIds[std::string( p.Id().c_str() )] = U8( v );
         }
      if ( !ran )
      {
         String changes = DescribeParameterChanges( parameters, tableParameters, 400 );
         changes.ReplaceString( "\n", "; " );
         throw ApplyError{ r.processId + " did not complete: the process stopped with an error while running, or the "
                           "user aborted it in PixInsight (the reason is in the Process Console). Do not simply retry: "
                           "if the user may have aborted it, ask them first; otherwise check the input files and the "
                           "values you set: " + changes };
      }
      r.ok = true;
   }
   catch ( const ApplyError& e )
   {
      r.error = e.message;
   }
   catch ( const pcl::Exception& x )
   {
      r.error = "run_global_process internal error: " + x.Message();
   }
   catch ( const std::exception& x )
   {
      r.error = String( "run_global_process internal error: " ) + String( x.what() );
   }
   catch ( ... )
   {
      r.error = "run_global_process internal error: unknown exception";
   }
   if ( started )
      for ( const std::string& id : OpenMainViewIds() )
         if ( before.count( id ) == 0 )
            r.createdWindows.push_back( id );
   if ( !r.ok )
      r.parametersSet = nlohmann::json::object();
   return r;
}
```

- [ ] **Step 5: The tool, the redirect, and the prompt.** In `AgentTools.cpp`:
  - Add `#include <pcl/ImageWindow.h>`.
  - In `ApplyProcessTool`, right after the `if ( pid.empty() )` check, add:
```cpp
   if ( HasFileTables( IsoString( pid.c_str() ) ) )
      return Fail( what, S16( pid ) + " integrates files from disk, not an open image: use run_global_process with "
                         "the file list in table_parameters" );
```
  - Add, next to `ApplyProcessTool`:
```cpp
ToolOutcome RunGlobalTool( const nlohmann::json& in, const ToolContext& ctx, clock::time_point t0 )
{
   const std::string pid = StringField( in, "process_id" );
   const nlohmann::json params = in.contains( "parameters" ) ? in["parameters"] : nlohmann::json::object();
   const nlohmann::json tables = in.contains( "table_parameters" ) ? in["table_parameters"] : nlohmann::json::object();
   nlohmann::json shown = params.is_object() ? params : nlohmann::json::object();
   if ( tables.is_object() )
      for ( auto it = tables.begin(); it != tables.end(); ++it )
         shown[it.key()] = it.value();
   const String what = "run_global_process " + S16( pid ) + " " + Shorten( S16( shown.dump() ), PICopilotToolLogParamChars );

   if ( ctx.mode == AgentMode::Advisor )
      return Fail( what, "run_global_process is not available in Advisor mode (read-only); give the user the settings instead" );
   if ( pid.empty() )
      return Fail( "run_global_process", "run_global_process needs process_id; call list_processes for valid ids" );

   const String pre = PrecheckGlobalRun( IsoString( pid.c_str() ), params, tables );
   if ( !pre.IsEmpty() )
      return Fail( what, pre );
   const SafetyVerdict safety = CheckProcessSafety( IsoString( pid.c_str() ), params, tables );
   if ( safety.kind == SafetyVerdict::Deny )
      return Fail( what, S16( pid ) + " is not allowed from PI Copilot: " + safety.reason
                         + ". Give the user the settings so they can run it themselves." );
   if ( ctx.mode == AgentMode::Guided || safety.kind == SafetyVerdict::Confirm )
   {
      if ( !ctx.confirm )
         return Fail( what, "internal error: no confirmation callback" );
      String changes = DescribeParameterChanges( params, tables, PICopilotConfirmChangesChars );
      if ( safety.kind == SafetyVerdict::Confirm )
         changes = "Why you are asked: " + safety.reason + ".\n\n" + changes;
      if ( !ctx.confirm( S16( pid ), "(global run: creates new images, changes no open image)", changes ) )
      {
         ToolOutcome o;
         o.isError = true;
         o.content.push_back( TextBlock( "The user declined this run_global_process call (" + pid
                                         + "). Nothing was run. Do not repeat it; ask what they would like instead." ) );
         o.logLine = S16( kErrMarkUtf8 ) + what + S16( kArrowUtf8 ) + "declined by user";
         return o;
      }
   }

   const GlobalRunResult g = RunGlobalProcess( IsoString( pid.c_str() ), params, tables );
   if ( !g.ok )
   {
      String e = g.error;
      if ( !g.createdWindows.empty() )
      {
         e += " (windows it opened before stopping: ";
         for ( size_t i = 0; i < g.createdWindows.size(); ++i )
            e += (i > 0 ? String( ", " ) : String()) + S16( g.createdWindows[i] );
         e += ")";
      }
      return Fail( what, e );
   }

   // The main result: the integration image if the process names one it opened, else the first new window.
   std::string primary = g.createdWindows.empty() ? std::string() : g.createdWindows.front();
   const std::string named = g.outputIds.value( "integrationImageId", std::string() );
   for ( const std::string& id : g.createdWindows )
      if ( id == named )
         primary = id;
   nlohmann::json windows = nlohmann::json::array();
   for ( const std::string& id : g.createdWindows )
   {
      if ( windows.size() >= PICopilotMaxDescribedWindows )
         break;
      nlohmann::json w = { { "id", id } };
      try
      {
         const ImageWindow iw = ImageWindow::WindowById( IsoString( id.c_str() ) );
         if ( !iw.IsNull() )
            w["context"] = CollapsedViewContext( BuildViewContext( iw.MainView() ) );
      }
      catch ( const pcl::Exception& x )
      {
         w["contextError"] = U8( x.Message() );
      }
      windows.push_back( w );
   }
   nlohmann::json summary = {
      { "result", "ok" },
      { "process", U8( g.processId ) },
      { "parametersSet", g.parametersSet },
      { "elapsedMs", std::lround( g.elapsedMs ) },
      { "outputIds", g.outputIds },
      { "createdWindows", windows },
      { "createdWindowCount", g.createdWindows.size() },
      { "note", "New image windows were created; no open image was modified. The preview shows " + primary + "." }
   };
   ToolOutcome o;
   o.content.push_back( TextBlock( summary.dump() ) );
   if ( !primary.empty() )
   {
      const ImageWindow iw = ImageWindow::WindowById( IsoString( primary.c_str() ) );
      if ( !iw.IsNull() )
      {
         const ViewPreviewResult p = RenderViewPreview( iw.MainView() );
         if ( p.ok )
            o.content.push_back( JpegImageBlock( p.base64 ) );
         else
            o.content.push_back( TextBlock( "preview failed: " + U8( p.error ) ) );
      }
   }
   o.logLine = OkLine( what, t0 );
   return o;
}
```
  - In `ToolDefinitions`, inside `if ( mode != AgentMode::Advisor )` after `tools.push_back( apply );`, add:
```cpp
      nlohmann::json gprops = nlohmann::json::object();
      gprops["process_id"] = { { "type", "string" }, { "description", "Process id, e.g. ImageIntegration" } };
      gprops["parameters"] = props["parameters"];
      gprops["table_parameters"] = { { "type", "object" },
                                     { "description", "{tableParameterId: [[row values in column order], ...]}. "
                                                      "ImageIntegration: {\"images\": [[enabled, path, drizzlePath, "
                                                      "localNormalizationDataPath], ...]} with absolute paths." } };
      nlohmann::json global = nlohmann::json::object();
      global["name"] = "run_global_process";
      global["description"] = "Run a PixInsight process in the global context (not on a view), e.g. ImageIntegration "
                              "over image files on disk. It creates NEW image windows and never modifies an open "
                              "image. Starts from the process's DEFAULT settings and sets only the parameters given. "
                              "File paths must be absolute and must exist. Returns the created windows (ids and "
                              "statistics) and a preview of the main result, or a precise error.";
      global["input_schema"] = { { "type", "object" }, { "properties", gprops },
                                 { "required", nlohmann::json::array( { "process_id" } ) } };
      tools.push_back( global );
```
  - In `ExecuteTool`, before the final unknown-tool `Fail`, add `if ( call.name == "run_global_process" ) return RunGlobalTool( in, ctx, t0 );`, and make the unknown-tool list say `", apply_process, run_global_process"` for non-Advisor modes.

  In `SystemPrompt.cpp`:
  - Append to `kApplyTool`:
```
"- run_global_process {process_id, parameters, table_parameters}: runs a process in the global context, e.g. "
"ImageIntegration over files on disk. It opens NEW image windows and never changes an open image.\n"
```
  - Replace the last `kApplyIdioms` bullet (`- Processes that only run globally … give the settings instead.\n`) with:
```
"- Processes that work on files rather than an open image (e.g. ImageIntegration) go through run_global_process. "
"For ImageIntegration pass the frames as table_parameters {\"images\": [[true, \"/abs/path/light_001.xisf\", \"\", "
"\"\"], ...]} (columns enabled, path, drizzlePath, localNormalizationDataPath), using absolute paths the user gave "
"you; at least 3 enabled frames. Never invent file paths: ask the user for the folder or the files.\n"
"- Some runs ask the user first even in Copilot mode, because they write files or close windows; if the user "
"declines, do not repeat the call.\n"
```

- [ ] **Step 6: Verify GREEN.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) && bash test/run-selftest.sh`
Expected: `PASS: self-test verdict all green`. `globalDetail.run.summary.createdWindows[0].id` equals `outputIds.integrationImageId`. The inc-4 `applyProcessOk` and `agentToolsOk` stay green. Also check the coverage gate: `processSafetyOk` stays green (the `fileTables` id `ImageIntegration` is installed).

- [ ] **Step 7: Commit.**
```bash
cd /home/scarter4work/projects/astro-pi
git add modules/pi-copilot/data/process-safety.json modules/pi-copilot/src/module/ProcessSafety.h \
        modules/pi-copilot/src/module/ProcessSafety.cpp modules/pi-copilot/src/module/ProcessApply.h \
        modules/pi-copilot/src/module/ProcessApply.cpp modules/pi-copilot/src/module/AgentTools.cpp \
        modules/pi-copilot/src/module/SystemPrompt.cpp modules/pi-copilot/src/module/PICopilotAgentSelfTest.cpp \
        modules/pi-copilot/src/module/PICopilotInc5SelfTest.cpp modules/pi-copilot/test/run-selftest.sh
git commit -m "feat(pi-copilot): run_global_process (ImageIntegration over files): path checks, safety policy, result windows + preview

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

## Task 9: `run_pjsr` — the off-by-default escape hatch (parse check → full-script approval → EvaluateScript with console capture)

This implements spec §3's escape hatch as Scott ruled it:
- **Off by default** (⚙ Allow scripts, Task 6), and never offered in Advisor.
- The parse check (Task 1's proven `new Function(<string literal>)`) runs first. A syntax error goes straight back to the model **without** bothering the user.
- Every script is then shown **in full** in a dialog whose default button is **Don't run**, **every time, in every mode**.
- Only then does it run on the root thread through `EvaluateScript`. `console.writeln` output is captured with `console.beginLog()/endLog()` inside the same evaluation, and the return value is JSON-serialised. The output is bounded, and errors carry the line number in the model's own code.

**Files:**
- Create: `modules/pi-copilot/src/module/PjsrRunner.h`, `PjsrRunner.cpp`
- Create: `modules/pi-copilot/src/module/ScriptConfirmDialog.h`, `ScriptConfirmDialog.cpp`
- Modify: `modules/pi-copilot/src/module/AgentTools.h`, `AgentTools.cpp`
- Modify: `modules/pi-copilot/src/module/SystemPrompt.h`, `SystemPrompt.cpp`
- Modify: `modules/pi-copilot/src/module/PICopilotInterface.h/.cpp`
- Modify: `modules/pi-copilot/src/module/PICopilotInc5SelfTest.cpp` (Section B8)
- Modify: `modules/pi-copilot/src/module/CMakeLists.txt`, `modules/pi-copilot/test/run-selftest.sh`

**Interfaces:**
- Consumes: `CopilotSettings::LoadRunPjsrEnabled()` (Task 6), `PICopilotInterface::PlainText()`, `U8()`. Also the Task 1 facts: parsing does not execute, a breakout is a SyntaxError, and console capture works inside one `EvaluateScript`.
- Produces:
```cpp
constexpr size_type PICopilotMaxScriptChars        = 20000;
constexpr size_type PICopilotMaxScriptConsoleChars = 8000;   // tail kept
constexpr size_type PICopilotMaxScriptValueChars   = 4000;
std::string ScriptLiteral( const String& code );              // ASCII-only JSON string literal
int PjsrLineOffset();                                         // calibrated once per session
struct PjsrCheck { bool ok = false; String error; int line = 0; };
PjsrCheck CheckPjsrSyntax( const String& code );              // root thread; never executes the code
struct PjsrRun { bool ok = false; String error; int line = 0; String value; bool valueTruncated = false;
                 String console; bool consoleTruncated = false; double elapsedMs = 0; };
PjsrRun RunPjsr( const String& code, const IsoString& targetViewId );   // root thread
struct ToolOptions { bool runPjsr = false; };
nlohmann::json ToolDefinitions( AgentMode mode, const ToolOptions& options = ToolOptions() );
using ConfirmScriptFn = std::function<bool( const String& purpose, const String& code, const IsoString& targetViewId )>;
// ToolContext gains: bool runPjsr = false; ConfirmScriptFn confirmScript;
String BuildSystemPrompt( AgentMode mode, const ToolOptions& options = ToolOptions() );
static bool ScriptConfirmDialog::Ask( const String& purpose, const String& code, const IsoString& targetViewId );
```

- [ ] **Step 1: Failing test (Section B8).** Add `'runPjsrOk',` after `'globalProcessOk',` in `required_true`. In `PICopilotInc5SelfTest.cpp`, add `#include "PjsrRunner.h"` and insert above the marker:
```cpp
   // ---- Section B8: run_pjsr (Task 9) ------------------------------------------
   {
      bool checkOk = false, breakoutOk = false, runOk = false, errorOk = false, boundOk = false, pixelOk = false,
           offOk = false, declineOk = false, approveOk = false, guidedOk = false, syntaxNoDialogOk = false,
           advisorOk = false, schemaOk = false, promptOk = false;
      nlohmann::json detail = nlohmann::json::object();
      String error;
      try
      {
         const PjsrCheck good = CheckPjsrSyntax( "var a = 1;\nreturn a + 2;" );
         const PjsrCheck bad = CheckPjsrSyntax( "var a = 1;\nvar b = ;\n" );
         detail["bad"] = { { "error", U8( bad.error ) }, { "line", bad.line }, { "offset", PjsrLineOffset() } };
         checkOk = good.ok && !bad.ok && bad.line == 2 && bad.error.StartsWith( "SyntaxError" );

         const PjsrCheck br = CheckPjsrSyntax( "}); new ImageWindow( 8, 8, 1, 32, true, false, \"PCBreakout\" ); (function(){" );
         breakoutOk = !br.ok && ImageWindow::WindowById( IsoString( "PCBreakout" ) ).IsNull();

         const PjsrRun r1 = RunPjsr( "console.writeln( \"hi pc\" );\nreturn { a: 1, t: targetViewId };", "PCPjsrT" );
         detail["r1"] = { { "value", U8( r1.value ) }, { "console", U8( r1.console ) }, { "error", U8( r1.error ) } };
         runOk = r1.ok && r1.value == "{\"a\":1,\"t\":\"PCPjsrT\"}" && r1.console.Contains( "hi pc" );

         const PjsrRun r2 = RunPjsr( "var a = 1;\nthrow new Error( \"pc-script-fail\" );", IsoString() );
         detail["r2"] = { { "error", U8( r2.error ) }, { "line", r2.line } };
         errorOk = !r2.ok && r2.line == 2 && r2.error.Contains( "pc-script-fail" );

         const PjsrRun r3 = RunPjsr( "for ( var i = 0; i < 400; ++i ) console.writeln( \"0123456789012345678901234567890123456789012345678\" );\n"
                                     "return \"x\";", IsoString() );
         boundOk = r3.ok && r3.consoleTruncated && r3.console.StartsWith( "[... " )
                && r3.console.Length() <= PICopilotMaxScriptConsoleChars + 64 && r3.value == "\"x\"";

         Inc5TestWindow tw( "PCPjsrPix", 32, 32, 1, 0.4 );
         View v = tw.MainView();
         const PjsrRun r4 = RunPjsr( "var v = View.viewById( targetViewId );\nv.beginProcess( UndoFlag_PixelData );\n"
                                     "v.image.apply( 0.5, ImageOp_Mul );\nv.endProcess();\nreturn v.image.median();", v.FullId() );
         detail["r4"] = { { "value", U8( r4.value ) }, { "error", U8( r4.error ) } };
         pixelOk = r4.ok && std::fabs( Inc5Median( v, 0 ) - 0.2 ) < 1e-6;

         int asks = 0;
         bool answer = false;
         String askedCode;
         ToolContext ctx;
         ctx.mode = AgentMode::Copilot;
         ctx.turnViewId = v.FullId();
         ctx.confirmScript = [&]( const String&, const String& code, const IsoString& ) { ++asks; askedCode = code; return answer; };
         const std::string markCode = "new ImageWindow( 8, 8, 1, 32, true, false, \"PCPjsrMark\" );\nreturn 1;";
         const ToolCall mark{ "p1", "run_pjsr", { { "purpose", "Create a marker window" }, { "code", markCode } } };
         auto markExists = []() { return !ImageWindow::WindowById( IsoString( "PCPjsrMark" ) ).IsNull(); };

         ctx.runPjsr = false;
         const ToolOutcome off = ExecuteTool( mark, ctx );
         offOk = off.isError && asks == 0 && !markExists()
              && off.content.at( 0 ).at( "text" ).get<std::string>().find( "turned off" ) != std::string::npos;

         ctx.runPjsr = true;
         answer = false;
         const ToolOutcome declined = ExecuteTool( mark, ctx );
         declineOk = declined.isError && asks == 1 && U8( askedCode ) == markCode && !markExists();

         answer = true;
         const ToolOutcome approved = ExecuteTool( mark, ctx );
         approveOk = !approved.isError && asks == 2 && markExists() && approved.mutated;
         ForceCloseWindows( { "PCPjsrMark" } );

         ctx.mode = AgentMode::Guided;   // asked in Guided too (and in Copilot above): every time
         answer = false;
         ExecuteTool( mark, ctx );
         guidedOk = asks == 3 && !markExists();

         ctx.mode = AgentMode::Copilot;
         const ToolOutcome syn = ExecuteTool( ToolCall{ "p2", "run_pjsr", { { "purpose", "broken" }, { "code", "var = ;" } } }, ctx );
         syntaxNoDialogOk = syn.isError && asks == 3
                         && syn.content.at( 0 ).at( "text" ).get<std::string>().find( "syntax error at line 1" ) != std::string::npos;

         ctx.mode = AgentMode::Advisor;
         const ToolOutcome adv = ExecuteTool( mark, ctx );
         advisorOk = adv.isError && asks == 3
                  && adv.content.at( 0 ).at( "text" ).get<std::string>().find( "not available in Advisor" ) != std::string::npos;

         ToolOptions on;
         on.runPjsr = true;
         const nlohmann::json tOn = ToolDefinitions( AgentMode::Copilot, on );
         const nlohmann::json tOff = ToolDefinitions( AgentMode::Copilot );
         const nlohmann::json tAdv = ToolDefinitions( AgentMode::Advisor, on );
         bool offHas = false, advHas = false;
         for ( const nlohmann::json& t : tOff ) offHas = offHas || t.at( "name" ) == "run_pjsr";
         for ( const nlohmann::json& t : tAdv ) advHas = advHas || t.at( "name" ) == "run_pjsr";
         schemaOk = tOn.back().at( "name" ) == "run_pjsr"
                 && tOn.back().at( "input_schema" ).at( "required" ) == nlohmann::json::array( { "code", "purpose" } )
                 && !offHas && !advHas && ToolDefinitions( AgentMode::Guided, on ).back().at( "name" ) == "run_pjsr";
         promptOk = BuildSystemPrompt( AgentMode::Copilot, on ).Contains( "run_pjsr" )
                 && !BuildSystemPrompt( AgentMode::Copilot ).Contains( "run_pjsr" )
                 && !BuildSystemPrompt( AgentMode::Advisor, on ).Contains( "run_pjsr" )
                 // folded inc-4 minor: assert the literal mode-switch phrase, not the ever-present "PI Copilot"
                 && BuildSystemPrompt( AgentMode::Advisor ).Contains( "switching the mode selector to Copilot" );
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      ForceCloseWindows( { "PCPjsrMark", "PCBreakout" } );

      const bool ok = checkOk && breakoutOk && runOk && errorOk && boundOk && pixelOk && offOk && declineOk
                   && approveOk && guidedOk && syntaxNoDialogOk && advisorOk && schemaOk && promptOk;
      out["runPjsrDetail"] = detail;
      out["runPjsrError"] = U8( error );
      out["runPjsrOk"] = ok;
      allOk = allOk && ok;
   }
```

- [ ] **Step 2: Verify RED.** Run the Task 2 Step 3 build command.
Expected: `PjsrRunner.h: No such file or directory`.

- [ ] **Step 3: The engine.** `PjsrRunner.h`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_PjsrRunner_h
#define PICopilot_PjsrRunner_h

#include <pcl/String.h>

#include <string>

namespace pcl
{

constexpr size_type PICopilotMaxScriptChars        = 20000;
constexpr size_type PICopilotMaxScriptConsoleChars = 8000;   // the TAIL is kept
constexpr size_type PICopilotMaxScriptValueChars   = 4000;

// The code as an ASCII-only JSON string literal (every non-ASCII character,
// U+2028/2029 included, \u-escaped). Embedded in the wrapper scripts below as
// DATA, so no text in it can close the wrapper and run (the breakout an inline
// "(function(){ <code> })" wrapper would allow). Throws std::exception if the
// text cannot be encoded.
std::string ScriptLiteral( const String& code );

// How many lines the core's `new Function` source puts before the body:
// measured once (a known syntax error on body line 3) and cached.
int PjsrLineOffset();

struct PjsrCheck
{
   bool   ok = false;
   String error;      // "SyntaxError: <message>"
   int    line = 0;   // 1-based line in the model's code (0 = unknown)
};

// Parses the code as the body of function( targetViewId ) with the core's own
// parser (new Function), WITHOUT running it. Root thread only. Never throws.
PjsrCheck CheckPjsrSyntax( const String& code );

struct PjsrRun
{
   bool   ok = false;
   String error;               // the thrown value, e.g. "Error: ..." / "TypeError: ..."
   int    line = 0;            // 1-based line in the model's code (0 = unknown)
   String value;               // JSON.stringify( returned value ) (else String( value )); empty if undefined
   bool   valueTruncated = false;
   String console;             // everything written to the console while it ran
   bool   consoleTruncated = false;
   double elapsedMs = 0;
};

// Runs the code as the body of function( targetViewId ) via
// MetaModule::EvaluateScript, capturing the console with beginLog()/endLog()
// in the same evaluation (endLog in a finally). Root thread only. Cannot be
// interrupted: an endless loop hangs PixInsight (the approval dialog says so).
// Never throws.
PjsrRun RunPjsr( const String& code, const IsoString& targetViewId );

} // namespace pcl

#endif // PICopilot_PjsrRunner_h
```
`PjsrRunner.cpp`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PjsrRunner.h"
#include "PICopilotModule.h"
#include "Utf8.h"

#include <pcl/Exception.h>
#include <pcl/Variant.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <climits>

namespace pcl
{

namespace
{

String S16( const std::string& s )
{
   return String::UTF8ToUTF16( s.c_str() );
}

nlohmann::json EvalJson( const std::string& asciiSource )
{
   const Variant v = ThePICopilotModule->EvaluateScript( String( asciiSource.c_str() ), "JavaScript" );
   return nlohmann::json::parse( U8( v.ToString() ) );
}

int BodyLine( int reported )
{
   return reported > 0 ? std::max( 0, reported - PjsrLineOffset() ) : 0;
}

} // namespace

std::string ScriptLiteral( const String& code )
{
   return nlohmann::json( U8( code ) ).dump( -1, ' ', true/*ensure_ascii*/ );
}

int PjsrLineOffset()
{
   static int offset = INT_MIN;
   if ( offset == INT_MIN )
   {
      offset = 0;
      try
      {
         const nlohmann::json j = EvalJson(
            "(function(){ try { new Function( \"targetViewId\", \"\\n\\nvar c = ;\\n\" ); return JSON.stringify( { line: 0 } ); }"
            " catch ( e ) { return JSON.stringify( { line: e.lineNumber|0 } ); } })()" );
         const int line = j.value( "line", 0 );
         offset = line > 0 ? line - 3 : 0;
      }
      catch ( ... )
      {
      }
   }
   return offset;
}

PjsrCheck CheckPjsrSyntax( const String& code )
{
   PjsrCheck c;
   try
   {
      const nlohmann::json j = EvalJson(
         "(function(){ try { new Function( \"targetViewId\", " + ScriptLiteral( code ) + " );"
         " return JSON.stringify( { ok: true } ); }"
         " catch ( e ) { return JSON.stringify( { ok: false, name: String( e.name ), message: String( e.message ),"
         " line: e.lineNumber|0 } ); } })()" );
      if ( j.value( "ok", false ) )
      {
         c.ok = true;
         return c;
      }
      c.line = BodyLine( j.value( "line", 0 ) );
      c.error = S16( j.value( "name", std::string( "Error" ) ) + ": " + j.value( "message", std::string() ) );
   }
   catch ( const pcl::Exception& x )
   {
      c.error = "the syntax check could not run: " + x.Message();
   }
   catch ( const std::exception& x )
   {
      c.error = String( "the script text could not be prepared: " ) + String( x.what() );
   }
   return c;
}

PjsrRun RunPjsr( const String& code, const IsoString& targetViewId )
{
   PjsrRun r;
   try
   {
      const std::string target = nlohmann::json( std::string( targetViewId.c_str() ) ).dump( -1, ' ', true );
      const std::string src =
         "(function(){ var out = { ok: true }; console.beginLog();"
         " try { var f = new Function( \"targetViewId\", " + ScriptLiteral( code ) + " ); var v = f( " + target + " );"
         "  if ( v !== undefined ) { try { out.value = JSON.stringify( v ); } catch ( x ) { out.value = String( v ); }"
         "   if ( out.value === undefined ) out.value = String( v ); } }"
         " catch ( e ) { out.ok = false; out.error = String( e ); out.line = e ? (e.lineNumber|0) : 0; }"
         " finally { try { out.console = console.endLog().toString(); }"
         "   catch ( x ) { out.console = \"\"; out.consoleError = String( x ); } }"
         " return JSON.stringify( out ); })()";
      const auto t0 = std::chrono::steady_clock::now();
      const nlohmann::json j = EvalJson( src );
      r.elapsedMs = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
      r.ok = j.value( "ok", false );
      r.value = S16( j.value( "value", std::string() ) );
      r.console = S16( j.value( "console", std::string() ) );
      if ( !r.ok )
      {
         r.error = S16( j.value( "error", std::string( "(no error text)" ) ) );
         r.line = BodyLine( j.value( "line", 0 ) );
      }
   }
   catch ( const pcl::Exception& x )
   {
      r.ok = false;
      r.error = "the script could not run: " + x.Message();
   }
   catch ( const std::exception& x )
   {
      r.ok = false;
      r.error = String( "the script result could not be read: " ) + String( x.what() );
   }
   if ( r.value.Length() > PICopilotMaxScriptValueChars )
   {
      r.value = r.value.Left( PICopilotMaxScriptValueChars );
      r.valueTruncated = true;
   }
   if ( r.console.Length() > PICopilotMaxScriptConsoleChars )
   {
      const size_type cut = r.console.Length() - PICopilotMaxScriptConsoleChars;
      r.console = String().Format( "[... %u earlier characters omitted]\n", unsigned( cut ) )
                + r.console.Right( PICopilotMaxScriptConsoleChars );
      r.consoleTruncated = true;
   }
   return r;
}

} // namespace pcl
```

- [ ] **Step 4: The approval dialog.** `ScriptConfirmDialog.h`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ScriptConfirmDialog_h
#define PICopilot_ScriptConfirmDialog_h

#include <pcl/Dialog.h>
#include <pcl/Label.h>
#include <pcl/PushButton.h>
#include <pcl/Sizer.h>
#include <pcl/TextBox.h>

namespace pcl
{

// Shows a model-written script IN FULL before it may run. Default button and
// Esc: Don't run. Shown for every run_pjsr call in every mode. Root thread.
class ScriptConfirmDialog : public Dialog
{
public:

   // True only when the user clicks "Run script".
   static bool Ask( const String& purpose, const String& code, const IsoString& targetViewId );

private:

   ScriptConfirmDialog( const String& purpose, const String& code, const IsoString& targetViewId );

   VerticalSizer   Global_Sizer;
   Label           Info_Label;
   TextBox         Code_TextBox;
   HorizontalSizer Buttons_Sizer;
   PushButton      Run_PushButton;
   PushButton      DontRun_PushButton;

   bool m_run = false;

   void e_Click( Button& sender, bool checked );
};

} // namespace pcl

#endif // PICopilot_ScriptConfirmDialog_h
```
`ScriptConfirmDialog.cpp`:
```cpp
// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ScriptConfirmDialog.h"
#include "PICopilotInterface.h"   // PlainText()

namespace pcl
{

namespace
{

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
      else
         out += c;
   }
   return out;
}

} // namespace

ScriptConfirmDialog::ScriptConfirmDialog( const String& purpose, const String& code, const IsoString& targetViewId )
{
   Info_Label.EnableRichText();
   Info_Label.EnableWordWrapping();
   Info_Label.SetText( "<p><b>PI Copilot wants to run this script.</b></p>"
                       "<p>Purpose: " + EscapeHtml( purpose ) + "</p>"
                       "<p>Image it is about: " + (targetViewId.IsEmpty() ? String( "(none)" ) : EscapeHtml( String( targetViewId ) )) + "</p>"
                       "<p>Scripts run with full access to PixInsight and your files, and cannot be interrupted once "
                       "running. Read the whole script before you run it.</p>" );
   Code_TextBox.SetReadOnly();
   Code_TextBox.SetScaledMinSize( 640, 360 );
   Code_TextBox.SetText( PICopilotInterface::PlainText( code ) );

   Run_PushButton.SetText( "Run script" );
   Run_PushButton.OnClick( (Button::click_event_handler)&ScriptConfirmDialog::e_Click, *this );
   DontRun_PushButton.SetText( "Don't run" );
   DontRun_PushButton.SetDefault();   // Return and Esc both mean: don't run
   DontRun_PushButton.OnClick( (Button::click_event_handler)&ScriptConfirmDialog::e_Click, *this );
   Buttons_Sizer.SetSpacing( 8 );
   Buttons_Sizer.AddStretch();
   Buttons_Sizer.Add( Run_PushButton );
   Buttons_Sizer.Add( DontRun_PushButton );

   Global_Sizer.SetMargin( 8 );
   Global_Sizer.SetSpacing( 6 );
   Global_Sizer.Add( Info_Label );
   Global_Sizer.Add( Code_TextBox, 100 );
   Global_Sizer.Add( Buttons_Sizer );

   SetWindowTitle( String::UTF8ToUTF16( "PI Copilot \xE2\x80\x94 Run a script?" ) );
   SetSizer( Global_Sizer );
   EnsureLayoutUpdated();
   AdjustToContents();
}

void ScriptConfirmDialog::e_Click( Button& sender, bool )
{
   m_run = &sender == &Run_PushButton;
   if ( m_run )
      Ok();
   else
      Cancel();
}

bool ScriptConfirmDialog::Ask( const String& purpose, const String& code, const IsoString& targetViewId )
{
   ScriptConfirmDialog d( purpose, code, targetViewId );
   d.m_run = false;
   return d.Execute() == StdDialogCode::Ok && d.m_run;
}

} // namespace pcl
```
Add `PjsrRunner.cpp` and `ScriptConfirmDialog.cpp` to `MODULE_SOURCES` after `ProcessSafety.cpp`.

- [ ] **Step 5: The tool, the gates and the prompt.** In `AgentTools.h`, add:
```cpp
// Which optional tools a message offers. run_pjsr: the user allowed scripts
// in ⚙ (never offered in Advisor, whatever this says).
struct ToolOptions
{
   bool runPjsr = false;
};

// run_pjsr: asked for EVERY script, in every mode; true = the user clicked Run script.
using ConfirmScriptFn = std::function<bool( const String& purpose, const String& code, const IsoString& targetViewId )>;
```
Add to `ToolContext`: `bool runPjsr = false;` ("the user allowed scripts for this message") and `ConfirmScriptFn confirmScript;` ("required for run_pjsr"). Change the declaration to `nlohmann::json ToolDefinitions( AgentMode mode, const ToolOptions& options = ToolOptions() );`.

In `AgentTools.cpp`, add `#include "PjsrRunner.h"`. Change `ToolDefinitions`' signature to match, and at its end (before `return tools;`) add:
```cpp
   if ( mode != AgentMode::Advisor && options.runPjsr )
   {
      nlohmann::json sprops = nlohmann::json::object();
      sprops["code"] = { { "type", "string" },
                         { "description", "JavaScript (PJSR) function body. `return` a value to get it back (JSON); "
                                          "console.writeln output is captured; targetViewId holds the id of the view "
                                          "this message is about (may be empty)." } };
      sprops["purpose"] = { { "type", "string" }, { "description", "One sentence for the user saying what the script does." } };
      nlohmann::json script = nlohmann::json::object();
      script["name"] = "run_pjsr";
      script["description"] = "Run a short PixInsight JavaScript (PJSR) script, for jobs no process can do "
                              "(inspection-driven decisions, window/preview management, custom measurements). The "
                              "user sees the whole script and must approve it every time. Prefer apply_process / "
                              "run_global_process. To change pixels directly, wrap the change in "
                              "view.beginProcess(UndoFlag_PixelData) ... view.endProcess() so it can be undone. A "
                              "script cannot be interrupted: never write loops that might not end.";
      script["input_schema"] = { { "type", "object" }, { "properties", sprops },
                                 { "required", nlohmann::json::array( { "code", "purpose" } ) } };
      tools.push_back( script );
   }
```
Add, next to `RunGlobalTool`:
```cpp
ToolOutcome RunPjsrTool( const nlohmann::json& in, const ToolContext& ctx, clock::time_point t0 )
{
   const std::string purpose = StringField( in, "purpose" );
   const String code = S16( StringField( in, "code" ) );
   const String what = "run_pjsr \"" + Shorten( S16( purpose ), 80 ) + "\"";
   if ( ctx.mode == AgentMode::Advisor )
      return Fail( what, "run_pjsr is not available in Advisor mode (read-only); give the user the script instead" );
   if ( !ctx.runPjsr )
      return Fail( what, "run_pjsr is turned off. The user can allow scripts in PI Copilot's settings (the gear "
                         "button); until then use the processes, or give the user the script to run themselves." );
   if ( code.Trimmed().IsEmpty() )
      return Fail( what, "run_pjsr needs {\"code\": \"<JavaScript function body>\", \"purpose\": \"<one sentence>\"}" );
   if ( code.Length() > PICopilotMaxScriptChars )
      return Fail( what, String().Format( "the script has %u characters; the limit is %u. Split the work, or use "
                                          "processes", unsigned( code.Length() ), unsigned( PICopilotMaxScriptChars ) ) );
   if ( purpose.empty() )
      return Fail( what, "run_pjsr needs a one-sentence purpose; it is shown to the user in the approval dialog" );

   // Parse first: a syntax error never reaches the user's dialog.
   const PjsrCheck check = CheckPjsrSyntax( code );
   if ( !check.ok )
      return Fail( what, (check.line > 0 ? String().Format( "syntax error at line %d: ", check.line ) : String( "syntax error: " ))
                         + check.error + " (the script was not shown to the user and did not run; fix it and call run_pjsr again)" );

   if ( !ctx.confirmScript )
      return Fail( what, "internal error: no script confirmation callback" );
   if ( !ctx.confirmScript( S16( purpose ), code, ctx.turnViewId ) )
   {
      ToolOutcome o;
      o.isError = true;
      o.content.push_back( TextBlock( "The user declined to run this script. Nothing was run. Do not send the same "
                                      "script again; ask what they would like instead." ) );
      o.logLine = S16( kErrMarkUtf8 ) + what + S16( kArrowUtf8 ) + "declined by user";
      return o;
   }

   const PjsrRun run = RunPjsr( code, ctx.turnViewId );
   ToolOutcome o;
   o.mutated = true;   // a script may have changed images (conservative: the turn-end note mentions History)
   if ( !run.ok )
   {
      const nlohmann::json e = {
         { "result", "error" }, { "error", U8( run.error ) }, { "line", run.line },
         { "console", U8( run.console ) }, { "consoleTruncated", run.consoleTruncated },
         { "note", "The script ran until the error: anything it changed before that point stays changed." }
      };
      o.isError = true;
      o.content.push_back( TextBlock( e.dump() ) );
      o.logLine = S16( kErrMarkUtf8 ) + what + S16( kArrowUtf8 )
                + (run.line > 0 ? String().Format( "error at line %d: ", run.line ) : String( "error: " ))
                + Shorten( run.error, 160 );
      return o;
   }
   const nlohmann::json summary = {
      { "result", "ok" },
      { "value", run.value.IsEmpty() ? nlohmann::json() : nlohmann::json( U8( run.value ) ) },
      { "valueTruncated", run.valueTruncated },
      { "console", U8( run.console ) },
      { "consoleTruncated", run.consoleTruncated },
      { "elapsedMs", std::lround( run.elapsedMs ) },
      { "note", "A script's changes are undoable only if it used view.beginProcess()/endProcess() or ran process instances." }
   };
   o.content.push_back( TextBlock( summary.dump() ) );
   o.logLine = OkLine( what, t0 );
   return o;
}
```
In `ExecuteTool`, add `if ( call.name == "run_pjsr" ) return RunPjsrTool( in, ctx, t0 );` before the unknown-tool `Fail`. Append `, run_pjsr (when the user allows scripts)` to the listed names for non-Advisor modes.

In `SystemPrompt.h`, change the declaration to `String BuildSystemPrompt( AgentMode mode, const ToolOptions& options = ToolOptions() );`. In `SystemPrompt.cpp`, add:
```cpp
const char* const kScriptTool =
   "- run_pjsr {code, purpose}: runs a PixInsight JavaScript (PJSR) script, but only after the user has read and "
   "approved the whole script in a dialog, every time. Use it only when no process can do the job (inspection-driven "
   "decisions, window or preview management, custom measurements). The code is a function body: `return` a value to "
   "get it back (as JSON); console.writeln output is captured; targetViewId is the id of the view this message is "
   "about. To change pixels directly, wrap the change in view.beginProcess(UndoFlag_PixelData) ... view.endProcess() "
   "so the user can undo it; prefer running process instances (P.executeOn(view)), which are undoable anyway. A "
   "script cannot be interrupted: never write loops that might not end. If the user declines, do not send the same "
   "script again.\n";
```
and in `BuildSystemPrompt`, after `p += kApplyTool;`, add `if ( options.runPjsr ) p += kScriptTool;`.

Panel (`PICopilotInterface.h/.cpp`):
  - Add `#include "ScriptConfirmDialog.h"` and the private member `ToolOptions m_turnTools;`.
  - In `SendCurrentInput()`, after `m_turnModel = …`, add `m_turnTools.runPjsr = CopilotSettings::LoadRunPjsrEnabled();`.
  - In `StartRequest()`, use `BuildSystemPrompt( m_turnMode, m_turnTools )` and `ToolDefinitions( m_turnMode, m_turnTools )`.
  - In `MakeToolContext()`, add `ctx.runPjsr = m_turnTools.runPjsr;` and `ctx.confirmScript = &ScriptConfirmDialog::Ask;`.

- [ ] **Step 6: Verify GREEN.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) && bash test/run-selftest.sh`
Expected: `PASS: self-test verdict all green`. `runPjsrDetail.bad.line` is 2, `offset` equals the Task 1 difference, and the inc-4 `agentToolsOk` (which calls `ToolDefinitions(mode)` / `BuildSystemPrompt(mode)` with default options) is unchanged.

- [ ] **Step 7: Commit.**
```bash
cd /home/scarter4work/projects/astro-pi
git add modules/pi-copilot/src/module/PjsrRunner.h modules/pi-copilot/src/module/PjsrRunner.cpp \
        modules/pi-copilot/src/module/ScriptConfirmDialog.h modules/pi-copilot/src/module/ScriptConfirmDialog.cpp \
        modules/pi-copilot/src/module/AgentTools.h modules/pi-copilot/src/module/AgentTools.cpp \
        modules/pi-copilot/src/module/SystemPrompt.h modules/pi-copilot/src/module/SystemPrompt.cpp \
        modules/pi-copilot/src/module/PICopilotInterface.h modules/pi-copilot/src/module/PICopilotInterface.cpp \
        modules/pi-copilot/src/module/PICopilotInc5SelfTest.cpp modules/pi-copilot/src/module/CMakeLists.txt \
        modules/pi-copilot/test/run-selftest.sh
git commit -m "feat(pi-copilot): run_pjsr escape hatch -- off by default, parse-only pre-check, full-script approval every time, console capture

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

---

## Task 10: Release 0.1.2.0 via the repository + user verification handoff

**Files:**
- Modify: `modules/pi-copilot/src/module/PICopilotVersion.h` (REVISION 1 → 2, BUILD 0)
- Modify: `modules/pi-copilot/src/module/PICopilotModule.cpp` (`GetReleaseDate`, `Description()`)
- Modify: `modules/pi-copilot/README.md`
- Regenerated by `./release.sh`: `repository/`

**Interfaces:**
- Consumes: everything above. Produces: signed PICopilot **0.1.2.0**, served from `https://raw.githubusercontent.com/scarter4work/astro-pi/main/repository/`.

- [ ] **Step 1: Version, date, description.**
  - In `PICopilotVersion.h`, set `#define PICOPILOT_MODULE_VERSION_REVISION  2` and keep `#define PICOPILOT_MODULE_VERSION_BUILD     0` (→ `0.1.2.0`).
  - In `PICopilotModule.cpp` `GetReleaseDate`, set the date you run `release.sh` (`date +%F`).
  - Replace the descriptive sentence in `Description()` with `"Chat that sees the active view, streams its replies and works on your images: Copilot (acts, undoable), Guided (asks first), Advisor (read-only); integrates files with ImageIntegration; optional scripts you approve one by one."`.

- [ ] **Step 2: README.**
  - Insert after the `## Process safety policy` section (Task 7):
```markdown
## Increment 5 — streaming, conversation, keyring, global processes, scripts (0.1.2.0)

- **Streaming:** replies appear as they are written. A request may take up to 600 s in total, and one that sends nothing for 120 s is stopped as stalled. Stop cancels mid-reply; a partial reply is shown but not kept in the conversation.
- **Conversation:** **New chat** (was Clear) starts fresh. Long chats stay within a history budget: the oldest whole exchanges stop being sent (the log says how many), and a tool call and its result are never split. Prompt caching reuses the unchanged start of each request.
- **⚙ Settings:** API key (with where it is stored), model (Claude Opus 4.8 default, Opus 5.5, Fable 5.1, Sonnet 5, Haiku 4.5), **Allow scripts** (off by default), and the default panel side (right/left).
- **API key in the system keyring:** stored with `secret-tool` (libsecret). A key from earlier versions is moved out of PixInsight's plain-text settings on first use, and only removed there after the keyring copy has been read back. Without a usable keyring the key stays in settings, and PI Copilot says why.
- **Global processes:** `run_global_process` runs processes that work on files, e.g. ImageIntegration over frames on disk (absolute paths, at least 3 frames). It opens new image windows and never changes an open image. The model gets the new windows' statistics and a preview. Guided asks first; Copilot asks only when the safety policy says so.
- **Scripts (`run_pjsr`, off by default):** when you allow scripts, the model may propose PixInsight JavaScript for jobs no process can do. It is syntax-checked first (broken scripts never reach you), and every script is then shown to you in full, with **Don't run** as the default. Only approved scripts run. Output and errors (with line numbers) go back to the model.
- **Polish:** "Capturing view…" while a large image is prepared; failures say what happened and what to do (timeout, stall, network, API error with status, refusal), never "Error 0".
- **Self-test** additions:
  - an ImageIntegration smoke on synthetic FITS
  - PJSR parse-without-execute and breakout checks
  - an SSE assembler on recorded and synthetic streams in any chunking
  - streamed deltas reaching the UI during a request (loopback), stalls, cancel and mid-stream errors
  - request shape (caching, thinking binding) and pair-safe history trimming
  - keyring storage, migration and fallback (on a test-only keyring item)
  - settings and failure wording
  - the safety-policy coverage gate over every installed process
  - global runs and file checks
  - run_pjsr gating, approval, capture and bounds
  - gated live checks: a real cache read, and an edited Opus 5.5 history accepted
```
  - In the older sections, change `each request has an overall **300 s** limit` to `**600 s**` (streamed requests also stop after 120 s of silence).
  - If the increment-4 text still says the preview "went with the message" when Include view is off, correct it to "the view is still the message's target; no preview or statistics are sent" (inc-4 final-review minor).
  - Under `## Verified`, add: `**<release date>** — headless self-test PASS incl. live checks (streamed agent run ratio <liveAgentRatio>, cache read <liveCacheRead> tokens, Opus 5.5 edited history accepted). GUI: pending user verification (0.1.2.0 via repository pull).`

- [ ] **Step 3: Final self-test on the release build, with every live check required.**

Run: `cd /home/scarter4work/projects/astro-pi/modules/pi-copilot && cmake --build build -j$(nproc) && PICOPILOT_REQUIRE_LIVE=1 bash test/run-selftest.sh`
Expected: `anthropic check`, `two-turn check`, `vision check`, `live agent check` and `live conversation check` all `RAN`, then `PASS: self-test verdict all green`. A SKIPPED check now fails the run, and that blocks the release.

- [ ] **Step 4: Release.**
```bash
cd /home/scarter4work/projects/astro-pi
test -s /tmp/.pi_codesign_pass && stat -c '%a' /tmp/.pi_codesign_pass   # expect 600
./release.sh
```
Expected: every stage runs, including `== 3a/6 package PICopilot module tarball ==` and the final `== 6/6 integrity check … ==` passing. `repository/<YYYYMMDD>-linux-x64-PICopilot-0.1.2.0.tar.gz` exists and `updates.xri` names it. The NukeX tarball and script zips are reused byte-for-byte (release.sh behaviour since 0.1.1.0).

- [ ] **Step 5: Commit version + artifacts, merge, push, shred.**
```bash
cd /home/scarter4work/projects/astro-pi
git status --short        # review: version files, README, repository/ only
git add modules/pi-copilot/src/module/PICopilotVersion.h modules/pi-copilot/src/module/PICopilotModule.cpp \
        modules/pi-copilot/README.md repository/
git add -u repository/
git commit -m "release(pi-copilot): ship PICopilot 0.1.2.0 (increment 5: streaming, conversation budget + caching, keyring, global processes, run_pjsr) via repository

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
git checkout main && git pull --ff-only && git merge --no-ff feat/pi-copilot-inc5 -m "Merge feat/pi-copilot-inc5: PI Copilot increment 5

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
git push origin main
shred -u /tmp/.pi_codesign_pass
```
Expected: the push succeeds, and `/tmp/.pi_codesign_pass` no longer exists. A superseded `…-PICopilot-0.1.1.0.tar.gz` that `release.sh` left in `repository/` is removed in a follow-up commit, the way 8972d10 did for 0.1.0.4, **after** Step 6 confirms the new manifest is served.

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
Expected: `manifest serves sha1 <40 hex>`, then `tarball sha1 matches`. If the harness blocks `sleep`, poll with the Monitor tool using the same until-condition.

- [ ] **Step 7: User verification handoff (USER, in PixInsight, repo pull only; never a local `-m=` load).** Give Scott this checklist and record the answers in the README "Verified" line:
  1. *Resources ▸ Updates ▸ Check for Updates*: install PICopilot **0.1.2.0**, restart, and open *Process ▸ Etc ▸ PICopilot*.
  2. **Keyring:** send any message. If your key was in PixInsight's settings, the log says it was moved into the system keyring. ⚙ shows "Key: stored in the system keyring". (After this, the agent verifies from the shell that `secret-tool search service picopilot account anthropic-api-key` lists one item, without printing the secret.)
  3. **Streaming:** ask a question with a long answer. The text appears progressively, not all at once.
  4. **Stop mid-reply:** press **Stop** while text is streaming. The partial text stays, followed by `(stopped)`, and your prompt is back in the input line.
  5. **New chat:** the button reads **New chat**. It clears the log, and the model no longer remembers the earlier conversation.
  6. **Model choice:** in ⚙ pick **Claude Opus 5.5** and send a message that uses a tool (e.g. "what's the median of this image?"). The log shows `(model: Claude Opus 5.5)` and the reply works. Restart PixInsight: ⚙ still shows Opus 5.5.
  7. **Capturing view…:** with a large image active and Include view on, the Send button briefly shows **Capturing view…** before **Thinking…**.
  8. **Panel side:** in ⚙ choose **Left**. The panel moves to the left edge. Restart PixInsight: it is still there.
  9. **ImageIntegration (Copilot):** "Integrate the FITS files in /path/to/lights" (≥ 3 calibrated, registered frames). A new integration window opens, the log shows `▶ run_global_process ImageIntegration … → ok`, and no open image changed.
  10. **Guided asks for global runs; the policy asks in Copilot:** in Guided the same request shows a dialog first (No = nothing opens). In Copilot, "…and generate drizzle data" asks first too, saying it would update the `.xdrz` files.
  11. **Scripts off by default:** ask "write and run a script that lists the open image windows". The model says scripts are turned off, or offers the script as text. Nothing runs.
  12. **Scripts on:** in ⚙ tick **Allow scripts** and repeat. A dialog shows the whole script with **Don't run** as default. Don't run → nothing happens and the model acknowledges it. Run script → the list appears in the chat. For a script that changes pixels, *Edit ▸ Undo* restores the image.
  13. **Clear failure wording:** disconnect the network and send a message. The log says `Could not reach the Anthropic API (…). Check the internet connection…`, never `Error 0`.

---

## Self-Review

- **Scope coverage (Scott's 7 items + ledger):**
  1. run_pjsr: off by default + persisted toggle (T6), pre-flight parse (T1 proof, T9), full-script confirm every time with default No (T9 `ScriptConfirmDialog`, B8 counts asks in Copilot and Guided), EvaluateScript on the root thread with console capture (T9), undo via beginProcess/endProcess (tool description + prompt + B8 pixel test; undo itself is checklist 12), precise errors with line numbers (T9), never in Advisor (T9 schema + executor), bounded output (T9 constants + B8 bound test).
  2. Streaming: incremental chunks (T3 loopback proves deltas arrive during the request), SSE parsed on the worker (T2/T3), deltas marshalled via Timer (T3 panel), tool_use from input_json_delta (T2), final message identical to the non-streamed shape (T2 via `ParseMessagesResponse`), mid-stream errors (T2/T3), Stop cancels (T3 cancel case + panel).
  3. Conversation: New chat (T4), history cap by token budget with pair-safe trimming (T4 `TrimHistoryToBudget` + validator), prompt caching on system + tools + rolling top-level breakpoint (T4 + live cache read).
  4. Keyring via secret-tool + ExternalProcess with LD_LIBRARY_PATH scrubbed (T5 `/usr/bin/env -u`), Settings fallback with visible note (T5), migration that removes plaintext only after a verified read-back (T5 `StoreVerified` + B4 migrate case).
  5. Polish: Capturing caption (T6), clear cancel/timeout messages (T3 kinds + T6 wording), model choice persisted with the listed ids (T4 catalog + T6 dialog; Haiku id per the skill, Ruling 3), left/right placement (T6).
  6. Global processes incl. ImageIntegration: smoke FIRST (T1), ExecuteGlobal path with file-list tables (T8), result windows by output ids + window diff (T8), Copilot/Guided only (T8), file paths validated (T8), the confirmation decision justified (Ruling 1).
  7. Deny-list review: enumeration + coverage gate + documented list (T7), applied to apply_process (T7) and run_global_process (T8), with PICopilot itself denied.

  Ledger items: the deny-list before run_pjsr (T7 precedes T9), caching + history cap (T4), the vacuous Advisor assertion (T9 promptOk) and hard-gated live checks (T3 `PICOPILOT_REQUIRE_LIVE`, used in T10).
- **Placeholder scan:** no TBD/TODO. The Task 7 classification is data the implementer produces from a failing, enumerating test, with an exact procedure and a gate; it is not a placeholder. `SetParameters()` is an explicit move of named blocks, with its full code shown. The signing password is referenced by location only.
- **Type consistency:**
  - `RequestShape{stream,maxTokens,streamIdleSeconds}` (T3) gains `promptCaching`/`thinkingBinding` in T4. `ProductionRequestShape(const IsoString&)` has the same signature in T3/T4/T6.
  - `ChatThread(apiKey, system, history, model, url, timeout, tools, shape)` and `AnthropicRequest(…, tools, shape)` use the same argument order in T3/T4/T6.
  - `RequestErrorKind` values are identical in T3/T6. `AgentStep::errorKind` is added in T6 and used only by `DescribeTurnEnd`.
  - `KeyStore::State{key,where,note}` is the same in T5/T6. `ToolOptions{runPjsr}`, `ToolContext::{runPjsr,confirmScript}` and `ConfirmScriptFn(purpose, code, targetViewId)` are the same in T9 and the panel.
  - `SafetyVerdict{Allow,Confirm,Deny}` is the same in T7/T8. `GlobalRunResult` fields are the same in T8's engine and tool.
  - `PICopilotMaxScript*Chars` are the same in T9 and the constraints. `PanelSide{Right=0,Left=1}` is the same in T6's placement, settings and dialog.
  - Every verdict key in `required_true` is set by exactly one section: `inc5SmokeOk`, `sseParserOk`, `streamTransportOk`, `conversationOk`, `liveConversationOk`, `keyStoreKeyringOk`, `configPolishOk`, `processSafetyOk`, `globalProcessOk`, `runPjsrOk`.
- **Unverified API facts, and how the plan de-risks each:**
  1. **ImageIntegration ExecuteGlobal on synthetic FITS headlessly.** This is Task 1, the first task, with a BLOCKED exit. A modal would fail by timeout under Xvfb, never on Scott's desktop.
  2. **`new Function` parses without executing, a breakout text is only a SyntaxError, and `console.beginLog/endLog` capture works inside one `EvaluateScript`.** All proven in Task 1 before any production code. PJSR's line offset is calibrated at runtime (`PjsrLineOffset`) and asserted in T9.
  3. **Real SSE byte shapes** (including Opus 5.5 thinking and signature deltas). Task 2 records real streams first. The assembler must reproduce them in 1-byte and 4 KB chunking, plus hand-written streams with exact expected JSON.
  4. **Deltas reaching the UI mid-POST.** Research showed chunks at the server's cadence, and T3's loopback asserts ≥ 2 takes while the worker is active and a first delta well before the end.
  5. **The API accepting `block_binding: drop_block` + the beta header, and top-level automatic `cache_control`.** T4's gated live checks: a 200 on an edited Opus 5.5 history and `cache_read_input_tokens > 0`. A rejection is BLOCKED with the verbatim body; nothing is improvised.
  6. **`secret-tool` reachable from PixInsight's process (D-Bus session, library path).** T5 runs the real keyring on a test-only item under the same harness, plus the forced-fallback path. The production item is proven untouched from the shell.
  7. **Which installed processes have side effects outside the image.** T7's enumeration makes this a measured list, not a guess, and the gate keeps it complete across PI updates.
  8. **GUI-only behaviour:** live rendering cadence, the three dialogs, the caption paint, the panel moving sides, and undo of a script's pixel change. Checklist items 3-13 cover them. They are never claimed from a headless run.
  9. **The stream idle limit (120 s) versus the API's ping cadence during long thinking** is not measured. The live runs (T3 A6 streamed, T4 Opus 5.5) would fail as `Stalled` if pings were sparser, and the constraint names the value for a later adjustment.
