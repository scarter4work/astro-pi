# Phase 8 — Stats-Driven Stretch Parameter Tuning with Per-User Learning

**Date:** 2026-04-21 (revised 2026-04-22: bootstrap cut deferred to Phase 8.5)
**Status:** Design signed off, ready for implementation planning
**Predecessor:** Phase 7 (v4.0.0.5) shipped the Auto-selector (classify filter → name a stretch). Phase 5 established Phase-5 champion params per stretch. v4.0.0.7 first real-data stack on M27 landed "a bit faint."

## Goal

Replace the current "Auto always applies Phase-5 champion defaults regardless of input" behaviour with a learning system that:

1. Computes image-level statistics on the stacked linear output.
2. Predicts stretch parameters from those stats using a ridge-regression model.
3. Collects user ratings of stretched outputs across five axes.
4. Refines the per-user model from accumulated ratings — NukeX learns the user's personal aesthetic over time.
5. Keeps every layer inspectable and revertable — no opaque magic, no ad-hoc constants, no "NukeX broke because Phase 8 failed."

## Non-goals (scoped out)

- **Finishing-stretch tuning.** Phase 8 only tunes the primary stretch. Finishing stretches and palette mapping are later phases.
- **Target-class tuning beyond a soft hint.** FITS OBJECT keyword feeds a low-weight categorical feature; no per-target-class model family.
- **Telemetry or community model aggregation.** Ratings stay local to each user's machine. Sharing a bootstrap across the community happens only at release-cutting time, via Scott's personally-labeled corpus, not via telemetry.
- **Online learning refinements** (retraining cadence beyond per-save, drift handling, explicit "forgetting" of old ratings, diff views). Deferred to Phase 9.

## Release scope

Phase 8 ships in two steps:

- **v4.0.1.0 (Phase 8):** learning infrastructure, rating UI, per-user Layer 3 — Layer 2 ships empty.
- **v4.0.1.x (Phase 8.5):** community bootstrap corpus → Layer 2 populated.

This split pulls the ~350-session labeling work off the v4.0.1.0 critical path while leaving the four-layer architecture unchanged. On a fresh v4.0.1.0 install the fallback chain reduces to Layer 3 → Layer 1, making cold-start Auto pixel-identical to v4.0.0.8 until either the user rates a run (activates Layer 3) or Phase 8.5 ships (activates Layer 2). See "Development order and release plan" below.

## Architecture

### Four layers of prediction

| Layer | Written by | Changes | Falls back to |
|---|---|---|---|
| 1. Factory defaults | Code constants in each `StretchOp` | Never | — |
| 2. Community bootstrap | Shipped coefficients fit from Scott's labeled corpus | Major release | Layer 1 |
| 3. User-learned current | Same ridge model, retrained locally on rating Save | Per rating | Layer 2 |
| 4. Per-image prediction | `Layer3.predict(stats_of_this_stack, target_hint)` | Per run | Layer 1 |

**Critical invariant:** Layers 2, 3, 4 are the *same ridge model at different training snapshots* plus one `predict()` call. Not three stacked models. The "4 layers" is a mental model for UX inspection and reset, not three independent model objects.

**Cold start:** new user has zero ratings → Layer 3 is identical to Layer 2.

**Initial state at v4.0.1.0 ship:** Layer 2 is empty (no `share/phase8_bootstrap_model.json`), so the fallback chain reduces to Layer 3 → Layer 1. Fresh-install Auto output is pixel-identical to v4.0.0.8. Phase 8.5 populates Layer 2 and shifts cold-start behaviour to Scott's bootstrap.

**Escape hatches (Interface menu or settings):**
- *Reset to factory* — ignore all learned layers; use Layer 1 constants.
- *Reset to bootstrap* — discard user DB rows; Layer 3 retrains on bootstrap only.
- *Explain* — print Layer-3 coefficients + kNN top-3 most-similar past runs for the most recent prediction.

### Rating collection

After a successful Execute:

