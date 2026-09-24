# PI Copilot Native PCL Module

This is the native PixInsight PCL implementation of PI Copilot, replacing the retired Node.js sidecar. It implements the core scripting engine and process management layer as a loadable PI module, enabling AI-assisted workflows directly within PixInsight through a dockable interface.

## Build

From `modules/pi-copilot/`:

```bash
cmake -B build -DPCLDIR=$HOME/PCL -DPICOPILOT_BUILD_MODULE=ON && cmake --build build -j$(nproc)
```

## Test

Self-test (proves `EvaluateScript==3`, `ProcessInstance` validity, a Settings round-trip, the worker-thread HTTP path, and request cancel/deadline):

```bash
bash test/run-selftest.sh
```

This signs the module and requires `/tmp/.pi_codesign_pass`. It covers:
- `EvaluateScript` execution: `1+2 == 3`
- `ProcessInstance` construction and validity
- Settings round-trip (throwaway key `PICopilot/SelfTestKey`, removed afterwards — never the stored API key)
- Worker-thread HTTP POST (invalid key → 401 from Anthropic API)
- Cancel and overall deadline against a local server that accepts and never replies (started by the harness): cancel mid-stall ends the turn with `request cancelled`; a 3 s deadline ends it with `request timed out after 3 s`
- Chat-log escaping: a literal `</raw>` in text stays literal in a real `TextBox`
- Real tiny Anthropic Messages API call (only if gitignored `test/.test_api_key` file is present with a valid key; otherwise skipped)

Also: `bash test/run-load.sh` — loads the module headlessly without running self-test.

## Design & Increment 1 Scope

Full specification: [`docs/superpowers/specs/2026-09-20-pi-copilot-native-pcl-design.md`](../../docs/superpowers/specs/2026-09-20-pi-copilot-native-pcl-design.md)

Increment 1 deliverables:
- Native module skeleton with CMake build
- Empty dockable `ProcessInterface` panel
- `EvaluateScript("1+2") == 3` proof of PJSR execution from C++
- Self-contained self-test shipped in-module. It runs on `ExecuteGlobal` **only** when the harness sets `PICOPILOT_SELFTEST_OUT`; in a normal install, executing the process just prints a console hint to open the panel — no Settings writes, no network, no files (harmless in production)
- Signing and headless loading verified

## Increment 2 — Text Chat

