# PI Copilot — Increment 2: Text Chat (KeyStore + AnthropicClient + Config + Panel) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** A working in-panel text chat: the user pastes their own Anthropic key via a ⚙ Config dialog, types a message in the dockable PI Copilot panel, and Claude's reply appears — the HTTP call running off the UI thread.

**Architecture:** `KeyStore` persists the BYO key in PixInsight `Settings` (local space). `ConfigDialog` (a `pcl::Dialog`) captures the key (password-masked). `AnthropicClient` builds the Messages API request with nlohmann/json, POSTs it via `NetworkTransfer` (synchronous, **non-streamed** for this increment), and parses the reply. A `ChatThread` (`pcl::Thread`) runs the blocking POST off the UI thread; a `pcl::Timer` on the UI thread polls a `Mutex`-guarded result and appends it to a `TextBox` chat log.

**Tech Stack:** C++17, PCL SDK (`$HOME/PCL`), nlohmann/json v3.11.3 (FetchContent), PixInsight 1.9.5, module signing.

**Spec:** `docs/superpowers/specs/2026-09-20-pi-copilot-native-pcl-design.md` (§3 execution model text path, §4 AnthropicClient/KeyStore/PICopilotInterface, §5 threading, §6 key handling). Builds on increment 1 (merged, commit 5072766; sidecar removed 3f05671).

## Global Constraints

- Module ID string is `"PICopilot"` (stable). C++17; flags/defines exactly as increment 1 (`-fPIC -fvisibility=hidden -fvisibility-inlines-hidden`, `__PCL_LINUX __PCL_BUILDING_MODULE _REENTRANT`).
- Output `PICopilot-pxm.so` (+ `.xsgn`). Bump `PICOPILOT_MODULE_VERSION_BUILD` to 2 before building.
- **Anthropic transport is PROVEN** (live spike this session: `NetworkTransfer` POST to `https://api.anthropic.com/v1/messages` with `SetSSL(true,false,true,true)` + `SetCustomHTTPHeaders("x-api-key: …\nanthropic-version: 2023-06-01\ncontent-type: application/json")` → `responseCode()==200`). Reuse that exact idiom.
- **Default model:** `claude-opus-4-8` (proven in the spike; the API echoes it back). Make it a single constant `PICOPILOT_DEFAULT_MODEL`.
- **Non-streamed** for this increment: request JSON has NO `"stream"` key; accumulate the whole response body, then `json::parse`. (SSE is a later increment.)
- **Threading (hard rules, from PCL headers):** `Thread::Run()` and anything it calls (including `NetworkTransfer::POST` and its `OnDownloadDataAvailable` handler, which fire synchronously on the worker thread) MUST NOT touch the GUI or the console (`Thread.h:289-299`). Only a UI-thread `Timer` handler (`Timer.h:227`, a `Control` member) may call `TextBox::Insert`/read a `View`. Cross-thread data passes through a `pcl::Mutex`-guarded buffer (`Mutex.h`, use `AutoLock` RAII). `MetaModule::EvaluateScript` and `ProcessInstance` execution are root-thread only (not used in this increment).
- **No masked failures** (spec §6/§8): a non-2xx surfaces `response["error"]["message"]` verbatim in the chat log; a missing key shows a clear "set your key in ⚙ Config" message, never a silent no-op.
- **Never `make install`.** Dev/CI tests load via `-m=`; harness recipe from increment 1 (`timeout` + `-r=` probe + private mktemp marker path; see [[pi-headless-run-gotchas]]).
- **The GUI cannot be tested headlessly** (`--automation-mode` cannot run interactive panels/dialogs). Auto-tests cover KeyStore + AnthropicClient only; the panel + dialog are verified by the user in PixInsight. Do NOT claim the UI is "tested" from a headless run.

## File Structure

