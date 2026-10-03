# Parallelising the inventory pass

**Status: DONE_WITH_CONCERNS.** The change works, is deterministic, keeps every
listed guarantee, and is faster. But the measured speedup is **2.4–2.9×, not the
20× the core count suggests**, and I can show exactly why: the per-frame work is
memory-bound, not compute-bound, and stops scaling at about four cores. Details
in "Measured speedup" below. That ceiling lives in `imagekeys`, not in the code I
was asked to change, and moving it is a deliberate decision rather than a
drive-by — see "Concerns".

## Commits

| SHA | Subject |
|---|---|
| `d9f914a` | perf: parallelise the per-frame inventory work across a process pool |
| `952a088` | feat: expose the inventory worker count on the scan subcommand |

## What changed

`src/astrometa/inventory.py`:

- **`_frame_payload(path_str, read_pixels)`** — all of one frame's per-file work
  (`content_hash`, header read, `fits.getdata`, `fingerprint`, `pixel_stats`,
  and the header-derived fields), moved verbatim out of the old loop body. Both
  the serial and the parallel path call this one function, so they cannot drift.
  It returns flat picklable scalars and **never a decoded array**.
- **`_record(conn, res, payload)`** — everything a scan writes. Parent process
  only. The upsert SQL is byte-identical to before (verified by normalising
  whitespace and diffing against `HEAD~2`).
- **`_scan_parallel(...)`** — a spawn-based `ProcessPoolExecutor` fed by a
  sliding window, four tasks deep per worker.
- **`_result_or_raise(path, future)`** and **`InventoryWorkerError`** — worker
  failure handling.
- **`default_workers()`** — `os.process_cpu_count() - 1`, floor 1.
- **`scan(conn, roots, read_pixels=True, workers=None)`** — new `workers`
  parameter.

`src/astrometa/cli.py`: `scan --workers N`. Default stays `None` so
`inventory.default_workers()` is the single definition; `--help` reports what
the current machine would use.

## Guarantees, and how each is held

Every one of these has a test **on the parallel path**, not only serially.

| Guarantee | How it survives | Test |
|---|---|---|
| Two independent try blocks; `header:` / `pixels:` prefixes, both recorded if both fail | `_read_header` / `_read_pixels` are unchanged and now run inside the worker, still catching only their own failure | `test_parallel_pixel_failure_still_records_header_fields`, `test_parallel_header_failure_still_records_pixel_fields` |
| `failed` overlaps `added`/`updated` | `_record` increments `failed` and one of added/updated, as before | `test_parallel_scan_matches_single_worker_scan_exactly`, `test_parallel_rescan_is_idempotent` |
| `ON CONFLICT` quarantine guard, `:header_ok` / `:pixel_ok` gating | SQL untouched, still executed in the parent | `test_quarantine_survives_a_parallel_rescan` |
| `first_seen` never overwritten | Absent from the update clause, unchanged | `test_parallel_rescan_is_idempotent` (asserts the stamp explicitly, since it is excluded from the row comparison) |
| Missing root raises `FileNotFoundError` | The walk stays a lazy generator consumed by the parent; the error propagates through `islice`/`next` | `test_missing_root_still_raises_on_the_parallel_path` |
| Exclusions and extensions | `_iter_fits` untouched | existing tests |
| Idempotency | `test_parallel_rescan_is_idempotent` |

### Determinism

Results are consumed **in submission order**, never completion order. This is
load-bearing rather than tidy: two files can share a `content_hash` (the archive
has genuine duplicates) and collide on one row whose `path` is written by
whichever is processed **last**. Completion order would make that column a race
against worker scheduling.

Proven two ways:

- `test_parallel_scan_matches_single_worker_scan_exactly` — a 37-file tree
  containing clean frames, a broken file, a zero-byte file, a NaN-pixel frame
  and a deliberate duplicate pair, scanned at `workers=1` and `workers=16` into
  separate databases. Every column of every row matches, plus the
  `added`/`updated`/`failed` counts and `seen_hashes`.
- On **real data**: the 120-frame benchmark set scanned at 1, 2, 4, 8, 16 and 31
  workers produced **row-for-row identical databases** across all six runs.

### Worker failures stay loud

Split deliberately, matching what the serial code already did:

- **Per-frame read failures** (bad header, unreadable pixels) are caught inside
  the worker exactly as before and come back in `read_error`. The scan continues.
- **Anything else** — a `stat()` that raises, a `content_hash` that cannot be
  computed, a worker that dies outright — aborts the run as
  `InventoryWorkerError`, naming the frame. `future.result()` is always
  retrieved, so an exception can never sit unread in a discarded future.
  `BrokenProcessPool` (segfault, OOM kill) gets its own message explaining why a
  dead worker is a hard abort: it gives no way to tell which frames were read and
  which were dropped, and a silently dropped frame is indistinguishable from a
  deleted one.

