### Task 11 (v2): Sidecar ops — spec §2.5's BATCHED protocol

> **This brief SUPERSEDES `task-11-brief.md`.** The original was written against a
> server-side loop (`optimize_step` calling `advance(session, NumpyExecutor(), ...)`),
> which contradicts spec §2.5 lines 108-128. Read the original only for its test
> style; its `_op_optimize_step` body is **wrong** and must not be copied.

**Files:**
- Modify: `src/autocontrast/sidecar.py` (add handlers, register in `_OPS`)
- Modify: `src/autocontrast/optimize/loop.py` (split the iteration; see below)
- Modify: `src/autocontrast/optimize/session.py` (persist in-flight batch state)
- Test: `tests/test_optimize_sidecar.py`

---

## Why this task exists in this shape

Spec §2.5 (lines 108-128) specifies the production protocol:

```
PJSR                          sidecar (spawned per call)
────                          ─────────────────────────
optimize_begin  ──────────►   load image, resolve reference, measure baseline
                ◄──────────   session_id + instruction BATCH 1
apply N processes in PI
save N candidates
optimize_step   ──────────►   measure all N, guardrail, score, prune beam
                ◄──────────   BATCH 2  |  or  converged + recipe
```

**The sidecar never applies pixels in production.** PixInsight does. The sidecar
measures, guardrails, scores, prunes.

A "PixInsightExecutor" implementing `executor.py`'s `apply(rgb, action) -> rgb`
protocol was considered and **rejected on cost**: `apply` is synchronous and
one-candidate-at-a-time, so the sidecar would have to launch a headless PixInsight
per candidate — ~540 launches for a 20-iteration run at the measured ~27
candidates/iteration. Batching those launches to avoid it reconstructs exactly this
protocol. Do not reintroduce that design.

---

## HARD CONSTRAINT: `advance()` is not deleted

`loop.advance()` and `loop.run_to_convergence()` stay exactly as they are, driving
`NumpyExecutor`. They are the offline/CI path, and Tasks 12 and 14 depend on them.

The batched ops are a **second entry point over the same proposer, guardrails,
scoring, and pruning**. If you find yourself copying logic out of `advance()` into a
new function, stop — extract the shared part and have both call it. A second copy of
the guardrail/score/prune block is the single worst outcome of this task. Note that
Task 10's fix round just removed a duplicate of exactly this kind (`_ranked_menu`
rebuilding `propose.py`'s menu, commit `df577e7`) — do not create a new one.

---

## The contract you must implement

### `optimize_begin`

Request: `{op, image, work_dir, reference_id, reference_fingerprint,
pixel_scale_arcsec, psf_fwhm_arcsec, palette_class, n_scales?, max_dim?,
session_id?}`

Behavior: open the session via `loop.begin(...)`, measure the baseline, persist to
`session_path(work_dir, session_id)`, then plan and return the first instruction
batch.

Response `result`: `{session_id, baseline_distance, reference_id, instructions,
converged, iteration}`.

### `optimize_step`

Request: `{op, work_dir, session_id, image, produced: [{instruction_id, path}, ...]}`

- `image` is REQUIRED and must be passed to `loop.resume(path, proxy_path=image)`.
  **Do not call `Session.load` directly.** `resume()` performs the exact-path guard
  that Task 9's ruling deliberately placed at this call site; bypassing it makes that
  guard dead code at the one seam it exists for.
- `produced` carries the candidates PixInsight actually saved. An instruction absent
  from `produced` means PixInsight failed to produce it — that is an **executor
  failure for that candidate only**, logged to `guardrail_log` with
  `"failed": ["executor"]` and a reason, exactly as `advance()` already does. It must
  not kill the run.
- A missing session file is a loud `FileNotFoundError` naming the id — never a silent
  fresh start, which would discard a run's progress (§12).

Behavior: ingest the produced candidates (no-op check → guardrails → score), then
either return a **supplementary batch** for the same iteration, or close the
iteration (prune → update best → check convergence) and return the next iteration's
batch, or report convergence.

Response `result`: the full `outcome(session)` dict plus `{iteration, converged,
instructions}`.

### Instruction shape

Each instruction must carry everything PixInsight needs to execute one action and
save one candidate, and everything the sidecar needs to reassociate the result:

`{instruction_id, branch_key, action_key, process, params, parent_path,
candidate_path}`

`process` and `params` come from the existing `Recipe.to_pixinsight_steps()` /
`Action` serialization — do not invent a second serialization format for actions.

---

## The part that is genuinely hard — read carefully

