# PI Copilot

In-app AI chat for PixInsight that writes and runs PJSR against your active view — local-first
(Ollama), with a human confirm-before-run gate and full undo.

> **Status:** first cut built — Phase 0 gate green (battery 5/5). See design §11 and
> [`../docs/superpowers/plans/2026-06-23-pi-copilot-phase0.md`](../docs/superpowers/plans/2026-06-23-pi-copilot-phase0.md).

- **Design:** [`docs/2026-06-13-pi-copilot-design.md`](docs/2026-06-13-pi-copilot-design.md)
- **Default model:** `qwen3-coder:30b` (Ollama) — fallback `qwen3:14b`
- **Architecture:** PixInsight PJSR thin client (UI + execution + confirm gate) ↔ Node sidecar
  (model agent loop + `pjsr_parser` grounding + pluggable backend)

Not a fine-tuned astro model — there isn't one for *processing*. PI competence comes from grounding
the model with the existing `pjsr_parser` (validation in the loop) plus a PixInsight-introspected
process catalog, not from training.

## Running (Phase 0 + thin PI dialog)

1. **Sidecar:** `cd sidecar && npm install && npm start` (serves `http://localhost:8765`, loopback only).
2. **Brain gate (real Ollama):** `cd sidecar && npm run battery` — all prompts must produce validated PJSR.
3. **Unit tests (no Ollama):** `cd sidecar && npm test`.
4. **In PixInsight:** open an image, run `pi/PICopilot.js`, chat. Generated PJSR is shown and only
   runs after you confirm; each run lands as one undo step.

Regenerate the process catalog after a PixInsight/module update (use `-n` so a running GUI session
is not affected):

```bash
PixInsight.sh -n --automation-mode --force-exit --default-modules \
  "-r=pi/dump-process-catalog.js,summaries=pi/process-summaries.json,out=sidecar/data/process-catalog.json"
```