```
modules/pi-copilot/
  CMakeLists.txt                       # MODIFY: add nlohmann/json FetchContent block
  src/module/CMakeLists.txt            # MODIFY: add new .cpp files; link nlohmann_json::nlohmann_json
  src/module/PICopilotVersion.h        # MODIFY: BUILD -> 2
  src/module/KeyStore.h / .cpp         # NEW: Settings-backed API key load/save
  src/module/AnthropicClient.h / .cpp  # NEW: build request, POST via NetworkTransfer, parse reply (non-streamed)
  src/module/ConfigDialog.h / .cpp     # NEW: pcl::Dialog, password Edit, returns key
  src/module/ChatThread.h / .cpp       # NEW: pcl::Thread running one AnthropicClient turn off the UI thread
  src/module/PICopilotInterface.h/.cpp # MODIFY: replace placeholder with chat log/input/send/mode/⚙ + Timer wiring
  src/module/PICopilotSelfTest.cpp     # MODIFY: add KeyStore round-trip + gated real-API AnthropicClient check
  test/run-selftest.sh                 # MODIFY: pass PICOPILOT_TEST_API_KEY from an ephemeral local file if present
```

---

## Task 1: nlohmann/json wiring + KeyStore

**Files:** Create `src/module/KeyStore.{h,cpp}`; modify `CMakeLists.txt`, `src/module/CMakeLists.txt`, `PICopilotVersion.h`, `PICopilotSelfTest.cpp`, `test/run-selftest.sh`.

**Interfaces:**
- Produces: `String pcl::KeyStore::Load()` (returns the stored key or empty String); `void pcl::KeyStore::Save(const String&)`. Key = local Settings `"PICopilot/AnthropicApiKey"`.

- [ ] **Step 1: Failing test** — extend the self-test to round-trip a *test* key and assert it reads back. In `PICopilotSelfTest.cpp` `RunSelfTest`, add (guarded so it uses a throwaway key name, not the real one):
```cpp
// KeyStore round-trip (test key, not the user's real key)
Settings::Write( "PICopilot/SelfTestKey", String( "rt-probe-42" ) );
String back; Settings::Read( "PICopilot/SelfTestKey", back );
bool keyStoreOk = (back == "rt-probe-42");
Settings::Remove( "PICopilot/SelfTestKey" );   // if Remove exists; else overwrite with ""
```
Add `keyStoreOk` to the JSON verdict and require it in `run-selftest.sh`'s python assertion. Run `bash test/run-selftest.sh` → FAIL (verdict lacks `keyStoreOk`).
- [ ] **Step 2: Verify RED.** Run: `cd modules/pi-copilot && bash test/run-selftest.sh` → fails on missing/false `keyStoreOk`.
- [ ] **Step 3: Implement KeyStore.** `KeyStore.h`:
```cpp
#ifndef PICopilot_KeyStore_h
#define PICopilot_KeyStore_h
#include <pcl/String.h>
namespace pcl { namespace KeyStore {
   String Load();               // "" if unset
   void   Save( const String& );
} }
#endif
```
`KeyStore.cpp` uses `Settings::Read("PICopilot/AnthropicApiKey", s)` (pre-init `s=String()`) and `Settings::Write(...)`. `#include <pcl/Settings.h>`. (Then wire the self-test to use `KeyStore` indirectly is unnecessary — the self-test uses `Settings` directly to avoid coupling; `KeyStore` is exercised by AnthropicClient/Config in later tasks. Keep the Step-1 self-test as the Settings round-trip proof.)
- [ ] **Step 4: nlohmann/json CMake.** In `modules/pi-copilot/CMakeLists.txt`, before `add_subdirectory(src/module)`, add the FetchContent block (verbatim shape from `modules/nukex/CMakeLists.txt`): declare `nlohmann_json` GIT_TAG `v3.11.3`, `JSON_BuildTests OFF`, `JSON_Install OFF`, `FetchContent_MakeAvailable`. In `src/module/CMakeLists.txt` add `KeyStore.cpp` to sources and `target_link_libraries(PICopilot PRIVATE nlohmann_json::nlohmann_json)`.
- [ ] **Step 5: Bump version** `PICOPILOT_MODULE_VERSION_BUILD` 1→2.
- [ ] **Step 6: Verify GREEN.** `cmake -B build -DPCLDIR=$HOME/PCL -DPICOPILOT_BUILD_MODULE=ON && cmake --build build -j$(nproc)`; `bash test/run-selftest.sh` → PASS incl. `keyStoreOk:true`.
- [ ] **Step 7: Commit.** `git add` the new/modified files; `git commit -m "feat(pi-copilot): KeyStore (Settings-backed API key) + nlohmann/json wiring"` (end body with `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`).

