# Task 13 report — PJSR driver AND the real PixInsight executor

Status: **COMPLETE**. Commit `56935e3` on `phase2-optimizer`. Nothing pushed.

Tests: `431 passed in 249.61s` — baseline 425 plus the 6 new 16-bit TIFF tests.
Nothing deselected, no `lenient`, no threshold touched.
`tests/test_sidecar.py tests/test_optimize_sidecar.py`: 29 passed.
`ruff`: 28 pre-existing errors on `HEAD` and 28 after — no new ones (the single
hit inside a file I touched is a pre-existing `E702` on a line I did not edit).

## Environment facts established first (they drove three design decisions)

| Fact | Value | How verified |
|---|---|---|
| PixInsight | 1.9.4 Lockhart, `/opt/PixInsight/bin/PixInsight.sh` | headless run |
| `StarXTerminator-pxm.so` | installed, models present | `ls`, `find` |
| Test print `M42_hoo_2_18_23.jpg` | 5318 x 3975 RGB, 30 MB JPEG, M42 + Running Man | Pillow, visual |
| `WORK` default `/tmp` | **tmpfs, 16 GB, backed by 30 GB RAM (13 GB free)** | `df -h /tmp`, `free -g` |
| `tifffile` | absent -> installed 2026.7.14, added to `pyproject.toml` | `pip install` |
| Pillow reading a 16-bit RGB TIFF | **silently truncates to uint8** | measured, below |
| The print's WCS | **UNSOLVABLE — see "The blocker" below** | measured |

---

## Decision 1 — bit depth: 16-bit TIFF via `tifffile` (`.tif`, not `.png`)

Measured, not assumed:

```
tifffile roundtrip uint16 True
PIL mode RGB (6, 4)
PIL arr uint8 (4, 6, 3) [255 156   1]     # source pixel was (65535, 40000, 257)
```

Pillow opens a 48-bit RGB TIFF **without error** and hands back 8 bits. That is
worse than the `.png` default the brief called out, because it is *silent*:
`.tif` was already in `supported_suffixes()` (Pillow registers it), so before
this change `candidate_suffix: ".tif"` passed `optimize_begin`'s validation and
would then have quietly quantized every candidate to 8 bits with nothing logged.
So this change does two things: it lifts the quality ceiling, and it closes a
latent silent-degradation path that the ec6c662 validation could not catch.

Implementation (`src/autocontrast/io/loaders.py`):

- `_TIFF_SUFFIXES = {".tif", ".tiff"}`, used for **both** the decode dispatch and
  the `supported_suffixes()` union — the same shape `_FITS_SUFFIXES` already
  had. The Pillow-derived half of the union is untouched, so nothing became a
  parallel hand-maintained list.
- `_decode_tiff()` reads through `tifffile` at native dtype; `_resize_native()`
  does the `max_dim` downsize per channel as 32-bit float ("F" mode), because
  Pillow has no RGB mode that holds uint16 — same bicubic default as the Pillow
  path, so the two do not disagree about what "downsized to 1600" means.
- Everything after the decode (channel promotion, alpha drop, normalization,
  clipping) is **shared** with the Pillow path, deliberately, so the two
  decoders cannot drift.
- `image_dimensions()` reads TIFF dimensions from the `tifffile` page header
  (no pixel decode), so `downsample_factor` measures against the same decoder
  that will open the file.

Proof is behavioural, in `tests/test_raster.py`:

| test | what it proves |
|---|---|
| `test_sixteen_bit_rgb_tiff_round_trips_without_losing_the_low_byte` | 257/65535 survives; 8-bit could not represent it |
| `test_pillow_would_have_truncated_that_file` | the test above is not vacuous — Pillow reads the same file as `[255,156,1]` |
| `test_load_image_reads_a_sixteen_bit_tiff_at_full_depth` | via `load_image`, which is what the session actually calls |
| `test_sixteen_bit_tiff_survives_max_dim_downsizing` | the resize on the round trip does not quantize |
| `test_supported_suffixes_includes_tiff` | `optimize_begin` will accept the suffix |
| `test_tiff_image_dimensions_are_native` | `downsample_factor` is right for TIFF |

