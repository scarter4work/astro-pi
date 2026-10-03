# Task 11 report — sidecar ops, §2.5's batched protocol

Status: **DONE_WITH_CONCERNS**. Commit `547ad1e` on `phase2-optimizer`.
Full suite 411 passed (baseline 390 + 21 new), nothing deselected, no warnings.

## Design decisions taken before writing code

### 1. The shared seam is `ingest_candidate`, not a copied block

`advance()`'s inner loop was split into three named pieces that BOTH entry points
call:

- `ingest_candidate(...)` — no-op check → guardrails (`noted` / `failed`) →
  optional save → score → `Branch`. Returns `None` for a discarded candidate.
  This is the whole §7 filter and the scoring step, in one place.
- `_close_iteration(session, candidates)` — prune → empty-beam convergence →
  best update → distance history → `_check_convergence`.
- `_Walk` + `_log_attempt_cap` — the `kept`/`attempts` bookkeeping and the
  §3.3 cap notice.

`advance()` keeps its own issue-one/ingest-one loop (it drives a synchronous
`Executor`); the batched path issues `top_k - kept` at a time and ingests what
comes back. Neither holds a second copy of the guardrail/score/prune block.

### 2. What is persisted (the brief's suggested simplification, adopted)

The ranked menu is NOT persisted. Per branch we persist only `cursor`, `kept`,
`attempts`; the menu is recomputed per batch from (parent pixels, reference
fingerprint, `applied_kinds`), which is deterministic. Justification against
persisting the ranked list: the menu on the measured fixture is ~50 actions,
each a dict with params — roughly 6 KB per branch per batch of JSON that has to
be re-read and re-validated by every spawned sidecar, and it would go stale the
moment `propose.py` changed, reproducing exactly the drift Task 10's `df577e7`
removed. Recomputation costs one `extract()` per branch per batch (~1.5 s), and
Task 16 caches precisely that measurement.

What IS persisted, because it cannot be recomputed:

- `pending` — the in-flight instruction batch. Needed to map an
  `instruction_id` PixInsight reports back to the branch and `Action` it came
  from. The entries are the instruction dicts themselves, so there is exactly
  one representation, not a wire format plus a shadow copy.
- `cursors` — per-branch `{branch_key, cursor, kept, attempts}`, parallel to
  `session.branches`. `branch_key` is checked against
  `session.branches[i].recipe.key` on every use, so a desync is loud.
- `candidates` — candidates scored so far in the OPEN iteration. They accumulate
  across supplementary batches and are pruned only when the iteration closes.
- `max_dim` — the sidecar is spawned fresh per call and `optimize_step`'s
  request carries no `max_dim`; loading the proxy at a different `max_dim` than
  `optimize_begin` used would silently change every measurement.
- `candidate_suffix` — the file extension PixInsight is told to save candidates
  under. See the open concern below.

### 3. `action` serialization is not reinvented

`recipe.py` grew two module-level helpers, `action_to_dict` / `action_from_dict`,
and `Recipe.to_dict` / `from_dict` now call them — same bytes as before. The
instruction's `process`/`params` come from `pixinsight_step(action)`, extracted
verbatim out of `Recipe.to_pixinsight_steps()`, which now calls it. So an
instruction and a recipe step render identically by construction.

### 4. Pixel scale is corrected for downsampling in `optimize_begin`

Matching `_op_analyze`: `effective = pixel_scale_arcsec * downsample_factor(...)`.
The request's `pixel_scale_arcsec` is the image's NATIVE on-sky scale; the
session stores the effective one for the array actually fingerprinted (§2.2).

## What was actually built

`src/autocontrast/optimize/loop.py`

- `_Walk` — `kept`/`attempts` plus the one definition of `may_issue`.
- `_log_executor_failure`, `_log_attempt_cap` — the two log shapes, once each.
  `_log_attempt_cap` carries the (D) guard condition itself, so neither entry
  point can forget it.
- `ingest_candidate(...) -> Branch | None` — no-op check (B), guardrail `noted`
  (A) and `failed` (§7), optional save, score.
- `_close_iteration(session, candidates)` — prune, empty-beam convergence, best
  update, distance history, `_check_convergence`.
- `advance()` rewritten to call all four. Its body shrank from ~120 lines to
  ~40; its 23 existing tests pass unchanged, which is the regression proof.
- `_open_iteration` / `_plan_batch` / `_ingest_batch` / `_close_iteration_batched`
  and the two public entry points `begin_batch` / `step_batch`.

