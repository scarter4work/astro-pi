# Task 13 review (v2 brief) — PJSR driver AND the real PixInsight executor

Diff: `4bc020c..56935e3` (1 commit, 5 files, +1084/-17).
Reviewed against `task-13-brief-v2.md` only. Read-only; PixInsight was not run.

---

## Spec Compliance

| Brief requirement | Verdict |
|---|---|
| No Python `PixInsightExecutor` class created | ✅ none in diff; the PJSR script is the executor |
| `pixinsight/autocontrast_optimize.js` created | ✅ 888 lines |
| `reference_fingerprint` added to `_op_analyze` | ✅ `sidecar.py:1131` (diff) / `sidecar.py:~176` |
| 16-bit-capable reader added as a project dependency | ✅ `tifffile>=2023.7`, `pyproject.toml` |
| Suffix genuinely round-trips at 16 bits, proven by test | ✅ proven, but the proof is weaker than claimed — see Issues |
| `supported_suffixes()` EXTENDED, not a parallel list | ✅ `loaders.py:987` — `_TIFF_SUFFIXES` unions into the existing derivation, same shape as `_FITS_SUFFIXES`, Pillow half untouched |
| Script passes that suffix | ✅ `CANDIDATE_SUFFIX = ".tif"`, sent on `optimize_begin` |
| `envOr()` for every path | ✅ all 8 paths + `MAX_DIM`, `PSF_FWHM`, suffix, manual annotation |
| `STATUS_FILE` reporting | ✅ PASS and FAIL both written |
| `ExternalProcess.execute` for the sidecar | ✅ `runSidecar()` |
| Guardrail discards reported with reasons | ✅ 25 in the run's `report.txt`, each with its reason |
| Every `noted` entry reported | ✅ all 3 `attempt_cap` notices present in `report.txt`, in their own section |
| Recipe written to `RECIPE_FILE` | ✅ |
| `improved=false` says DECLINED plainly + "file was not modified" | ✅ code path present and correct (script lines 844–859) — **not exercised by the run** |
| Headless run performed, wall clock reported | ✅ real artifacts under `/tmp/autocontrast_optimize` (18 candidates, proxy, report, session) corroborate the report |
| No tunable adjusted | ✅ diff touches no config/beam file; `MAX_DIM=1600` is the sidecar's own default, `MAX_BATCHES` is a driver liveness stop |
| Guardrail violations discard, never a score term | ✅ driver only prints the log; never reads a verdict into a decision |
| Sidecar response contract unchanged | ✅ one additive field |
| No AI | ✅ |
| No test deselected | ✅ (report claim; `tests/test_raster.py` runs 15/15 clean, no skips) |
| Input must be already-stretched; linear is a loud error | ⚠️ **not implemented anywhere in the repo** — no linearity check exists in `src/autocontrast/`; `loaders.py:192` merely comments that a linear master "will (correctly) fingerprint as flat". Pre-existing gap, not introduced here, and outside the v2 brief's steps — but the constraint is not satisfied. |

---

## Claim verification

### Claim 1 — "Pillow reading a 16-bit RGB TIFF silently truncates to uint8"

**TRUE, and reproduced independently.** Pillow 12.3.0, tifffile 2026.7.14:

```
PIL mode RGB (6, 4)          # <- mode "RGB" = 8-bit, no error
PIL dtype uint8 pixel [255 156 1]   # source pixel (65535, 40000, 257)
warnings: []                 # <- no warning either
tifffile roundtrip uint16 [65535 40000 257]
PIL write 16bit RGB FAILS: TypeError Cannot handle this data type: (1, 1, 3), <u2
```

The conclusion follows and the severity claim is correct: `.tif` was already in
`supported_suffixes()` via `Image.registered_extensions()`, so before this commit
`candidate_suffix: ".tif"` *passed* `optimize_begin`'s `ec6c662` validation and would
then have quantized every candidate to 8 bits with nothing logged. The dependency is
justified on a correct premise.

`supported_suffixes()` was extended correctly — `_TIFF_SUFFIXES` is a single constant
serving *both* the decode dispatch (`load_raster`, `image_dimensions`) and the union,
mirroring `_FITS_SUFFIXES`. No parallel hand-maintained list.

