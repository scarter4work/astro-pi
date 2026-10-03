# Task 10 review — the loop, orchestration, convergence, and the fail-safe

**Range reviewed:** `094133f..9c5e19c` (f1c8e41 initial, eb2ac3e spec, 9c5e19c fix round)
**Files:** `src/autocontrast/optimize/loop.py` (new, 351 lines), `tests/test_optimize_loop.py` (new, 575 lines), `src/autocontrast/optimize/beam.py` (+26), `docs/superpowers/specs/2026-07-27-phase2-optimizer-design.md` (+34/-3)

---

## Spec Compliance

**❌ Issues found.** The brief's interface and all four carried requirements are implemented and tested. Three gaps remain: no session persistence, verbatim duplication of `propose.py`'s menu construction, and a spec table whose own arithmetic contradicts the budget it argues from.

### Brief interface and global constraints

| Requirement | Where | Verdict |
|---|---|---|
| `begin / advance / run_to_convergence / outcome` | `loop.py:52`, `:139`, `:325`, `:332` | ✅ |
| Fail-safe seeded with the input | `loop.py:65` (`root = Branch(..., image_path=proxy_path, distance=baseline)`); replacement only on strict improvement `loop.py:298-300`; decline returns `proxy_path` + `Recipe.empty()` `loop.py:335-343` | ✅ |
| Guardrail violations discard, never score (§7) | `loop.py:246-254` — `continue`; there is no penalty term anywhere in `advance`, the only score is `fingerprint_distance` at `loop.py:259` | ✅ |
| No AI | ranking is `propose_actions`; no model call, no new dependency | ✅ |
| Chroma gated by palette (§2.3) | `loop.py:123-131` passes `palette_compatible=compatible` into `available_actions`, mirroring `propose.py:135-145` | ✅ |
| No action below PSF FWHM (§2.2/§4.4) | `loop.py:128` `psf_fwhm_arcsec=max(reference_fp…, parent_fp…)` — the same expression `propose.py:141` uses, so the loop cannot widen the menu past the resolvable limit | ✅ |
| Every degraded path logs (§12) | executor failure `loop.py:196-201`, no-op `loop.py:217-224`, guardrail trip `loop.py:249-254`, passing-with-reason `loop.py:240-244`, attempt cap `loop.py:271-279` | ✅ |
| Branch is `phase2-optimizer`, not `master` | commit list in the review package | ✅ |
| No test deselected | reported run: `21 passed`, `387 passed`, nothing skipped/xfailed; output pristine, no warnings | ✅ |
| No tunable adjusted to rescue a test (§3.7) | `epsilon_improve`, `epsilon`, `iteration_cap`, `top_k`, `width` all unchanged in `beam.py:17-22`; the only new constant is `max_attempts`, introduced by spec commit eb2ac3e rather than to fix a test | ✅ |

### The four requirements absent from `task-10-brief.md`

#### (A) Passing guardrail verdicts carrying a non-empty reason are surfaced — ✅ IMPLEMENTED

`loop.py:238-244`:

```python
noted = [v for v in verdicts if v.ok and v.reason]
if noted:
    session.guardrail_log.append({
        "iteration": iteration, "action": action.key,
        "noted": [v.name for v in noted],
        "reason": "; ".join(v.reason for v in noted),
    })
```

- Keyed by **field** (`noted` vs `failed`), so a reader never has to parse a string to tell an observation from a discard.
- Recorded **before** the `failed` check (`loop.py:246`), so a candidate that notes *and* trips produces both entries — correct, since "star integrity was never evaluated" is equally true of a candidate another guardrail went on to reject.
- Written against the property (`v.ok and v.reason`), not against `check_star_integrity` by name.
- Tests: `tests/test_optimize_loop.py:287` (positive — starless fixture, asserted with `detect_stars(...).count == 0` at `:296` rather than trusted) and `:314` (negative — a starry fixture, with `assert session.branches` at `:331` so "nothing noted" is not vacuous).
- Both tests filter on `"star_integrity" in e.get("noted", [])` (`:295`, `:323`) rather than on the presence of the field, so the fix round's reuse of `noted` for `attempt_cap` cannot make them pass on the wrong event. That tightening was made without being asked.

#### (B) No-op candidates are discarded so they cannot burn a beam slot — ✅ IMPLEMENTED (ordering half unaddressed, see Minor #4)

`loop.py:203-224` — `if np.array_equal(produced, parent):` → log `failed: ["no_op"]`, `continue`. Placed **before** `evaluate_guardrails`, which is right: guardrailing an image identical to the baseline it is compared against has a foregone result.