Verified against PixInsight, not just against `tifffile`: PI's `saveAs` after
`setSampleFormat(16, false)` produced files that read back `uint16`.

> **CORRECTED (fix round 1, per review I-2).** This paragraph previously argued
> the low byte was in use because `(value % 256 != 0)` held for **100%** of
> pixels. **That check is vacuous and proves nothing.** Promoting 8-bit data is
> `v -> v*257`, and `v*257 mod 256 == v mod 256`, which is nonzero for every
> `v` except 0 — so the check returns 100% on any promoted 8-bit image. It did
> in fact return 100% on a file that was provably 8-bit data (the proxy; see
> the I-1 section appended below). An invalid check must not stand in the
> record as if it were proof.
>
> The valid check is a distinct-level count plus divisibility by 257. Measured
> on the original run's own artifacts:
>
> | file | dtype | distinct levels | levels NOT a multiple of 257 |
> |---|---|---|---|
> | `proxy.tif` | uint16 | 250 | **0** — 8-bit data in a 16-bit container |
> | `cand-2-0-4.tif` | uint16 | **499** | **495** |
>
> The candidate carries 499 distinct levels, more than 8 bits can hold, and 495
> of them are not reachable by promoting an 8-bit value. **So the 16-bit
> candidate decision itself stands, and now on evidence that can distinguish
> the two cases.** The proxy row is the defect I-1 identifies, fixed below.

---

## Decision 2 — PixInsight processes a PROXY at `max_dim`, not the 5318 px master

`optimize_begin` already distinguishes `image` (the proxy the session optimizes)
from `source_path` (what the recipe is ultimately for); `Session` carries both.
The driver builds the proxy in PixInsight and passes it as `image`.

Why it is not optional: if the master were passed, every instruction's
`parent_path` would be the master and PixInsight would process and save ~60
candidates at 5318x3975. At 16-bit that is **127 MB per candidate, ~8 GB per
run**, into a `WORK` directory whose default is `/tmp` — tmpfs, 16 GB, on a box
with 13 GB of free RAM. At `max_dim` the same run writes ~15 MB per candidate.

**This is not the "pre-corrected pixel scale" mistake the brief warns about.**
The effective scale the fingerprint is taken at is *identical* either way,
because `optimize_begin` multiplies by `downsample_factor` of whatever file it
is handed:

```
master : native_scale x downsample_factor(master, load(master)) = native x 3.32
proxy  : proxy_scale  x downsample_factor(proxy,  load(proxy))  = (native x 3.32) x 1.0
```

The brief's rule ("wire `analyze`'s raw scale straight through") is the correct
rule *when `image` is the master*. What the field means is "the native on-sky
scale of the file you are passing", and the file being passed is the proxy, so
the driver passes the proxy's own scale — computed from the two images' actual
dimensions, not from the requested `MAX_DIM`. `MAX_DIM` is passed explicitly as
`max_dim` on the same call so both ends are the same number by construction.

---

## Decision 3 — trust `executeOn`'s return value, and omit rather than fabricate

`StarXTerminator` **SIGSEGVs under `--automation-mode`** on this install. PI
catches the signal, the script keeps running, and the view comes back
byte-identical:

```
PixInsight 1.9.4 - Critical Signal Backtrace
Received signal 11 (SIGSEGV)
 37: /opt/PixInsight/bin/StarXTerminator-pxm.so(+0xb55ab)
 ...
 33: pi::MetaProcess::ExecuteOn(pi::View*, void*) const
```

Probed directly: `P.executeOn(view)` returns **`false`** for StarXTerminator and
`true` for every other process, and mean/MAD are unchanged before and after. So
the driver checks the boolean, omits the instruction from `produced`, and prints
a named executor failure. The sidecar then logs it as an executor failure for
that candidate only and the run continues — the designed path. Nothing is
fabricated, and the loop's own `no_op` guard is not relied on to clean up after
a crash.

All seven other processes were verified to run headless and change pixels:

