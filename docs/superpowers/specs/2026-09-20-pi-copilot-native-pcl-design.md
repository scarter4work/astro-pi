# PI Copilot — Native PCL Module Design

**Date:** 2026-09-20
**Status:** Draft for review
**Supersedes:** the Node sidecar architecture (`pi-copilot/sidecar/`) and
`pi-copilot/pi/PICopilot.js` (both become reference material, not shipped).

## 1. Goal

Ship PI Copilot as a **native, signed PixInsight PCL module** (`PICopilot-pxm.so`),
distributed and updated through the existing signed repository
(`https://raw.githubusercontent.com/scarter4work/astro-pi/main/repository/`),
exactly like NukeX. No Node runtime, no separate sidecar process, no localhost
hop, no API-key-at-launch problem.

The copilot is an in-app AI assistant that:
- Sees the active view (STF-stretched preview + geometry + stats + FITS keywords + process history).
- Converses with Claude (Opus 4.8) over the Anthropic Messages API.
- Acts on the image through native process application, with a guarded PJSR escape hatch.

## 2. Why native (settled)

Feasibility was spiked before committing:

- **Outbound HTTPS from PCL — PROVEN.** A live `NetworkTransfer` POST to
  `api.anthropic.com/v1/messages` from inside PixInsight returned `200` with the
  correct model reply. `NetworkTransfer` supports `SetURL`, `SetSSL(useSSL,
  forceSSL, verifyPeer, verifyHost)`, `SetCustomHTTPHeaders`, `POST(body)`, and
  chunked streaming via `OnDownloadDataAvailable`. The "no redirects" gotcha in
  project notes does not apply — the Messages endpoint is a direct POST.
  Transfers are **synchronous**, so the POST runs on a worker thread.
- **PJSR execution from C++ — API-confirmed.** `MetaModule::EvaluateScript(source,
  "JavaScript")` returns a `Variant`, runs in the core PJSR runtime with the full
  object model available, and **throws on syntax error / exception / invalid
  code** (native validation for free). Constraint: **root-thread only** — it
  throws if called from a running thread. Runtime-confirmed in build increment #1
  (not a throwaway).
- **Dockable panel — confirmed.** `ProcessInterface` (base `Control`) windows are
  inherently dockable in the PI workspace. `Features()` returns
  `InterfaceFeature::None` to suppress the default process control bar (the
  copilot is not a conventional apply-to-view process).

## 3. Execution model — Hybrid (settled)

The copilot exposes these tools to Claude:

| Tool | Native mechanism | Notes |
|---|---|---|
| `list_processes` | Grounding catalog | Ported from sidecar data |
| `describe_process` | Grounding catalog | Parameters + template for a process id |
| `get_view_context` | `ImageWindow::ActiveWindow` + `View`/`Image` + `FITSHeaderKeyword` + process history + STF preview | Built in-process, no round-trip |
| `apply_process(id, params)` | `ProcessInstance` by id → set params → `ExecuteOn(view)` / `ExecuteGlobal()` | **Primary path.** Deterministic, reversible via PI history, no eval |
| `run_pjsr(code)` | `MetaModule::EvaluateScript(code)` | **Escape hatch. Off by default.** Gated: pre-flight validate → user confirm dialog → execute on root thread |

**Default posture equals the "native-only" model**: out of the box the copilot
applies processes and cannot run arbitrary code. A single Settings toggle unlocks
`run_pjsr` for power users. Rationale (see decision log §10): `apply_process`
alone cannot express inspection-driven decisions, window/preview management, or
custom measurements — large parts of the PJSR object model have no
`ProcessInstance` form; the escape hatch restores full capability at near-zero
cost now that `EvaluateScript` provides both execution and validation.

## 4. Architecture / components

Each unit is independently understandable and testable.

- **`PICopilotModule`** — `MetaModule` subclass; `InstallPixInsightModule` entry
  point. Owns the `EvaluateScript` capability (root thread).
- **`PICopilotProcess`** — minimal `MetaProcess`; exists so the interface can
  register. Degenerate (no image-processing parameters of its own).
- **`PICopilotInterface`** — `ProcessInterface` (the dockable panel): chat log,
  input box, mode selector (copilot / advisor / guided), **⚙ Config** button,
  run-PJSR confirm dialog, streaming render. `Features() = InterfaceFeature::None`.
- **`AnthropicClient`** — builds Messages API requests, performs the synchronous
  `NetworkTransfer` POST on a worker `Thread`, parses responses, streams via
  `OnDownloadDataAvailable`. Ported from `sidecar/src/backends/claude.js`
  (message translation, tool schema, vision image-block handling).
- **`AgentSession`** — the tool-calling loop: message history, tool dispatch,
  PJSR-validate-retry, one-tool-per-step. Ported from `sidecar/src/agent/loop.js`.
- **`Grounding`** — native PI introspection: process catalog, `describe_process`,
  `get_view_context`. Replaces the sidecar's `pjsr_parser`-based grounding.