---

## Task 2: AnthropicClient (non-streamed) + gated real-API self-test

**Files:** Create `src/module/AnthropicClient.{h,cpp}`; modify `src/module/CMakeLists.txt`, `PICopilotSelfTest.cpp`, `test/run-selftest.sh`.

**Interfaces:**
- Consumes: nlohmann/json; `NetworkTransfer`.
- Produces: a message type and a client:
```cpp
namespace pcl {
struct AnthropicMessage { IsoString role;  String content; };   // role: "user" | "assistant"
struct AnthropicResult  { bool ok; String text; String error; int httpStatus; };
class AnthropicClient {
public:
   AnthropicClient( String apiKey, IsoString model = PICOPILOT_DEFAULT_MODEL );
   // Blocking. Safe to call from a worker Thread (does NOT touch GUI). Non-streamed.
   AnthropicResult Send( const String& systemPrompt, const Array<AnthropicMessage>& history );
};
}
```

- [ ] **Step 1: Failing test.** In `PICopilotSelfTest.cpp`, add a **gated** real-API check: only when `std::getenv("PICOPILOT_TEST_API_KEY")` is set, construct `AnthropicClient(key)`, call `Send("You are a test.", {{"user","Reply with exactly: WORKING"}})`, and set `anthropicOk = result.ok && result.text.Contains("WORKING")`. When the env var is absent, set `anthropicOk = true` and `anthropicSkipped = true` (so CI without a key still passes). Add both to the verdict JSON. Update `run-selftest.sh`: if a local key file `modules/pi-copilot/test/.test_api_key` exists (gitignored), export `PICOPILOT_TEST_API_KEY="$(cat …)"` before running PI; assert `anthropicOk` (and print whether it was skipped). Run → FAIL (no `anthropicOk` yet).
- [ ] **Step 2: Verify RED.** `bash test/run-selftest.sh` → fails on missing `anthropicOk`.
- [ ] **Step 3: Implement AnthropicClient.** `Send`:
  1. Build request with nlohmann/json: `{"model":model, "max_tokens":4096, "system":systemPrompt, "messages":[{"role","content"}…]}` (NO `stream`). `.dump()` to a `std::string`, wrap in `pcl::String`.
  2. `NetworkTransfer t; t.SetURL("https://api.anthropic.com/v1/messages"); t.SetSSL(true,false,true,true); t.SetConnectionTimeout(120); t.SetCustomHTTPHeaders(String("x-api-key: ")+apiKey+"\nanthropic-version: 2023-06-01\ncontent-type: application/json");`
  3. Accumulate body: member `IsoString m_buf;` and `bool onData(NetworkTransfer&, const void* p, fsize_type n){ m_buf.Append((const char*)p, size_type(n)); return true; }` wired via `t.OnDownloadDataAvailable((NetworkTransfer::download_event_handler)&AnthropicClient::onData, *dummyControl)`. NOTE: the handler receiver must be a `Control&`. Since `AnthropicClient` is not a `Control`, use a tiny private `Control` receiver held by the client, OR (simpler) capture into a local struct receiver. If the member-pointer receiver requirement is awkward off a `Control`, fall back to `t.POST` then read the whole response via a `ByteArray`/download into a buffer the client owns — verify the cleanest mechanism against `NetworkTransfer.h:492` during implementation and note the choice in the report.
  4. `bool okHttp = t.POST(reqStr); int code = t.ResponseCode();`
  5. Parse: `auto j = json::parse(m_buf.c_str());` If `code>=200 && code<300`: `text = j["content"][0]["text"]`. Else: `error = j.value("/error/message"_json_pointer, String(t.ErrorInformation()))`. Guard all parsing in try/catch → on parse failure, `ok=false, error="unparseable response: "+first200chars`.
  6. Return `{ok, text, error, code}`. Never throws across the thread boundary; errors ride in the struct.