```
OK   MultiscaleLinearTransform   151 ms
OK   LocalHistogramEqualization  278 ms
OK   HDRMultiscaleTransform      355 ms
OK   CurvesTransformation         50 ms
OK   HistogramTransformation      63 ms   (1st percentile = 0.1333 via Histogram.normalizedClipLow)
OK   ColorSaturation              67 ms
OK   BackgroundNeutralization     77 ms
FAIL StarXTerminator              57 ms   executeOn -> false, pixels identical
```

Also probed: PixInsight **raises** on out-of-range parameters rather than
clamping (`LocalHistogramEqualization.radius(): numeric value out of range: 8`;
valid range measured as [16, 512]). The driver lets that raise propagate into
the same omit-and-log path rather than clamping, because a clamped parameter
would make the recipe describe something other than what ran.

---

## The blocker: the finished print has no obtainable WCS

The brief's verification run cannot start, and the reason is upstream of
everything Task 13 built:

```
FAIL ValueError: cannot solve a WCS for /mnt/qnap/astro_data/prints/M42_hoo_2_18_23.jpg:
No WCS for M42_hoo_2_18_23.jpg after tiers ['avm']: AVM tag present but unreadable:
NoSpatialInformation: AVM meta-data does not contain any spatial information.
```

(Consistent with the `never-force-parse-avm` finding: pyavm's strictness is
protecting us, and forcing the parse would yield corrupt coordinates.)

### Tier 3 (PixInsight ImageSolver) does not solve this print

Three headless attempts, using this project's own proven ImageSolver recipe
(`pixinsight/solve_spike.js`, `#engine v8` + the astrometry include chain):

| attempt | centre hint | FoV sweep (arcmin) | result |
|---|---|---|---|
| 1 | M42 83.822 / -5.391 | 30, 45, 60, 90, 120, 180, 240 | UNSOLVED — "Failed to perform the initial field alignment" below 120' |
| 2 | 83.85 / -5.28 | 88–110, `distortionCorrection=false` | UNSOLVED — `solveImage` returned false throughout |
| 3 | **83.841 / -5.394 (measured)** | 115–135 around the **measured** 124.4' | UNSOLVED — `solveImage` returned false throughout |

Attempt 3 is the decisive one: it was given the field's true centre and true
plate scale and still failed, so this is not a bad-hint problem. The image is
not starless either — `guardrails.detect_stars` finds **2198 stars** in it at
1600 px. Headless PixInsight prints nothing from inside `solveImage`, so the
reason is not recoverable without a GUI session (consistent with the
`pjsr-imagesolver-recipe` finding). **This is left as a real, open finding, not
worked around.**

### Tier 4 (manual annotation) — with MEASURED numbers, not guessed ones

§5.2: "manual annotation (caller) — first-class fallback, not an error." The
numbers come from the source data of this very print, found on the NAS:
`/mnt/qnap/astro_data/2_18_2023/M42/Light_M42_300.0s_Bin1_20230217-222013_0001.fit`
— same night, same instrument, and **already plate-solved**
(`CTYPE1 = 'RA---TAN-SIP'`).

| quantity | value | source |
|---|---|---|
| pixel scale | **1.4035 "/px** | the light frame's own WCS (`proj_plane_pixel_scales`) |
| pixel scale, independent check | 1.4018 "/px | `206.265 x XPIXSZ 5.94µm / FOCALLEN 874mm` — agrees to 0.1% |
| field centre | 83.84079, -5.39443 | the light frame's WCS at its centre pixel |
| fov radius | 77.66' | half-diagonal of 5318x3975 at that scale |

The print (5318x3975) is a **crop** of the stack (6072x4042), not a resize —
5318/6072 = 0.876 but 3975/4042 = 0.983, and a resize would give one ratio, so
the print is at native scale and the light frame's scale is the print's scale.

Why this mattered: my first estimate of the scale, eyeballed from the M42 ↔
NGC 1977 separation in a preview, was 1.10 "/px — **27% wrong**. A guessed scale
would have silently shifted §4.4's band limit and changed which actions were
proposable at all. That is exactly why the driver requires all four annotation
values together and refuses a partial one.

### What was added to the driver for this — and what was NOT

