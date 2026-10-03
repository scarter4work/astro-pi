# Task 11 — Fix round 1 re-review

Fix base: `547ad1e`  Head: `ec6c662`
Diff reviewed: `.superpowers/sdd/2026-07-27-phase2-optimizer/review-547ad1e..ec6c662.diff`
Scope: verify the fix for Important #1 (`candidate_suffix` accepted unvalidated), confirm no
new breakage, confirm scope limits honored. Read-only — no git commands run, no test suite
re-executed.

## Finding under verification

Reviewer's finding (`task-11-review.md:359-380`): `_op_optimize_begin` accepted
`candidate_suffix` unvalidated; a value like `.xisf` (PixInsight's native format, the obvious
PJSR choice) cannot be opened by `load_image` (Pillow has no plugin for it), so every
candidate fails inside `_ingest_batch`, every branch is discarded, and the run's
`convergence_reason` blames "guardrails or executor failure" for what is a one-field config
mistake.

## What the diff does

- `src/autocontrast/io/loaders.py:73-86` — new `supported_suffixes()`:
  `frozenset(_FITS_SUFFIXES) | frozenset(Image.registered_extensions())`, calling
  `Image.init()` first to populate Pillow's plugin table.
- `src/autocontrast/sidecar.py:255-267` — in `_op_optimize_begin`, before
  `session.max_dim`/`session.candidate_suffix` are set and before `loop.begin_batch`/
  `session.save` run: read `candidate_suffix` (default unchanged, `.png`), reject if
  `candidate_suffix.lower() not in supported_suffixes()` with a `ValueError` naming the
  offending suffix and the full supported set.
- Two new tests in `tests/test_optimize_sidecar.py`: `.xisf` refused with no session file
  left behind; `.png` (default) still begins successfully.

## Verification performed

Read `src/autocontrast/io/loaders.py` in full (not just the diff hunk) to check
`load_image`'s actual dispatch condition against what `supported_suffixes()` derives from:

```
_FITS_SUFFIXES = {".fits", ".fit", ".fts"}          # loaders.py:72, single module-level definition

def load_image(...):
    path = Path(path)
    if path.suffix.lower() not in _FITS_SUFFIXES:    # loaders.py:132 — load_image's own dispatch
        return load_raster(path, max_dim=max_dim)
    ...                                               # else: astropy FITS path

def supported_suffixes() -> frozenset[str]:
    Image.init()
    return frozenset(_FITS_SUFFIXES) | frozenset(Image.registered_extensions())
```

`_FITS_SUFFIXES` is defined exactly once at module scope and is the identical object
`load_image` branches on — `supported_suffixes()` wraps that same set in `frozenset(...)`,
it does not redeclare or copy the literal. There is no second list anywhere that could drift
from `load_image`'s dispatch condition. `load_raster` (the non-FITS branch) opens the file via
`Image.open(path)` with no suffix filtering of its own, so "not a FITS suffix" +
"suffix registered with Pillow" is exactly the condition under which `load_image` takes the
Pillow branch and Pillow recognizes the format.

Confirmed empirically in the venv (read-only import, no test suite invocation):

```
>>> from autocontrast.io.loaders import supported_suffixes
>>> s = supported_suffixes()
>>> '.xisf' in s
False
>>> sorted(x for x in s if x in ('.png','.tif','.tiff','.jpg','.jpeg','.fits','.fit','.fts'))
['.fit', '.fits', '.fts', '.jpeg', '.jpg', '.png', '.tif', '.tiff']
```

`.xisf` absent, all suffixes the codebase actually uses (FITS trio + common raster) present —
matches the claim.

Confirmed the placement of the check: `_op_optimize_begin` (`sidecar.py:233-274`) computes
`candidate_suffix`, validates, and only *after* the `raise` sets `session.candidate_suffix`,
calls `loop.begin_batch`, and calls `session.save(...)`. Traced `loop.begin()` (`loop.py:52-74`,
called earlier in `_op_optimize_begin` to build the `Session` object) — it is pure in-memory
construction, no disk I/O, so nothing is written before the validation runs. `handle_request`
(`sidecar.py:316-326`) wraps the handler in `try/except Exception`, turning the `ValueError`
into `{"ok": False, "error": ...}` — this is the pre-existing error path, not a new one.

