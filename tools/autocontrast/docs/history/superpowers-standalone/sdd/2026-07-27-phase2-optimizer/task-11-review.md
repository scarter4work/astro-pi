# Task 11 review — batched sidecar protocol (§2.5)

**Diff reviewed:** `df577e7..547ad1e` (1 commit, 6 files, +1268/-140)
**Brief:** `task-11-brief-v2.md` (v1 explicitly ignored)
**Verdict:** Approved with minor fixes. The structural requirement — one search, two
entry points — is genuinely met.

---

## Part 1: Spec Compliance

### `optimize_begin` — ✅

`sidecar.py:_op_optimize_begin`. Request fields all consumed: `image`, `work_dir`,
`reference_id`, `reference_fingerprint`, `pixel_scale_arcsec`, `psf_fwhm_arcsec`,
`palette_class`, `n_scales?`, `max_dim?`, `session_id?`. Calls `loop.begin(...)`,
persists to `session_path(work_dir, session_id)`, then plans and returns batch 1.

Response `result` is exactly `{session_id, baseline_distance, reference_id,
instructions, converged, iteration}` — `_batch_result` supplies the last four.
Verified against the brief's list field by field.

`downsample_factor` correction (§2.2) applied to `pixel_scale_arcsec`, matching
`_op_analyze`. Correct, and the implementer's concern #3 (native vs pre-corrected
scale in the caller's contract) is a real thing for the PJSR task to state, not a
defect here.

### `optimize_step` — ✅

`sidecar.py:_op_optimize_step`.

- `image` is `req["image"]` (required, KeyError if absent) and is passed to
  `loop.resume(path, proxy_path=req["image"])`. **Not** `Session.load`. Verified the
  guard is live: `loop.py:99` is `if proxy_path != session.proxy_path:` — a bare
  string `!=`, no `Path`, no `resolve()`, no `normpath`.
- Missing session file → `FileNotFoundError` naming the id and the expected path,
  raised *before* any session mutation. Never a silent fresh start.
- An instruction absent from `produced` → `_log_executor_failure` with
  `"failed": ["executor"]` and a reason naming the instruction id and the PI process
  (`loop.py` `_ingest_batch`, the `path is None` branch). Run continues.
- Response is `{**loop.outcome(session), **_batch_result(session, instructions)}`.
  I checked for key shadowing: `outcome()` returns `improved, baseline_distance,
  distance, result_path, recipe, pixinsight_steps, iterations, convergence_reason,
  reference_id, guardrail_log`; `_batch_result` returns `session_id, converged,
  instructions, iteration`. **Disjoint.** No silent override.

### Instruction shape — ✅ (superset)

`loop.py:_instruction` emits `{instruction_id, branch_key, action_key, action,
process, params, parent_path, candidate_path}` — the brief's seven plus `action`.

The extra field is load-bearing, not decoration: on ingest the sidecar must rebuild
the exact `Action` to call `recipe.extend(action)`, and re-deriving it from
`process`/`params` would be parsing the PI rendering backwards. It carries
`action_to_dict(action)` — the canonical serialization, not a new format.

`process`/`params` come from `pixinsight_step(action)`, which
`Recipe.to_pixinsight_steps()` now also calls. So an instruction and an audit-log
step render identically **by construction**, which is stronger than the brief's
"do not invent a second serialization" — it made a second one impossible.

### Supplementary batches / `max_attempts` across batches — ✅

`loop.py:_plan_batch` bounds each branch at
`min(top_k - kept, max_attempts - attempts)` reading both counters from the
*persisted* cursor, and writes `cursor["attempts"]` back at issue time. `kept` is
incremented only in `_ingest_batch`, after guardrails.

`step_batch` re-plans after every ingest and only closes the iteration when the plan
comes back empty — so a guardrail trip provokes a supplementary batch inside the same
iteration rather than costing a beam slot (§7/§3.3).

### Global constraints

- **No AI** — ✅ ranking is still `propose_actions`; nothing added.
- **Guardrails discard, never score** — ✅ `ingest_candidate` returns `None` on any
  failed verdict; nothing derived from verdicts reaches `Branch(distance=...)`.
- **Chroma gate / PSF floor** — ✅ untouched; both live in `propose_actions`, which
  the batched planner calls through the *same* `_ranked_menu`
  (proved by `test_the_batch_planner_uses_the_shared_ranked_menu`).