Added: `manualAnnotation()`, reading `AUTOCONTRAST_MANUAL_RA` / `_DEC` /
`_FOV_RADIUS` / `_PIXEL_SCALE`, passed as the `manual` field `analyze` already
accepts. All four required together or none. When the image will not solve and
no annotation was given, the driver rewrites the sidecar's error to name those
four variables and to say the scale must be measured.

**Not** added, after building and testing it and then reverting it: a `blind`
request field on the sidecar wiring PJSR's ImageSolver result into `acquire_wcs`'s
tier-3 `blind_solver` callback with `wcs_source="blind"`. It is the seam §5.2
documents, and it would have been the honest provenance label — but with tier 3
unable to solve this image there is no caller for it, and shipping a tested-but-
unused tier is speculative code. `manual` is both sufficient and the accurate
label for a number a human asserted.

---

## Decision 4 — the report is mirrored to a FILE, because headless PI eats the console

First full run: `console.writeln` produced **nothing** on stdout. The only thing
that came through was StarXTerminator's SIGSEGV backtraces, which PI's C++ side
writes to stderr. So the whole §12 surface — every guardrail reason, every
`noted` entry — existed only in an interactive session, which is the one place a
batch run never is.

Every line the script prints now goes through `say()` / `warn()` / `note()`,
which mirror to `WORK/report.txt`. The failure path flushes it too, so a run
that dies still leaves everything reported up to that point. Without this, the
brief's requirement to surface the `attempt_cap` and guardrail notices would have
been satisfied on paper and not in fact.

---

## The headless run

```
AUTOCONTRAST_IMAGE=/mnt/qnap/astro_data/prints/M42_hoo_2_18_23.jpg \
AUTOCONTRAST_MANUAL_RA=83.84079 AUTOCONTRAST_MANUAL_DEC=-5.39443 \
AUTOCONTRAST_MANUAL_FOV_RADIUS=77.66 AUTOCONTRAST_MANUAL_PIXEL_SCALE=1.4035 \
/opt/PixInsight/bin/PixInsight.sh -n --automation-mode \
  -r=pixinsight/autocontrast_optimize.js --force-exit
```

`/tmp/autocontrast_optimize/status.txt`, verbatim:

```
PASS improved=true distance=0.2361004170559736 baseline=0.24668278892212744 iterations=3 reference=eso1103a discarded=25 noted=3 executor_failures=9
```

**Wall clock: 31.6 s** (`real 0m31.575s`), of which the script itself measured
27.8 s over 13 sidecar calls, and **PixInsight's own process work was 3 s** —
18 candidates applied and saved across 11 batches. The PixInsight round trip
Task 16 called "additive and unmeasured" is therefore about **0.17 s per
candidate** at the 1600 px proxy, against the sidecar's measured 2.14 s per
scored candidate. It is ~8% of the per-candidate cost, not a new order of
magnitude — Task 16's budget survives contact with a real executor.

Reference resolved: `eso1103a`, CC BY 4.0, ESO / Igor Chekalin. Palette
`unknown` (a JPEG has no FILTER keyword) vs `RGB` → **chroma gated OUT** (§2.3),
and indeed no `chroma` action appears anywhere in the run.

### FINDING 1 — it reported `improved=true` on a finished print

The brief expected `improved=false`. Per its own instruction this is recorded,
not tuned away; **no threshold was touched**.

```
D 0.2467 -> 0.2361   (-4.3%)
recipe (2 stock processes):
  1. BackgroundNeutralization   background_neutralize@-/-
  2. CurvesTransformation       tonal_reshape@-/strong     {monotone: true, strength: 0.85}
convergence: every branch was discarded by guardrails or executor failure
```

Two things make this less of a contradiction than it looks, and both are worth
the reviewer's attention:

1. **The improvement never reaches the master's own measured distance.** The
   sidecar measures the master at **D = 0.2264**; the search's proxy baseline is
   **D = 0.2467**; the "improved" result is **D = 0.2361**. So the optimizer
   climbed back part of the way toward where the master already was, and the
   headline "improvement" is smaller than the gap the proxy step introduced.
2. The beam **collapsed** — §6.3 condition 3, "every branch was discarded" —
   rather than converging on a flat distance. Only one branch was ever alive at
   a time (every instruction is `br0`), because at most 1 of 3 candidates
   survived each iteration.

