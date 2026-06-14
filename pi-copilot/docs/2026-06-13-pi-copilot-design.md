# PI Copilot — Design

**Status:** Approved — building first cut (Phase 0 + thin PI dialog)
**Date:** 2026-06-13 (build decisions resolved 2026-06-14)
**Working name:** "PI Copilot" (provisional — rename freely)
**Home:** `astro-pi/pi-copilot/`

---

## 1. Vision

An **in-app AI chat inside PixInsight** that doesn't just give advice — it **writes and runs
PJSR on the fly** against your active view. You type a goal in plain English ("remove the green
cast and do a gentle stretch"); the agent writes the PJSR, shows it to you, and on your click
executes it on the image (undoably).

The key differentiator from existing tools (see §3): this is a **full agent that executes**, not
an advisor, and it runs **in-app** rather than driving PixInsight from the outside.

## 2. Goals / Non-Goals

**Goals**
- Natural-language → executed PixInsight processing, in-app, with a human confirm gate.
- **Local-first**: works with zero cloud account via Ollama. No Claude/OpenAI sub required.
- Backend is **pluggable** — local Ollama by default, optional remote (Claude/OpenAI) for users
  who want max capability.
- Grounded into "PixInsight's worldview" cheaply via the existing **`pjsr_parser`** (validation +
  templates + class info), not an up-front fine-tune.
- Every executed action is **undoable** and **previewed as code first**.

**Non-Goals (YAGNI for v1)**
- No autonomous unattended pipeline (the headless harness already covers batch automation).
- No fine-tuned/custom model in v1 — grounding first; LoRA is a later polish step (§9).
- No multimodal "look at the image pixels" in v1 — context is numeric stats + history, not vision.
- No multi-view / project-wide orchestration in v1 — operate on the **active view**.

## 3. Prior Art (and how we differ)

| Tool | What it does | Gap we fill |
|------|--------------|-------------|
| **LLM Assistant** (Stirling Astrophoto) | In-app, bring-your-own local/remote LLM, **advises** | Doesn't execute; we run PJSR |
| **PIAdvisor** | Native PI module, workspace-aware **guidance** | Advisory only; we act |
| **pixinsight-mcp** | MCP server drives PI **from outside** an assistant | External; we're in-app + agentic |
| *Scott's headless harness* | Autonomous Claude-driven PI via file IPC | Batch/unattended; we're interactive |

Action item before building: spend ~10 min with LLM Assistant's UI to borrow proven chat-panel
patterns instead of reinventing them.

## 4. Model Choice

Priority for this use case is **tool-calling + code generation**, NOT astronomy-science knowledge.
(There is **no** model fine-tuned on astrophotography *processing* / PJSR. AstroSage / AstroLLaMA /
AstroLLaVA are trained on astro-ph research papers — they know the science, not PixInsight.)

Target hardware: RTX 5070 Ti, 16 GB VRAM.

| Model | Role | Notes |
|-------|------|-------|
| **`qwen3-coder:30b` (A3B MoE)** | **Default** | ~3.3B active → fast; RL-tuned for agentic coding; tool-calling fixed in Ollama. Tight at 4-bit (~16–18 GB), may need slight CPU offload. |
| **`qwen3:14b` (dense)** | Fallback | Explicit tool-use, 128K ctx, comfortable (~9–10 GB at 4-bit). |
| `qwen2.5-coder:14b` | Last resort | Best raw code completion but weaker tool-calling. |

The grounding layer (§7) matters more than the exact model: with `pjsr_parser` validating in the
loop, even the 14B reliably emits *runnable* PJSR.

**Decided (2026-06-14):** `qwen3-coder:30b` pulled and set as the default (18 GB, installed). The
sidecar takes the model name as config, so swapping to the `qwen3:14b` fallback is one setting.

## 5. Architecture

Two processes, one clean interface. **PixInsight drives the loop; the sidecar wraps the model.**

```
┌─────────────────────────────┐         ┌──────────────────────────────┐
│  PixInsight  (PJSR script)  │         │  Sidecar  (Node)             │
│  ─ chat Dialog UI           │  HTTP   │  ─ POST /chat   {session,msg}│
│  ─ executes PJSR (ONLY here)│ <─────> │  ─ POST /continue {tool_res} │
│  ─ confirm gate + undo wrap │ localhost│  ─ model agent loop          │
│  ─ get_view_context()       │  :PORT  │  ─ pjsr_parser grounding     │
└─────────────────────────────┘         │  ─ backend: ollama|claude|... │
                                        └──────────────────────────────┘
                                                     │
                                          ┌──────────┴───────────┐
                                          │ Ollama (default)     │
                                          │ or Claude/OpenAI API │
                                          └──────────────────────┘
```

**Why this split**
- Only PI can run PJSR, and you want to approve code before it runs → PI must own execution and
  the confirm gate. The sidecar never executes anything against your images.
- The agent loop (multi-turn, tool-use, streaming, secrets, MCP/`pjsr_parser`) is painful in PJSR's
  blocking `NetworkTransfer` but trivial in Node → put it in the sidecar.
- Pluggable backend lives entirely in the sidecar; the PI script never knows which model is behind it.

### 5a. Protocol (request/response, PI-driven)

1. User types a message in the PI chat dialog.
2. PI → `POST /chat {sessionId, message}`.
3. Sidecar runs the model one step. It returns **one of**:
   - `{type: "message", text}` → PI renders it; turn ends.
   - `{type: "tool_use", tool: "run_pjsr", code}` → PI shows the code in the chat with **[Run] [Skip]**.
   - `{type: "tool_use", tool: "get_view_context"}` → PI gathers context, no user prompt needed.
4. On **Run** (or auto for read-only tools), PI executes and calls
   `POST /continue {sessionId, toolResult}`.
5. Loop to step 3 until a `message` ends the turn.

Conversation state is keyed by `sessionId` in the sidecar. (Streaming tokens are a later
enhancement — v1 shows a "thinking…" state per step.)

### 5b. Tools the model can call

| Tool | Executed by | Returns |
|------|-------------|---------|
| `get_view_context` | PI script | active view id, geometry, channel stats (median/MAD/min/max), FITS keywords, process history |
| `run_pjsr(code)` | PI script (after confirm) | console output, success/error, post-run image stats diff |
| `list_processes` / `describe_process(id)` | sidecar via `pjsr_parser` | class info, parameter schemas, templates (grounding — no PI round-trip) |

`run_pjsr` is the only mutating tool and the only one gated behind a human click.

## 6. Execution Safety

Full-agent power, fenced by three rules:
1. **Code-before-run**: the generated PJSR is always shown in the chat; nothing mutates until you
   click **Run**. (This is the [Run] / [Skip] gate from §5a.)
2. **Undoable**: every `run_pjsr` execution wraps the target view in
   `view.beginProcess()` / `view.endProcess()` so it lands as a single PixInsight undo step.
3. **Validated**: before code is offered to you, the sidecar runs it through `pjsr_parser` validate;
   on failure the model self-corrects (bounded retries) rather than handing you broken code.

No silent fallbacks: if execution throws, the real PJSR/console error is surfaced verbatim in the
chat and fed back to the model — never swallowed.

## 7. Grounding ("turning it to PI's worldview")

Cheapest path to a PI-competent agent, no fine-tune required:
- **System prompt**: a curated PixInsight process catalog + PJSR idioms + "house style" (your
  processing philosophy — e.g. preferred stretch approach, SCNR conventions).
- **`pjsr_parser` in the loop**: the model can look up exact class info / parameter schemas /
  templates, and all emitted code is validated before you see it. This is the load-bearing piece —
  it makes a modest local model produce correct, runnable PJSR.
- **Few-shot examples** drawn from the EZ-Stretch / NukeX PJSR you already ship.

## 8. Error Handling & Testing

**Error handling**
- Sidecar unreachable → PI dialog shows a clear "start the copilot sidecar" message (with the command), not a silent hang.
- Model emits invalid PJSR after N self-correct attempts → surfaces the last validation error and asks you how to proceed.
- PJSR runtime error → real error fed back to the model + shown to you.

**Testing**
- **Sidecar**: unit-test the agent loop + tool dispatch + `pjsr_parser` grounding with a mock model,
  then a CLI harness that runs real Ollama against a battery of prompts ("stretch", "SCNR",
  "background extraction") asserting the emitted PJSR *validates*.
- **PI script**: manual + scripted PJSR tests for `get_view_context` and the `beginProcess`/undo wrap
  on a throwaway view.
- **End-to-end**: a fixed prompt set run against a sample `.xisf`, asserting the view's process
  history changes as expected.

## 9. Phasing

- **Phase 0 — Prove the brain (no PI).** Sidecar + Ollama + tool schema + `pjsr_parser` grounding,
  driven by a CLI. Success = `qwen3-coder:30b` reliably emits *validating* PJSR for a prompt battery.
- **Phase 1 — In-app MVP.** PJSR chat dialog, localhost protocol, `get_view_context`, `run_pjsr` with
  confirm gate + undo wrap. Local Ollama only.
- **Phase 2 — Backend + polish.** Pluggable cloud backend (Claude/OpenAI), token streaming, richer
  context, conversation persistence.
- **Phase 3 — Deepen the worldview.** Optional LoRA on PJSR scripts + processing walkthroughs;
  expanded process-catalog RAG.

## 10. Open Questions — RESOLVED (2026-06-14)

1. **Sidecar packaging** → documented `npm start` for v1. Auto-spawn from the PI script deferred.
2. **`pjsr_parser` coupling** → **in-process library.** Confirmed `~/projects/EZ-suite-bsc/pjsr_parser`
   exports `PJSRAnalyzer` (`.analyze()` / `.validate()` / `.getTemplates()`). No third process.
3. **Name** → keep "PI Copilot" for now.
4. **Distribution** → **separate GitHub release.** It carries a Node sidecar, so it is not a signed
   PI repository artifact like NukeX / EZ scripts. Built later; not part of this cut.

---

## 11. Build Design — first cut (Phase 0 + thin PI dialog)

Scope for this cut: prove the brain (Phase 0) **and** stand up a thin PI dialog so the localhost
protocol is exercised end-to-end early. No backend pluggability beyond Ollama, no streaming, no
conversation persistence (those are Phase 2).

### 11a. Module layout

```
pi-copilot/
  sidecar/
    package.json            # ES module; minimal deps (built-in http; Ollama via fetch)
    src/
      server.js             # HTTP: POST /chat, POST /continue, GET /health
      agent/loop.js         # per-session state machine + tool dispatch (the "brain")
      backends/ollama.js    # Ollama chat + tool-calling; backends/index.js = factory
      grounding/pjsr.js     # thin wrapper over PJSRAnalyzer (validate, class info, templates)
      prompt/system.js      # system prompt: PI process catalog + PJSR idioms + house style
    test/
      loop.test.js          # node:test — agent loop + dispatch with a MOCK model (no Ollama)
      battery.js            # CLI harness: real Ollama vs prompt battery, asserts PJSR validates
  pi/
    PICopilot.js            # thin PixInsight chat dialog (second client of the same protocol)
```

### 11b. Protocol (client-driven — identical for CLI and PI)

- `POST /chat {sessionId, message}` → runs one model step.
- `POST /continue {sessionId, toolResult}` → feeds a tool result back, runs the next step.
- `GET /health` → `{ok, model, backend}` so the PI dialog can warn "start the sidecar" not hang.

Per-session state (keyed by `sessionId`): message history incl. tool calls/results + a retry counter.
Each step returns exactly one of:
- `{type:"message", text}` — turn ends.
- `{type:"tool_use", tool:"run_pjsr", code}` — **already validated**; client shows `[Run]/[Skip]`.
- `{type:"tool_use", tool:"get_view_context"}` — read-only; client runs it and auto-`/continue`s.

### 11c. Tool contracts

| Tool | Runs where | Gated? | Contract |
|------|-----------|--------|----------|
| `list_processes` | sidecar (pjsr_parser) | no | `→ [{id, summary}]` |
| `describe_process(id)` | sidecar (pjsr_parser) | no | `→ {paramsSchema, template, classInfo}` |
| `get_view_context` | client | no (read-only) | `→ {viewId, geometry, channelStats, fitsKeywords, history}` |
| `run_pjsr(code)` | client | **yes** | sidecar validates first → client runs in `beginProcess/endProcess` → `→ {ok, consoleOutput, error?, statsDiff?}` |

**Validation before offering (load-bearing, §6.3):** when the model emits `run_pjsr`, the sidecar runs
`PJSRAnalyzer.validate()` *before* returning it. On failure it feeds the error back to the model
(bounded retries, default 3) and only surfaces `tool_use:run_pjsr` once it validates — otherwise it
surfaces the last error verbatim. No silent fallback.

### 11d. Testing

- **Sidecar unit (`loop.test.js`, no Ollama):** mock backend returns scripted tool-calls; asserts
  grounding tools dispatch in-process, `run_pjsr` is validated before surfacing, invalid code triggers
  self-correct retry, and retry exhaustion surfaces the real error.
- **Prompt battery (`battery.js`, real Ollama):** drives the sidecar over a fixed prompt set ("gentle
  stretch", "SCNR green removal", "background extraction", …). **Success bar = emitted `run_pjsr`
  code validates.** This is the Phase 0 gate.
- **Thin PI dialog:** manual-test only this cut — proves the protocol round-trips against a throwaway
  view, including `get_view_context` and the `beginProcess`/undo wrap.

### 11e. Build order

grounding wrapper → backend → agent loop → server → mock tests → battery (validate the brain) →
PI dialog (validate the protocol).
