# Task 13 re-review — fix round 1 (diff 56935e3..7635c42)

Read-only, no git mutations, PixInsight not run. Ran `.venv/bin/python -m pytest
tests/test_raster.py -q` (17 passed) and independently re-derived the proxy's
distinct-level count from the fixer's own `/tmp/autocontrast_optimize/proxy.tif`
artifact (still on disk, timestamps match the report) to check the re-run claim
without re-executing PixInsight.

---

### Finding Verdicts

- **I-1**: ADDRESSED — `pixinsight/autocontrast_optimize.js:270` (`setSampleFormat`) now runs before the `if (factor < 1.0)` Resample block at line 272. Independently confirmed: `tifffile.imread('/tmp/autocontrast_optimize/proxy.tif')` → uint16, 62443 distinct levels, 62198 not multiples of 257 — matches the report's table exactly.
- **I-2**: ADDRESSED — `task-13-report.md:73-94`, the vacuous `% 256` claim is struck and replaced in place with the distinct-level/`%257` measurement; the record no longer carries the invalid check as evidence.
- **I-3**: ADDRESSED — `src/autocontrast/io/loaders.py:144-166` (`_decode_tiff` returns `(arr, full_scale)`), `:278` (Pillow branch), `:287-297` (`load_raster` uses `full_scale` when known). Two new tests (`tests/test_raster.py:163-191`) assert exact scaled values (`img.max() < 0.01`, `assert_allclose(img, 200/65535, ...)`), not merely that the path runs — genuine proof, and both pass locally.
- **I-4**: ADDRESSED — `pixinsight/autocontrast_optimize.js:411-427`, the comment now states the construction bounds only how far the black point moves, cites the measured 2.4-4.0% clip rate against the claimed 1%, names the per-channel mechanism, and defers the real bound to §7's guardrail. Action code (`buildHistogramTransformation`) is byte-identical below the comment — behavior unchanged as required.

### Scope Limits

- §6.2 band ladder / LHE substitution: honored — no changes to any band-ladder or process-selection file; diff touches only `autocontrast_optimize.js`, `loaders.py`, `tests/test_raster.py`.
- No tunable adjusted: honored — grepped the diff for `top_k`, `MAX_DIM`, `max_attempts`, `epsilon_improve`, `iteration_cap`, `epsilon`; the only `MAX_DIM` hits are unchanged context lines around the relocated `setSampleFormat` call.
- `improved=true` fail-safe untouched: honored — `loop.py`/`sidecar.py` do not appear in the diff at all.
- Minor findings M-1..M-4 not addressed: honored — `test_sixteen_bit_tiff_survives_max_dim_downsizing` (M-1, flat-field test) is unchanged; the D-label (M-2, `autocontrast_optimize.js:805`), `begun.converged` check (M-3), and `ImageWindow.open` per-instruction pattern (M-4) are all untouched in the diff.

### The Scope Excursion

Justified. I-3 as written names `_decode_tiff` only, but the fixer also added `full_scale = _INTEGER_FULL_SCALE.get(arr.dtype)` to the Pillow branch (`loaders.py:278`). The file's own docstring requires the two decoders to never disagree about what a loaded image is; fixing only the TIFF side would have created exactly that disagreement (a near-black `.tif` normalizing by full scale while a byte-identical `.png` did not). The blast radius is real but narrow — behavior changes only when a uint8/uint16 Pillow array has `max <= 1`, i.e. a pathological near-black image — and is evidence-backed: the suite still reports the same 433/433, and the real-run master D (loaded via this exact Pillow branch) is unchanged at 0.2264 across both runs. Low risk, correctly self-reported, and consistent with the file's stated invariant. Keep it.

### The Re-run

Internally consistent and honestly reported. Cross-checked against the actual run artifacts still present on disk (`/tmp/autocontrast_optimize/`, 30 `cand-*.tif` files, `status.txt`, `report.txt`, all dated to the report's timestamp):

- `status.txt` verbatim: `distance=0.22820433163518158 baseline=0.24534493736630653 ... discarded=41 noted=5 executor_failures=15` — matches the report's table exactly, to full precision.
- `report.txt`'s "Candidates discarded (41)" section lists exactly 41 entries, "PixInsight executor failures" lists exactly 15 (6 LHE-range + 6 StarXTerminator SIGSEGV-adjacent... breakdown: 3+6+6 across iterations = 15), and "Noted (5)" lists exactly 5 `attempt_cap` entries. All header counts match their own body counts.
- `proxy.tif` independently re-measured: uint16, 62443 distinct levels, 62198 not multiples of 257 — matches the report's I-1 table exactly, and is strong evidence this was a real headless PixInsight execution, not a fabricated number (fabricating a specific 5-digit distinct-level count that also matches a second derived metric is implausible).

Claim 1 — *"only about a twelfth of Finding 2 was an 8-bit artifact"*: doesn't quite follow from the numbers as precisely as stated. Divergence went from 0.0202 to 0.0189, a reduction of 0.0013, which is 6.4% of the original 0.0202 — closer to a fifteenth than a twelfth. The qualitative conclusion (most of the 8.3% that remains is a genuine PixInsight-vs-Pillow resampler disagreement, not an 8-bit artifact) is correct and well-supported; the specific fraction named is a minor arithmetic overstatement, not a substantive error. Doesn't change any of the report's conclusions or Task 14's obligation to name the array.

Claim 2 — master D unchanged at 0.2264: reasoning holds. The master path is a pure-Python Pillow downsample of the JPEG, entirely upstream of the PJSR proxy fix, and for a normal-range uint8 JPEG the I-3 excursion is provably a no-op (only affects `max <= 1` arrays). An unchanged control value across two runs that changed everything downstream of the proxy is exactly the evidence a correctly-scoped fix should produce.

`improved=true` reproducing with a *larger* margin (-7.0% vs -4.3%) off a full-depth proxy is consistent with, and actually strengthens, the prior review's conclusion. The prior review already established structurally that `improved` cannot be true unless a real PixInsight candidate is closer to the reference than the baseline the search was given, on the same footing (`_optimize_loader` shared by baseline and candidates). The fixer's own data shows *why* the margin grew: the 8-bit proxy was starving the search (1 live branch, 18 candidates applied) and the 16-bit proxy gave it real breadth (2 live branches, 30 candidates applied, a simpler and better-scoring recipe). That is exactly the shape of result the fail-safe should produce once the artifact suppressing search breadth is removed — not evidence of a defect in it.

### New Breakage in the Fix Diff

None. `ruff check src/autocontrast/io/loaders.py` passes clean. The one pre-existing `E702` in `tests/test_raster.py:217` is on `test_load_image_dispatches_raster_to_pillow`, untouched by this diff, matching the report's claim. Reviewed the Pillow-branch full_scale change for edge cases (grayscale, RGBA-with-alpha-drop, mode "P", bool masks, float data) — all resolve to identical behavior to the pre-fix code except the intentional near-black-image case, which is the one case being fixed.

### Out-of-Scope Observations

Minor: the "about a twelfth" phrasing in the re-run section (task-13-report.md) is a loose overstatement of a 6.4% reduction — "about a fifteenth" would be more accurate. Non-blocking; doesn't affect any conclusion the report or Task 14 draws.

### Verdict

**Fix round:** All findings addressed, no new Critical/Important breakage.