### FINDING 2 — the proxy resampler moves the distance by 9%

The same image measured two ways:

| array | resampler | D |
|---|---|---|
| master, downsampled by the sidecar | Pillow bicubic | 0.2264 |
| proxy, downsampled by PixInsight | `Resample`, interpolation Auto | 0.2467 |

The fingerprint measures fine structure (§4.2), and a resampler changes fine
structure, so this is not a bug — but it is a **9% shift in the number the whole
system is built around**, arising purely from which library did the downsize.
The search remains internally consistent (every candidate is measured against
the proxy baseline on the same footing), and the driver now prints both numbers
with what each was measured on plus a warning when they diverge by more than
0.01. Flagging for Task 14: the §10 exit criterion should be explicit about
which of these two it is asserting on.

### FINDING 3 — LocalHistogramEqualization cannot realize the finer half of its bands

`local_equalize` maps to LHE (§6.2), and **PixInsight's LHE radius is bounded to
[16, 512] px** (measured: it raises, it does not clamp). At the 1600 px proxy the
print's scale is 4.665 "/px, so the proposer's band ladder in pixels is
1, 2, 4, 8, 16, 32, 64 — and **the four finest bands are unreachable**:

```
local_equalize@18.660/strong  -> LocalHistogramEqualization.radius(): numeric value out of range: 4
local_equalize@9.330/moderate -> LocalHistogramEqualization.radius(): numeric value out of range: 2
```

Six of the nine executor failures are this. It is structural, not incidental: it
will recur on every image whose proxy scale puts the lower bands under 16 px,
i.e. essentially every image. The driver handles it correctly — omit, log, let
the branch ask for a replacement — but the §6.2 mapping is proposing actions
PixInsight cannot perform. **Not fixed here**: changing the band ladder or
substituting a different process would be a §3.7 calibration change, which this
task may not make.

### FINDING 4 — StarXTerminator SIGSEGVs headless, every time

Three attempts, three SIGSEGVs, all caught by PI and all returning
`executeOn -> false`. `star_split` therefore cannot be applied at all in a
headless run on this install. Reported per instruction; the run continued.

### The §12 surface, from the run's own `report.txt`

25 candidates discarded, each with its reason; the guardrails that fired were
`shadow_clipping` (14), `highlight_clipping` (10), `executor` (9),
`star_integrity` (7), `noise_floor` (6), `channel_ratio_drift` (1).

And the three `noted` entries — the exact condition Task 12 predicted, and the
one the brief singled out:

```
* iter 1  branch (root)  [attempt_cap] branch stopped after 9 attempts (cap 9)
    holding only 1 of 3 live candidates; guardrails or the executor rejected
    nearly everything tried on this image
* iter 2  branch background_neutralize@-/-  [attempt_cap] ... only 1 of 3 ...
* iter 3  branch background_neutralize@-/- | tonal_reshape@-/strong
    [attempt_cap] ... holding only 0 of 3 live candidates ...
```

The attempt cap bound on **every** iteration of a real run.

---

## Files

| file | change |
|---|---|
| `pixinsight/autocontrast_optimize.js` | NEW, 888 lines — the PixInsight executor and the §2.5 driver |
| `src/autocontrast/io/loaders.py` | `_TIFF_SUFFIXES`, `_decode_tiff`, `_resize_native`, TIFF branch in `image_dimensions` |
| `src/autocontrast/sidecar.py` | +7 lines: `reference_fingerprint` on the `analyze` response |
| `tests/test_raster.py` | +6 tests |
| `pyproject.toml` | `tifffile>=2023.7` |

Run artifacts (not committed): `/tmp/autocontrast_optimize/{status,report}.txt`,
`recipe.json`, `session-*.json`, 18 candidate TIFFs.

## Constraints held

- No AI anywhere. Action ranking is `propose.py`'s, untouched.
- No tunable adjusted — `top_k`, `width`, `max_attempts`, `epsilon_improve`,
  `iteration_cap`, `epsilon` are all as committed. `MAX_DIM` is the sidecar's own
  existing `max_dim` default, now passed explicitly so both ends agree by
  construction; `MAX_BATCHES` is a driver liveness stop that never bound (11 of
  200) and reports loudly if it ever does.