- **No tunable adjusted** — ✅ `BeamConfig` unchanged (`width=3, top_k=3,
  iteration_cap=20, epsilon=1e-3, epsilon_improve=1e-3`); `_op_optimize_begin`
  deliberately accepts no config from the request (implementer's concern #4 — I agree
  with the call; a request field would make every §3.7 tunable wire-settable).
- **Response contract unchanged** — ✅ `handle_request` untouched; both ops registered
  in `_OPS` and reachable through the same wrapper.
- **Degraded paths log and surface** — ✅ mostly; see Important #1 for the one place a
  degraded path logs the *wrong cause*.
- **§12 linear-input check** — ⚠️ Cannot verify from diff, and the implementer's
  concern #2 says it does not exist anywhere in `src/`. Not built here and not
  claimed. Genuinely an open gap for whichever task owns input validation; I do not
  count it against this task, because adding a linearity threshold *is* introducing a
  tunable and §3.7 forbids that without exit-criterion runs.

### Extra / over-engineering

Nothing gratuitous. The five new `Session` fields are each justified by an actual
cross-process need (`pending` for reassociation, `cursors` for the §3.3 cap,
`candidates` for supplementary accumulation, `max_dim` for measurement stability,
`candidate_suffix` for the read-back format). `_produced_index`'s duplicate-id
rejection and `_ingest_batch`'s unknown-id rejection are additions beyond the brief
but are the correct §12 posture — see Minor #4 on the retry consequence.

---

## Part 2: Carried Requirements (A)(B)(C)(D)

### (A) PASS-with-reason logged under `noted`, distinguished by FIELD — ✅ HOLDS

One implementation, `loop.py:ingest_candidate`:
`noted = [v for v in verdicts if v.ok and v.reason]` → appends `{"noted": [...]}`;
`failed = [v for v in verdicts if not v.ok]` → appends `{"failed": [...]}`. The
discriminator is the dict key, never a substring. Because `advance()` and
`_ingest_batch` both call this one function, (A) cannot hold on one path and not the
other.

Batched-path test: `test_a_passing_guardrail_that_assessed_nothing_is_still_logged`
drives a real `optimize_begin`/`optimize_step` round trip and asserts
`"failed" not in e` on the noted entries — field, not string.

### (B) No-op discarded by PROPERTY before taking a beam slot — ✅ HOLDS

`ingest_candidate`'s first statement is `if np.array_equal(produced, parent):` → log
`failed: ["no_op"]`, return `None`. Same function on both paths.

The batched test is better than a name check:
`test_a_no_op_candidate_from_pixinsight_does_not_take_a_beam_slot` reads the *top
ranked* action's kind out of the returned batch, asserts `kind != "star_split"`, and
has a fake PixInsight return `np.array(rgb, copy=True)` for exactly that kind. An
implementation matching on the string `star_split` would let it through. It then
asserts the kind appears in neither `session.branches` nor `session.candidates`.

Note the batched path's no-op check compares two 8-bit disk round trips rather than
two in-memory float arrays, so it is *coarser* than `advance()`'s: a change smaller
than one 8-bit quantization step reads as a no-op. That is arguably the right answer
(it changed nothing observable) and is a consequence of Important #1's format
question, not a separate defect.

### (C) `resume()` exact comparison; `optimize_step` must not call `Session.load` — ✅ HOLDS

`sidecar.py:_op_optimize_step` calls `loop.resume(path, proxy_path=req["image"])`.
`Session.load` appears nowhere in the new sidecar code. I read `loop.py:99` directly
to confirm the guard is still a bare string `!=`.

Two tests, and the second is the one that matters:
`test_an_equivalent_looking_path_is_still_refused` passes `f"{tmp_path}/./proxy.png"`
— a path that any normalization would accept. It is refused. That test fails if
anyone ever "helpfully" adds `.resolve()`, and it fails if `optimize_step` is
rewritten to load directly.

### (D) `noted: ["attempt_cap"]` keyed by BRANCH when the cap binds — ✅ HOLDS

`loop.py:_log_attempt_cap` holds the guard condition *itself*
(`if walk.kept >= top_k or walk.attempts < max_attempts: return`), so neither entry
point can call it and forget the condition. I verified this is the exact De Morgan
negation of the original inline `if kept < top_k and attempts >= max_attempts:` —
equivalent, no behavior change on `advance()`.