- [ ] **Step 4: CMake** — add `AnthropicClient.cpp` to sources.
- [ ] **Step 5: Verify GREEN.** Controller will place a real key at `modules/pi-copilot/test/.test_api_key` (gitignored, ephemeral) before the test. Build; `bash test/run-selftest.sh` → PASS with `anthropicOk:true` (not skipped).
- [ ] **Step 6: gitignore** — add `modules/pi-copilot/test/.test_api_key` to `.gitignore`.
- [ ] **Step 7: Commit.** `feat(pi-copilot): AnthropicClient (non-streamed Messages API over NetworkTransfer)`.

---

## Task 3: ConfigDialog (BYO-key entry)

**Files:** Create `src/module/ConfigDialog.{h,cpp}`; modify `src/module/CMakeLists.txt`.

**Interfaces:**
- Consumes: `KeyStore`.
- Produces: `class ConfigDialog : public Dialog { public: ConfigDialog(); String Run( const String& currentKey ); };` — returns the entered key on OK (also `KeyStore::Save`s it), or `String()`/unchanged on Cancel.

- [ ] **Step 1** — Port `modules/nukex/src/module/RatingDialog.{h,cpp}` shape 1:1: a `Dialog` subclass with `VerticalSizer root_`, a `Label`, an `Edit ApiKey_Edit` with `EnablePasswordMode()` (Edit.h:185), `PushButton save_`/`cancel_` wired via `OnClick((Button::click_event_handler)&ConfigDialog::…, *this)`, `SetWindowTitle("PI Copilot — API Key")`, `SetSizer(root_); AdjustToContents(); SetFixedSize();`. `Run(currentKey)` pre-fills `ApiKey_Edit.SetText(currentKey)`, `Execute()`, and on OK stores `result_ = ApiKey_Edit.Text()` then `Ok()`.
- [ ] **Step 2** — Add `ConfigDialog.cpp` to CMake; build clean (`cmake --build build`).
- [ ] **Step 3** — Commit `feat(pi-copilot): ConfigDialog for BYO Anthropic key (password-masked)`. **No headless test** — GUI-only; verified by the user in PI (Task 5). State this in the report.

---

## Task 4: Chat panel + worker thread + Timer marshaling

**Files:** Create `src/module/ChatThread.{h,cpp}`; modify `src/module/PICopilotInterface.{h,cpp}`, `src/module/CMakeLists.txt`.

**Interfaces:**
- Consumes: `AnthropicClient`, `KeyStore`, `ConfigDialog`.
- Produces: `class ChatThread : public Thread` holding an `AnthropicClient`, a system prompt, a copy of the history, a `Mutex`, and an `AnthropicResult m_result` + `bool m_done`; `void Run() override` calls `client.Send(...)` and stores the result under the lock, sets `m_done`. Accessors `bool TryTakeResult(AnthropicResult&)` (locks, returns true once done).