### Round-trip proof: real, but the report's verification argument is wrong

Verified end-to-end on the run's own PixInsight output:

| file | dtype | distinct values | all multiples of 257? |
|---|---|---|---|
| `proxy.tif` | uint16 | **250** | **yes** |
| `cand-2-0-4.tif` | uint16 | 499 | no (495 of 499 are not) |

- The **candidate** carries 499 distinct levels — more than 8 bits can hold — so the
  16-bit candidate format is genuinely load-bearing. The decision was right.
- The **proxy** is 250 distinct values, every one an exact multiple of 257. It is an
  8-bit image in a 16-bit container.

The report (§Decision 1) states: *"`(value % 256 != 0)` held for 100% of pixels — the
low byte is genuinely in use, not a zero-padded 8-bit image wearing a 16-bit header."*
**That test is vacuous.** For 8-bit promotion `v -> v*257`, `v*257 mod 256 == v mod 256`,
which is nonzero for every `v` except 0. The test cannot distinguish promoted 8-bit data
from real 16-bit data, and on the actual proxy it returned 100% for a file that *is*
8-bit data in a 16-bit container. The right test is distinct-level count / divisibility
by 257, which fails for the proxy.

The cause is a code-order defect, not a measurement fluke — see Issue I-1.

### Claim 3 — Finding 2, the 9% resampler shift

**Reported honestly, and the driver does what it says.** Script lines 723–724 print the
master's D labelled `(the MASTER, downsampled by the sidecar)`, line 753 prints the proxy
baseline labelled `(the PROXY — this is the number the search descends)`, and lines
763–768 emit a warning when they diverge by more than 0.01. The run's `report.txt`
carries all three lines verbatim, including `! the two differ by 0.0202`.

Implication for Task 14: **yes, §10's exit criterion must name its array.** 0.2264 and
0.2467 are the same image and differ by 9%; a criterion phrased as "D on the image" is
ambiguous by that margin. Note also that the sidecar-side number is itself a Pillow
8-bit RGB resize of an 8-bit JPEG, so neither number is a 16-bit measurement of the
master.

### Claim 4 — Finding 3, LHE radius bounded [16, 512]

Corroborated by the run's own log: 6 of the 9 executor failures are
`LocalHistogramEqualization.radius(): numeric value out of range: 4` / `: 2`, once per
iteration for each of the two finest proposed bands. At 4.665"/px the ladder's lower
bands land at 1/2/4/8 px, all below 16.

**Declining to fix was right.** Every available fix — reshaping the band ladder, clamping
the radius, or substituting another process for `local_equalize` — changes which actions
are proposable or what a recipe means, which is a §3.7 calibration change this task may
not make. Clamping would additionally make the recipe describe something other than what
ran, which the script's own comment correctly refuses.

**The driver's handling is correct per §2.5**: `LocalHistogramEqualization.radius = px`
raises, `applyInstruction`'s catch converts it to `null`, the instruction is omitted from
`produced`, and the sidecar logs `[executor] PixInsight reported no candidate for
instruction 'itN-br0-acM'`. No fabricated file, not fatal, branch asks for a replacement.
All of that is visible in the run's `report.txt`.

### Claim 5 — Finding 4, StarXTerminator SIGSEGV

**Verified from the run log**: three occurrences (one per iteration), each producing
`executeOn returned false`, each omitted and logged as an executor failure, and the run
continued to completion in all three cases. Trusting the boolean rather than the absence
of a throw is the correct call given PI catches the signal and hands back a byte-identical
view — saving that view would have fabricated a no-op candidate.

---

## The Fail-Safe Judgment

**§10's fail-safe is INTACT. It did not fail — this run simply never exercised it.**

The mechanism, traced end to end:

- `loop.py:89–97` — `best` is initialized to the *root branch*, i.e. the proxy itself,
  with `distance = baseline_distance`.
- `loop.py:307` — `best` is replaced only by a scored candidate that beats it by
  `epsilon_improve`.
- `loop.py:703` — `improved = best.distance < baseline_distance - epsilon_improve`.

