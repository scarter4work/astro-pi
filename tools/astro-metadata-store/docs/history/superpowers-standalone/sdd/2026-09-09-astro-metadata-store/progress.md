# SDD ledger — plan: docs/superpowers/plans/2026-09-09-astro-metadata-store.md

Spec: docs/superpowers/specs/2026-09-09-astro-metadata-store-design.md (read; binding authority)
Worktree: .worktrees/metadata-store on branch feat/metadata-store
BASE: c3c1f59
Venv: .venv (python 3.14.7, numpy 2.5.3, astropy 8.0.1, pytest 9.1.1)
NOTE: dev box is python 3.14.7; DEPLOY TARGET scott-server is 3.13.5.
      pyproject declares >=3.13. Implementers must not use 3.14-only syntax/stdlib.

## Pre-flight scan

### Per-task self-consistency (does each task's text agree with itself?)
| Task | Self-consistent? | Finding |
|---|---|---|
| 1 db/config/schema | yes | tests match DDL; disposition values match Tasks 5/11/12 |
| 2 fitsheader | yes | parser handles all 5 test cases incl. quoted-slash |
| 3 classify | text ok, count wrong | claims "17 tests"; actual is 14 (11 parametrized + 3) |
| 4 imagekeys | NO | flat-image early return width 68 chars vs real 64 (F2) |
| 5 inventory | NO | failed frames counted in BOTH `failed` and `added` (F5) |
| 6 cluster | NO | assign_fields cannot pass its own idempotency test (F3) |
| 7 solve | yes | -ra hours / -spd = dec+90 verified against astap_cli help |
| 8 quality | yes | -analyse (not -analyse2) correct; bg reused from inventory |
| 9 naming | text ok, count wrong | claims "16 tests"; actual is 14 |
| 10 grouping | yes | panel regex verified not to match Sh2-106; dusk rule coherent |
| 11 disposition | yes | quarantine moves, never deletes; dry_run default True |
| 12 manifest | NO | LIKE on paths: `_` in "astro_data" is a wildcard (F4) |
| 13 cli | yes | argparse contract matches both tests |
| 14 first run | NO | reconciliation math uses added+failed, double-counts (F5) |

### Cross-task shared files / interfaces
| Tasks | Produces -> Consumes | Finding |
|---|---|---|
| 1 -> 5,6,9,11,12,13 | db.connect/init_schema | clean; schema covers every column later tasks write |
| 1 -> 5,7,8 | config.Config, EXCLUDED_PATH_MARKERS | clean |
| 2 -> 3,5 | read_header dict | clean; classify tolerates {} |
| 3 -> 5 | classify(filename, header) | clean |
| 4 -> 5,6 | content_hash/fingerprint/hamming/pixel_stats | F2 width bug affects 6's hamming input |
| 5 -> 11,13 | InventoryResult.seen_hashes | clean; mark_missing consumes it correctly |
| 6 -> 7,10 | fields.id, field_id | F3 breaks re-runs |
| 7 -> 9,10 | fields.solved_ra/dec, solve_source | clean |
| 9 -> 10 | fields.object_id | clean; only trusted sources set it |
| 10 -> 13 | build_projects | clean |
| 11 -> 13 | mark_missing/quarantine | clean |
| 12 -> 13 | export_all | F4 |
| 6,7,8,12 all mutate conn.row_factory | shared connection | fragile, contained (F7) |
| GlobalConstraints -> 11 | "quarantine ... (Task 12)" | F1: wrong task number |

## Rulings (pre-flight)

Ruling: F1 — Global Constraints' "(Task 12)" for quarantine is a typo; quarantine
  belongs to Task 11 (disposition.py), per spec §8. Task 12 is Manifest.
  — Why: spec §8 places quarantine with disposition; plan body agrees, only the
    cross-reference is wrong. — Cost if wrong: a reviewer looks in the wrong file. Trivial.

Ruling: F2 — Task 4's flat-image early return must use width (size*size+3)//4 = 64,
  not (size*(size+1)+3)//4 = 68, matching the 256 bits the real path emits.
  — Why: fingerprint strings must be one fixed width or comparisons are misleading.
  — Cost if wrong: flat frames carry an odd-length key; hamming still functions. Low.