`stat()` and `content_hash` are deliberately **not** wrapped into `read_error`. A
`stat` that raises is a statement about the archive (an unmounted root, a
permission change), not about one frame; turning it into 34,400 recorded
`read_error`s would be exactly the silent fallback this project forbids. It
aborted the scan before this change and still does.

`test_worker_failure_aborts_loudly_instead_of_dropping_the_frame` proves this
with a mode-000 file among 24 good ones: `InventoryWorkerError` naming the file,
`__cause__` is the real `PermissionError`, and nothing is committed.

This also got an unplanned live test — my own benchmark harness lacked an
`if __name__ == "__main__"` guard, every worker re-ran the whole benchmark, and
the failure surfaced immediately as `InventoryWorkerError` naming the frame
rather than as missing rows. I then extended the message to name that cause.

### Bounded memory

- The walk is a generator; the tree is never materialised. Only enough of it is
  drawn up front (`8 × max_workers` paths, at most a few hundred) to size the
  pool.
- Work is submitted through a sliding window instead of `executor.map`, which
  would submit all 34,400 items at once and hold every completed-but-unconsumed
  result.
- The window holds path strings and small result dicts. **Decoded arrays exist
  only inside workers, at most one per worker.**
- Measured peak RSS: **0.48 GB per worker**, 0.48 GB parent. A 31 MB `uint16`
  frame becomes ~0.5 GB resident because of the `float64` expansions inside
  `imagekeys`. At the default 47 workers on the 48-core host that is ~23 GB
  against 107 GB free — fine, but worth knowing.

## Measured speedup

Real frames: 120 ASI2600 M31 subs from `/mnt/qnap/astro_data/08_10/M31`
(31 MB each, 3.7 GB total), reached through symlinks. **Nothing was written to
`/mnt/qnap`.** This box: AMD Ryzen 9 9950X, 16 cores / 32 threads, one NUMA node.

### A. Cold, over the SMB link (two disjoint but equivalent 120-frame sets)

| | wall | throughput |
|---|---|---|
| `workers=1` | 49.70 s | 74.8 MB/s |
| `workers=31` | 20.39 s | 182.4 MB/s |

**2.44×** — and capped by the NAS link, not by the CPU.

### B. Frames on tmpfs, NAS removed from the measurement

Serial here is **99.7% CPU-bound** (33.54 s CPU against 33.65 s wall), which
matches the 94% you measured on the deployment host.

| workers | wall | speedup |
|---|---|---|
| 1 | 33.65 s | 1.00× |
| 2 | 18.00 s | 1.87× |
| 4 | 15.23 s | 2.21× |
| 8 | 13.83 s | 2.43× |
| 16 | 14.26 s | 2.36× |
| 31 | 11.77 s | **2.86×** |

**So: 2.4–2.9×, not 20×.** Here is why, measured rather than assumed.

### Why it plateaus

A probe with **zero file I/O** — each worker builds its own frame-sized array in
memory and runs only `fingerprint` + `pixel_stats` on it, the same number of
times regardless of worker count:

| workers | frames/s | scaling |
|---|---|---|
| 1 | 2.29 | 1.00× |
| 2 | 3.98 | 1.74× |
| 4 | 7.15 | 3.11× |
| 8 | 8.14 | 3.55× |
| 16 | 10.81 | 4.71× |

Per-worker inner time inflates from 2.54 s to 8.70 s as workers rise. No disk, no
network, no lock, no queue — so the contention is for memory.

Per-frame CPU breakdown (one warm 31 MB `uint16` frame):

| stage | time | share |
|---|---|---|
| `content_hash` (BLAKE2b) | 50.8 ms | 15% |
| `read_header` | 0.1 ms | — |
| `fits.getdata` | 15.1 ms | 4% |
| `imagekeys.fingerprint` | 129.4 ms | 37% |
| `imagekeys.pixel_stats` | 153.6 ms | 44% |
| **total** | **349 ms** | |

**81% of the frame is `fingerprint` + `pixel_stats`**, and inside those:

| operation | time |
|---|---|
| `np.median(float64)` (in `pixel_stats`) | 103.0 ms |
| `np.percentile(float64, (1,99))` (in `fingerprint`) | 65.9 ms |
| `np.asarray(a, float64)` | 6.7 ms |
| `np.isfinite(f64).all()` | 9.3 ms |

Roughly **half of every frame is two partition-based selection algorithms over
16.2 million elements**. Selection is random-access and latency-bound, and it
saturates the memory subsystem long before it saturates cores. `content_hash`
(BLAKE2b) is genuinely compute-bound and does scale — but it is only 15% of the
frame.