Read the two new tests (`tests/test_optimize_sidecar.py`, appended after
`test_the_first_batch_never_exceeds_the_beam_width_of_the_root`) and the `_begin` helper
(`tests/test_optimize_sidecar.py:67-75`, `req.update(extra)`) — `candidate_suffix=".xisf"` is
genuinely threaded through to the real `_op_optimize_begin` code path, not stubbed. The
"no session left behind" assertion (`not any(tmp_path.glob("*.json"))`) is consistent with the
save-ordering traced above — `tmp_path` is `work_dir`, and the only thing written into it
otherwise (the fixture PNG) is not JSON.

Did not re-run the test suite or the pre-fix `git stash` comparison (out of scope per
instructions); the report's claimed pre/post output and pass counts are consistent with the
diff and the traced control flow, and I have no reason to doubt them given the above holds up
independently.

## Findings

### Finding Verdict
ADDRESSED. `src/autocontrast/sidecar.py:255-267` validates `candidate_suffix` against
`supported_suffixes()` before any session state is committed or persisted, raising a
`ValueError` that names both the offending suffix and the full supported set. Covered by
`tests/test_optimize_sidecar.py::test_an_unreadable_candidate_suffix_is_refused_at_begin_not_misdiagnosed_later`.

### Scope Limits
- No 16-bit reader / new dependency added — honored. `supported_suffixes()` only reads
  Pillow's existing `registered_extensions()`; no new import in `loaders.py` diff.
- Default suffix unchanged (`.png`) — honored, `sidecar.py:256`
  (`req.get("candidate_suffix", ".png")`, unchanged from base).
- No tunable (`top_k`, `width`, `max_attempts`, `epsilon_improve`, `iteration_cap`, `epsilon`)
  touched — honored; diff touches only `loaders.py`, `sidecar.py`'s `_op_optimize_begin`, and
  tests.
- Minor findings from the original review left untouched — honored; diff contains no changes
  outside the Important #1 fix and its tests.
- Batched protocol (one search, two entry points, no duplicated guardrail/score/prune logic)
  unrestructured — honored; diff does not touch `loop.py`, `recipe.py`, or `session.py` at all.

### Derivation Check
Genuinely derived, not a fresh hardcoded list. `_FITS_SUFFIXES` is a single module-level set
literal (`loaders.py:72`) that both `load_image`'s dispatch (`loaders.py:132`) and
`supported_suffixes()` reference directly — one definition, two readers, no second copy that
could drift. The raster half comes from `Image.registered_extensions()`, which is exactly the
plugin table `Image.open()` (called by `load_raster`, the branch `load_image` falls through to
for every non-FITS suffix) consults, so the two cannot disagree by construction — a suffix
either matches `_FITS_SUFFIXES` and takes the astropy path, or it takes the Pillow path where
"Pillow recognizes this suffix" and "Pillow will try to open it" are the same fact.

On `registered_extensions()` being environment-dependent: this is a real property, not a
flaw, and the right tradeoff here. The whole point of the fix is "does *this* sidecar process,
on *this* machine, know how to read this suffix back" — a hardcoded list would claim
portability it can't back up (e.g. claiming `.webp` support that isn't actually installed in a
given deployment) and would still need updating by hand whenever Pillow's build changes.
Environment-dependence does mean a suffix accepted in dev could be refused in prod if the
Pillow installs differ (or vice versa) — worth a one-line note if this ever needs to be
guaranteed identical across environments (e.g. pin/audit Pillow's optional codec deps in
whatever installs the sidecar), but that's a packaging concern, not a defect in this fix, and
well outside this task's scope.

### New Breakage in the Fix Diff
None. The added code path is a pure validation added before any state mutation in
`_op_optimize_begin`; `_op_optimize_step` and `loop.py`/`recipe.py`/`session.py` are untouched
by this diff. `supported_suffixes()` is a pure function with no side effects beyond the
idempotent `Image.init()` (documented as such and consistent with Pillow's own API contract).

### Test Quality
The new suffix-rejection test asserts on the actual observable contract: `resp["ok"] is
False`, both `.xisf` and `candidate_suffix` named in `resp["error"]`, and no session JSON left
in `work_dir`. That is a genuine regression catch, not a tautology against the new function —
it exercises `_op_optimize_begin` end-to-end through `handle_request`, the same entry point
PixInsight calls, and would fail again if the validation were removed, moved after
`session.save`, or if `supported_suffixes()` were changed to wrongly include `.xisf`. The
companion default-suffix test guards the "don't accidentally reject `.png`" direction. Both
are traceable to real code paths, not mocks of the new function.

## Verdict

**Fix round: All findings addressed, no new Critical/Important breakage.**