- **Config button** (⚙) opens a password-masked dialog; you paste your own Anthropic API key (BYO-key). The field is trimmed; a key containing spaces, line breaks or other non-printable-ASCII characters is rejected with an error and the dialog stays open (nothing saved).
- **Key storage**: the key is stored **in plaintext** in your PixInsight user settings (`PICopilot/AnthropicApiKey`). It is never shipped in the repo or package. **To clear it**, open ⚙, empty the field and press OK — the setting is removed.
- **Dockable panel** provides a chat log, input line (Return or Send button), and mode selector (Copilot / Advisor / Guided — selector only in this increment).
- **Requests** are non-streamed Messages API calls (default model `claude-opus-4-8`); the reply appears when complete, and the Send button reads "Thinking…" while a turn is in flight. Every text block of the reply is shown; a reply cut off by `max_tokens` ends with `[truncated: max_tokens]`. Streaming is a later increment.
- **Timeout / cancel**: each request has an overall **300 s** limit (not just a connect timeout), enforced from the transfer's progress callback; a stalled request ends with `request timed out after 300 s`. Closing PixInsight with a request in flight cancels it instead of waiting.
- **Threading**: the NetworkTransfer + response sink are constructed on the UI thread (PCL refuses to create a Control off the root thread — `CreateControl(): API function error`); a worker `pcl::Thread` performs only the blocking POST + parse; a 0.2 s UI `Timer` drains the result into the chat log.
- **Error handling**: a missing key shows a notice pointing to ⚙; a non-2xx shows `Error <status>: <API error message>` verbatim. A failed turn is dropped from the conversation history so user/assistant turns keep alternating, and its prompt is put back in the input line (if empty) for a resend.
- **GUI scope**: the panel and dialog cannot be tested headlessly (`--automation-mode` can't run GUI); they are verified by hand in PixInsight.

## Increment 3 — Vision (the panel sees the active view)

- **Include view** (checkbox, on by default): each message carries the active view — an auto-stretched JPEG preview (long edge ≤ 1024 px, quality 85; display only) plus a JSON context: view id, **file name only** (never the full path), geometry, per-channel median / raw MAD / mean / min / max of the **real (usually linear) data**, and the first 60 FITS keywords (values cut at 80 characters). **Location/identity keywords are never sent** (SITELAT, SITELONG, SITEELEV, OBSGEO-B/L/H, LAT-OBS, LONG-OBS, ALT-OBS, OBSERVER). There is no process-history field: PCL has no API for it.
- **Your image is never modified.** The preview reads the view read-only and block-averages it into a small copy (≈32 MiB for a 24 MP frame, never a full-resolution duplicate) → resample → auto-STF → JPEG via a temp file that is always deleted. The self-test proves the image byte-identical before/after for 32-bit float, 16-bit RGB and 16-bit mono.
- **Busy view** (locked by a running process or script) → the message is sent as text with a visible note, instead of waiting on the lock (which would freeze PixInsight).
- **No active image** → text only, with a visible note. A context or preview failure is shown in the chat log and the text still sends.
- **Token cost:** only the latest message carries an image; older turns keep a one-line note instead of the picture and a collapsed context (view id + geometry).
- **Placement:** PCL has no docking API. On first open after this update the panel is placed at the right edge, full height, of the primary screen (estimated from its centre — PCL exposes nothing else); after that, PixInsight remembers wherever you move it.
- **Process catalog** (`list_processes` / `describe_process` JSON from native introspection + compiled-in summaries) is built and self-tested; it becomes a model tool in increment 4.
- **Self-test** additions: vision smoke (ImageWindow/Bitmap/JPEG headless), ViewContext values + redaction, ViewPreview (JPEG, size, unchanged image, temp removed, red square decodes red; float, uint16, mono, >2048 px), busy-view capture, ProcessCatalog, request content-block shape, history stripping/collapse, placement maths, and a gated **real vision call** (synthetic red square, image only → model must answer exactly "red"). Key source: system keyring (`secret-tool lookup service anthropic account default`), then `test/.test_api_key`, else skipped. Tests run in an isolated PixInsight instance slot (`PICOPILOT_TEST_SLOT`, default 90, must be ≥ 50) so they never touch your own PixInsight settings.

## 0.1.0.4 — UTF-8 wire fix

- Any chat turn containing non-ASCII text (e.g. a model reply with "—" or "→" re-sent as history) failed with `Error 400: ... not valid UTF-8: surrogates not allowed`. Cause: `NetworkTransfer::POST(const String&)` is transmitted by the PI core one byte per UTF-16 code unit (low 8 bits). The body is now widened byte-for-byte from UTF-8 (`PostBytes`), and all outbound text uses our own surrogate-aware `U8()` because PCL's `String::ToUTF8` mis-encodes astral characters (📷 → 📽).
- This relies on undocumented PI-core transmit behaviour, proven by a loopback echo server that captures the wire bytes (self-test Section 8) plus a live two-turn check. **Re-run `bash test/run-selftest.sh` on every PixInsight upgrade** — if the core ever starts UTF-8-encoding POST bodies itself, Section 8 fails instead of users silently getting double-encoded text.

## Verified

**2026-09-20** — self-test PASS on built module:

```
{"evalResult":3,"evalOk":true,"processInstanceValid":true,"ok":true}PASS: EvaluateScript==3 and ProcessInstance valid
```

Full harness: signs module, loads headlessly under `PixInsight --automation-mode`, executes self-test, and exits with no interactive UI required.

**2026-09-23** — headless self-test PASS (Settings round-trip, worker-thread 401, cancel + deadline on a stalled connection, `</raw>` escaping). GUI chat: verified 2026-09-24 by the user on the released 0.1.0.2 installed from the repository URL (⚙ key entry → "say hello" → reply rendered in the panel).

**2026-09-24** — headless self-test PASS incl. real text chat and real vision round-trip (synthetic red square sent as the image only, no context → model answered "Red"). GUI: pending user verification (0.1.0.3 via repository pull).