- Guardrail violations discard candidates and are never a score term — the driver
  only reports the log, it never reads a verdict back into a decision.
- Sidecar response contract unchanged; the one added field is additive.
- No silent fallbacks: nine executor failures, twenty-five discards and three
  `noted` entries all reached `report.txt` with their reasons.
- Input already stretched — the test print is a finished JPEG; no auto-stretch
  exists anywhere in this path.

## Concerns, in priority order

1. **`improved=true` on a finished print (Finding 1).** Recorded, not tuned. The
   proposed gain (0.2467 -> 0.2361) does not reach the master's own measured
   0.2264, and the run ended by beam collapse rather than convergence. Worth a
   decision before Task 14 asserts an exit criterion on this image.
2. **The 9% proxy-resampler shift (Finding 2)** is the same concern from the
   other side, and it is the one I would look at first: two legitimate
   downsamples of one file give distances 9% apart. Task 14 should state which
   array its criterion is about.
3. **LHE cannot realize four of seven bands (Finding 3).** Structural, will recur
   on every image. §6.2's mapping proposes what PixInsight cannot do. Fixing it
   is a §3.7 calibration change this task may not make.
4. **StarXTerminator is unusable headless (Finding 4)** — `star_split` never
   applies in a batch run on this install.
5. **The print cannot be plate-solved (the blocker).** Tier 4 with numbers
   measured off the source lights is a sound workaround for this one image, but
   `AUTOCONTRAST_MANUAL_*` is a per-image manual step; any batch use of the
   driver over a print archive needs tier 3 to work.
6. **`ImageWindow.open` is called per instruction** (18 opens for 18 candidates).
   At 0.17 s/candidate that is not worth optimizing now, but it is the obvious
   lever if the round trip ever matters.

---
---

# Fix round 1 — the four Important review issues

Status: **COMPLETE**. Four issues fixed, nothing else touched. The §6.2 band
ladder, every tunable, and the `improved=true` fail-safe are exactly as they
were. Nothing pushed.

Tests: **433 passed in 246.42s** — the 431 baseline plus 2 new tests for I-3.
Nothing deselected, no skips, no warnings. `ruff` on the two files I touched:
1 error before and 1 after, the same pre-existing `E702` on a line I did not
edit; repo-wide 28 before and 28 after.

## I-1 — the search proxy carried only 8 bits. Fixed, and it mattered more than expected.

`pixinsight/autocontrast_optimize.js:261-270`: `w.setSampleFormat(CANDIDATE_BITS,
false)` now runs **above** the `if (factor < 1.0)` Resample block instead of
below it, so PixInsight computes the downsize into a 16-bit buffer. No tunable
moved; this changes the precision of an intermediate.

The reviewer's measurement reproduced exactly, and the fix is confirmed on the
new run's own `proxy.tif`:

| `proxy.tif` | dtype | distinct levels | levels NOT a multiple of 257 |
|---|---|---|---|
| before the fix | uint16 | **250** | **0** |
| after the fix | uint16 | **62443** | **62198** |

250 distinct values all divisible by 257 is 8-bit data in a 16-bit container.
62443 levels is a real 16-bit downsize. The proxy is the baseline D and the root
parent of every branch, so this was the one array where the approximation cost
the most.

## The re-run: new numbers against old, both from real headless PixInsight runs

Same command, same manual annotation, `/tmp/autocontrast_optimize` cleared first.

`/tmp/autocontrast_optimize/status.txt`, verbatim:

```
PASS improved=true distance=0.22820433163518158 baseline=0.24534493736630653 iterations=3 reference=eso1103a discarded=41 noted=5 executor_failures=15
```

