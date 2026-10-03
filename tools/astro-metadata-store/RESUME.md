# Resume note — astro metadata store (2026-09-09)

## Where things stand
Tasks 1-13 of the plan are COMPLETE, reviewed, and committed on branch `feat/metadata-store`
(38 commits, 247 tests green, also verified on scott-server's python 3.13.5).
Task 14 — the first real pass over the archive — was RUNNING when I signed off.

## The run
On **scott-server** (192.168.68.53), fully detached (parent PID 1), unaffected by any
workstation shutdown.

    ssh root@192.168.68.53
    tail -f /data/astro-metadata/run.log        # pipeline progress
    ls -la /data/astro-metadata/store.sqlite    # the store

It runs: inventory (live NAS root, 16 workers) -> cluster -> group -> solve sample of 20 ->
status, writing everything to `run.log`. Look for `### ALL DONE`.

Quick progress check:

    /data/astro-metadata/app/.venv/bin/python -c "
    import sqlite3
    c=sqlite3.connect('file:/data/astro-metadata/store.sqlite?mode=ro',uri=True)
    print(c.execute('SELECT COUNT(*) FROM frames').fetchone()[0])"

It commits every 500 frames, so an interruption costs ~3 minutes of rework, not the pass.
Re-running the same command resumes — every pass is idempotent.

## The numbers that matter when it finishes
1. **Frames inventoried**, reconciled by content hash — settles the 28,135 vs 28,123
   discrepancy the spec has carried from the start.
2. **Distinct fields.** ~34,400 frames should collapse to a few hundred. That ratio IS the
   design — it turns 34,400 plate solves at ~12s into a few hundred. Thousands would mean
   the fingerprint threshold (80, measured on the ASI585MC) doesn't generalise to the
   older cameras.
3. **Read errors** — including whether the two known zero-byte files fail loudly.

## Still to do after the run
- Write `docs/runbook.md` with the real numbers and commit it (Task 14 step 5).
- Then `superpowers:finishing-a-development-branch` to merge.

## Top follow-ups (recorded, not done)
- `init_schema` has NO migration path. Latent today because the store is fresh; it goes live
  the moment a schema change lands after this run.
- Live SIMBAD resolution — deliberately deferred, scaffolded in `naming.py`, needs this
  run's real solve data to calibrate the search radius.
- Spec 6.9 adjacent-night project merge — not implemented; every night is its own project.
- Inventory speedup ceiling is memory-bound (`np.median`/`np.percentile` over 16.2M px are
  81% of per-frame time). Subsampling would help but shifts fingerprints, so it needs the
  clustering threshold re-measured.

## Full decision record
`.superpowers/sdd/2026-09-09-astro-metadata-store/progress.md` (gitignored, on disk) —
every ruling I made and why, including my own errors.