`src/autocontrast/optimize/recipe.py` — `action_to_dict`, `action_from_dict`,
`pixinsight_step`; `Recipe.to_dict` / `from_dict` / `to_pixinsight_steps` now
delegate to them (byte-identical output, covered by the existing recipe tests).

`src/autocontrast/optimize/session.py` — the five new fields, saved and loaded,
all with defaults so a session file written by an earlier build still opens.

`src/autocontrast/sidecar.py` — `_op_optimize_begin`, `_op_optimize_step`,
`_optimize_loader`, `_batch_result`; both registered in `_OPS`.

## The batch-size rule (the one real bug found while implementing)

First cut had `_plan_batch` walk `while walk.may_issue(config)`, mirroring
`advance()`. That is wrong: `kept` cannot advance during planning because
nothing has been produced yet, so the loop ran until `attempts` hit
`max_attempts` and the FIRST batch was 9 instructions instead of 3. Caught by
`test_the_first_batch_never_exceeds_the_beam_width_of_the_root`. The batch is
now bounded explicitly at `min(top_k - kept, max_attempts - attempts)`, which
is exactly what `advance()` would issue before it next needed to know whether
anything survived.

## Equivalence with `advance()`, argued

- Batch size `top_k - kept` can never over-issue: if every candidate in a batch
  survives, `kept` lands exactly on `top_k`.
- If some are discarded, a supplementary batch resumes at the same cursor
  `advance()` would have reached, with the same `attempts` already spent.
- The cursor only ever moves forward, and `attempts` increments on ISSUE, so the
  total actions tried per iteration is identical to `advance()`'s and bounded by
  `max_attempts` across all batches.

## Tests (19 new in `tests/test_optimize_sidecar.py`, 2 in `test_optimize_session.py`)

The brief's 8 cases, plus: the beam-width bound on batch 1, requirement (A) on
the batched path, requirement (B) on the batched path, an unreadable candidate
file, an `instruction_id` the sidecar never issued, "a guardrail trip costs no
search breadth", both structural no-duplication tests, a full run driven batch
by batch to convergence, and re-stepping a converged session.

Test 3 (`test_the_sidecar_scores_candidates_it_did_not_produce`) is the
structural proof: `_play_pixinsight` loads the parent, applies `NumpyExecutor`
and writes the PNG, and the test then asserts every surviving branch's
`image_path` is a file the TEST wrote.

`test_both_entry_points_route_through_one_ingestion` patches
`loop.ingest_candidate` to refuse everything and asserts BOTH `advance()` and
the batched step were starved by it — a duplicate on either path would leave the
patch inert for that path.

### Mutations run against the new tests (all caught)

| mutation | test that failed |
| --- | --- |
| `attempts` reset per batch (cap becomes per-batch) | `..._attempt_cap_bounds_attempts_across_supplementary_batches` |
| `_log_attempt_cap` call removed from the batched close | same |
| unproduced instruction logged nowhere | `..._unproduced_instruction_kills_the_candidate_not_the_run` |
| `optimize_step` calls `Session.load` instead of `loop.resume` | `..._against_a_different_image_is_refused` + `..._equivalent_looking_path_is_still_refused` |
| batched path binds its own copy of `ingest_candidate` | `..._both_entry_points_route_through_one_ingestion` |

One test expectation was wrong on first run and was corrected, not the code:
I had assumed one step closes one iteration. It does not on this fixture —
`hue_invention` and `noise_floor` discard two of the first three ranked actions,
so a supplementary batch is the ordinary case. `_drive_to_iteration` now drives
the real protocol.

## Verification

```
$ .venv/bin/python -m pytest tests/test_optimize_sidecar.py -v
... 19 passed in 58.62s

$ .venv/bin/python -m pytest tests/test_sidecar.py tests/test_optimize_loop.py \
      tests/test_optimize_session.py -v
... 37 passed in 31.49s

$ .venv/bin/python -m pytest -q
411 passed in 127.12s (0:02:07)

$ .venv/bin/python -m ruff check src/autocontrast/optimize src/autocontrast/sidecar.py \
      tests/test_optimize_sidecar.py tests/test_optimize_session.py
All checks passed!
```

Nothing deselected; no `-m` filter used, so the `live` tests ran. No warnings.

## Concerns

1. **`candidate_suffix` defaults to `.png` — 8-bit, and I could not do better
   here.** The sidecar must READ back what PixInsight writes, and reading goes
   through Pillow. Pillow in this venv cannot write 16-bit RGB at all
   (`TypeError: Cannot handle this data type: (1, 1, 3), <u2` for both TIFF and
   PNG), and `tifffile`/`imageio`/`cv2` are not installed — so the test cannot
   play PixInsight at 16 bits, and I have no way to verify a 16-bit read path.
   `.png` also matches what `advance()` already names its candidates. This is a
   real quality ceiling on the production search: every candidate makes an 8-bit
   round trip through disk. It is a request field and a session field
   (`candidate_suffix`) precisely so the PJSR task can raise it without touching
   the sidecar, but **that task needs a 16-bit reader added as a dependency** —
   it is not free. Flagging rather than deciding, since adding a dependency is
   outside this task.