| quantity | OLD (8-bit proxy) | NEW (16-bit proxy) | change |
|---|---|---|---|
| baseline D (the PROXY — what the search descends) | **0.24668** | **0.24534** | -0.00134 |
| master D (sidecar's own Pillow downsample) | **0.2264** | **0.2264** | unchanged, as expected |
| Finding 2 divergence (proxy vs master) | 0.0202 (**9.0%**) | 0.0189 (**8.3%**) | narrowed, but only slightly |
| final D | 0.2361 | **0.2282** | -0.0079 |
| improvement over baseline | -4.3% | **-7.0%** | |
| `improved` | true | true | unchanged |
| iterations / batches | 3 / 11 | 3 / 12 | |
| candidates applied | 18 | **30** | +67% |
| guardrail discards | 25 | 41 | |
| `noted` entries | 3 | 5 | |
| executor failures | 9 | 15 | |
| PixInsight's own process time | 3.0 s | 4.9 s | |
| sidecar time / calls | 27.8 s / 13 | 40.9 s / 14 | |
| **wall clock** | **31.6 s** | **44.7 s** (`real 0m44.731s`) | +41% |

Three things worth the reviewer's attention in that table:

1. **Only about a twelfth of Finding 2 was an 8-bit artifact.** The divergence
   went from 0.0202 to 0.0189. The remaining 0.0189 — 8.3% of the master's own D
   — is a genuine disagreement between PixInsight's `Resample` and Pillow's
   bicubic about what the fine structure of this image is. My concern #2 from the
   original report **stands essentially unchanged**: Task 14 must still say which
   array §10's exit criterion asserts on. Note also that the master's D is a
   Pillow 8-bit read of an 8-bit JPEG, so neither number is a 16-bit measurement
   of the master; only the proxy side of this comparison is now at full depth.

2. **The search got real breadth for the first time.** 30 candidates applied
   against 18, and the beam actually held two live branches (`br0` and `br1`
   both appear in iterations 2 and 3, where the old run was `br0` throughout).
   Iteration 1's attempt cap now reports "2 of 3 live candidates" where it
   previously reported 1 of 3. More candidates survived scoring, which is also
   why the discard and executor-failure counts rose — more instructions were
   issued, not a higher failure rate. The 8-bit proxy was starving the search.

3. **The recipe changed, and got simpler.** Old: `BackgroundNeutralization` +
   `CurvesTransformation`. New: `CurvesTransformation tonal_reshape@-/strong`
   alone, reaching a *lower* distance (0.2282 vs 0.2361). The extra 13 s of wall
   clock is the cost of scoring 12 more candidates.

Everything else about the run reproduced: `improved=true` on the finished print
(Finding 1), LHE's four unreachable bands (Finding 3 — 8 of the 15 executor
failures), StarXTerminator's SIGSEGV every time (Finding 4 — the other 5, plus
2 more from the wider search), the beam ending by collapse rather than
convergence, and the attempt cap binding on every iteration. **No threshold was
touched and no fail-safe was altered.**

## I-2 — the invalid 16-bit verification, corrected in place

Corrected in the "Decision 1" section above, not appended here, so the record
does not carry an invalid check anywhere. The `% 256` argument was vacuous —
`v*257 mod 256 == v mod 256` — and it returned 100% on a file that was provably
8-bit data. Replaced with the distinct-level and `% 257` measurements, which do
distinguish the two cases and which confirm the candidates (499 levels, 495 not
multiples of 257) always did carry sub-8-bit detail. The 16-bit TIFF decision
itself was right; only its stated evidence was not.

## I-3 — `load_raster`'s bit-depth heuristic

`src/autocontrast/io/loaders.py`. The heuristic picked its divisor from the
pixel values (`65535 if arr.max() > 255 else 255`). Reproduced before touching
anything: a uint16 TIFF of constant 200 (true value 0.003052) loaded as
**0.784314** — 257x too bright, no log, no exception.

Fix: the *decoder* now carries the true full scale forward instead of the
normalization inferring it. `_decode_tiff` returns `(array, full_scale)` where
`full_scale` comes from a new `_INTEGER_FULL_SCALE` map. This has to happen at
the decode: `_resize_native` hands back float32, so by the time the
normalization runs the dtype is already gone.

`_INTEGER_FULL_SCALE` covers **uint8 and uint16 only, deliberately.** For wider
integer types the container width is not the data range — Pillow stores 16-bit
grayscale TIFF as mode `I` (int32), where full scale is 65535 and not 2**31-1 —
so there is no defensible constant and those keep the range inference. That is
also why the pre-existing `test_sixteen_bit_is_normalized_by_full_range` still
passes unchanged rather than needing its expectations rewritten.