`advance()` calls it per branch after the inner loop; `_close_iteration_batched`
calls it per branch before pruning. Entry keyed `"branch": branch.recipe.key or
"(root)"`, `noted` not `failed`.

`test_the_attempt_cap_bounds_attempts_across_supplementary_batches` asserts
`len(caps) == 1`, `caps[0]["branch"] == "(root)"`, and `"failed" not in caps[0]`.

### Attempt counted on ISSUE, not on success — ✅

`_plan_batch` increments `walk.attempts` in the emit loop, before anything is
produced, and persists it. The proof is behavioral and clean: the cap test has
PixInsight produce **nothing at all**, so zero candidates ever succeed, yet exactly
`max_attempts` instructions are issued and then the branch stops. If attempts were
counted on success the run would walk the entire ~50-action menu.

### `max_attempts` across ALL batches, not per batch — ✅

Same test: `issued` accumulates across every batch and is asserted
`== config.max_attempts` (9), with `len({ids}) == len(issued)` ruling out
double-counting. A per-batch reset would let the run continue to menu exhaustion and
fail this assertion.

---

## Structural Verdict — no second copy

**There is one copy of the guardrail/score/prune logic, and `advance()` routes
through it.** Evidence, from the diff itself:

- The old inline block in `advance()` (diff lines 292-363: no-op check, `verdicts`,
  `noted`, `failed`, `save`, `fingerprint_distance`, `Branch(...)`) is **deleted** and
  replaced by a single call to `ingest_candidate(...)` (diff lines 346-356).
- The old inline attempt-cap block (diff lines 365-383) is **deleted** and replaced by
  `_log_attempt_cap(session, iteration, branch, walk)` (diff line 384).
- The old inline prune/best/convergence tail (diff lines 387-408) is **deleted** and
  replaced by `return _close_iteration(session, candidates)` (diff line 409).
- The batched path calls those same four functions: `_ingest_batch` →
  `ingest_candidate` and `_log_executor_failure`; `_close_iteration_batched` →
  `_log_attempt_cap` and `_close_iteration`.

These are deletions in the diff, not additions alongside. There is no surviving second
implementation to drift.

The `_ranked_menu` class of problem (`df577e7`) is **not** reintroduced:
`_plan_batch` calls `_ranked_menu(reference_fp, _measure(...), applied_kinds=...,
n_scales=...)` — the identical call `advance()` makes. It does not rebuild
`propose.py`'s menu, and does not persist the menu (which would have been the disk
equivalent of the same bug: a stale copy of the proposer's output).

`advance()`'s behavior is preserved exactly. The one thing worth checking is the
candidate path: it used to be computed *after* guardrails passed, and is now computed
by the caller *before* `ingest_candidate` runs — but it is
`f"...cand-{iteration}-{len(candidates)}.png"` and `candidates` is appended to only on
success, so the string produced is identical at both points. No behavior change. The
23 existing `test_optimize_loop.py` tests passing unchanged is the regression proof,
and it is a real one because they exercise `advance()` directly.

### Two structural tests, judged

`test_both_entry_points_route_through_one_ingestion` — **genuine.** It patches
`loop.ingest_candidate` to refuse everything, then asserts (a) the batched step made
exactly `len(produced)` calls, (b) `advance()` under the *same* patch added more
calls, and (c) `session.branches == []` afterwards. A duplicated inline block on
either path would leave the patch inert for that path and the corresponding assertion
would fail. This is the right shape of test for this constraint.

`test_the_batch_planner_uses_the_shared_ranked_menu` — **genuine**, and it takes a
baseline assertion first (`assert loop.begin_batch(...)` before patching), so it
cannot pass vacuously.

---

## The structural proof test, judged specifically

`test_the_sidecar_scores_candidates_it_did_not_produce`.

**It does prove what it claims.** The chain:

1. `_play_pixinsight` — in the *test* — loads `inst["parent_path"]` via `load_image`,
   calls `NumpyExecutor().apply(...)`, and calls `_write_png(inst["candidate_path"],
   out)`. The test writes the bytes.
2. It returns only `[{instruction_id, path}]` — paths, no pixels.
3. `_step` sends that to `handle_request`, i.e. through the real sidecar entry point,
   not a direct `loop` call.
4. The assertions: `scored = {b.image_path for b in session.branches}`;
   `assert scored <= set(written)` where `written` is the list of paths **the test
   wrote**. Plus `all(b.distance != session.baseline_distance)` (distances were really
   measured, not inherited) and `{i["parent_path"] for i in next_batch} <= set(written)`
   (the next iteration descends from test-written files, so the beam really moved).