Both sides of that comparison are measured on the **same footing**: `sidecar.py:247,305`
build one `_optimize_loader(max_dim)` and use it for the baseline *and* every candidate,
and on this run the proxy and all 18 candidates are 16-bit TIFFs going through the same
`_decode_tiff` path at the same `max_dim`. The resampler divergence of Finding 2 shifts
the baseline and every candidate by the same construction, so it cannot manufacture an
`improved=true`. Structurally, `improved` cannot be true unless a real PixInsight-produced
candidate really is closer to the reference fingerprint than the input the search was
given. That is exactly what the fail-safe is specified to assert, and it held.

**The implementer's stated defence is not the reason, and does not survive scrutiny.**
The argument is that D=0.2361 "only climbs part-way back" toward the master's 0.2264, so
the improvement is an artifact of the proxy step. That compares a *processed proxy*
against an *unprocessed Pillow-downsampled master* — two different arrays measured
through two different resamplers, which is precisely the incomparability Finding 2 itself
identifies. It cannot be used as evidence about the fail-safe. It is also implausible on
its face: the winning recipe is BackgroundNeutralization + CurvesTransformation, two
global tone/colour operations that do not undo resampling artifacts in fine structure.

**The correct explanation is simpler, and it is not a defect:** the brief's `improved=false`
expectation was a prediction about the *image*, not a requirement on the *code*. It rests
on the premise that a finished print sits at the valley floor. It does not — the print
measures D ≈ 0.23–0.25 from `eso1103a`. That is a long way from zero. With that much
headroom, a 4.3% reduction from two conservative global operations is unremarkable, and
the fail-safe declining would have been the *wrong* answer.

What the run does establish, and what Task 14 must decide, is a **calibration** question,
not a fail-safe question: is D ≈ 0.24 against an ESO/Chekalin M42 render the right thing
for a competent amateur print to score? The reference is a different instrument, different
integration, different processing house. If the answer is "a good print should be near the
floor", the problem is in the reference/normalization, not in §10's fail-safe.

Two run facts sharpen that: the beam **collapsed** (`convergence_reason: every branch was
discarded by guardrails or executor failure`) rather than converging on a flat distance,
and the attempt cap bound on **all three** iterations. The search never had breadth; the
improvement came from the only two candidates that survived at all.

**Bottom line: no threshold should be touched, and no fix to the fail-safe is warranted.**

---

## The Five Findings

1. **Pillow silently truncates 16-bit RGB TIFF** — accurate, reproduced independently; the dependency is justified and `supported_suffixes()` was extended, not duplicated. The *verification* claim about the low byte is wrong (see I-1).
2. **`improved=true` on a finished print** — the fact is reported honestly and no threshold was touched (correct); the *explanation* offered is unsound, but the conclusion (fail-safe intact) is right for a different reason.
3. **Proxy resampler moves D by 9%** — accurate, honestly reported, and the driver genuinely prints both numbers with a divergence warning. Task 14 must state which array §10 asserts on.
4. **LHE radius bounded [16, 512]** — accurate (6 of 9 executor failures, confirmed in the run log), structural as claimed; declining to fix was the right call and the omit/log/replace handling is correct per §2.5.
5. **StarXTerminator SIGSEGVs headless** — accurate; the run continued correctly in all three occurrences, treated as a per-candidate executor failure and never as fatal.

---

## Contract Requirements

