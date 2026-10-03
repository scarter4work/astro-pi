### Task 15: §3.6 full-resolution validation checkpoint

**Why this exists:** the plan's own self-review named §3.6 as a known gap — "Tasks 1-14
run the search entirely on the proxy and replay once; the *periodic full-res checkpoint*
is not implemented… **It must be added before Phase 2 is called complete.**" The user
ruled it in scope for this branch.

**Task 13 turned this from a nice-to-have into the branch's most load-bearing check.**
Its Finding 2 measured the *same image* at **D = 0.2264** when downsampled by Pillow and
**D = 0.2467** when downsampled by PixInsight's `Resample` — a **9% shift in the number
the whole system is built around**, from nothing but the choice of resampler. The
fingerprint measures fine structure (§4.2) and a resampler changes fine structure, so
this is expected physics, not a bug. But it means proxy-derived decisions are not
automatically valid at full resolution, and nothing currently checks whether they are.

That is exactly what §3.6 is for.

---

## What to build

Every `config.validation_interval` iterations, `advance()` (and the batched path — see
below) replays `session.best.recipe` against `session.source_path` at **full
resolution**, re-measures, and appends to a new `validation_log`:

```
{"iteration", "proxy_distance", "full_distance", "diverged"}
```

Divergence beyond `config.divergence_tolerance` sets a `diverged` flag that is surfaced
in `outcome()` and printed **loudly** by the PJSR driver (§12 — degraded paths surface).

## The architectural constraint you must design around

`loop.advance()` is the offline/CI path and can replay locally through its `Executor`.
**The batched path cannot.** Spec §2.5 makes PixInsight the executor, and the sidecar is
a child process that cannot call back into its parent — so a full-resolution replay
requires PixInsight to execute the recipe, which means its own instruction round-trip.

Design it so BOTH paths share one implementation of "replay this recipe, measure, compare,
log". Task 11 established the pattern: one search, two entry points, through a shared
seam (`ingest_candidate`). Do the same here. **A second copy of the replay/compare logic
is the worst outcome of this task** — Task 10's fix round (`df577e7`) and Task 11's review
both turned on exactly that failure class.

Suggested shape (adopt or better, and justify in your report): `optimize_step` may return
`validation_instructions` alongside `instructions`; the driver executes them, saves the
full-res result, and returns it in `produced` keyed the same way. The sidecar then
measures at full resolution and logs. If you find a cleaner seam, take it.

## Cost is a first-order concern here

A full-resolution replay is expensive. Task 13 measured the print at 5318×3975 — at
16-bit that is ~127 MB per candidate. **This is why it is periodic, not per-iteration.**

- `validation_interval` and `divergence_tolerance` are **new** tunables, not adjustments
  to existing ones — introducing them does not violate §3.7, but you must justify their
  default values with reasoning, and state the cost per checkpoint you measured.
- Do NOT replay every candidate. Only `session.best.recipe`, and only on the interval.
- Report the measured wall-clock cost of one checkpoint, and what it implies for
  Task 16's budget (which explicitly excludes PixInsight round trips and calls itself a
  floor).

## Test requirement

The plan's own words: *"a deliberately scale-sensitive action must show divergence
between an 800px proxy and the 1600px original."* Build that test. A checkpoint that
cannot detect divergence when divergence genuinely exists proves nothing.

Also test: a scale-INSENSITIVE recipe must NOT trip the flag. A detector that always
fires is as useless as one that never does.

## Do not adjust existing tunables

`top_k`, `width`, `max_attempts`, `epsilon_improve`, `iteration_cap`, `epsilon` — §3.7.
That includes not "fixing" `max_attempts` even though three independent runs now show it
binding; that decision is pending Task 14's evidence and is the user's.

## Global constraints

- Branch `phase2-optimizer`. Do not commit to master. Do not push.
- No AI anywhere in this plan.
- Guardrail violations discard candidates; never a score term (§7).
- No test may be deselected to make the suite green.
- Every degraded path logs and surfaces (§12). No silent fallbacks — a `diverged` flag
  that does not reach the user is the same defect Tasks 3 and 10 fixed twice.
- Sidecar response contract unchanged: `{"ok": true, "op":…, "result":…}` /
  `{"ok": false, "op":…, "error":…}`.
- Session state you add must round-trip through `Session.save`/`Session.load`, and you
  must test that — Task 9's contract is that every field survives.

## Verification

- `.venv/bin/python -m pytest -q` — full count, nothing deselected. Baseline will be
  whatever Task 14 leaves; check `git log` and the ledger rather than assuming.
- The divergence test above, both directions.
- Measured cost of one full-resolution checkpoint.

## Report

Write to `.superpowers/sdd/2026-07-27-phase2-optimizer/task-15-report.md` AS YOU GO.
Reply with only: status, commit SHA(s), test summary, the measured checkpoint cost, the
defaults you chose with their justification, and concerns.