**Does the sidecar produce pixels anywhere on that path?** No. `_ingest_batch` calls
`ingest_candidate` **without** `save`, and `ingest_candidate`'s only write is
`if save is not None: save(...)`. There is no `Executor` import reachable from
`step_batch`, and no `executor.apply` call in the batched section. Confirmed by
reading the whole batched block. The sidecar reads, measures, and scores only.

The one honest caveat: the sidecar *chose* the paths (`inst["candidate_path"]`), so
"a file the test wrote" is a file the sidecar named. That does not weaken the claim —
naming a path is not producing pixels — but it means the test proves "the sidecar did
not write these" rather than "the sidecar could not have". Good enough; the
`save=None` reading above closes the gap.

## Mutation claims, judged

Four of five described mutations would genuinely be caught, by the mechanism claimed:

| mutation | caught? | why |
| --- | --- | --- |
| `attempts` reset per batch | ✅ | `len(issued) == max_attempts` becomes `len(issued) == menu length` |
| `_log_attempt_cap` removed from batched close | ✅ | `len(caps) == 1` → 0 |
| unproduced instruction logged nowhere | ✅ | `assert failures` is unconditional |
| `Session.load` instead of `resume` | ✅ | both refusal tests assert `ok is False` |
| batched path binds its own `ingest_candidate` | ✅ | monkeypatch on the module attribute goes inert |

One caveat on the first: `test_the_attempt_cap_bounds_attempts_across_supplementary_batches`
has an unguarded `while result["instructions"]:` loop. Under that mutation the loop
still terminates (the cursor walks the menu to exhaustion), so the test fails rather
than hangs — but a *different* mutation that let the cursor stall would hang the suite
instead of failing it. See Minor #3.

---

## Part 3: Code Quality

### Separation of concerns

Clean. The batched section is a coherent pipeline of small single-purpose functions:
`_open_iteration` / `_cursor_for` / `_instruction` / `_plan_batch` / `_produced_index`
/ `_ingest_batch` / `_close_iteration_batched`, with `begin_batch` / `step_batch` as
the only public surface. Each does one thing and the names say which.

`_cursor_for` deserves specific credit: `cursors` is parallel to `branches` by
construction, and rather than trusting that, every access re-verifies
`cursor["branch_key"] == branch.recipe.key` and raises a message naming the session
and both keys. A desync there would otherwise read as a normal run searching the wrong
menu positions — the exact class of bug that is invisible in output.

### DRY

This is the diff's strongest quality. Three separate near-duplications were removed
rather than created: the guardrail/score block, the attempt-cap log, and the
prune/converge tail. `recipe.py`'s `pixinsight_step` extraction means the instruction
and the audit artifact cannot describe different processes.

### Error handling

Consistently loud. Every failure names the thing that failed: the session id, the
instruction id, the branch key, the unreadable path. `_op_optimize_step` raises
`FileNotFoundError` before touching state. `session.save(path)` happens only after
`step_batch` returns, so a mid-ingest raise leaves the on-disk session at its previous
consistent state.

### `loop.py` size — is it two modules now?

It grew 561 net lines to roughly 670. I judge it still **one** module, and would not
ask for a split: the whole point of the task is that the two entry points share a
search, and `ingest_candidate` / `_close_iteration` / `_log_attempt_cap` / `_Walk` are
called from both sides. Splitting `loop_batched.py` out would either re-export those
across a module boundary (fine but noisy) or tempt exactly the duplication the brief
forbids. The `# §2.5: the batched protocol` banner comment marks the seam clearly, and
the file reads top-to-bottom as: shared primitives → offline entry point → batched
entry point → reporting. That is a defensible single responsibility ("run the search").

If it grows again in Task 12/14, revisit.

### Test quality

High, and unusually so on the things that matter. The tests exercise the real
`handle_request` seam, use real pixels and real guardrails, and mock almost nothing —
the two `monkeypatch` uses are both *structural* assertions (starve the shared
function, prove both paths starve), not behavior stubs. `_NoOpFor` is a hand-written
fake executor standing in for PixInsight, which is the right level.

`_drive_to_iteration`'s docstring records a real discovery — that one step does *not*
close one iteration on this fixture, because two of the first three ranked actions
trip guardrails — and the helper drives the actual protocol rather than the assumed
one. The report says the test expectation was corrected rather than the code; the
resulting helper is consistent with that and I see no sign of a code change bent to
fit a test.