`advance()` holds per-branch `kept`, `attempts`, and its position in the ranked menu
as **locals** across its inner loop (`loop.py` around lines 182-251). In the batched
protocol those locals must survive a round-trip through PixInsight.

A branch walks the ranked menu until it holds `config.top_k` **LIVE** candidates or
until `config.max_attempts` actions have been tried. Liveness is knowable only AFTER
guardrails run on produced pixels. **Therefore a batch cannot know up front how many
candidates will survive.** An iteration whose guardrails trip needs a *supplementary*
batch. "One batch per iteration" is the nominal case, not an invariant.

Required invariants:

- `max_attempts` bounds the attempts across **all batches of one iteration**, not per
  batch. The existing semantics hold: an attempt is counted when the instruction is
  ISSUED, not when it succeeds (`advance()` counts before the executor runs, and the
  reason is in the comment there).
- When the cap binds, emit the same `noted: ["attempt_cap"]` log entry keyed by
  branch that `advance()` emits. Requirement (D) from Task 10 must survive this
  refactor — silent truncation would read as "explored fully" when it was not.
- Passing guardrail verdicts carrying a non-empty reason must still be logged under
  `noted` (requirement A). No-op candidates (pixel-identical to parent) must still be
  discarded before they can take a beam slot (requirement B).

**Suggested simplification, which you may adopt or better:** the ranked menu is
deterministic given (parent pixels, reference fingerprint, `applied_kinds`). So you
need persist only the per-branch **cursor**, `kept`, and `attempts` — not the menu
itself, and not the ranked action list. Recomputing the menu costs one parent
re-measure per batch; a task immediately after this one (Task 16) caches exactly that
measurement. If you choose differently, justify it in your report.

Session state you add must round-trip through `Session.save`/`Session.load`, and you
must test that it does — Task 9's contract is that every field survives.

---

## Method

Test-driven, and the tests must fail first for the right reason.

Cover at minimum:
1. An unknown op lists both new ops in its error.
2. `optimize_begin` opens a resumable session, writes the session file, and returns a
   non-empty first batch.
3. A full `begin → produce candidates → step` round-trip advances the iteration.
   Produce the candidates by applying `NumpyExecutor` **in the test** — the test
   plays PixInsight's role. This is the key structural proof: the sidecar scored
   candidates it did not itself produce.
4. `optimize_step` on a missing session id is a loud error naming the id.
5. `optimize_step` with a different `image` than the session was begun with is
   REFUSED by `resume()`'s exact-path guard.
6. An instruction missing from `produced` is logged as an executor failure for that
   candidate and does not kill the run.
7. The attempt cap bounds attempts across a supplementary batch, and the
   `attempt_cap` notice still appears.
8. Added session state survives `save`/`load`.

Existing tests must not regress — `tests/test_sidecar.py`, `tests/test_optimize_loop.py`,
and `tests/test_optimize_session.py` especially.

**Two brief defects inherited from v1, already ruled on — do not reproduce them:**
- `f.ptp()` is removed from ndarray in NumPy 2.5. Use `np.ptp(f)`.
- `FingerprintData` and `load_raster` are ALREADY imported at `sidecar.py:34` and
  `:36`. Do not add duplicate imports.

---

## Global constraints

- Branch `phase2-optimizer`. Do not commit to master.
- No AI anywhere in this plan. Action ranking is heuristic.
- Input must be already-stretched. Linear input is a loud error, never a silent
  auto-stretch (§12).
- Guardrail violations discard candidates; they are never a score term (§7).
- Chroma actions only when the palette gate admits chroma (§2.3).
- No action below the image's PSF FWHM resolvable limit (§2.2, §4.4).
- No test may be deselected to make the suite green.
- **No tunable may be adjusted** (`top_k`, `width`, `max_attempts`, `epsilon_improve`,
  `iteration_cap`, `epsilon`) — spec §3.7 forbids it without re-running all
  exit-criterion tests, which have not been written yet.
- Every degraded path logs and surfaces (§12). No silent fallbacks.
- Sidecar response contract unchanged: `{"ok": true, "op":…, "result":…}` /
  `{"ok": false, "op":…, "error":…}`.

## Verification

Run and paste real output:
- `.venv/bin/python -m pytest tests/test_optimize_sidecar.py -v`
- `.venv/bin/python -m pytest tests/test_sidecar.py tests/test_optimize_loop.py tests/test_optimize_session.py -v`
- `.venv/bin/python -m pytest -q` — report the count, confirm nothing deselected.

Test output must be pristine; warnings are findings.

## Report

Append your report to `.superpowers/sdd/2026-07-27-phase2-optimizer/task-11-report.md`.
Reply with only: status, commit SHA(s), one-line test summary, concerns.