2. **The §12 "linear input is a loud error" constraint has nothing to inherit.**
   I grepped: no stretch/linearity check exists anywhere in `src/`.
   `optimize_begin` therefore does not enforce it. I did not add one, because
   any linearity detector is a threshold, and §3.7 forbids introducing a tunable
   without exit-criterion runs that do not exist yet. This is an open gap for
   whichever task owns §12 input validation.

3. **`optimize_begin` corrects `pixel_scale_arcsec` for downsampling**
   (`* downsample_factor(...)`), matching `_op_analyze`. So the request's
   `pixel_scale_arcsec` must be the image's NATIVE on-sky scale. A caller that
   wired `analyze`'s output straight through would be correct, since `analyze`
   reports `wcs.pixel_scale_arcsec` raw — but a caller that pre-corrected would
   double-correct. Worth stating explicitly in the PJSR bridge's contract.

4. **No `BeamConfig` is accepted from the request.** The sidecar always uses the
   defaults. That is my reading of "no tunable may be adjusted" (§3.7); a
   request field would make every tunable adjustable by whoever writes the JSON.
   The attempt-cap test reaches a bound state through behavior (PixInsight
   produces nothing) rather than by setting a config, which is a better test
   anyway. If Task 12/14 needs to vary the beam from PJSR, that is a deliberate
   decision to take then.

5. **An `instruction_id` the sidecar never issued raises rather than being
   ignored.** My call, not the brief's. A missing candidate is a dead candidate
   (the brief says so explicitly and I implemented that); an EXTRA one means PI
   and the session disagree about what run they are on, and ingesting it would
   attribute pixels to the wrong branch and parent. Covered by
   `test_an_instruction_id_the_sidecar_never_issued_is_refused`. Easy to soften
   to a log entry if the reviewer disagrees.

## Fix round 1 — Important #1: unvalidated `candidate_suffix` misdiagnoses as executor failure

Reviewer's finding: `_op_optimize_begin` accepted `candidate_suffix` unvalidated.
`.xisf` — PixInsight's own native format, the obvious choice for a PJSR caller —
cannot be opened by `load_image` (Pillow has no plugin for it). Left unvalidated,
every candidate would raise inside `_ingest_batch`'s try, every branch would be
logged `failed: ["executor"]`, `_close_iteration` would find zero survivors, and
the whole run would terminate reporting "every branch was discarded by
guardrails or executor failure" — blaming PixInsight/guardrails for one bad
config field. Fixed by validating at `optimize_begin`, before any work starts.

### The supported set is derived, not hand-copied

`src/autocontrast/io/loaders.py` gained `supported_suffixes()`:

```python
def supported_suffixes() -> frozenset[str]:
    Image.init()  # populate Image.registered_extensions(); idempotent
    return frozenset(_FITS_SUFFIXES) | frozenset(Image.registered_extensions())
```

`load_image`'s dispatch is exactly two branches: `_FITS_SUFFIXES` goes through
astropy, everything else goes to Pillow's `Image.open`. So the supported set is
the union of the same `_FITS_SUFFIXES` constant `load_image` already tests
against (no second copy of that list) and Pillow's own registered plugin
extensions, read off `Image.registered_extensions()` rather than hardcoded —
confirmed `.xisf` is absent from it and ordinary raster suffixes (`.png`,
`.tif`, `.tiff`, ...) are present. If a later task adds a 16-bit reader by
teaching `load_image` a new Pillow-backed path, this set picks it up for free;
if it adds a wholly new format (its own FITS-like branch), it extends
`_FITS_SUFFIXES` the same way FITS already does — one place either way, not a
list duplicated here.

### The validation

`src/autocontrast/sidecar.py`, `_op_optimize_begin`:

```python
candidate_suffix = req.get("candidate_suffix", ".png")
supported = supported_suffixes()
if candidate_suffix.lower() not in supported:
    raise ValueError(
        f"candidate_suffix {candidate_suffix!r} is not a format the sidecar "
        "can load back (it reads every candidate through the same loader as "
        f"everything else); supported suffixes are {sorted(supported)}"
    )
```