Ruling: F3 — Task 6's assign_fields MUST seed its `reps` list from fields already in
  the DB (each with one representative frame's fingerprint and pointing) before the
  loop, or it is not idempotent and fails its own test.
  — Why: the plan mandates a test the plan's own code cannot pass; the test encodes
    the real requirement (Global Constraint: every pass idempotent).
  — Cost if wrong: every rescan duplicates every field, corrupting field counts and
    orphaning solves. HIGH — this one is load-bearing.

Ruling: F4 — Task 12 must not interpolate a raw path into LIKE. Use an ESCAPE clause
  escaping \, % and _ in the prefix.
  — Why: "astro_data" contains _, a LIKE single-char wildcard, so the pattern is
    broader than intended. — Cost if wrong: a manifest could capture sibling dirs. Low
    probability, wrong-data consequence.

Ruling: F5 — `failed` is an OVERLAPPING diagnostic counter, not disjoint from
  added/updated: a frame whose header or pixels fail to read is still inventoried with
  read_error set. Task 14 step 3 reconciles with (added + updated), never (added + failed).
  — Why: the row must exist so the failure is visible and retryable; that is the
    "errors are loud" constraint. — Cost if wrong: reconciliation off by the count of
    unreadable frames, i.e. the exact thing Task 14 is trying to measure.

Ruling: F6 — "Expected: PASS, N tests" counts in the plan are informational. Tasks 3
  and 9 state 17 and 16; actual are 14 and 14. Implementers report the real number.
  — Cost if wrong: none; cosmetic.

Ruling: F7 — global conn.row_factory mutation in Tasks 6/7/8/12 is accepted as-is
  (each CLI subcommand is its own process; current tests are unaffected), recorded as
  a deferred minor for the final whole-branch review.
  — Cost if wrong: a future caller chaining passes gets Row where it expects tuples.

Task 3: minor (deferred): expected test count says 17, actual 14
Task 9: minor (deferred): expected test count says 16, actual 14
Task 6/7/8/12: minor (deferred): conn.row_factory mutated globally on a shared connection

Task 1: minor (deferred): no CHECK constraint on frames.disposition; the three
  literal values are a contract for Tasks 5/11/12 and a CHECK would catch typos at
  insert time. Brief-mandated omission, not an implementer defect.
Task 1: minor (deferred): pyproject declares no test/dev dependency group, so
  `pip install -e .` alone does not provide pytest (the .venv does).
Task 1: complete (commits c3c1f59..59b6201, review clean)

## Task 2 review: 2 Important findings, both plan-mandated. Rulings:

Ruling: T2-F1 — `decode("ascii", errors="replace")` must become a strict decode whose
  UnicodeDecodeError is re-raised as FitsHeaderError. FIX IT.
  — Why: spec §9 requires "unreadable or corrupt FITS recorded with the failure, never
    silently skipped". errors="replace" yields a garbled dict that looks valid, which is
    the opposite. Task 5's inventory already catches exceptions and writes read_error, so
    a loud raise lands exactly where the spec wants it. The plan mandated this line; the
    spec outranks the plan.
  — Cost if wrong: a frame with one stray non-ASCII header byte fails its header read
    instead of parsing. It is still inventoried with read_error set, so it stays visible
    and retryable. Acceptable.

Ruling: T2-F2 — escaped single-quote ('' inside a FITS string) must be parsed per the
  FITS standard, not truncated. FIX IT, and add a covering test.
  — Why: this is the project's documented live bug class — silent truncation of an
    object name — reintroduced. Real targets carry apostrophes (Barnard's Loop, Bode's
    Galaxy, Hind's Variable Nebula), all plausible in this archive. A truncated OBJECT
    card silently mis-identifies frames, and object identity is the whole point of the
    store. Not hypothetical, and it ships untested under the plan as written.
  — Cost if wrong: none identified; correct FITS parsing is strictly better. The risk is
    in NOT fixing it.

Note: my plan's Task 2 code mandated both defects. The reviewer was right to flag them
  as findings rather than defer to the plan's authorship.

Ruling: T2-F2 ambiguity — reviewer left "unescape vs raise" open. UNESCAPE, per the FITS
  standard: '' inside a quoted string denotes one literal apostrophe, so it is valid
  input, not an error. Scan for a closing quote not part of a doubled pair; collapse ''
  to '. — Cost if wrong: none; raising on valid FITS would be strictly worse.
Task 2: minor (deferred): brief's Produces line says dict[str, str | float | bool] but
  the code returns int for whole numbers and its hints say so. Code is more correct than
  the brief; brief interface line should be amended if it is ever reused.
Task 2: fix round 1/5 dispatched (2 findings: strict-ascii decode, escaped-quote
  truncation). FIX_BASE c860a03.
Task 2: fix round 1/5 (2 addressed, 0 open; commits c860a03..221ea47)
Task 2: minor (deferred): test_non_ascii_byte_raises uses a function-local
  `from tests.conftest import ...` instead of the file's fixture pattern, and imports
  across `tests` with no __init__.py (works via pytest rootdir insertion).
Task 2: complete (commits 59b6201..221ea47, review clean after 1 fix round)

## Task 3 review: spec OK, 1 Important (plan-mandated) + 2 warn items. Rulings:

Ruling: T3-F1 — case-sensitivity asymmetry (Light_/Dark/Bias case-SENSITIVE while the
  flat branch is case-INSENSITIVE) must be FIXED: make all filename branches
  case-insensitive.
  — Why: verified against the real archive, not argued from principle. 575 `light_*.fit`
    and 15 `dark*.fit` files exist AND carry NO IMAGETYP card, so they depend entirely on
    the filename branch and currently classify as `unknown`. 575 of them are real light
    frames — science data silently excluded from object identification, which is the exact
    class of invisibility this store exists to remove.
  — Checked the rest of the unknown bucket too: L_n_Lights_ (290) has IMAGETYP='Light
    Frame', F_n_ (120) has 'Flat Frame', Preview_* (~175) has 'Light' — all three already
    classify correctly via the header branch. So the header-wins design is sound; only the
    case sensitivity is wrong.
  — Cost if wrong: making the prefixes case-insensitive could over-claim a file named e.g.
    "lightcurve_*.fit". None exist in this archive (checked); risk is negligible against
    590 frames recovered.

Ruling: T3-W1 (warn: 28,135 archive counts unverified) — NOT a Task 3 gap. Verifying
  classifier output against the real archive is Task 14 Step 2-3 by design. Resolved, no
  action. — Cost if wrong: none; Task 14 still performs the check.

Ruling: T3-W2 (warn: basename vs full path into classify) — MOOT. Task 5's inventory calls
  classify(path.name, header), i.e. basename only, so the unanchored _FLAT regex cannot
  match a "Flats/" directory component. I hold the plan text the reviewer could not see.
  Resolved, no action. — Cost if wrong: if a later caller ever passes a full path, a
  "Flats/" ancestor would misclassify; noted for the final review.

Task 3: minor (deferred): 8 `Stacked_Planet_*.fit` have no IMAGETYP and classify as
  `unknown`; they are arguably `derived`. Left alone to avoid scope creep at 8 files.
Task 3: minor (deferred): classify(None) raises TypeError rather than returning a string.
  Signature is typed `filename: str` (not Optional), and all callers pass path.name, so
  loud-raise is acceptable. Noted only because the review brief asked.
Task 3: minor (deferred): bare-prefix "Dark"/"Bias" matching has no delimiter, so it is
  theoretically over-broad (e.g. "Darkroom.fit"). No such filenames exist in the archive
  (checked). Widened slightly by the case-insensitivity fix; accepted deliberately.
Task 3: fix round 1/5 dispatched (1 Important: case-sensitivity costing 590 real frames;
  1 Minor: correct the report's wrong "redundant" note on the master_flat clause).
  FIX_BASE d41612f.
Task 3: fix round 1/5 (2 addressed, 0 open; commits d41612f..f8555ea)
Task 3: VERIFIED against real archive by controller: filename-branch classification of all
  28,135 names now yields light 24,527 / flat 1,205 / derived 956 / unknown 691 / bias 400
  / dark 356. Exactly 590 frames recovered; no file migrated between other categories.
Task 3: complete (commits 221ea47..f8555ea, review clean after 1 fix round)

## Task 4 review: 2 Important. Rulings:

Ruling: T4-F1 — hamming() must reject differing-width inputs with ValueError. FIX.
  — Why: Task 6 clusters on Hamming distance, so a mismatched-width comparison silently
    yields wrong sky-field groupings, which then decide what gets solved and what object a
    frame is filed under. Same failure class as the width bug caught in pre-flight.
  — Cost if wrong: none; the guard only rejects input that was already meaningless.

Ruling: T4-F2 — pixel_stats must guard non-finite input. FIX.
  — Why: NaN propagates into bg_median -> sky_background -> Task 11 session cull
    thresholds, where every comparison against NaN is false, so nothing is culled and
    nothing says why. Silent defeat of the pass. Implementer chooses raise vs explicit
    absent-value, consistent with fingerprint's degenerate handling, and states which.
  — Cost if wrong: a frame with NaN pixels may now fail its stats read instead of storing
    a wrong number; it is still inventoried with the failure recorded. Acceptable.

Ruling: T4-W1 (warn: non-default `size` / sub-grid arrays) — RESOLVED, no action. Measured
  real frame dimensions: smallest in the archive are 320x240 (planetary), then 480x360 and
  640x480; nothing approaches the 16x17 block grid. All callers use default size. Told the
  implementer NOT to add a _block_mean small-input guard or size validation — dead code.
  — Cost if wrong: a future sub-17px input would produce an all-NaN block mean with a
    numpy RuntimeWarning. No such frames exist today.

Task 4: minor (deferred): _block_mean on sub-grid input yields all-NaN via RuntimeWarning
  rather than the intentional degenerate path. Unreachable for this archive.
Task 4: minor (deferred): fingerprint's `size` parameter is unvalidated; all callers use
  the default.
Task 4: fix round 1/5 dispatched (hamming width guard, pixel_stats NaN guard).
  FIX_BASE 40b4b0b.

## Archive findings (for Task 14 and the runbook)
- 2 ZERO-BYTE .fit files exist and are genuinely truncated:
    6_18/TF Images/ASIAIR/Preview/FOV Cross/Preview_FOV Cross_5.0s_Bin1.fit
    6_18/EMMC Images/Autorun/Light/M27/Light_M27_200.0s_Bin1_0030.fit
  Expected behaviour: content_hash succeeds, read_header raises FitsHeaderError
  (truncated block), Task 5 records read_error and still inventories the row. These are
  the natural real-world regression case for "errors are loud" — use them in Task 14.

Ruling: T4-F2 downstream (CARRY INTO TASK 5) — pixel_stats now RAISES on non-finite
  pixels. Task 5's inventory catches every exception into one `read_error` field, which
  would conflate "file is corrupt" with "file legitimately contains NaN pixels". Registered
  /stacked derived products (949 files) plausibly carry NaN edges and are NOT corrupt.
  Task 5 must therefore record a stats failure SEPARATELY from a header/read failure, so
  Task 14's reconciliation does not report ~949 healthy derived files as unreadable.
  — Why: the point of "errors are loud" is a true signal; a loud but WRONG signal is worse
    than none, and Task 14 exists to reconcile counts.
  — Cost if wrong: Task 14 over-reports failures and someone chases 949 non-problems.
Task 4: fix round 1/5 (2 addressed, 0 open; commits 40b4b0b..96c50ce)
Task 4: minor (deferred): NaN guard test covers NaN but not Inf, though the guard and
  docstring name both.
Task 4: complete (commits f8555ea..96c50ce, review clean after 1 fix round)

Ruling: T5-R1 (plan defect I am correcting) — the plan's Task 5 code wraps header read AND
  pixel read in ONE try, setting header={} on any exception. So a frame whose header parses
  fine but whose pixels raise (now possible: pixel_stats raises on non-finite) loses its
  camera/filter/exposure/pointing metadata too. Split into two independent try blocks and
  record the failures distinctly, via a category prefix in read_error ("header: ..." vs
  "pixels: ..."), keeping the single column so an already-reviewed schema needs no migration.
  — Why: a frame that is 90% readable must keep the 90%. Losing header metadata because
    NaN pixels raised would silently shrink the store's coverage, and ~949 derived files
    plausibly carry NaN edges while being perfectly good metadata sources.
  — Cost if wrong: read_error strings carry a prefix Task 14 must parse. Trivial.

Ruling (pre-emptive, T5 named risk #1) — "read amplification" is NOT a defect. MEASURED,
  not argued: on 5 real 16 MB frames, current (content_hash full read + astropy full read)
  = 0.175 s/frame; a single-buffer variant (one read, hash the bytes, decode from BytesIO)
  = 0.176 s/frame. Speedup 1.00x. Projected full-archive inventory ~82 min either way.
  — Why: the second read is served from the OS page cache (per-file locality), so the
    bottleneck is CPU — decode + fingerprint + hash — not I/O. The single-buffer variant
    would also hold raw bytes AND the decoded array at once, i.e. worse peak memory.
  — Therefore: if the review raises read amplification as Important, it is contestable on
    evidence and I will park it rather than dispatch a refactor that buys 0%.
  — Cost if wrong: if real runs are I/O-bound in a way this 5-frame local test missed
    (e.g. cold cache over CIFS), the fix remains available and cheap. Task 14 measures the
    real wall-clock and will expose it.
USEFUL FOR RUNBOOK: projected inventory pass ~82 min for 28,135 frames, CPU-bound.

## Task 5 review: Approved, 2 Important + 1 warn. Rulings:

Ruling: T5-F1 — read amplification: PARKED, not fixed. Measurement above (1.00x, ~82 min
  either way, page-cache served, CPU-bound). Refactoring buys 0% and worsens peak memory.
  The reviewer's finding is correct as an observation and wrong as a cost claim.

Ruling: T5-F2 — missing root must RAISE, not `continue`. FIX.
  — Why: worse than the review stated. CLI does scan() then mark_missing(seen_hashes). An
    unmounted root returns added=0 with an EMPTY seen_hashes, so mark_missing would flip
    all ~28,000 rows to 'missing' — and 'missing' is exactly how this system records a
    deliberate cull. An unmounted NAS would be indistinguishable from the operator having
    deleted the whole archive. Catastrophic silent failure.
  — Cost if wrong: a deployment with a legitimately absent optional root now fails loudly
    instead of proceeding. Acceptable; both roots are mandatory bind mounts.

Ruling: T5-W1 — `.fits` extension: FIX, and it is a SPEC-level gap I created.
  — Evidence: the archive holds 6,290 `.fits` files alongside 28,123 `.fit`. Sampled: real
    light frames from a mono filter-wheel rig (L/R/G/B/Ha/Oiii, Gain/HDR/Temp naming) on
    M57, NGC 281, M33, M27, carrying IMAGETYP='Light'. classify() already handles them via
    the header branch; the walk simply never sees them. ~22% more science data invisible.
  — My spec's problem statement said "28,135 .fit files" and scoped the whole project on
    that. I never checked for the long extension. The spec's counts need amending.
  — Excluded deliberately: 2,913 .xisf (PixInsight format, different parser, processed
    intermediates not subs).
  — Cost if wrong: none identified; widening the glob only adds real FITS data.

Ruling: T5-F2 downstream (CARRY INTO TASK 11) — mark_missing must additionally refuse to
  operate on an empty/degenerate seen_hashes set, as belt-and-braces against the same
  catastrophe from any other cause. Raising in scan() closes today's path; the guard in
  mark_missing closes the class.

Task 5: minor (deferred): read_pixels=False path untested.
Task 5: minor (deferred): .xisf (2,913 files) not inventoried; out of scope by ruling.
Task 5: fix round 1/5 dispatched (.fits glob; loud missing root). FIX_BASE 577dceb.
Task 5: fix round 1/5 (2 addressed, 0 open; commits 577dceb..4065bfa)
Task 5: VERIFIED by controller against real data: _iter_fits over /mnt/qnap/astro_data/M57
  returns 311 files, all .fits, exactly matching find's ground truth of 311.
Task 5: complete (commits 96c50ce..4065bfa, review clean after 1 fix round)

## Task 6: CONTROLLER-FOUND DEFECT (pre-review), measured against real data

Ruling: T6-R1 — the plan's `fp_threshold` default of 12 is WRONG. Change to 80.
  — Measured 2026-09-09 on real frames (2026-09-08 night, ASI585MC Air, 6 frames each):
      WITHIN  Sh2-106     min=24 med=50 max=143 (143 = the 182deg meridian-flipped frame)
      WITHIN  IC1848_1-1  min=20 med=26 max=30
      WITHIN  IC1848_2-2  min=24 med=38 max=45
      BETWEEN Sh2-106 vs IC1848_1-1  min=118 med=126 max=138
      BETWEEN Sh2-106 vs IC1848_2-2  min=120 med=129 max=138
      BETWEEN IC1848_1-1 vs 1848_2-2 min=113 med=126 max=135
  — Why it matters: at 12, NO two real frames cluster. All ~24,000 light frames become
    their own field, so Task 7 solves every one at ~12s instead of one per field — roughly
    80 hours instead of minutes. The pass's entire reason for existing is destroyed.
  — Why the tests did not catch it: the brief's synthetic fixture (fixed-seed noise + 40
    bright blocks) yields a same-field distance of 0. Real frames carry dithering, seeing
    variation and noise. GREEN unit tests were actively misleading here.
  — Why 80: above observed within-field max (45, ex-rotation), 33 below observed
    between-field min (113). Harms are asymmetric — too low costs solve time, too high
    files frames under the WRONG OBJECT. Err low. Pointing is ANDed in and is the strong
    discriminator, so a false merge also requires identical sky position, making 80 safe.
  — Accepted behaviour, not fixed: a meridian-flipped frame (182deg vs 1deg) measures
    135-143 against its own field and will split off. Costs one extra solve; still resolves
    to the same object by coordinates. Rotation-invariant hashing is out of scope.
  — Cost if wrong: threshold is one named constant with the measurement recorded beside it;
    Task 14 validates against the real archive and can retune in one line.
  — SPEC/PLAN IMPACT: the plan's Task 6 text and the constant both need amending. Logged.

Ruling: T6-R2 — assign_fields returns `created` (fields newly inserted this call), not
  len(reps). The interface says "number of fields created" and len(reps) would misreport
  after seeding. Implementer's reading upheld.
  — CARRY INTO TASK 13: the CLI prints `fields={assign_fields(...)}`; label it
    fields_created so a re-run printing 0 is not misread as "no fields exist".

Task 6: minor (deferred): greedy single-link matching against one fixed representative per
  field — a frame matching a non-representative member but not the rep creates a new field.
  Accepted; bounded by the 80 threshold plus the pointing AND. Flagged for final review.

Ruling: T6-F1 — representative drift breaks idempotency. FIX, two parts.
  — Finding: _seed_reps picks the smallest-content_hash member as each field's rep. A
    BLAKE2b digest sorts independently of capture/insertion order, so on an incremental
    run a new frame has ~50% odds of taking over as rep. Greedy single-link only
    guarantees members match the CHAIN, so a member that matched the old rep can fail
    against the new one and be moved to a different field. Violates "two runs must not
    change assignments" — the very constraint the seeding fix existed to satisfy.
  — Why the existing tests missed it: the manual 3-run check used literal hashes
    "h1"/"h2"/"h3", which sort in insertion order, so the rep never changed hands.
  — Fix part 1 (the guarantee): never reassign a frame that already has field_id; cluster
    only field_id IS NULL rows. Makes idempotency structural rather than dependent on rep
    selection being lucky.
  — Fix part 2 (convergence): seed reps by insertion order (implicit rowid) rather than
    content_hash, so incremental runs converge on the same result as one full run. Relies
    on rowid stability, which holds absent VACUUM — to be noted in a comment.
  — Cost if wrong: part 1 means a frame assigned under an early, wrong threshold keeps
    that assignment until explicitly re-clustered. Acceptable: Task 14 runs against an
    empty store, and a deliberate re-cluster can null field_id first.
Task 6: fix round 1/5 dispatched (representative drift; + Minor fingerprint IS NOT NULL
  filter in _seed_reps). FIX_BASE 7a94fca.
Task 6: fix round 1/5 (1 Important + 1 Minor addressed, 0 open; commits 7a94fca..55b9fa7)
Task 6: minor (deferred): test comment says "0.5 deg apart"; actual computed separation is
  0.383 deg. Test remains valid; only the comment is imprecise.
Task 6: complete (commits 4065bfa..55b9fa7, review clean after 1 fix round + 1 pre-review
  controller correction)

Ruling: T7-W1 (warn: reviewer could not confirm from the diff that the real-ASTAP
  integration test actually ran rather than skipped) — RESOLVED by controller. Ran
  tests/test_solve.py -rs: `test_solve_frame_against_real_astap PASSED`, not skipped. The
  skipif guard did not fire on this host, so the real binary was genuinely exercised.
  No action. — Cost if wrong: none; verified directly.

Ruling: T7-F1 — unchecked subprocess returncode. FIX, but NOT as the reviewer proposed.
  — Measured against the real /opt/astap/astap_cli before ruling:
      genuine unsolvable frame (Moon, no star field) -> returncode 1
      successful solve                               -> returncode 0
      bad database dir (-d /nonexistent)             -> returncode 1, .ini STILL produced
      missing input file                             -> returncode 0, no .ini
    So returncode CANNOT discriminate an astronomical failure from an environment failure
    (both 1), and a missing input file reports success. Any cap-branching keyed on
    returncode would be built on sand. Ruled it out on evidence.
  — Why it still matters: solve_attempts caps at 3, so a misconfigured install on the first
    run burns the cap on every field and permanently marks ~24,000 fields failed,
    recoverable only by manual DB surgery. Poisoning risk, not just diagnosability.
  — Fix ordered: (a) record returncode + bounded stdout/stderr tail into fields.solve_error
    so an operator can tell "no stars" from "database missing" from the row; (b) preflight
    astap_bin executable + astap_db_dir present ONCE before the loop and raise loudly,
    catching the misconfiguration class before any attempts are burned.
  — (b) subsumes the review's Minor about FileNotFoundError on a missing binary.
  — Cost if wrong: preflight adds two stat calls per pass and could refuse to start in an
    exotic deployment where the binary is resolved via PATH rather than an absolute path.
    Acceptable; config names an absolute path.
Task 7: minor (folded into fix): LIMIT string-formatted rather than parameterised.
Task 7: fix round 1/5 dispatched (solve diagnostics + environment preflight + LIMIT
  parameterisation). FIX_BASE 537e03c.

CORRECTION to my T7-F1 measurement (controller error, caught by the implementer):
  I recorded "missing input file -> returncode 0, no .ini". That was WRONG, and wrong
  because my test was bad: I passed `-f /nonexistent.fit` at filesystem root, where astap
  cannot write its sidecar, so the absent .ini was an artifact of my own setup. Clean
  re-measure in a writable temp dir: returncode **1**, .ini IS produced, containing
  `ERROR=Error reading image file.`
  Corrected fact table:
      success                    -> rc 0
      unsolvable frame (Moon)    -> rc 1, .ini PLTSOLVD=F
      bad database dir           -> rc 1, .ini ERROR=No star database found.
      missing input file         -> rc 1, .ini ERROR=Error reading image file.
  So returncode still cannot discriminate (every failure is 1), but the implementer found
  the real discriminator I missed: the .ini's **ERROR=** field, which parse_ini was
  silently dropping by reading only WARNING=. Its fix (prefer ERROR over WARNING, fold in
  returncode + bounded output tail) is strictly better than what I ordered.
  — Lesson for the final review: my "evidence" here was a flawed experiment stated as
    fact. The implementer re-measured rather than trusting me, which is why it held.
Task 7: fix round 1/5 (1 Important + 2 Minor addressed, 0 open; commits 537e03c..6a9eecb)
Task 7: complete (commits 55b9fa7..6a9eecb, review clean after 1 fix round)

Ruling: T8-R1 (CARRY INTO TASK 8, learned from Task 7) — quality.measure_frames shells out
  to the same astap_cli and inherits the same poisoning risk. It must (a) preflight the
  binary/db once before the loop and raise loudly, and (b) NOT write a `quality` row when
  the subprocess itself fails.
  — Why (b): measure_frames skips already-measured frames via LEFT JOIN on `quality`. The
    plan's code writes a row with NULL metrics on timeout, which would permanently mark the
    frame measured-with-no-data and never retry it. NULL must mean "not measured"; a real
    run that found no stars is a legitimate result and DOES get a row.
  — Cost if wrong: a permanently unmeasurable frame is retried on every pass. Acceptable —
    the pass is manual and Task 14 surfaces the failure count.
Task 8: minor (deferred): quality.py imports solve._preflight/_diagnostics (underscore
  names) across modules. Reviewer recommendation: LEAVE IT — both helpers are generic,
  reuse was controller-directed, and extraction would touch Task 7's reviewed module for
  zero behavioural gain. Revisit only if a third consumer appears.
Task 8: minor (deferred): measure_frames commits per row rather than batching; matches
  solve.py precedent, fine at these volumes.
Ruling: T8-W1 (warn: real-binary probe claims unverifiable from diff) — RESOLVED. I
  independently captured `-analyse` output earlier this session: HFD_MEDIAN=3.9, STARS=240
  on stdout, and separately confirmed -analyse2 performs a solve instead. Consistent with
  the report. No action.
Task 8: complete (commits 6a9eecb..a180a1d, review clean, no fix round)

Ruling: T9-F1/F2 — both Important findings are coverage gaps, not logic errors; FIX as
  tests only, no implementation change.
  — F1: the confidence gate's NEGATIVE path had zero coverage. That path (a dirname or
    object_card claim must NOT set fields.object_id) is the property that stops a
    directory-name guess from eventually authorising a physical move into the wrong object
    folder in phase 2. Correct-by-inspection is insufficient for the project's single most
    safety-critical behaviour. Also required: proof that a weak claim does not poison the
    field for a later trustworthy one.
  — F2: the COALESCE ra/dec deviation is endorsed but untested; the exact clobber scenario
    it exists to prevent was never exercised.
  — Cost if wrong: none; adding regression guards to correct code carries no risk.
Task 9: fix round 1/5 dispatched (gate negative-path tests, COALESCE test, KeyError pin).
  FIX_BASE dc2d598.
Task 9: fix round 1/5 (2 Important + 1 Minor addressed, 0 open; commits dc2d598..2656cc6)
Task 9: complete (commits a180a1d..2656cc6, review clean after 1 fix round)

Ruling: T10-R1 (controller-found, pre-review) — the plan's `build_projects` is NOT
  idempotent, and my pre-flight scan MISSED it (I marked Task 10 self-consistent after
  checking the panel regex and dusk rule, but never checked idempotency — same defect class
  as T6-F3 which I did catch). It INSERTs a row per bucket on every call with no conflict
  handling, so a second run duplicates every project; `frame_projects` then accumulates
  memberships across the duplicates via INSERT OR REPLACE on a new project_id each time.
  — Fix: make build_projects a full REBUILD of the derived tables — clear `frame_projects`
    and `projects`, then rebuild from `frames`. Both are wholly derived from frame data, so
    a rebuild is deterministic and idempotent by construction, and needs no schema change.
    (The "never delete by machine" constraint governs ARCHIVE FILES, not derived DB rows.)
  — Rejected alternative: dedupe by (object_id, filter, started_at). started_at = min of
    the bucket's capture instants, which shifts if frames earlier in the same night are
    pulled later — so the key is not stable. A session_date column would fix that but means
    migrating an already-reviewed schema.
  — Cost if wrong: project ids churn on every rebuild. Nothing references them today; if
    anything ever does, this needs revisiting.

Ruling: T10-V1 — the dusk rule is VALIDATED against real ground truth; the 193 apparent
  mismatches are ARCHIVE inconsistencies the store correctly surfaces, NOT code defects.
  — Method: NAS dirs named YYYY-MM-DD encode the night a session started, so
    session_date(capture_instant(filename)) should reproduce the folder name. Ran it over
    all 2026-06..09 dirs: 2,572 frames MATCH, 193 mismatch in exactly four clusters.
  — Cluster 1: "2026-07-22 -> 2026-07-14" x52. This is the KNOWN over-pull already
    documented in project memory (the 07-22 pull copied the whole M 17 dir and dragged the
    07-14/07-15 session along). Memory records 52 frames; the tool independently found 52.
    Strong validation.
  — Clusters 2-4: 2026-06-04 (x50), 2026-06-05 (x48), 2026-06-16 (x43) are off-by-one-day.
    Inspected the capture instants: 2026-06-04 holds TWO nights (50 frames at 00:00-04:xx =
    the post-midnight tail of the 06-03 session, plus 17 at 22:00-23:xx = the start of the
    06-04 session). 2026-06-05 and 2026-06-16 are ENTIRELY post-midnight, i.e. tails of the
    preceding nights filed under their datestamp rather than their session night.
  — Conclusion: code correct, archive inconsistent. No fix to Task 10.
  — FOR TASK 14 / RUNBOOK: this check is a genuinely useful archive audit in its own right.
    Report the mismatch clusters; they mark folders that mix two sessions or are named for
    the morning rather than the night. A stack pointed at 2026-06-04 would silently
    integrate two different nights.

Ruling: T10-F1 — panel regex `_(\d-\d)_` misses double-digit panels. UPGRADED from the
  review's Minor to Important on real-archive evidence, and FIXED.
  — Evidence: M42 is a large mosaic whose real panel tokens run 1-1..9-1 AND 1-10, 1-11,
    1-12, 2-10 ... 8-12. Enumerated from actual filenames: 24 distinct double-digit
    combinations, 5 frames each = 120 frames. Matching NAS dirs exist (2_4_2023/M42_1-11,
    M42_7-12, M42_6-12, ...).
  — Failure trace: `_1-10_` puts `0` where the regex demands `_`, so panel_of returns None.
    Those 120 frames lose panel identity, bucket as kind='session' not 'mosaic', and get
    flattened together — the exact "stacker registers disjoint sky regions" failure the
    panel level exists to prevent.
  — Fix: `\d` -> `\d+` in both groups. Verified safe against false positives on real names
    before ordering it: Sh2-106 still yields no panel (preceding `_` is followed by `S`),
    and datestamps like `_20260713-225546_` cannot match because the regex still requires
    `\d+(\.\d+)?s_` to follow and `182deg_` does not.
  — Cost if wrong: a target literally named `<digits>-<digits>` followed by an exposure
    token would be misread as a panel. No such name exists in this archive.
Task 10: fix round 1/5 dispatched (double-digit panel regex). FIX_BASE 0c58bcc.
Task 10: fix round 1/5 (1 Important addressed, 0 open; commits 0c58bcc..ee7aa57)
Task 10: VERIFIED by controller across all 28,135 real filenames: panels detected
  1,998 -> 2,118, exactly 120 recovered, 24 double-digit panels, 0 malformed values.
Task 10: complete (commits 2656cc6..ee7aa57, review clean after 1 fix round)

Ruling: T11-R1 (cross-task integration defect, raised by the Task 11 implementer) —
  quarantine() moves a frame to rejected/, then the next inventory.scan() walks in,
  recomputes the same content_hash, and its ON CONFLICT sets disposition='present',
  silently erasing the cull. Same failure class this task exists to kill, one layer down.
  — Implementer offered two options: add "rejected" to EXCLUDED_PATH_MARKERS, or defer.
    I ruled a THIRD: fix the clobber, not the walk.
  — Why not exclude: culled frames are deliberately KEPT, not deleted. Excluding rejected/
    means the store stops knowing whether they still exist on disk — trading one blindness
    for another, in the one project whose entire purpose is recording why a frame is absent.
  — The invariant: inventory owns `present` and `missing`; the operator owns `quarantined`.
    A scan may move missing -> present when a file reappears. It must NEVER move
    quarantined -> present.
  — Ordered: surgical change to inventory.py's ON CONFLICT so it preserves `quarantined`;
    keep the path update (relocating a quarantined frame's recorded path to rejected/ is
    useful); leave EXCLUDED_PATH_MARKERS untouched; add a full-cycle integration test
    (inventory -> quarantine -> rescan -> still quarantined) with RED proven first.
  — This touches inventory.py, reviewed under Task 5. Controller-directed, not scope creep.
  — Cost if wrong: a frame manually moved OUT of rejected/ by the operator would stay
    marked quarantined until explicitly un-quarantined. That is arguably correct anyway —
    intent should outlive a file move — and it is recoverable with one UPDATE.
Ruling: T11-R1 implemented in commit 396450c as
  `disposition=CASE WHEN disposition='quarantined' THEN disposition ELSE 'present' END`.
  Controller VERIFIED the load-bearing SQLite semantic independently (in-memory DB,
  the exact shipped clause shape): an unqualified column in DO UPDATE SET resolves to the
  PRE-CONFLICT row, so a 'quarantined' row survives while a 'missing' row correctly becomes
  'present'. If that resolution had been backwards the guard would have silently done
  nothing — checked rather than assumed.

Ruling: T11-F1 — repeat quarantine nests rejected/rejected/. FIX by making quarantine
  idempotent (already-quarantined, or path already under rejected/, returns current
  location without moving).
  — Why: every other pass in this project is idempotent by constraint; this one was not.
  — Cost if wrong: an operator wanting to re-quarantine after manually restoring a frame
    must clear the disposition first. Acceptable and discoverable.

Ruling: T11-F2 — shutil.move silently overwriting the destination. UPGRADED from the
  review's Minor to must-fix. FIX by refusing loudly when dest exists, in BOTH dry-run and
  real mode; no uniquifying suffix.
  — Why the upgrade: on one filesystem shutil.move is os.rename, which overwrites without
    complaint. For the single function whose promise is "moves, never deletes", silently
    destroying what already sat at the destination IS a deletion, violating the project's
    hardest constraint. And it is not hypothetical: the operator's own documented manual
    cull method is "move the bad sub into rejected/", so that directory routinely already
    holds files he put there by hand — and 1,989 filenames in this archive collide.
  — Chose raise over auto-rename deliberately: a name collision in rejected/ is something
    the operator must see and resolve, not something the tool should paper over.
  — Cost if wrong: an auto-cull run halts on a collision instead of proceeding. Correct
    trade for a delete-shaped risk.
Task 11: minor (deferred): backfill_known_culls' return counts matches, not distinct
  newly-culled frames. Documented behaviour, correct as specified.
Task 11: fix round 1/5 dispatched (quarantine idempotency; refuse-on-existing-dest).
  FIX_BASE 396450c.
Task 11: fix round 1/5 (1 Important + 1 upgraded-Minor addressed, 0 open;
  commits 396450c..cb572cd)
Task 11: minor (deferred): quarantine's structural guard tests only the IMMEDIATE parent
  (src.parent.name == 'rejected'), not 'rejected' as an ancestor. Unreachable via this
  function alone (it only ever creates one level); would need an external process to write
  a deep rejected/ path while leaving disposition='present'.
Task 11: complete (commits ee7aa57..cb572cd, review clean after 1 fix round + 1
  controller-directed cross-file fix)

Ruling: T12-R1 (SPEC CONTRADICTION I created) — the Global Constraint says "never write to
  the archive", and the deployment mounts are read-only by design (/archive ro, /live ro
  CIFS). But Task 12's export_dir writes `.astro-manifest.json` INTO each leaf dir on the
  NAS. Those cannot both hold.
  — Resolution: "never write to the archive" governs FITS DATA — no modify, move, rename or
    delete of frames, and no FITS header writes. The manifest sidecar is the ONE sanctioned
    write, designed into the spec as the durability mechanism so labels survive DB loss and
    travel with a moved folder.
  — But it still cannot be written through a read-only mount. The plan's export_dir already
    has an `out_dir` escape hatch; `export_all` does NOT accept or propagate it, so the
    default path fails on the deployment host. Ordered: export_all takes out_dir and
    propagates it, preserving relative structure so a mirrored manifest maps back to its
    leaf dir; and a write failure must raise loudly, never be skipped.
  — Cost if wrong: manifests land in a mirror tree rather than beside the frames unless the
    operator mounts rw. Acceptable — the DB remains the working store either way.

Ruling: T12-F1 — _mirror_leaf_dir defends only the absolute case; a relative leaf_dir with
  `..` segments escapes out_dir because joinpath does not collapse them, and export_dir then
  mkdirs there. FIX with a resolved containment check.
  — Why not defer despite low exposure: the function CREATES DIRECTORIES, and the Task 13
    CLI is about to add a second, unvalidated caller. Cheap guard, real blast radius.
  — Cost if wrong: a legitimate relative leaf_dir now raises. All current callers pass
    absolute DB-sourced paths, so no real caller is affected.
Task 12: fix round 1/5 dispatched (traversal guard + 2 Minor docstring/comment notes).
  FIX_BASE 507f9a9.
Task 12: fix round 1/5 (1 Important + 2 Minor addressed, 0 open; commits 507f9a9..8a9a3ac)
Task 12: complete (commits cb572cd..8a9a3ac, review clean after 1 fix round)
Task 13: minor (deferred): cli.py connects and init_schemas before dispatch, so even a
  doomed `scan` touches the sqlite file before raising. Harmless, idempotent.
Task 13: complete (commits 8a9a3ac..8ff3dcb, review clean, no fix round)

## FINAL WHOLE-BRANCH REVIEW (opus): Needs fixes before merge. 5 cross-cutting defects.

Ruling: FR-5 severity CORRECTED by controller measurement, finding UPHELD on other grounds.
  — The reviewer reported "Measured: RA = '02 51 27.0' parses to str". I sampled 200 frames
    archive-wide: every RA card is a bare decimal (N.N shapes only, 187 hits). The
    sexagesimal form appears solely under OBJCTRA (e.g. '21 01 37'), a keyword this code
    does not read. So the stated trigger is NOT present in this archive's RA cards.
  — The finding nonetheless stands, for a better reason: cluster.py commits ONCE at the end,
    so a single uncoercible value would abort and DISCARD an entire 34,000-frame run. That
    is a real robustness defect independent of today's data, and across 7 cameras and three
    naming eras an odd card is a matter of time. Fix ordered.

Ruling: FR-9 (backfill silently no-ops on the motivating case) — CONFIRMED by measurement,
  and it is the most important finding of the run.
  — Checked: the ZFS backup at /data/backups/qnap holds 91 IC 59 frames, the same post-cull
    count as the live NAS, with HaO3 indices stopping at 0018 while the culled range is
    0019-0040. The culled frames are gone from BOTH copies the store can see. They survive
    only on the ASIAIR, which is not one of the store's roots.
  — So backfill_known_culls, which only UPDATEs existing rows, would find nothing to update
    and return 0 silently — on the exact 39 frames this project exists to stop mis-reporting.
  — Ordered: (a) report matched AND unmatched counts loudly; a cull entry matching zero
    frames must be a visible outcome, never a silent 0. (b) persist unmatched culls in a
    small `known_culls` table so the knowledge exists independently of a frame row, since
    there may never be one.
  — Rejected: synthesising fake `frames` rows with invented content_hashes. The primary key
    means "the bytes of this file"; fabricating one to represent a file that exists nowhere
    would corrupt the store's central identity claim.

Ruling: FR-7 (spec 6.9 adjacent-night project merge) — DEFERRED, not fixed in this wave.
  — Why: it is a genuine spec gap but not a correctness or safety defect, and it is a
    behaviour change to bucketing that Task 14's real output should inform. Every night
    being its own project is wrong but harmless and visible.
  — Cost if wrong: multi-night campaigns appear as separate projects until implemented.

Ruling: FIXWAVE-C2 (flagged: two tests reference /mnt/qnap, "violating" the standing test
  constraint) — NOT a violation. My constraint said "no test may touch /mnt/qnap", but I
  also explicitly invited skip-guarded real-binary integration tests in Tasks 7 and 8, which
  require a real frame. My own instructions conflicted; the implementer was right to flag it
  rather than silently delete the tests.
  — The constraint's INTENT is: no test may WRITE to, move, rename or mutate the archive.
    A read-only, skip-guarded read is exactly what I asked for and is correct.
  — Verified the references are `root = Path("/mnt/qnap/astro_data")` used only to locate a
    frame to read, plus one comment in test_manifest.py. No test writes there.
Ruling: FIXWAVE-C4 (manifest sidecar does not carry object_card/leaf_dir, so a restored DB
  has no grouping identity until rescanned) — PARKED, real but not blocking.
  — Why: FIX 1 makes restore-then-rescan fully functional, and a rescan is the normal
    recovery path, so the manifest remains a genuine safety net. Adding the two fields to
    _FIELDS is a one-line follow-up.
  — Cost if wrong: a restore-only recovery groups everything into one project until the
    first rescan. Visible, not silent.
Ruling: FIXWAVE-C1 (a pre-fix store could hold a field representative with a text header_ra;
  implementer chose per-row containment over refusing to run) — ACCEPTED. Task 14 runs
  against an empty store, so the degradation path is unreachable for the first real run.
Ruling: FIXWAVE-C3 (quality.py still accepts a frame with NULL bg_median) — CARRIED. FIX 1
  removed the way that state persisted; the leniency itself is harmless.

## Final scoped re-review: all 8 fixes ADDRESSED; wave introduced 2 regressions + 2 minors.

Ruling: RR-1 — `frame_type=excluded.frame_type` left UNGATED while every sibling column was
  correctly gated on :header_ok. FIX (targeted, same clause).
  — Verified by the reviewer: a frame with IMAGETYP='Light Frame' named Autosave001.fit
    scans as `light`; a transient header-read failure on rescan rewrites it to `derived`,
    dropping it from cluster.assign_fields (filters frame_type='light') and build_projects.
    That is the exact silent-drop class FIX 1 existed to close, reintroduced by a column the
    fix newly added to the update clause.
  — Why I am fixing this despite "no second fix wave": it is a regression the wave itself
    introduced, one line, in the clause already under edit, and it would bite Task 14 — a
    transient header failure across 34,000 files over CIFS is plausible, not exotic.

Ruling: RR-3 — captured_at gated on :header_ok, so FIX 8's filename fallback never applies
  on a rescan whose header read failed, which is the one case it was added for. FIX
  (same clause, same one-line family).

Ruling: RR-2 — init_schema has no migration, so a store built from the pre-wave schema lacks
  the new columns and build_projects fails OperationalError. PARKED.
  — Latent, not live: no .sqlite exists in the tree and Task 14 creates a fresh store. It
    becomes live the moment a schema change lands after the first real run.
  — TOP FOLLOW-UP for the next phase. Recorded as such.
  — Cost if wrong: a future schema change silently breaks an existing store until someone
    writes a migration.

Ruling: RR-4 — known_culls.matched stores a count, not the boolean its name and DEFAULT 0
  imply. PARKED, cosmetic.
Final fix wave: complete (commits 8ff3dcb..4ae4928, 9 commits, all 8 findings addressed;
  2 wave-introduced regressions corrected; 2 residuals parked with rulings).
Branch state: 35 commits, 219 tests passing, working tree clean.
Tasks 1-13 COMPLETE. Task 14 (first real run / deployment) NOT started — it is deliberately
  manual and has side effects on scott-server, so it goes to the user for a decision.

## Parallelisation (post-plan, driven by the real Task 14 run)

Measured on the deployment host before the change: 2.6 TB to read, CPU 3h37m of 3h52m
  elapsed (94% CPU-bound) on ONE core with 47 idle, projecting ~14h. Killed at 28%.
Implementer measured after: 2.44x cold over SMB, 2.86x on tmpfs, flat past 8 workers.
  NOT the ~20x I implied was available. Root cause measured, not guessed: np.median (103ms)
  + np.percentile (66ms) over 16.2M elements are memory-latency-bound and are 81% of the
  349ms per frame; only content_hash (15%) parallelises cleanly.
Reviewer verified determinism is STRUCTURAL (submission-order deque), and verified failure
  handling by injecting os._exit(9) into a worker: aborts naming the frame, 0 rows committed.

Ruling: PAR-F1 — `--workers` unbounded. FIX before the run. Reviewer replayed the damping
  formula: --workers 200 -> 200 processes (>10 GB RSS); --workers 100000 -> 4,300. That is
  a fork bomb one digit away on a host running the user's Minecraft server and two other
  containers. Clamp + route bad values through parser.error().

Ruling: PAR-F2 — whole pass is ONE transaction. FIX before the run (pre-existing, not a
  regression).
  — Why now: the run reads 2.6 TB, roughly half over SMB, where a transient EIO in stat()
    or content_hash is realistic. Combined with correct abort-on-worker-failure, one blip at
    frame 34,000 discards the entire multi-hour pass. I already lost a 4-hour run today.
  — Periodic commit is safe precisely because every pass is idempotent: a partial commit
    plus a re-run converges. Trades whole-run atomicity for resumability, the right trade
    for a multi-hour pass.
  — Must verify mark_missing still cannot run against a partial walk; a sweep after an
    aborted scan would mark thousands of frames `missing`, the worst outcome in this system.

Ruling: PAR-F3 — BrokenProcessPool names the queue-head frame, not necessarily the killer.
  CARRIED. Honest enough for diagnosis; fix not worth the complexity.

CORRECTION TO MY OWN ESTIMATES (recorded for the runbook):
  I told the user "~450 GB" and "a couple of hours". Actual: 2.6 TB across both roots and
  ~14h serial. Wrong on volume (assumed 28,135 x 16 MB; the archive has 34,400+ frames and
  many are far larger) and wrong on run design (pointed it at both roots, which hold the
  same data, doubling the work for ~205 frames of delta).

Ruling: RUN-1 — scan the LIVE NAS root only, not both roots.
  — Why: /archive (ZFS backup) and /live (NAS) hold the same data, and dedup requires
    hashing, so scanning both costs 2.6 TB to gain ~205 frames of delta. /live is the
    authoritative, current copy and is the same 1.3 TB. Halves the run AND gets today's
    state rather than yesterday's.
  — Consequence accepted: --root narrows the scan, so FIX 2 correctly suppresses the
    whole-DB missing sweep. That is right for a fresh store — there is nothing to mark
    missing on a first build.
  — Cost if wrong: reading over SMB rather than local ZFS. Measured as CPU-bound, so the
    I/O path matters far less than the halved file count.

RUN LAUNCHED 2026-09-09T22:27:53-04:00 — scan --root /live/astro_data --workers 16.
  Verified genuinely parallel: load average 11.5 (serial run sat near 1.0), spawn workers
  present. Deployment suite green first: 245 passed / 2 skipped on python 3.13.5 as non-root.
  Estimate ~3h (1.3 TB at the measured 2.44x over SMB).
RUN PROGRESS (measured, not estimated): 443 -> 943 frames over 180s = 2.77 frames/s.
  ETA 3.3 hours for ~34,400 frames. Checkpointing confirmed working in production.
  NOTE: the 500-frame commit interval was sized against 10 frames/s (warm local); real
  throughput over SMB is 2.77 frames/s, so rework on failure is ~3 min, not <2 min. Same
  order, still the right call — recording the correction rather than leaving the claim.