`_reference_image`'s docstring explains why it is deliberately noisy at the finest
scale (8-bit round trip would otherwise let quantization dominate the noise
guardrail). That is the fixture author thinking about the measurement, which is rare
and correct. `np.ptp(f)` is used, not `f.ptp()` — the inherited v1 defect is not
reproduced. No duplicate imports added to `sidecar.py`.

Test output pristineness (411 passed, no warnings, nothing deselected) is the
implementer's claim and I did not re-run per instruction — reported here as
unverified, not as confirmed.

---

## Issues

### Critical (Must Fix)

None.

### Important (Should Fix)

**1. `candidate_suffix` is accepted unvalidated, and an unreadable choice
misdiagnoses itself as PixInsight failing.**

`sidecar.py:_op_optimize_begin` — `session.candidate_suffix = req.get("candidate_suffix", ".png")`.

The suffix flows into `_instruction`'s `candidate_path` and is handed to PixInsight,
but the sidecar must read those files back through `_optimize_loader` →
`load_image` → FITS via astropy, everything else via **Pillow**. The obvious choice
for a PJSR caller is `.xisf` (PixInsight's native format), which Pillow cannot open.

The failure mode is not a loud config error. Every candidate raises inside
`_ingest_batch`'s `try`, is logged as `failed: ["executor"]`, the branch spends its
attempts, `_close_iteration` finds zero survivors, and the run terminates with
`convergence_reason = "every branch was discarded by guardrails or executor failure"`.
The read errors *are* in `guardrail_log`, so this is not silent — but the top-line
diagnosis blames PixInsight and the guardrails for what is a one-field configuration
mistake, and a caller reading `outcome()` would go looking in the wrong place.

Fix: validate at `optimize_begin` against the extensions `load_image` actually
handles, and raise naming the suffix and the supported set. One `if`, and it turns a
whole-run misdiagnosis into an immediate correct error. This also gives the
implementer's concern #1 (8-bit PNG ceiling) a place to live: whichever task adds a
16-bit reader extends that same set.

### Minor (Nice to Have)

**2. `iteration` and `iterations` in the same response dict, meaning different things.**

`sidecar.py:_op_optimize_step` returns `{**outcome(session), **_batch_result(...)}`.
`outcome()` contributes `iterations` (count of COMPLETED iterations);
`_batch_result` contributes `iteration` (the iteration the returned instructions
belong to). They differ by one while a batch is in flight, and the tests rely on both
(`result["iterations"] == 0` alongside `result["iteration"] == 1`).

This is documented in `_batch_result`'s docstring and there is no collision, so it is
correct — but it is a genuine footgun for the PJSR bridge in Task 12/14, where a
one-character typo silently reads the wrong number. Consider renaming the batch field
to `batch_iteration` before PJSR consumes it, while the contract has exactly one
consumer.

**3. `test_the_attempt_cap_bounds_attempts_across_supplementary_batches` has an
unguarded `while` loop.**

`tests/test_optimize_sidecar.py` — `while result["instructions"]:` with no bound. The
end-to-end test does this correctly (`assert calls < 200, "the batched protocol never
terminated"`). Under the mutations actually described this test fails rather than
hangs, but a cursor-stall regression would hang the suite instead of reporting. Add
the same guard; it costs one line and converts a hang into a message.

**4. A retried `optimize_step` after a PJSR-side crash is a hard failure.**

`loop.py:_ingest_batch` raises on any `instruction_id` not in `session.pending`, and
`_op_optimize_step` clears `pending` and saves after a successful step. So if PJSR
loses the response to a step it completed and re-sends the same `produced`, the
session has already moved to batch N+1 and the resend raises "handed results for
instructions it never issued", killing the run.

I think the strict posture is right (the implementer's concern #5 — ingesting an
unrecognized candidate would attribute pixels to the wrong branch and parent, which is
worse than a loud stop). But the error message should say what to do: it currently
reads as a protocol violation when the likely cause is a retry, and the session on
disk is intact and re-steppable with the *current* `pending`. Adding "the session's
current in-flight batch is <ids>; re-execute those" would make it recoverable without
weakening the check.

**5. `_plan_batch` / `_close_iteration_batched` raise a bare `IndexError` when
`cursors` is shorter than `branches`.**

`loop.py:_cursor_for` — `session.cursors[index]` before the branch-key check. Every
path created by `optimize_begin` populates `cursors`, so this is only reachable for a
session file written by another flow (`loop.begin` + `save` without `begin_batch`, or
a pre-this-commit file — which `Session.load` now defaults to `cursors=[]`). Given how
carefully every other desync in this diff names itself, a bare `IndexError` here is
the one inconsistent voice. Range-check and raise the same style of named error.

**6. `_op_optimize_begin` decodes the image twice.**

`downsample_factor(image, load(image))` loads it, then `loop.begin(..., load=load)`
loads it again. At 1600px that is a duplicated decode+resize per begin. Harmless
(once per run, not per iteration) and Task 16 is described as caching measurements —
worth folding in there rather than fixing separately.

**7. Two tests cannot distinguish `top_k` from `width`.**

`test_the_first_batch_never_exceeds_the_beam_width_of_the_root` asserts
`len(instructions) == session.config.top_k` while its name says "beam width", and
the cap test asserts `len(result["instructions"]) <= config.top_k`. Both defaults are
3, so either bound satisfies the assertion and a mutation swapping one for the other
survives. Not worth changing the tunables to prove (§3.7 forbids it) — but the test
name should match what it asserts, so the next reader is not told the wrong invariant
was proven.

---

## Scope check: `recipe.py` (+70, unlisted in the brief)

**In scope, and required.** The brief says `process` and `params` must come from the
existing `Recipe.to_pixinsight_steps()` / `Action` serialization and forbids inventing
a second format. `to_pixinsight_steps()` was a loop over `self.actions` with the
per-action rendering inline; there was no way to render *one* action without either
extracting it or copying it. The implementer extracted it (`pixinsight_step`) and
made `to_pixinsight_steps()` call it — the DRY-preserving option, and precisely the
"extract the shared part and have both call it" instruction the brief gives for
`advance()`.

Same reasoning for `action_to_dict` / `action_from_dict`: the instruction must carry
an `Action` across a JSON round trip, and `Recipe.to_dict`/`from_dict` had that shape
inlined.

Both changes are pure extractions producing byte-identical output, covered by the
pre-existing recipe tests (which pass unchanged) plus the round-trip assertions in
`test_optimize_session.py` that compare `to_pixinsight_steps()` output. Not scope
creep — the alternative was the duplication the brief forbids.

---

## Strengths

1. **The shared seam is real and the diff proves it by deletion.** `advance()`'s
   guardrail/score block, attempt-cap block, and prune tail are removed and replaced
   by calls. There is no second implementation to drift, and the two structural tests
   would catch one being introduced.
2. **`_log_attempt_cap` owns its own guard condition**, so requirement (D) cannot be
   half-implemented by a future caller that forgets the `if`.
3. **The cap test proves counting-on-issue behaviorally** — PixInsight produces
   nothing, yet exactly `max_attempts` instructions are issued — rather than by
   inspecting a counter.
4. **`_cursor_for` verifies an invariant that holds by construction anyway.** That is
   the right instinct for state that survives a process boundary.
5. **The batch-size bug was found and fixed with a test, and reported.** `while
   walk.may_issue(...)` during planning is a genuinely subtle error (`kept` cannot
   advance before anything is produced) and the explicit
   `min(top_k - kept, max_attempts - attempts)` bound with its comment is the correct
   fix, not a patch over the symptom.
6. **Backward-compatible session loading is tested**, not assumed
   (`test_a_session_written_before_the_batched_protocol_still_loads` strips the new
   keys from the JSON and reloads).

---

## Assessment

**Task quality: Approved** (with Important #1 to fix before the PJSR task consumes
this contract).

The one requirement this task existed to get right — a second entry point over the
same search, not a reimplementation — is met, and met in the strongest available way:
the old inline logic is deleted rather than paralleled, and two tests fail if anyone
re-parallels it. All four carried requirements (A)(B)(C)(D) hold on the batched path
by construction rather than by coincidence, because both paths call the same
functions. The `_ranked_menu` class of bug from `df577e7` is not reintroduced.

Important #1 is a real edge the PJSR task will hit on its first `.xisf`, and it is a
few lines to close. Everything else is polish.

The §12 linear-input gap (implementer's concern #2) is correctly out of scope here and
should be tracked against whichever task owns input validation — but it is a real
unmet spec constraint and should not be allowed to disappear.