- [ ] **Step 1: ChatThread.** Implement per the interface above. `Run()` does NOT touch GUI. Store any exception state as `ok=false` in the result (Thread swallows exceptions — Thread.h).
- [ ] **Step 2: Panel controls.** In `PICopilotInterface`, replace `Placeholder_Label` with: `TextBox ChatLog` (`SetReadOnly()`, scaled min size ~500×300), `Edit ChatInput` (`OnReturnPressed`), `PushButton Send_Button` (`OnClick`), `ComboBox Mode_ComboBox` (AddItem "Copilot"/"Advisor"/"Guided"), `ToolButton Config_ToolButton` (`SetText("\xE2\x9A\x99")`, `OnClick`). Lay out with sizers per `NukeXInterface.cpp` idiom; `SetSizer/EnsureLayoutUpdated/AdjustToContents`.
- [ ] **Step 3: Send flow.** On Send/Return: read `ChatInput.Text()`; if empty, ignore. Load key via `KeyStore::Load()`; if empty, `ChatLog.Insert` a "<raw>Set your Anthropic API key via the ⚙ button.</raw>" notice and return. Append the user line to `ChatLog` and to an in-memory `Array<AnthropicMessage> m_history`. Disable Send while busy. Construct a `ChatThread` (heap, owned member `AutoPointer<ChatThread>`), `Start()` it, and start a periodic `Timer` (0.2s) if not already running.
- [ ] **Step 4: Timer drain.** Timer handler (UI thread): if the thread exists and `TryTakeResult(r)` returns true → append `r.ok ? r.text : ("<raw>Error "+String(r.httpStatus)+": "+r.error+"</raw>")` to `ChatLog`, append assistant turn to `m_history` (only on ok), re-enable Send, destroy the thread, stop the Timer. Use `<raw>…</raw>` / `TextBox::PlainText` for all model text and errors to avoid tag injection.
- [ ] **Step 5: ⚙ handler.** On Config click: `ConfigDialog d; String k = d.Run( KeyStore::Load() ); if ( !k.IsEmpty() ) KeyStore::Save(k);` (ConfigDialog already saves on OK; keep it idempotent).
- [ ] **Step 6: Build clean.** `cmake --build build -j$(nproc)`; also run `bash test/run-selftest.sh` to confirm no regression (self-test still PASS).
- [ ] **Step 7: Commit** `feat(pi-copilot): chat panel — worker-thread Anthropic turn + Timer-marshaled reply`. Report that the panel is GUI-verified by the user (Task 5), not headless.

---

## Task 5: User GUI verification + README/verified update

**Files:** modify `modules/pi-copilot/README.md`.

- [ ] **Step 1** — Update README: document ⚙ Config (BYO key), the chat panel, non-streamed note, and that GUI is user-verified. Add build/test commands.
- [ ] **Step 2 (USER, in PixInsight — a hands-on step the controller cannot automate):** load the built `PICopilot-pxm.so` in PI, open Process ▸ PI Copilot, click ⚙, paste key, send "say hello", confirm the reply renders. Controller: present this as the verification handoff to the user; record the outcome the user reports in README "Verified".
- [ ] **Step 3** — Commit `docs(pi-copilot): increment-2 chat usage + verified run`.

---

## Self-Review

- **Spec coverage:** KeyStore→T1; AnthropicClient (non-streamed, worker-thread-safe)→T2; ConfigDialog/BYO-key→T3; chat panel + threading/Timer marshaling→T4; user GUI verification→T5. Streaming deferred (research + design-doc open risk) — noted, not dropped.
- **Threading:** POST + its download handler run on the worker thread and touch no GUI; only the Timer handler touches `TextBox`; cross-thread via Mutex — matches Thread.h/Timer.h constraints.
- **Testability honesty:** T1/T2 are headless-testable (T2 does a real gated API call); T3/T4 are GUI, human-verified in T5 — stated explicitly, not claimed as headless-green.
- **No masked failures:** non-2xx and missing-key both surface visibly.
- **Placeholder scan:** one implementer-verify item — the exact `OnDownloadDataAvailable` receiver mechanism for a non-`Control` `AnthropicClient` (T2 Step 3.3); the plan names the fallback and requires the choice be reported. Not a hidden TODO.
- **Security:** the real test key lives only in a gitignored `test/.test_api_key`, injected at test time, never committed.