Only ~19% of the per-frame cost parallelises cleanly. That is the whole story.

### What this means for the deployment host

I will not put a number on it. That box has 48 cores and is very likely
dual-socket, giving it two memory controllers where this desktop has one, so its
knee should sit higher than the ~4 cores measured here. But the same wall exists.
A ~14 hour serial projection should land in low single-digit hours, not minutes.

Two caveats on the numbers above, in the honest direction:

- 120 frames is a small sample; spawn startup (~2 s) is ~15% of a 13 s run. The
  real 34,400-frame pass amortises that away, so it should sit slightly better
  than these figures.
- My harness's `cpu_children` column used `getrusage(RUSAGE_CHILDREN)`, which is
  cumulative across runs in a process. I noticed after the fact and have quoted
  **wall clock only**, which is unaffected.

## Testing

- **233 passing**, up from 219. All 219 pre-existing tests pass unmodified — none
  was edited, deleted, or had its premise weakened.
- One new test of mine had a wrong premise on first run (`second.updated ==
  first.added`, which ignores that the duplicate pair walks twice but adds one
  row). I fixed my assertion, not the code.
- 14 new tests: determinism, parallel idempotency incl. `first_seen`, both
  independent-failure directions through the pool, loud worker abort, the
  quarantine guard through the pool, missing root through the pool, a positive
  proof that the pool is used **and runs out of process**, the no-pool-for-a-
  handful case, `workers=0` rejection, and three CLI wiring tests.
- Every test uses `tmp_path`. Nothing writes to `/mnt/qnap`, `/archive` or
  `/live`.

The out-of-process proof is worth naming: it monkeypatches `_read_pixels` to
raise **in the parent only**. A spawned worker re-imports the module clean and
never sees the patch, so frames coming back with fingerprints and no `read_error`
are proof the reads did not happen in the parent.

### Python 3.13

Target is `>=3.13`; this box only has 3.14.7, so I could not run the suite on
3.13.5. What I did instead: both changed modules parse under the 3.13 feature
version (`compile(..., _feature_version=13)`), and the only post-3.12 stdlib call
used is `os.process_cpu_count()`, which is 3.13+ — exactly at the floor. Pinning
the start method also removes the 3.13-vs-3.14 default difference (fork vs
forkserver) as a source of divergence between the two.

## Concerns

1. **The ceiling is memory, not cores — and the fix is not in this module.**
   Nearly half of every frame is `np.median` and `np.percentile` over 16.2 M
   elements. The obvious win is to stop computing an exact median over the full
   array (a subsample, or a histogram-based estimate, would be far cheaper). But
   changing `fingerprint` would invalidate every fingerprint already stored, and
   changing `pixel_stats` moves a number that feeds the culling thresholds. That
   is a deliberate decision about the data contract, not something to slip into a
   performance change. **Not done, deliberately.**

2. **The default of `cpu_count - 1` is probably higher than useful.** On this box
   the knee is around 8 workers; 47 would burn ~23 GB and 47 cores for roughly
   what 8–12 achieve. I kept `cpu_count - 1` because the brief asked for a
   default relative to `os.cpu_count()`, and because the deployment host has more
   memory channels than this desktop, so I would only be guessing its knee from
   the wrong machine. **Recommend trying `scan --workers 12` against
   `--workers 47` on the first real pass and keeping whichever wins** — it is one
   flag, and the flag now exists.

3. **spawn imposes a requirement on callers.** A worker re-imports the caller's
   `__main__`, so module-level code must sit behind
   `if __name__ == "__main__":`. `astrometa.cli` is guarded, so the shipped path
   is fine, but a `python - <<EOF` heredoc cannot drive a parallel scan at all
   (its `__main__` has no importable file). Both surface as a first-frame
   `BrokenProcessPool`, and the error message now names both causes and suggests
   `workers=1`.

4. **The whole pass is still one SQLite transaction.** `conn.commit()` runs only
   at the end, unchanged from before this task, so an abort 30,000 frames in
   loses all of them. This is pre-existing and I left it alone as instructed
   ("perform the upserts exactly as it does today"), but a 34,400-frame run is
   exactly where it starts to matter. Flagging, not fixing.

5. **I got a rationale wrong and corrected it.** My first draft justified spawn by
   claiming a forked child closing an inherited descriptor would drop the
   parent's SQLite lock. I tested it: fcntl locks are not inherited across fork,
   the child holds none to release, and the parent's write lock survives intact.
   The claim was removed from both the docstring and the commit message before
   they landed, and the docstring now records the negative result so nobody
   re-adds it. The two reasons that do hold — an unstable default across 3.13/3.14
   and the fork-with-threads hazard that made 3.14 change that default — are the
   ones stated.