- The check is the **property** (pixels unchanged), never `action.kind == "star_split"`. This matters concretely: `star_split` is a no-op only because `NumpyExecutor` has no starless layer to route; the PixInsight executor's `star_split` does change the image and must survive.
- I verified the mechanism the requirement described, at its source: `beam.py:68` is `sorted((c for c in candidates if c.alive), key=lambda b: b.distance)` — a **stable** sort — and `beam.py:69` dedupes by `branch.recipe.key`, which a no-op's recipe does not share with its parent. So on an already-good image, where the parent's distance is the minimum available, the no-op ties the floor, sorts first, and takes the top slot. The implementer's claim is correct as stated.
- Tests: `:483` asserts `"star_split" not in kinds` and that the discard is recorded with a reason; `:506` asserts **both** directions via `_NoOpFor("chroma")` (a non-`star_split` no-op *is* discarded) and `_EffectiveStarSplit` (a `star_split` that really changes pixels is *not*).
- Acknowledged consequence, correctly reasoned: the no-op never enters `applied_kinds`, so it is re-proposed each iteration. Recording a pixel-inert action in the recipe would put a lie in the §12 audit artifact; one wasted executor call per branch per iteration is the cheaper error.

#### (C) `resume()` does an EXACT `proxy_path` comparison — ✅ IMPLEMENTED

`loop.py:99`: `if proxy_path != session.proxy_path:` — plain string inequality, no `Path(...)`, no `resolve()`, no `normpath`. The `ValueError` at `loop.py:100-106` names **both** paths and states why re-verification is impossible (the session records no image dimensions).

- Tests: `:532` (mismatch raises, both paths in the message), `:544` (matching path resumes and round-trips `baseline_distance`), `:560` (`/mem/./proxy.png` is rejected — this is the one that pins *exact* rather than *resolved*).
- The docstring's second justification is materially correct and worth keeping: `proxy_path` need not be a filesystem path at all, since the executor seam may address images through an in-memory or PixInsight-side store — so there is not always anything to resolve. The test file's own `_memory_io()` store is an instance of exactly that.

#### (D) The bounded retry surfaces when the cap binds — ✅ IMPLEMENTED

Two halves, both present.

*The bound* — `beam.py:44-48`:

```python
max_attempts: int | None = None

def __post_init__(self) -> None:
    if self.max_attempts is None:
        object.__setattr__(self, "max_attempts", 3 * self.top_k)
```

and `loop.py:182-190`, where the attempt is counted **before** the executor runs, so executor failures, no-op discards and guardrail trips all consume budget — which is what makes it a real bound on work rather than a bound on failures.

*The surfacing* — `loop.py:263-279`:

```python
if kept < session.config.top_k and attempts >= session.config.max_attempts:
    session.guardrail_log.append({
        "iteration": iteration,
        "branch": branch.recipe.key or "(root)",
        "noted": ["attempt_cap"],
        "reason": f"branch stopped after {attempts} attempts (cap ...) holding only {kept} of {top_k} live candidates; ..."
    })
```

- `noted`, not `failed` — nothing was discarded here, the branch stopped looking. Conflating the two would let a reader count discards wrong.
- Keyed by `branch`, not `action` — it is a property of the whole expansion, and it is what makes the two `noted` event kinds mechanically distinguishable.
- Tests: `:344` (the default tracks `top_k`: `BeamConfig().max_attempts == 9`, `BeamConfig(top_k=5).max_attempts == 15`, override survives), `:353` (survives a `save`/`resume` round-trip), `:366` (the cap binds, exactly one event, `branch == "(root)"`, no `failed` key, the real attempt count in the reason), `:404` (does **not** bind on an ordinary branch), `:417` (`test_a_tight_cap_binds_where_a_loose_one_does_not` — same image, same executor, same limits, only the bound differs).
- `:417` is the load-bearing one and it is correct to have written: without it, "the default cap did not bind" would be compatible with an implementation whose cap never binds at all.
- One residual gap in the surfacing logic: see Minor #5 — the condition cannot distinguish "the cap bound" from "the menu ran out at exactly `max_attempts`".

### ⚠️ Cannot verify from this diff

- **Linear-input rejection (§12), listed as binding on this task.** `begin` (`loop.py:52-73`) accepts any array and measures it. Named risk checked once: `grep -rn "linear" src/autocontrast/` returns only colorimetry (`fingerprint/color.py`), a loader docstring, a seed-script comment, and a guardrail comment — **no linear-input guard exists anywhere in `src/`**. Ownership may be Task 11's (the sidecar entry point), but the controller should confirm *someone* owns it, because today nothing does.
- **Rollback.** Spec line 89 assigns `beam.py` "beam expansion, pruning, **checkpoint/rollback**". Checked `beam.py`'s top-level definitions: `BeamConfig`, `Branch`, `prune` — no rollback exists. The loop checkpoints `best` in memory (`loop.py:300`) but never rolls the beam back to it, so the beam may wander uphill for the remaining iterations after a good `best` is found early. `Branch.alive` is likewise never set `False` by this consumer; branches die by omission. Not resolvable from this diff — the controller should decide whether §6.1 requires a rollback.

