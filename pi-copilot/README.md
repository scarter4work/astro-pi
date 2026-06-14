# PI Copilot

In-app AI chat for PixInsight that writes and runs PJSR against your active view — local-first
(Ollama), with a human confirm-before-run gate and full undo.

> **Status:** design approved — building first cut (Phase 0 + thin PI dialog). See design §11.

- **Design:** [`docs/2026-06-13-pi-copilot-design.md`](docs/2026-06-13-pi-copilot-design.md)
- **Default model:** `qwen3-coder:30b` (Ollama) — fallback `qwen3:14b`
- **Architecture:** PixInsight PJSR thin client (UI + execution + confirm gate) ↔ Node sidecar
  (model agent loop + `pjsr_parser` grounding + pluggable backend)

Not a fine-tuned astro model — there isn't one for *processing*. PI competence comes from grounding
the model with the existing `pjsr_parser` (validation + templates), not from training.