---

# Addendum — review fixes (commit `4f307b0`)

Two Important findings closed before the production run. Suite: **247 passing**
(was 233). No pre-existing test modified.

## FIX 1 — `--workers` is now bounded

`inventory.py:558-561` had no ceiling: the damping only limits workers by how
much *work* there is, so `--workers 100000` against 34,400 frames asked for
4,300 processes.

- **`max_workers()`** — the hard cap, one per available CPU, computed per call
  (affinity can change after import).
- **`resolve_workers(workers)`** — `None` → `default_workers()`; below the cap →
  honoured untouched; above → clamped; `< 1` → `ValueError`.

**Why the CPU count and not an arbitrary number:** more processes than cores
cannot help — measured throughput on this 16-core box is already flat from 8
workers on (2.43× at 8, 2.86× at 31). It also bounds the sharper risk, memory, at
roughly `cpu_count × 0.5 GB`.

**Why too-large clamps but zero/negative raises:** "use everything" is a
satisfiable intent and the ceiling delivers exactly that; zero and negative are
not, so inventing a value would be the silent correction this project forbids.

**The clamp is never invisible.** The scan line now reports the effective count:

```
... workers=32 (--workers 100000 clamped to one per CPU) ...
... workers=4 ...
```

**Bad values are a usage error, not a traceback** (the folded-in Minor). A new
argparse `type=_worker_count` puts them with argparse's own errors:

```
$ astrometa scan ... --workers 0
usage: astrometa scan [-h] --db DB [--root ROOT] [--workers WORKERS]
astrometa scan: error: argument --workers: must be at least 1, got 0
exit=2
```

All four verified against real frames through the installed console script, not
just in tests.

## FIX 2 — the pass commits periodically

`_record()` checkpoints every `COMMIT_EVERY_FRAMES` frames, using
`res.added + res.updated` as the counter (it rises by exactly one per call, so
there is no second piece of state to keep in step). Both the serial and the
parallel path reach it, so they cannot diverge in durability either. `scan()`
still commits once more on the way out, which is what saves the tail.

### The interval: **500 frames**

Sized off measured throughput rather than taste. A parallel scan manages ~10
frames/s on warm local frames and less over SMB, so:

- **Rework budget on a failure:** under two minutes.
- **Cost across the whole archive:** 69 commits over 34,400 frames — unmeasurable
  on a multi-hour pass.

That is the whole trade: whole-run atomicity for resumability, which is only safe
because every pass here is idempotent.

### The safety property you flagged

Verified and now pinned by a test. An aborted scan still propagates its
exception and `cli.main` still does not catch it, so
`disposition.mark_missing()` cannot run against a partial walk.
`test_an_aborted_scan_never_runs_the_missing_sweep` does a clean pass (25 frames
`present`), aborts a second pass mid-walk, and asserts **zero** frames came out
`missing`. This one passes both before and after the change — that is the point:
it guards that the change did not break the ordering, rather than proving the
change.

### Guards proven RED first

I disabled the checkpoint and re-ran before accepting either as green:

| test | without the fix | with it |
|---|---|---|
| `..._keeps_the_frames_already_committed` | 0 rows committed | 16 |
| `..._matches_an_uninterrupted_scan` | `added=25 updated=0` | `added=9 updated=16` |

The second one initially passed **either way** — it only asserted that the final
state converged, which a full restart also achieves. That made it a convergence
test wearing a resumability label, so I strengthened it to assert the
`added`/`updated` split, which is the thing that actually distinguishes resuming
from restarting. Worth recording: it would have shipped looking like a guard
without being one.

The re-run also closes and reopens the connection rather than reusing it. A real
abort kills the process, so the transaction open at that instant is rolled back;
reusing the connection would have quietly carried those uncommitted rows into the
re-run and tested a kinder failure than the one that actually happens.

### Placing the failure deterministically

`rglob` order is not sorted, so "the failure happened partway through" would
otherwise be a coin toss. The tests walk the tree first via `_iter_fits`, pick
`order[17]`, and chmod *that* file — making the arithmetic exact
(`(17 // 4) * 4 == 16` committed) rather than probabilistic.

## Left alone, as directed

`BrokenProcessPool` still names the queue-head frame rather than the frame that
actually killed the worker. Honest enough for diagnosis, and the fix is not worth
the complexity.

## Standing concerns from the main report

Unchanged: the 2.4–2.9× ceiling is memory-bound and lives in `imagekeys`; the
`cpu_count - 1` default is probably above the knee (try `--workers 12` against
`--workers 47` on the first pass); spawn requires a guarded `__main__`.

Concern 4 from the main report — "the whole pass is one SQLite transaction" — is
**now closed** by FIX 2.