---

## Strengths

- **The fail-safe is structural, and the test proves the mechanism rather than the symptom.** `tests/test_optimize_loop.py:160` asserts that *every* candidate off the already-good image scores strictly worse than baseline, so the decline cannot be an accident of convergence timing. `:117` additionally pins the fixture at the valley floor (`pytest.approx(0.0, abs=1e-12)`) so the decline test cannot silently degrade into "a test of a bad reference". `:143` compares `dtype`, `shape` and `tobytes()`, not merely `array_equal`.
- **The `_ranked_menu` bound is genuinely sound, not hand-waved.** I verified the claim at its source rather than accepting the docstring: `propose.py:150-170` groups the menu by `(priority, band, kind)` and emits exactly one action per group, so the group count can never exceed the menu length and `top_k=len(menu)` provably cannot truncate the ranking.
- **`evaluate_guardrails(produced, parent, source, limits)` (`loop.py:227`) gets the argument semantics right.** Checked the signature at `guardrails.py:355-374`: noise-floor and star-integrity run against the *immediate parent*, hue-invention and channel-ratio-drift against the *original source*. Reversing these would have been an easy and completely invisible mistake.
- **The §2.2 design decision was diagnosed, not tuned.** The brief warned "do not lower `epsilon_improve` to force it". The implementer instead measured the ranking, found the gap-closing action at rank 5, and changed the *semantics of `top_k`* without moving a single calibrated constant. Whatever one concludes about the change, that is the right method, and the escalation was flagged rather than buried.
- **Termination is honestly reported.** `test_iteration_cap_terminates_the_loop` (`:183`) asserts the exact string `"iteration cap (2) reached"`, not merely `converged` — and the implementer records that under the brief's unmodified code it converged for a *different* reason, which a loose assertion would have hidden. `test_exhausting_every_branch_is_not_dressed_up_as_success` (`:205`) excludes `no_op`/`executor` entries from its "every discard names `noise_floor`" assertion (`:220-224`) rather than folding them in, so a run that discarded everything for the wrong reason still fails.
- **Reported test output is pristine** — no warnings, nothing deselected, skipped, or xfailed, at both 16 tests and 21 tests.

---

## Issues

### Critical (Must Fix)

None.

### Important (Should Fix)