| Requirement | Verdict |
|---|---|
| `image` sent on EVERY step, byte-identical to `optimize_begin`'s | ✅ `autocontrast_optimize.js:739` and `:799` both send the same `proxy.path` JS value; never normalized, resolved, or rebuilt |
| `pixel_scale_arcsec` is the NATIVE scale of the file passed; no pre-correction | ✅ `sidecar.py:252` multiplies by `downsample_factor(image, load(image))`; the proxy is ≤ `max_dim` so that factor is exactly 1.0 (verified: `downsample_factor(proxy.tif, ...) == 1.0`). Effective scale is `1.4035 × 3.3238 = 4.665"/px` either way — identical to what the master path would have produced. **No double-correction.** Deriving the scale from real dimensions (`buildProxy` line 316) rather than `MAX_DIM` is correct and necessary: `round()` on the short side means the achieved factor is not exactly `MAX_DIM/longest`. |
| Unappliable instruction OMITTED from `produced`, logged, run continues; never fabricated, never fatal | ✅ `applyInstruction` returns `null` on every failure route (`:552`, `:562`, `:571`), `executeBatch:581` filters nulls. 9 omissions in the real run, all logged, run completed |
| Every guardrail-discarded candidate reported | ✅ `reportGuardrailLog` — 25 in `report.txt` with reasons |
| Every `noted` entry reported | ✅ separate "Noted" section; all 3 `attempt_cap` notices present verbatim in `report.txt` |
| `envOr()` for every path | ✅ |
| `STATUS_FILE` reporting | ✅ both PASS and FAIL; `report.txt` mirror is a genuine improvement, since headless PI swallows `console` |
| `ExternalProcess.execute` | ✅ |

All eight honored.

---

## Strengths

- **The `report.txt` mirror is the single best decision in the diff.** Discovering that
  headless PI emits nothing from `console` and that the entire §12 surface was therefore
  invisible — then fixing it, including flushing on the failure path — is what makes the
  §12 requirement true in fact rather than on paper. The run artifacts prove it works.
- **Failure handling is uniformly correct.** Every route out of `applyInstruction` returns
  `null` with a named reason; nothing fabricates a file; nothing aborts the run. Three
  genuinely different failure modes (a SIGSEGV, a parameter raise, a save failure) all land
  on the one designed path.
- **Refusing to clamp the LHE radius** is the right instinct for the right reason: a clamped
  parameter makes the recipe lie about what ran.
- **Refusing a partial manual annotation.** Requiring all four values together, having
  measured that an eyeballed scale was 27% off, is exactly the §2.2/§4.4 reasoning.
- **Finding 2 was surfaced rather than buried**, with a runtime warning, not just a report line.
- **Honest reporting of an unexpected result.** `improved=true` was recorded and escalated
  with no threshold touched, per the brief's own instruction.
- **The proxy decision is sound** and the pixel-scale reasoning behind it is correct, not
  the double-correction trap the brief warned about.

## Issues

### Critical (Must Fix)

None. Nothing in this diff is wrong in a way that invalidates the run or the contract.

### Important (Should Fix)

**I-1. `buildProxy` resamples *before* promoting the sample format, so the search proxy
carries only 8 bits.** `pixinsight/autocontrast_optimize.js:281–299` — the `Resample` block
runs, and only then `w.setSampleFormat(CANDIDATE_BITS, false)`. On an 8-bit source (the
stated use case: finished JPEG prints), PI resamples in an 8-bit buffer and rounds every
output pixel to 8 bits; the promotion afterwards just multiplies by 257. Measured on the
run's own `proxy.tif`: **250 distinct values, all exact multiples of 257.**

Why it matters: the proxy is the baseline D *and* the root parent of every branch. The
file's own header comment justifies 16-bit candidates because "an 8-bit intermediate is an
approximation, and production does not optimize against one" (§2.2) — and then the one
array everything descends from is an 8-bit intermediate. A 3.3× bicubic downsize averages
~11 source pixels, so the discarded precision is real information, not rounding noise
(measured: the same resize done at native depth yields ~15000 distinct levels on varying
data, vs 250 here).

Fix: move `w.setSampleFormat(CANDIDATE_BITS, false)` **above** the `if (factor < 1.0)`
block so the resample computes into a 16-bit buffer. Two lines. Not a §3.7 change — no
tunable moves; it changes the precision of an intermediate, which is the task's own stated
mandate. It will shift the baseline D and probably narrow Finding 2's divergence, so re-run
and re-record both numbers.

**I-2. The report's low-byte verification is not evidence, and should not be left in the
record as if it were.** `task-13-report.md` §Decision 1: `(value % 256 != 0)` holding for
100% of pixels is automatic for any 8-bit-promoted image, since `v*257 mod 256 == v mod 256`.
It returned 100% on a file that is provably 8-bit data (I-1). Replace with distinct-level
count or a `% 257` check — both of which correctly show the *candidates* (499 levels, 495
not multiples of 257) do carry sub-8-bit detail, so the 16-bit decision itself stands.