Raised before `session.max_dim`/`session.candidate_suffix` are set and before
`loop.begin_batch`/`session.save` run, so a rejected suffix leaves no session
file on disk implying a run is in flight (`handle_request` turns the
`ValueError` into `{"ok": false, "error": ...}` as usual — no new error path).

### Covering tests (TDD — shown failing first)

Added to `tests/test_optimize_sidecar.py`, section 2 ("begin"):

- `test_an_unreadable_candidate_suffix_is_refused_at_begin_not_misdiagnosed_later`
  — `_begin(..., candidate_suffix=".xisf")` must come back `ok: False` with
  `.xisf` and `candidate_suffix` named in the error, and no session JSON left
  under `work_dir`.
- `test_the_default_candidate_suffix_still_works` — confirms `.png` (the
  default, unchanged per scope) still begins successfully and the returned
  `candidate_path` ends in `.png`.

Ran the new test against the pre-fix code first (`git stash` the two source
files, run just that test):

```
$ .venv/bin/python -m pytest tests/test_optimize_sidecar.py::test_an_unreadable_candidate_suffix_is_refused_at_begin_not_misdiagnosed_later -v
FAILED ... assert True is False
```

i.e. pre-fix, `.xisf` was silently accepted at `begin` (`resp["ok"]` was
`True`). After the fix:

```
$ .venv/bin/python -m pytest tests/test_optimize_sidecar.py -v
============================= test session starts ==============================
...
tests/test_optimize_sidecar.py::test_unknown_op_lists_the_optimizer_ops PASSED
tests/test_optimize_sidecar.py::test_optimize_begin_writes_a_resumable_session_and_a_first_batch PASSED
tests/test_optimize_sidecar.py::test_the_first_batch_never_exceeds_the_beam_width_of_the_root PASSED
tests/test_optimize_sidecar.py::test_an_unreadable_candidate_suffix_is_refused_at_begin_not_misdiagnosed_later PASSED
tests/test_optimize_sidecar.py::test_the_default_candidate_suffix_still_works PASSED
tests/test_optimize_sidecar.py::test_the_sidecar_scores_candidates_it_did_not_produce PASSED
tests/test_optimize_sidecar.py::test_a_passing_guardrail_that_assessed_nothing_is_still_logged PASSED
tests/test_optimize_sidecar.py::test_a_no_op_candidate_from_pixinsight_does_not_take_a_beam_slot PASSED
tests/test_optimize_sidecar.py::test_stepping_a_missing_session_is_a_loud_error PASSED
tests/test_optimize_sidecar.py::test_stepping_against_a_different_image_is_refused PASSED
tests/test_optimize_sidecar.py::test_an_equivalent_looking_path_is_still_refused PASSED
tests/test_optimize_sidecar.py::test_an_unproduced_instruction_kills_the_candidate_not_the_run PASSED
tests/test_optimize_sidecar.py::test_a_candidate_file_that_cannot_be_read_is_an_executor_failure PASSED
tests/test_optimize_sidecar.py::test_an_instruction_id_the_sidecar_never_issued_is_refused PASSED
tests/test_optimize_sidecar.py::test_the_attempt_cap_bounds_attempts_across_supplementary_batches PASSED
tests/test_optimize_sidecar.py::test_a_guardrail_trip_costs_no_search_breadth PASSED
tests/test_optimize_sidecar.py::test_the_in_flight_batch_state_survives_a_session_round_trip PASSED
tests/test_optimize_sidecar.py::test_both_entry_points_route_through_one_ingestion PASSED
tests/test_optimize_sidecar.py::test_the_batch_planner_uses_the_shared_ranked_menu PASSED
tests/test_optimize_sidecar.py::test_a_full_run_driven_batch_by_batch_improves_a_flattened_image PASSED
tests/test_optimize_sidecar.py::test_stepping_a_converged_session_reports_the_outcome_without_new_work PASSED
============================= 21 passed in 58.66s ==============================

$ .venv/bin/python -m pytest -q
........................................................................ [ 17%]
........................................................................ [ 34%]
........................................................................ [ 52%]
........................................................................ [ 69%]
........................................................................ [ 87%]
.....................................................                    [100%]
413 passed in 120.28s (0:02:00)
```

413 passed (baseline 411 + 2 new), nothing deselected, no warnings.

### Explicitly not touched

No 16-bit reader or new dependency added; `.png` default unchanged; no tunable
(`top_k`, `width`, `max_attempts`, `epsilon_improve`, `iteration_cap`,
`epsilon`) touched; batched protocol structure untouched; Minor findings from
the review left for the final whole-branch pass, per the brief.

Status: fix applied and verified. Commit follows this entry.