- **`ViewPreview`** — STF-stretched JPEG + base64 of a *copy* of the active view
  (never mutates the user's image). Ported from `PICopilot.js` `renderPreview`.
- **`KeyStore`** — persists the user's Anthropic key in PixInsight `Settings`
  (per-user, local, on disk, never in the repo or tarball).
- **`SystemPrompt`** — per-mode prompts. Ported from `sidecar/src/prompt/system.js`.

### Third-party (vendored, header-only — per repo vendoring policy)
- **nlohmann/json** — JSON build/parse. (Vendor `include/` + LICENSE only.)
- Base64: use PCL's built-in facilities if available; otherwise vendor a
  header-only encoder. (Confirm in increment #1.)

## 5. Threading model

- **Root/UI thread:** panel rendering, `EvaluateScript` (root-thread-only),
  `ProcessInstance` execution, all `View`/`Image` reads.
- **Worker `Thread`:** the synchronous `NetworkTransfer` POST + streaming.
- **Loop:** user message → worker thread HTTP → response parsed → if the response
  is a tool call, marshal to root thread → execute (`apply_process` /
  `run_pjsr` / read `get_view_context`) → result → back to worker thread for the
  next turn. Streamed assistant text is marshalled chunk-by-chunk to the UI.

## 6. Key handling (BYO-key)

- The distribution ships **code only — never a key.**
- **⚙ Config** dialog: user pastes their own `sk-ant-…` key → stored in PI
  `Settings`. Each user (Scott included) supplies their own key.
- The key never leaves the process, is never logged, and travels only in the
  `x-api-key` header to `api.anthropic.com` over TLS.
- Loud, clear state: if no key is set, the panel says so and points at ⚙ Config —
  never a silent failure or a mid-request error.

## 7. Data flow (vision)

Native and round-trip-free: `get_view_context` reads the active window directly,
`ViewPreview` STF-stretches a copy and renders a JPEG, and `AnthropicClient`
embeds it as an Anthropic `image` content block — the same vision path proven in
the sidecar, minus the HTTP shuttling of `previewJpegBase64`.

## 8. Error handling

- **No key / bad key:** surfaced in the panel with the fix (⚙ Config). 401 from
  Anthropic is shown verbatim, not masked.
- **Bad PJSR:** `EvaluateScript` throws → caught → fed to the validate-retry loop
  (max retries), then reported. `pjsr_parser` pre-flight gives friendlier errors
  before the confirm dialog and prevents partial runs.
- **Network failure:** `NetworkTransfer` failure / non-2xx surfaced with the real
  status and body. No mock fallbacks.
- **Infinite-loop guard:** `EvaluateScript` warns it can hang the platform — the
  confirm dialog is the human gate; `run_pjsr` stays off by default.

## 9. Packaging & release (NukeX-identical)

- Build `PICopilot-pxm.so`; sign with `PixInsight.sh --sign-module-file`.
- New step in `release.sh`: build → native-sign → package tarball
  (`bin/PICopilot-pxm.so` + `.xsgn`) → rewrite sha1/date → declare in the
  `type="module"` platform block of `updates.xri` → sign manifest LAST →
  integrity-check.
- Version macro in `modules/pi-copilot/src/.../PICopilotVersion.h` (bump before build).
- Commit version bump + `repository/` artifacts together, then push.
- **NEVER `make install`.** Validated by pulling the signed release from the repo
  URL in PI (ship-then-test, per project convention).

## 10. Decision log

- **Native PCL over Node sidecar** — eliminates runtime install, process
  management, localhost hop, key-at-launch; grounding becomes direct and reliable.
- **Dockable panel over modal dialog** — fixes modal/focus pain; a copilot must
  live alongside the work.
- **Hybrid over native-only (B)** — `apply_process` is the safe primary path, but
  cannot reach inspection/window/measurement work or novel compositions without a
  signed release per capability. Pure B's safety is also partly illusory
  (PixelMath is itself an expression executor). `EvaluateScript` collapsed the
  escape hatch's cost (execution + validation in one documented call), so hybrid
  wins. `run_pjsr` ships **off by default** so the out-of-box posture equals B.

## 11. Risks / open details (resolve during build)

1. Base64 encoder source (PCL built-in vs vendored) — confirm in increment #1.
2. PI `Settings` API specifics for `KeyStore` — confirm in increment #1.
3. Streaming UX: chunk marshalling cadence from worker thread to UI without
   flicker; fall back to non-streamed if needed for v1.
4. Process parameter catalog for `describe_process` / `apply_process` — source
   and PI-version sync strategy (reuse `modules/pi-copilot/data/process-summaries.json` /
   `modules/pi-copilot/data/dump-process-catalog.js` as the seed — preserved from the
   retired sidecar tree, which was deleted 2026-09-20).

## 12. Build decomposition (increments)

1. **Module skeleton + execution proof.** Buildable `PICopilot-pxm.so` with an
   empty dockable panel; smoke test asserts `EvaluateScript("1+2")==3` and a
   trivial `apply_process`. Proves both execution paths in kept code.
2. **AnthropicClient + KeyStore + Config dialog.** End-to-end text chat with
   BYO-key; worker-thread HTTP; streaming.
3. **Grounding + get_view_context + ViewPreview.** Vision round-trip.
4. **AgentSession + tool loop + apply_process.** The hybrid primary path.
5. **run_pjsr escape hatch** (validate → confirm → EvaluateScript), off-by-default toggle.
6. **Packaging + release.sh + updates.xri**, signed and repo-pull tested.

Each increment gets its own plan via the writing-plans skill.