**I-3. `load_raster`'s bit-depth heuristic is now a live silent-corruption path.**
`src/autocontrast/io/loaders.py:261–265` picks the divisor from `arr.max()`:
`65535 if max > 255 else 255`. This code is pre-existing and untouched — but this diff is
what makes 16-bit TIFF the production candidate format. Before it, a `.tif` came back from
Pillow as uint8 and `max <= 255` was correct by construction. Now a genuinely 16-bit
candidate whose max is ≤ 255 is divided by 255 and comes out **257× too bright**, silently.
Measured: a uint16 TIFF of constant 200 (true value 0.003052) loads as **0.784314**.

Realistically rare for an astro candidate (max is normally near full scale), but it is
exactly the failure class this whole task set out to eliminate, it fires with no log and no
error (§12), and it now has a live caller. Fix: `_decode_tiff` knows the true dtype — carry
the full-scale value (`np.iinfo(dtype).max`) forward instead of inferring it from the data.

**I-4. A false safety claim in a code comment, contradicted by the run's own data.**
`pixinsight/autocontrast_optimize.js:424–427`: *"the black point provably never reaches the
1st percentile and no more than 1% of pixels can be clipped by construction. §7's
shadow-clipping guardrail is the independent check; this is the action refusing to need it."*
Every single `black_point@-/strong` in the real run was discarded by `shadow_clipping` at
**2.44%, 2.54%, 3.97% and 2.81%** — two to four times the claimed bound. (Most likely the
per-channel `HistogramTransformation` maps three channels' worth of sub-threshold pixels to
zero while the 1st percentile is taken once.) The guardrail did its job, so behaviour is
safe; the *claim* is false, and a comment asserting a construction-level bound is precisely
the kind of thing a later change will trust and lean on. Either correct the bound or drop
the "refusing to need it" claim.

### Minor (Nice to Have)

**M-1. `test_sixteen_bit_tiff_survives_max_dim_downsizing` uses a flat field**
(`tests/test_raster.py:1225`, all pixels 40000), which resamples to itself under any kernel
and so proves only that the *value* is not requantized. It does not exercise whether the
float32 resize preserves sub-8-bit precision in *varying* data. I checked directly: it does
(a random uint16 image downsized 4× yields ~15000 distinct levels), so the code is right and
the test is merely weaker than its docstring implies. Adding a varying-data case would close
the gap. The other five tests are sound, and
`test_pillow_would_have_truncated_that_file` is a genuinely good anti-vacuity guard.

**M-2. Per-batch `D` label is slightly misleading.** `autocontrast_optimize.js:805` prints
`result.distance`, which `loop.outcome:709` defines as `best.distance if improved else
baseline_distance` — so it reads as a flat 0.2467 for eight batches and then steps. It is
not the batch's own distance. Label it "best-so-far" or print `distance_history`'s tail.

**M-3. `begun.converged` is not checked before entering the loop.**
`autocontrast_optimize.js:771–777` — if `optimize_begin` ever returns already-converged with
no instructions, the driver still calls `optimize_step` once with `produced: []`. Harmless
today; an explicit early exit would be clearer.

**M-4. `ImageWindow.open` per instruction** (author's own concern 6). 18 opens for 18
candidates. At 0.17 s/candidate it is not worth optimizing now; noted only so it is not
rediscovered.

---

## Assessment

**Task quality:** Approved with follow-ups (Important issues should be fixed before Task 14
asserts on these numbers)

**Reasoning:** The contract is honored on all eight points, §12's surface is genuinely real
rather than notional, the failure handling is uniformly correct, and the `improved=true`
result is a sound outcome of an intact fail-safe rather than a defect — though the reasoning
offered for it in the report does not hold. The one substantive code defect (I-1: the search
proxy is 8-bit despite the task's own §2.2 rationale) is a two-line ordering fix, and it
should be corrected and the run repeated before Task 14 pins §10's exit criterion to either
of these D values.