1. A modal popup appears (unless opted out in settings) with the stretched output visible behind it. Five axes:
   - Brightness: signed slider, `−2` (too dark) … `0` (just right) … `+2` (too bright)
   - Saturation: signed slider, washed … pumped
   - Color balance: signed slider, cool … warm (hidden for mono and narrowband filter classes)
   - Star bloat: signed slider, tight … bloated
   - Overall: 1–5 stars (quality gate for training data)
2. Save / Skip / Don't-show-again. Save-only persists the rating.
3. A "Rate last run" button in the NukeX Interface panel lights up after Execute and provides the same dialog. The button backs purely onto in-memory last-run state. A new Execute overwrites the slot; the previous unrated run is discarded.
4. The opt-out checkbox (*Don't show again*) disables the popup but keeps the Interface button.

### Sliders are signed, zero-defaulted

Pre-zeroed at "just right." User only moves axes that feel off. Fast to rate (one or two nudges, not five deliberate ratings). Directional signal lets the model learn *which way* to correct, not merely that something was wrong.

## Components

### New library: `src/lib/learning/`

- **`rating_db.{hpp,cpp}`** — SQLite CRUD over the `runs` table. Handles the user DB and `ATTACH`-ing the shipped bootstrap DB.
- **`train_model.{hpp,cpp}`** — orchestration: load bootstrap + user rows, fit a ridge-regression model per (stretch, param), serialize coefficients.
- **`ridge_regression.{hpp,cpp}`** — pure math utility, closed-form normal equations with L2 regularization, Eigen-backed.

### Additions to `src/lib/stretch/`

- **`image_stats.{hpp,cpp}`** — extract the 29-column stat vector (13 populated for single-channel images) from a `nukex::Image`.
- **`param_model.{hpp,cpp}`** — per-stretch wrapper that holds the ridge coefficients and knows how to `predict(stats, target_hint) → params` for its stretch. Handles feature normalization and output clamping against `StretchOp::param_bounds()`.

### Additions to `src/module/`

- **`rating_dialog.{h,cpp}`** — PCL `Dialog` subclass: sliders + star rating + Save/Skip/Don't-show-again.
- **`stretch_factory.cpp`** (modified) — `build_primary` / `build_finishing` now compute stats, load the active model (Layer 3 if available, else Layer 2, else Layer 1), predict params, mutate the returned `StretchOp`. One log line per prediction naming the active layer.
- **`NukeXInterface.{cpp,h}`** (modified) — "Rate last run" button, "Don't show rating popup" checkbox wired to persistent module settings.
- **`NukeXInstance.cpp`** (modified) — after Execute, capture `(stats, params_applied, stretch_name, target_hint)` into in-memory last-run state; show rating popup unless opted out.

### Shipped assets

- **`share/phase8_bootstrap.sqlite`** — read-only SQLite containing Scott's labeled bootstrap corpus (~50 rows per stretch). **Deferred to Phase 8.5.** v4.0.1.0 ships without this file; Layer 2 falls through to Layer 1 until Phase 8.5.
- **`share/phase8_bootstrap_model.json`** — pre-fitted coefficients from the bootstrap corpus. Loaded at module startup as Layer 2. **Deferred to Phase 8.5.** Absent at v4.0.1.0 ship; committed to the repo at v4.0.1.x ship.

### Vendored dependency

- **SQLite** via `FetchContent` + amalgamation (same pattern as cfitsio in v4.0.0.3). Single `.c` file, WAL mode enabled.

### Tests

See "Testing" section below for the full matrix.

## Data flow

### Execute flow

1. User clicks Execute.
2. Phases A / B / C run as today → `result.stacked` (linear image).
3. Read FITS metadata from first light frame (FILTER, BAYERPAT, OBJECT).
4. `classify_filter(meta)` → FilterClass.
5. Auto-selector picks a stretch name (currently every class → VeraLux).
6. **NEW:** `compute_image_stats(stacked, meta.bayer_pattern)` → stat vector.
7. **NEW:** Load active `ParamModel` for this stretch. If Layer 3 coefficients exist on disk and are valid, use Layer 3; else use Layer 2; else Layer 1.
8. **NEW:** `model.predict(stats, target_hint) → params`. Clamp each predicted param against `StretchOp::param_bounds()`.
9. **NEW:** Mutate the StretchOp with predicted params. Emit log line: `Auto: <stretch>, Layer <N> (<description>, N_rows=<k>): <param>=<value>, ...`.
10. Stretch pipeline runs; stretched window opens.
11. **NEW:** Capture in-memory last-run state `(stats, params_applied, stretch, target_hint)`.
12. **NEW:** Show rating popup (unless opted out); Interface's "Rate last run" button becomes enabled.

### Rate flow

1. User moves sliders, clicks Save.
2. Single SQLite transaction:
   1. `INSERT INTO runs ...` with stats, params, ratings, stretch, target_hint, timestamp.
   2. Load all rows for this stretch from user DB + attached bootstrap DB.
   3. Ridge-fit model per param for this stretch.
   4. Atomically write coefficients file (tmp + fsync + rename).
3. If any step fails, transaction rolls back entirely — no partial row, no partial retrain. User sees an error, tries again or moves on.
4. On success, invalidate the cached `ParamModel` for this stretch. Next Execute picks up the refreshed Layer 3.

**Retraining is per-stretch, not global.** Rating a VeraLux run retrains only VeraLux's model. Other stretches' models stay unchanged.

### Skip / opt-out flow

- **Skip** — dialog closes; in-memory last-run state stays until the next Execute or Reset overwrites it.
- **Don't show again** — popup disabled in persistent settings; Interface button still works.
- **Un-rated at next Execute** — previous in-memory last-run state is discarded without trace.

No PENDING DB rows ever exist. A row exists iff it was rated and saved successfully.

## Storage schema

### Two SQLite databases

1. **`share/phase8_bootstrap.sqlite`** — shipped read-only. Scott's labeled corpus.
2. **`<user-data>/nukex4/phase8_user.sqlite`** — per-user read-write. Path via PCL `File::ApplicationData()`.

Both use identical schema. Training loads both via `ATTACH DATABASE`.

### Schema v1

```sql
CREATE TABLE runs (
    run_id           BLOB PRIMARY KEY,        -- 16-byte UUID
    created_at       INTEGER NOT NULL,        -- Unix seconds
    stretch_name     TEXT NOT NULL,           -- 'VeraLux', 'GHS', 'MTF', ...
    target_class     INTEGER NOT NULL,        -- 0=unknown, 1=galaxy, 2=emission_neb,
                                              -- 3=planetary_neb, 4=reflection_neb,
                                              -- 5=cluster, 6=other
    filter_class     INTEGER NOT NULL,        -- LRGB_mono, Bayer_RGB,
                                              -- Narrowband_HaO3, Narrowband_S2O3

    -- Per-channel stats (NULL if not applicable to this filter class)
    stat_median_r    REAL, stat_median_g    REAL, stat_median_b    REAL,
    stat_mad_r       REAL, stat_mad_g       REAL, stat_mad_b       REAL,
    stat_p50_r       REAL, stat_p50_g       REAL, stat_p50_b       REAL,
    stat_p95_r       REAL, stat_p95_g       REAL, stat_p95_b       REAL,
    stat_p99_r       REAL, stat_p99_g       REAL, stat_p99_b       REAL,
    stat_p999_r      REAL, stat_p999_g      REAL, stat_p999_b      REAL,
    stat_skew_r      REAL, stat_skew_g      REAL, stat_skew_b      REAL,
    stat_sat_frac_r  REAL, stat_sat_frac_g  REAL, stat_sat_frac_b  REAL,

    -- Global stats
    stat_bright_concentration REAL,
    stat_color_rg    REAL, stat_color_bg    REAL,
    stat_fwhm_median REAL,
    stat_star_count  INTEGER,

    -- Applied stretch params as JSON (shape varies per stretch)
    params_json      TEXT NOT NULL,

    -- Ratings (all NOT NULL — no PENDING rows)
    rating_brightness  INTEGER NOT NULL CHECK (rating_brightness  BETWEEN -2 AND 2),
    rating_saturation  INTEGER NOT NULL CHECK (rating_saturation  BETWEEN -2 AND 2),
    rating_color       INTEGER          CHECK (rating_color       BETWEEN -2 AND 2),
    rating_star_bloat  INTEGER NOT NULL CHECK (rating_star_bloat  BETWEEN -2 AND 2),
    rating_overall     INTEGER NOT NULL CHECK (rating_overall     BETWEEN  1 AND 5)
);

CREATE INDEX idx_runs_stretch ON runs(stretch_name);
CREATE INDEX idx_runs_target  ON runs(target_class);

PRAGMA user_version = 1;
PRAGMA journal_mode = WAL;
```

**Stats are flat columns, not JSON.** They participate in every training query — JSON parsing × N_rows × N_trainings wastes effort. Flat columns `SELECT` straight into Eigen matrices.

**Params are JSON.** Variable shape per stretch (VeraLux: 3 params, GHS: 3, CLAHE: possibly 5). Only read during training `SELECT`s, never in the prediction hot path, so JSON cost amortizes fine and avoids per-stretch schema-migration debt.

**Ratings are NOT NULL.** A row exists only if the user saved a rating. Rating `color` is the one exception — NULL means the filter class didn't expose the color axis.

### Coefficients file format

`~/.nukex4/phase8_user_model.json` (or `.bin` if size grows). Keyed by stretch name, one entry per stretch. Each entry holds, per trainable param:

- `feature_mean` (vector, for z-score normalization)
- `feature_std` (vector, clipped to avoid division by zero)
- `coefficients` (vector, one per feature)
- `intercept` (scalar)
- `lambda` (the L2 regularization strength used during fit)
- `n_train_rows` (for observability)
- `cv_r_squared` (for the bootstrap release gate)

Total expected size: ~50 KB for 7 stretches × ~3 params each. Human-readable JSON is preferred for debuggability unless file size becomes material.

Loaded once at module startup; invalidated and reloaded after each rating Save.

### Housekeeping

Because there are no PENDING rows, **no housekeeping pass is needed**. The DB grows only when the user rates, and rated rows are valuable training data that we don't prune.

## Error handling

One rule: **fail fast, discard, log, use factory defaults.** No mid-run recovery, no partial state, no transaction replay logic.

### Three failure domains

| Domain | On failure |
|---|---|
| **Module load** (DB missing or corrupt, coefficients file invalid) | Fall back one layer: Layer 3 → Layer 2 → Layer 1. Log which layer is active and why the higher one was unavailable. |
| **Predict at Execute time** (stats NaN, model predicts NaN, impossible range, etc.) | Use factory defaults for this run. Log the reason. Stretched output still produced. |
| **Save rating** (DB insert fails, retrain fails, coeffs write fails) | Transaction rolls back; the rating is discarded entirely. User sees "Couldn't save rating." No partial row. |

### Additional hardening (cheap, defensive)

1. **Param bounds clamping.** `StretchOp::param_bounds()` returns a `std::map<std::string, std::pair<float, float>>` per op. Predictor always clamps output. Not recovery — input validation on the model output.
2. **Atomic coefficient writes.** Write `.tmp`, `fsync`, `rename`. Never leaves the file partial if power dies mid-write.
3. **SQLite WAL mode** + **`PRAGMA user_version`** for schema migration gating.
4. **Every Phase 8 log line names the active layer.** Foundation for the "Explain" story and support-by-reading-logs.

### Corrupt DB handling

On module load, `PRAGMA integrity_check` runs once. If it fails, the DB file is renamed to `.corrupt.<timestamp>` and a fresh one is created. A warning appears in the Process Console. User keeps working with Layer 2 until they rate new runs.

## Testing

### Unit tests (TDD, RED-before-GREEN)

| File | Covers |
|---|---|
| `test/unit/stretch/test_image_stats.cpp` | Each stat against a synthetic image with a known analytic answer. Pathological cases: all-zero image, all-one image, single-pixel, NaN in input. |
| `test/unit/learning/test_ridge_regression.cpp` | Closed-form ridge against scipy-computed reference coefficients (2-3 hand-fit cases). L2 correctness. Singular matrix returns a failure signal, not NaN. |
| `test/unit/stretch/test_param_model.cpp` | Memorization — fit on N rows, predict training inputs, coefficients match scipy within 1e-5. Cross-validation on held-out points. Serialize/deserialize byte-exact. Clamping against `StretchOp::param_bounds()`. |
| `test/unit/learning/test_rating_db.cpp` | CRUD round-trip. Schema v1→v2 migration sanity. `integrity_check` on a deliberately-corrupted file returns false. Separate bootstrap + user DBs via ATTACH produce expected unioned row set. |
| `test/unit/learning/test_train_model.cpp` | End-to-end: seed DB, fit, write coefficients, read back, predict. Per-stretch isolation (rating VeraLux doesn't perturb GHS's model). |

### Integration tests

- `test/unit/module/test_stretch_factory.cpp` (extended) — `build_primary(Auto, meta)` with a mock `ParamModel` returning canned params → verifies the returned `StretchOp` carries the predicted values. Deliberately-broken model → verifies factory-defaults fallback + log line.

### E2E regression (existing `make e2e` harness)

- **v4.0.1.0:** existing cases remain unchanged. Layer 2 is absent at ship, so fresh-install Auto's pixel output is identical to v4.0.0.8 — the baseline case confirms this, which is a stronger additivity proof than a new golden would be.
- **v4.0.1.x (Phase 8.5):** new case `lrgb_mono_ngc7635_phase8_bootstrap` — same FITS input as the existing case, Phase 8 active with the shipped bootstrap. Golden = pixel hash of stretched output with Layer 2 populated. Catches "someone rebuilt the bootstrap and pixel output drifted unexpectedly."
- Layer 3 (user-learned) activation is covered by integration tests rather than E2E goldens, because its behaviour depends on user-specific DB state rather than shipped artifacts.

### Bootstrap corpus validation (Phase 8.5 only)

Not a unit test — a Phase 8.5 release-gate workflow step. **No-op at v4.0.1.0 ship** because Layer 2 is empty.

- `tools/phase8_inspect.sh` — loads shipped bootstrap coefficients, runs prediction on 5 held-out test stacks (committed as a small fixture), prints predicted params per stretch per case. Eyeball check before cutting a Phase 8.5 release.
- Cross-validation R² per param is emitted during bootstrap training and checked into the repo next to the coefficients file. Phase 8.5 release gate: minimum R² ≥ 0.3 per param. Low bar; catches only "model is worthless."

### Failure-path tests (the "NukeX4 can't break" promise)

`test/unit/module/test_phase8_fallback.cpp`:

- Corrupt DB file → run still produces output with factory defaults + expected log line.
- Missing coefficients file → Layer 3 falls back to Layer 2.
- NaN in stats → factory defaults.
- Predicted params out of clamp range → clamp succeeds, no NaN downstream.
- DB write fails (e.g., read-only user dir via `chmod 0444`) → Save rating shows error, no partial row persisted.

### CI gate changes

- `ctest` runs all new unit tests. Expected addition: ~8 test binaries, < 20 s total runtime.
- `make e2e` adds one case; runtime stays in the ~5-min range.
- Pre-release manual step: `tools/phase8_inspect.sh` (added to the release checklist in CLAUDE.md).

### Not tested (deliberate)

- Rating UI visual appearance — PCL dialogs don't have a test harness. Manual smoke test.
- Exact param values produced by the bootstrap model on arbitrary inputs — we test that the math is correct, not that specific predictions match any external reference. The "is the model good?" signal is the user's subjective rating of the output, which is the whole point of the system.

## Development order and release plan

Phase 8 is split into two releases. v4.0.1.0 delivers all the learning infrastructure with Layer 2 empty. v4.0.1.x (Phase 8.5) adds Scott's labeled bootstrap. This keeps the v4.0.1.0 critical path short and the four-layer architecture untouched.

Nothing in Phase 8 is a stub: Layer 2's absence is a real, exercised fallback path, not a placeholder. The "no stubs" rule is honored because the missing artifact is not missing code — the code handles the missing artifact correctly and is tested doing so.

### Phase 8 — v4.0.1.0

1. **Infrastructure (unreleased internal):** `image_stats`, `rating_db`, `ridge_regression`, `param_model`, `train_model` library code. Unit tests pass.
2. **Integration (unreleased internal):** wire into `stretch_factory` and `NukeXInstance`. Rating dialog + Interface button. Opt-out setting. Local smoke test: run → rate → re-run verifies Layer 3 picks up the rating.
3. **Public release — v4.0.1.0.** Phase 8 is live but Layer 2 is absent. Fresh-install Auto behaviour is pixel-identical to v4.0.0.8 (Layer 3 empty → Layer 2 absent → Layer 1 factory defaults). After the user rates one run, their Layer 3 activates and per-image prediction takes over for that stretch.

The existing E2E goldens in `test/fixtures/golden/` remain valid at v4.0.1.0 ship because the no-rating / no-bootstrap Auto path is bit-identical to v4.0.0.8.

### Phase 8.5 — community bootstrap add (v4.0.1.x)

Split out from Phase 8 to keep the v4.0.1.0 critical path short. Can start any time v4.0.1.0 is stable. Scope:

1. **Corpus labeling:** Scott runs v4.0.1.0 (or a private build) on ~50 real stacks per stretch across filter classes / target types — roughly 350 rating sessions, estimated 2-3 focused afternoons.
2. **Bootstrap cut:** export Scott's user DB → build `share/phase8_bootstrap.sqlite` + `share/phase8_bootstrap_model.json`. Commit both.
3. **E2E goldens:** add `lrgb_mono_ngc7635_phase8_bootstrap` case exercising the shipped bootstrap; pixel hash regresses against re-cuts of the bootstrap.
4. **Release gate:** cross-validation R² ≥ 0.3 per param; `tools/phase8_inspect.sh` eyeball check.
5. **Public release — v4.0.1.x.** Cold-start users now get Scott's taste via Layer 2; rating-informed users continue to refine Layer 3.

Phase 8.5's exact version number (v4.0.1.1 vs v4.0.2.0) is deferred until ship time — it depends on whether any non-bootstrap work accumulates on `main` in the interim.

## Deferred to Phase 9 and beyond

- Retraining cadence beyond per-save (nightly batch, drift detection).
- Aging / decay of old user ratings.
- "What changed from bootstrap?" diff view.
- Per-target-class model selection (beyond the soft hint feature).
- Finishing-stretch and palette-mapping tuning.
- Community telemetry aggregation (opt-in, separate design entirely).

## Open questions / tracked risks

1. **Bootstrap corpus diversity (Phase 8.5).** Fifty rows per stretch × seven stretches = 350 rating sessions for Scott. Roughly two to three focused afternoons on varied real data. De-risked from v4.0.1.0 by moving the entire bootstrap cut into Phase 8.5 — it is now no longer on the v4.0.1.0 critical path.
2. **Per-stretch ridge coefficient count.** With ~40 features and small bootstrap (~50 rows), feature selection (drop near-zero-coefficient features after initial fit) may be necessary to avoid noisy predictions. Deferred to implementation.
3. **PI module settings persistence.** "Don't show rating popup" depends on PCL's module-settings mechanism working reliably across PI restarts. Worth a smoke test early in implementation before committing to the opt-out UX.

## References

- `project_phase8_stats_tuning.md` — original Phase 8 memory (2026-04-19)
- `project_m27_first_stretch_feedback.md` — motivating user feedback
- `project_stretch_defaults.md` — Phase 5 champion values (the factory baseline)
- `feedback_stretches_wrong.md` — the "no ad-hoc rules" mandate
- `feedback_no_averages_no_stubs.md` — "NO averages, NO stubs" constraints
- `docs/superpowers/specs/2026-03-31-phase5-stretch-pipeline-design.md` — Phase 5 design (predecessor)
- `docs/superpowers/specs/2026-04-19-phase7-closeout-design.md` — Phase 7 close-out