**1. `resume()` has no producer — nothing in this task ever persists the session.**
`loop.py:77` reads a session file. `advance` (`loop.py:139-305`) and `run_to_convergence` (`loop.py:325-329`) never call `session.save(...)`. `session.py:1-7` states the contract this violates verbatim — *"Everything the loop knows serializes here between calls. A crashed run leaves an inspectable session file rather than nothing"* — and spec §2.1 (line 61) requires loop state to serialize **between calls**. As written, `run_to_convergence` runs an entire 20-iteration search (≈413 executor calls, by the implementer's own measurement) wholly in memory; a crash leaves nothing at all. The `# checkpoint (§6.1)` comment at `loop.py:300` labels an in-memory field assignment as a checkpoint.
*Fix:* have `advance` accept a session-save callable, or have `run_to_convergence` write `session_path(work_dir, session_id)` after each iteration. If this is deliberately Task 11's, say so explicitly — but then `resume` has no exercised call site in production code, and the only things calling it are its own unit tests at `:353`/`:532`, which construct the file by hand.

**2. `_ranked_menu` (`loop.py:110-134`) is a verbatim duplicate of `propose.py:135-145`.**
The `palette_chroma_compatible(...)` call and the five-argument `available_actions(...)` call are character-for-character the same expressions used inside `propose_actions`, differing only in the local name (`parent_fp` vs `target`). The failure mode is silent, not loud: if `propose.py` ever changes how it builds its menu — a different `psf_fwhm_arcsec` expression, a new keyword argument — then `len(menu)` computed here becomes *smaller* than the real group count, `propose_actions` truncates, and the loop quietly stops seeing the tail of the ranking. That tail is precisely what the §3.3 change exists to reach (the gap-closing action was at rank 5). Nothing raises; the search just gets worse.
*Fix:* give `propose_actions` an explicit "no bound" contract (`top_k: int | None = None` meaning "the whole ranking") and delete the menu reconstruction from `loop.py`.

**3. The spec's own budget table contradicts the option it adopts** — `docs/superpowers/specs/2026-07-27-phase2-optimizer-design.md:181-195`.
Line 184 rejects the unbounded retry because ~127 min exceeds "a 15-minute budget". The adopted row at line 192 is **"~23 min worst case"** — also over that budget, by 50%. The same reasoning is reproduced in the field comment at `beam.py:33-38`. Either the 15-minute budget is wrong, or `3 × top_k` is the wrong default; as written the argument does not reach its own conclusion. Worth resolving before Task 12 calibrates against real data, where the implementer already flags the cap will need re-measuring.

### Minor (Nice to Have)

**4. Requirement (B)'s ordering half is unaddressed.** The requirement named two things — "candidates appended in proposal order and prune sorts stably, so distance ties break arbitrarily" *and* the `star_split` no-op. Only the no-op is fixed. Confirmed at `beam.py:68`: the sort is stable, so a genuine tie between two *different* real candidates resolves by (branch index, then proposal rank), with no stated policy and no test. Within a branch that is defensible (the higher-ranked action wins); across branches it is arbitrary. A one-line explicit tie-break key, or a comment stating that insertion order *is* the policy, would close it.

**5. The `attempt_cap` event fires on menu exhaustion that coincides with the cap.** `loop.py:263` tests `kept < top_k and attempts >= max_attempts`. If the menu happens to run out at exactly `max_attempts` entries, the branch was not truncated but is nonetheless reported as *"stopped after 9 attempts (cap 9)"*. The report claims "a branch that ran out of menu has not been truncated… Neither logs", but the code cannot distinguish the two cases. *Fix:* record whether the `break` at `loop.py:185` was actually taken. Relatedly, `:400-401` asserts `len({e["action"] …}) <= max_attempts` over a *set* of keys drawn only from logged (i.e. discarded) entries — it has teeth, but it does not distinguish cap-bound from menu-exhausted either.

**6. `noted` entries are appended per candidate with no dedup** (`loop.py:238-244`). On a starless image every candidate emits the identical `"star integrity not assessed…"` entry — on the implementer's own measured run, roughly 400 near-identical rows in a log whose §12 purpose is to make the real discards visible. `outcome()` returns the whole list (`loop.py:344`) and `Session.save` serializes it (`session.py:68`). Consider recording a `noted` reason once per `(iteration, guardrail)`.

**7. The parent fingerprint is recomputed every iteration for every live branch** (`loop.py:176`), although the identical extraction was already performed when that branch was created as a candidate (`loop.py:259`) and then discarded. At the 1600px proxy that is 3 × 1.5 s of pure waste per iteration — ~1.5 min over 20 iterations, against the budget Important #3 is already straining. Caching the `FingerprintData` on `Branch` removes it.

**8. `max_attempts: int | None` (`beam.py:44`) leaks an impossible `None` into every use site.** `loop.py:185` compares `attempts >= session.config.max_attempts` against a declared-`Optional` int; the invariant lives only in the comment at `beam.py:42` ("Always an int after construction"). Relatedly, `dataclasses.replace(cfg, top_k=5)` would carry the *old* resolved `max_attempts` rather than re-deriving it — exactly the "silently decoupling from `top_k`" the comment at `beam.py:40-42` claims is prevented. No caller does this today (checked: no `replace(` on `BeamConfig` in `src/` or `tests/`), so it is latent. A `max_attempts` property over a private field would carry the invariant in the type.

**9. `_Stub` (`loop.py:42-49`) is a duck-typed fake `Session`** existing only because `_measure` takes a `Session`. Inverting it — `_measure(rgb, *, pixel_scale_arcsec, psf_fwhm_arcsec, palette_class, n_scales)` plus a `_meta(session)` helper at the call sites — removes the class and the "looks like a Session but isn't" hazard.

**10. `guardrail_log` now carries three non-guardrail event kinds** (`executor`, `no_op`, `attempt_cap`). The name has drifted from the contents, and `tests/test_optimize_loop.py:220-224` already has to filter `no_op`/`executor` back out to make an assertion about guardrails. The field is declared in `session.py:52`, so a rename crosses tasks — a note in that field's docstring is the minimum.

**11. `advance` on a converged session silently no-ops** (`loop.py:166-167`), returning the session unchanged with no signal. Harmless inside `run_to_convergence`; a trap for a Task 11 caller driving iterations one at a time.

---

## Assessment

**Task quality:** Needs fixes

**Reasoning:** The core of the task is right and unusually well-evidenced — the fail-safe is structural and its mechanism is asserted directly rather than inferred, all four carried requirements are implemented on properties rather than on names, and each has a test that fails against the brief's code for its own distinct reason. What blocks it is that `resume()` was built with no counterpart that ever writes a session file, leaving spec §2.1's "a crashed run leaves an inspectable session file" unmet by the only code that runs the loop, and that `_ranked_menu` duplicates `propose.py`'s menu construction in a way that silently degrades the search if the two ever drift.