Two new tests in `tests/test_raster.py`:

- `test_a_dark_sixteen_bit_tiff_is_not_rescaled_as_if_it_were_eight_bit` — the
  exact reported case; asserts `img.max() < 0.01`, which fails loudly at 0.784.
- `test_a_dark_sixteen_bit_tiff_survives_the_downsizing_path_too` — the same
  through `max_dim`, which is the path the optimizer actually uses and the one
  where the dtype is destroyed by the resize.

### One thing I did beyond the literal wording of I-3, and why

I-3 names `_decode_tiff`. I applied the same full-scale lookup to the **Pillow**
branch as well (one line, `full_scale = _INTEGER_FULL_SCALE.get(arr.dtype)`).
Flagging it explicitly because it is a behaviour change on the path that loads
every reference render in the corpus.

The reason is that fixing only the TIFF branch would make the two decoders
disagree: a near-black uint8 `.tif` would normalize by 255 while a byte-identical
`.png` would not (`max <= 1.0` skips the old division entirely, so a uint8 image
of constant 1 returned **1.0** — white — where its true value is 1/255). The
file's own docstring states that everything downstream of the decode is shared
"deliberately, so the two decoders cannot come to disagree about what a loaded
image is." A one-branch fix would have installed exactly that disagreement.

Measured blast radius: for uint8 data the two rules differ *only* when
`max(arr) <= 1`, i.e. an essentially black image. No test changed behaviour
(433 pass, same set), and the master's D in the headless re-run is **0.2264**,
identical to the old run — the JPEG path is byte-for-byte unaffected. If you
would rather this were reverted to TIFF-only and tracked separately, say so and
I will.

## I-4 — the false safety claim

`pixinsight/autocontrast_optimize.js:401-416`. The comment claimed the
clip-limited black point meant "no more than 1% of pixels can be clipped by
construction… this is the action refusing to need it." The run's own data
contradicts it — every `black_point@-/strong` was discarded by `shadow_clipping`
at 2.44%, 2.54%, 3.97%, 2.81% in the old run, and at 4.03%, 4.91%, 2.34%, 2.42%,
2.56% in the new one.

The comment now says what is actually true: the construction bounds **how far
the black point moves** (a fraction of the 1st percentile, strength <= 0.85), and
it does **not** bound the clipped fraction. It names the likely mechanism — the
percentile is taken once over the whole image while `HistogramTransformation`
applies per channel, so one channel under the new black point is enough to count
a pixel as clipped — records the measured 2.4-4.0% against the claimed 1%, and
states plainly that §7's guardrail is the real bound and that nothing should be
built on a stronger claim. **The action's behaviour is unchanged**; only the
comment was wrong.

## Not touched, per the fix brief

- §6.2's band ladder and the LHE substitution (Finding 3) — a §3.7 calibration
  change.
- Every tunable: `top_k`, `width`, `max_attempts`, `epsilon_improve`,
  `iteration_cap`, `epsilon`.
- The `improved=true` fail-safe. The reviewer traced it end to end and found it
  intact; I agree and changed nothing. It reported `improved=true` again on the
  re-run, from a *larger* margin, off a proxy that is now at full depth.
- The reviewer's four Minor findings (M-1 through M-4).

## Concerns

1. **Finding 2 mostly survived the fix.** 8.3% divergence between two legitimate
   downsamples of one file, now with the proxy side at genuine 16 bits. Task 14
   still has to name the array. This is unchanged as my top concern.
2. **The 8-bit proxy was suppressing the search, not just its precision.** 18 ->
   30 candidates applied and one -> two live branches from a two-line ordering
   change. Any earlier measurement of beam breadth, attempt-cap frequency or
   per-run cost taken against the old proxy is understated; Task 16's budget in
   particular should use **44.7 s / 30 candidates**, not 31.6 s / 18.
3. **The Pillow-branch line in I-3** (above) is the one place I went past the
   literal fix scope. Argued, measured, and easy to revert.
4. Findings 1, 3 and 4 are unchanged and remain open exactly as originally filed.
