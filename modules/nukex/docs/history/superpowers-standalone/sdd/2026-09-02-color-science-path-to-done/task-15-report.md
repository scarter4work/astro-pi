# Task 15 Report: `tools/import_qe_research.py`

## Status: DONE (after ruling below)

9 of 10 required tests pass. `test_real_research_file_round_trip` fails against
the real `research/qe_database_research.json`, and the failure is a genuine
data gap the brief's own constraints ("never invent QE numbers", "never relax
a validation", "never special-case a product name") forbid me from coding
around. Nothing is committed.

## What I implemented

- `tools/test_import_qe_research.py` — transcribed verbatim from the brief's
  Step 1 code block.
- `tools/import_qe_research.py` — transcribed verbatim from the brief's Step 3
  code block, `chmod +x` applied.

Before writing anything I diffed both files byte-for-byte against the code
blocks embedded in
`.superpowers/sdd/2026-09-02-color-science-path-to-done/task-15-brief.md`
(extracted programmatically) — both are **identical** to the brief. The
failure below is not a transcription error.

## TDD evidence

**RED** — `python3 -m pytest tools/test_import_qe_research.py -q` (script not
yet created):

```
FAILED tools/test_import_qe_research.py::test_drops_meta_and_research_only_fields
FAILED tools/test_import_qe_research.py::test_osc_camera_inherits_sensor_qe_with_G_mean_and_bayer_key
FAILED tools/test_import_qe_research.py::test_mono_camera_ships_mono_pk_only_and_no_bayer
FAILED tools/test_import_qe_research.py::test_canonical_dual_nb_entries_use_median_fwhm
FAILED tools/test_import_qe_research.py::test_classifier_product_canonicals_present_and_hb_dropped
FAILED tools/test_import_qe_research.py::test_single_line_canonicals_and_broadband_products
FAILED tools/test_import_qe_research.py::test_generic_sony_osc_camera_is_mean_of_sony_osc_cameras
FAILED tools/test_import_qe_research.py::test_missing_required_field_fails_loud
FAILED tools/test_import_qe_research.py::test_missing_canonical_filter_fails_loud
FAILED tools/test_import_qe_research.py::test_real_research_file_round_trip
10 failed in 0.11s
```
Failure was `[Errno 2] No such file or directory` for the script, as expected.

**GREEN (partial)** — after implementing, `python3 -m pytest tools/test_import_qe_research.py -q`:

```
.........F                                                               [100%]
=================================== FAILURES ===================================
______________________ test_real_research_file_round_trip ______________________
    def test_real_research_file_round_trip(tmp_path):
        dst = tmp_path / "shipped.json"
        r = run(RESEARCH, dst)
>       assert r.returncode == 0, r.stderr
E       AssertionError: import_qe_research: Camera 'atik-460ex-color': wavelength 486 lacks R/G/B photosites: ['mono']
1 failed, 9 passed in 0.28s
```

The other 9 tests (all synthetic-fixture tests) pass cleanly with no warnings.

## Root cause of the one failure

`research/qe_database_research.json`'s sensor `ICX694` (Sony EXview HAD II
CCD, `"type": "both-variants"` — i.e. sold in both mono and Bayer-color camera
bodies) was only ever researched with a single flat QE curve per wavelength,
keyed `"mono"`:

```json
"ICX694": {
  "type": "both-variants",
  "qe": {"486": {"mono": 0.65}, "501": {"mono": 0.67}, "656": {"mono": 0.65}, "672": {"mono": 0.6}}
}
```

Two real cameras inherit from it:
- `atik-460ex` (`type: mono`) — fine, the brief's mono path only needs
  `mono_pk`, which is absent here too, so this camera ships with an empty
  `qe` block and is correctly reported in the `mono_without_qe` note.
- `atik-460ex-color` (`type: OSC`, `bayer_pattern: RGGB`) — this is the
  problem. An OSC camera's QE has to be split by Bayer photosite (`R`,
  `Gr`/`Gb`/`G`, `B`); the research file for this sensor never recorded that
  split, only a panchromatic "mono" number. `photosites()` correctly raises:
  there is no R/G/B data to extract, at any of the sensor's 4 wavelengths.

I confirmed (diagnostically, without touching any shipped file) that this is
the **only** camera in the real file that fails:

```python
# ad hoc check, not part of the shipped script:
failures = []
for name, cam in research["cameras"].items():
    try:
        transform_camera(name, cam, sensors)
    except TransformError as e:
        failures.append((name, str(e)))
# -> [('atik-460ex-color', "Camera 'atik-460ex-color': wavelength 486 lacks R/G/B photosites: ['mono']")]
```

## Why I did not fix it myself

Every fix I could construct either invents data, relaxes a validation, or
special-cases a camera by name — all three explicitly forbidden by the task
message:

1. **Synthesize R/G/B from the flat mono value** (e.g. `R=G=B=mono`) —
   physically wrong (a Bayer filter does not pass R/G/B equally; every other
   camera in the dataset shows large R/B divergence, e.g. IMX585 at 656 nm:
   R=0.73, B=0.03) and is exactly "inventing QE numbers."
2. **Loosen the OSC validation** to accept a camera with no derivable R/G/B
   data (mirroring the mono `mono_without_qe` escape hatch) — this is
   "relaxing a validation," and it also breaks
   `test_real_research_file_round_trip`'s `assert len(out["cameras"]) >= 56`:
   dropping `atik-460ex-color` from the shipped cameras leaves 54 real
   cameras + `generic_sony_imx_osc` = 55, which is `< 56`. I am not authorized
   to edit the test (Step 1 says transcribe it verbatim), so I can't reconcile
   that number myself either.
3. **Special-case `atik-460ex-color`** to skip or substitute — explicitly
   forbidden ("never special-case a product name").

I do not have the authority (file scope is `tools/import_qe_research.py` +
`tools/test_import_qe_research.py` only) to add real per-channel QE
measurements to `research/qe_database_research.json`, which is the only fix
that wouldn't fabricate data, relax validation, or change the test's expected
camera count.

## Secondary, non-blocking finding (same root shape, worth flagging)

Six sensors (`IMX174`, `IMX264`, `KAF-8300`, `KAF-16200`, `ICX694`, `ICX805`)
record their monochrome QE under the key `"mono"` instead of `"mono_pk"` —
inconsistent with the ten sensors that do use `"mono_pk"` (e.g. `IMX585` in
the test fixture) for the identical concept. This doesn't fail any test
(`mono` cameras degrade gracefully to an empty `qe` block + a stderr note),
but it means **8 of 24 real mono cameras** ship with no usable QE data purely
because of a key-name inconsistency in the research file, not because the
measurement is actually missing:

Diagnostic run (real file, with the one blocking camera excluded, to observe
the rest of the pipeline — **not what would ship**):

```
wrote <output>: 55 cameras, 96 filters
note: 8 mono camera(s) ship without a QE block (sensor has no mono_pk; mono frames never enter the Q-solve): ['qhy268m', 'asi174mm', 'qhy174', 'atik-460ex', 'atik-16200', 'atik-383l', 'asi1600mm', 'qhy461m']
```

The stderr note's wording ("sensor has no mono_pk") is technically accurate
but potentially misleading for these 8: the sensors *do* have monochrome QE
measurements, just filed under `"mono"`. Recognizing `"mono"` as an alias for
`"mono_pk"` would fix this cleanly for the mono side (it doesn't invent
numbers — it's the same measurement under a different key) but is still a
change to the brief's documented photosite-key vocabulary, so I left it alone
and am flagging it here rather than acting unilaterally. Fixing it would
**not** resolve the blocking `atik-460ex-color` failure (that camera needs an
actual Bayer-split measurement, which no key alias can supply).

## Resolution options for the team-lead / user to choose from

1. Add real per-channel QE data for `ICX694`'s OSC variant to
   `research/qe_database_research.json` (the "no invented numbers" fix).
2. Deliberately exclude `atik-460ex-color` from the shipped camera set (needs
   a documented behavior change to the transform — a per-camera skip path —
   plus lowering `test_real_research_file_round_trip`'s
   `len(out["cameras"]) >= 56` to `>= 55`, since it's currently hard-coded
   against the assumption all 55 research cameras ship).
3. Something else the team-lead prefers (e.g. mark `atik-460ex-color` as
   research-incomplete in a separate research-data flag the transform is
   allowed to check).

Separately, decide whether to fix the `"mono"` vs `"mono_pk"` key
inconsistency in the research data (recommended — it's free, real QE data
currently being silently dropped) or leave the transform's `mono_pk`-only
behavior as documented.

## Files changed

- `tools/import_qe_research.py` (new, not committed)
- `tools/test_import_qe_research.py` (new, not committed)

No commit was made — Step 4's gate ("10 passed, pristine output") is not met,
and Step 5 was not reached.

## Self-review (pre-ruling)

- Both files verified byte-identical to the brief via automated diff.
- Confirmed via ad hoc diagnostic (not shipped) that exactly one camera fails,
  and captured the exact error message and root cause.
- Did not modify the test file, did not relax validation, did not invent QE
  numbers, did not special-case the failing camera name in the shipped
  script.
- Cleaned up `tools/__pycache__/` before finishing; it is untracked and not
  staged.

---

## Fix report (after team-lead ruling)

Team-lead ruled on both findings from the NEEDS_CONTEXT above:

1. An OSC camera whose sensor has **zero** wavelengths with a Bayer split
   (`atik-460ex-color` / `ICX694`) is **excluded** from `cameras`, with a
   stderr note — this is the spec 6.3 unresolved-INSTRUME path by design; the
   engine falls back to `generic_sony_imx_osc` at runtime with a Process
   Console warning. A camera with a Bayer split at **some** wavelengths but
   not others stays a hard `TransformError` — inconsistent data, not a
   missing measurement.
2. `"mono"` is aliased to `"mono_pk"` in `photosites()` — vocabulary drift in
   the research data, not a different quantity; the shipped key stays
   `mono_pk`.

### What changed

`tools/import_qe_research.py`:
- `photosites()`: for OSC, wavelengths lacking R/G/B are now collected into
  an `incomplete` list instead of raising immediately. After the loop, raises
  only if `out` is non-empty AND `incomplete` is non-empty (partial/
  inconsistent data — still loud). If `out` ends up empty, returns `{}` and
  lets the caller decide. For mono, `sites.get("mono_pk", sites.get("mono"))`
  aliases the two keys; shipped key is always `mono_pk`.
- `transform_camera()`: now returns `None` when `cam["type"] == "OSC"` and
  `out["qe"]` is empty (zero Bayer-split wavelengths) — signals exclusion
  rather than raising.
- `transform()`: collects `None` results into a new `osc_without_bayer_qe`
  list instead of adding them to `cameras`; returns it as a third tuple
  element.
- `main()`: prints a second stderr note when `osc_without_bayer_qe` is
  non-empty, using the team-lead's specified wording.
- Module docstring updated to document both research-data gaps and how the
  transform handles them.

`tools/test_import_qe_research.py`: added
`test_osc_camera_without_bayer_qe_is_excluded_with_note` and
`test_mono_alias_key_ships_as_mono_pk` per the team-lead's spec; adjusted
`test_real_research_file_round_trip`'s camera-count bound from `>= 56` to
`>= 55` and added a loop asserting every shipped OSC camera has R/G/B at
`>= 2` wavelengths.

### Covering tests

- `test_osc_camera_without_bayer_qe_is_excluded_with_note` — covers the
  exclusion path and its stderr note.
- `test_mono_alias_key_ships_as_mono_pk` — covers the `"mono"` alias.
- `test_real_research_file_round_trip` — covers both against the real file.
- All 7 pre-existing tests re-verified green (no regressions).

### Command and output

```
$ python3 -m pytest tools/test_import_qe_research.py -v
tools/test_import_qe_research.py::test_drops_meta_and_research_only_fields PASSED
tools/test_import_qe_research.py::test_osc_camera_inherits_sensor_qe_with_G_mean_and_bayer_key PASSED
tools/test_import_qe_research.py::test_mono_camera_ships_mono_pk_only_and_no_bayer PASSED
tools/test_import_qe_research.py::test_canonical_dual_nb_entries_use_median_fwhm PASSED
tools/test_import_qe_research.py::test_classifier_product_canonicals_present_and_hb_dropped PASSED
tools/test_import_qe_research.py::test_single_line_canonicals_and_broadband_products PASSED
tools/test_import_qe_research.py::test_generic_sony_osc_camera_is_mean_of_sony_osc_cameras PASSED
tools/test_import_qe_research.py::test_missing_required_field_fails_loud PASSED
tools/test_import_qe_research.py::test_missing_canonical_filter_fails_loud PASSED
tools/test_import_qe_research.py::test_osc_camera_without_bayer_qe_is_excluded_with_note PASSED
tools/test_import_qe_research.py::test_mono_alias_key_ships_as_mono_pk PASSED
tools/test_import_qe_research.py::test_real_research_file_round_trip PASSED

============================== 12 passed in 0.35s ==============================
```

### Real-file summary and stderr notes (verbatim)

Ran directly against the real research file (diagnostic run, output not
committed — Task 16 owns generating the shipped `share/qe_database.json`):

```
note: 1 OSC camera(s) excluded — research has no Bayer-split QE; generic_sony_imx_osc applies at runtime with a Process Console warning: ['atik-460ex-color']
wrote /tmp/.../shipped.json: 55 cameras, 96 filters
```

Note the `mono_without_qe` stderr note (`"note: N mono camera(s) ship without
a QE block..."`) does **not** appear on the real file post-fix: the `"mono"`
alias recovered real QE for all 8 previously-empty mono cameras (`qhy268m`,
`asi174mm`, `qhy174`, `atik-16200`, `atik-383l`, `asi1600mm`, `qhy461m`,
`atik-460ex`) — verified each now ships `mono_pk` values at all 4
wavelengths, e.g. `asi174mm: {"486": {"mono_pk": 0.71}, "501": {"mono_pk":
0.74}, "656": {"mono_pk": 0.55}, "672": {"mono_pk": 0.5}}`.

Camera count: 55 (54 real cameras, `atik-460ex-color` excluded, +
`generic_sony_imx_osc`). Filter count: 96 (unchanged).

### Commit

`b00ad25` — `feat(tools): import_qe_research.py maps research JSON onto the
engine's lookup keys`, including the ruling's exclusion/alias paragraph in
the body, both required trailer lines.

### Self-review (post-fix)

- Neither fix invents QE numbers (the alias ships the exact same researched
  value under a different key; the exclusion ships nothing rather than
  fabricating a Bayer split) or special-cases a camera/sensor name in code —
  both are behavior driven by data shape (`sites` keys present, `out["qe"]`
  empty), not string matching on `atik-460ex-color` or `ICX694`.
- Partial-Bayer-data case remains a hard `TransformError`, unchanged in
  spirit from the original brief, now reachable via the `incomplete` list
  rather than an immediate per-wavelength raise (verified no existing test
  depended on the old immediate-raise wording, since none asserted on it).
- Pytest output is pristine: 12 passed, no warnings.
- `tools/__pycache__/` cleaned before commit; git status confirms only the
  two intended files are tracked.

---

## Fix report round 2: enforce Ha/SII-before-OIII order (review finding)

Review found `tools/import_qe_research.py:112-121`: the `PRODUCT_CANONICAL`
entries (L-eXtreme, L-eNhance, L-Ultimate, ALP-T) shipped `q_lines` in the
source product's `passes` order, unsorted. Only the derived `HaO3`/`S2O3`
path sorted by descending `wavelength_nm`. The four real products happen to
list Ha/SII first, so output was correct by accident, not by code — a
product researched with OIII listed first in `passes` would have shipped
`["OIII", "Ha"]` silently.

### What changed

`tools/import_qe_research.py`:
- Added `ordered_for_q_solve(lines)`: `sorted(lines, key=lambda l:
  -l["wavelength_nm"])`, defined next to `q_solve_lines()`.
- Product-canonical path: `out[PRODUCT_CANONICAL[name]] = {"type": "DUAL_NB",
  "lines": ordered_for_q_solve(q_lines)}` (was unsorted `q_lines`).
- Derived dual-NB path: `out[key] = {"type": "DUAL_NB", "lines":
  ordered_for_q_solve(median_lines(sources))}`, replacing the inline
  `lines.sort(key=lambda l: -l["wavelength_nm"])`. Both paths now go through
  the one helper.

`tools/test_import_qe_research.py`: added
`test_product_canonical_line_order_enforced_regardless_of_source_order` —
copies `BASE`, overwrites `Optolong-LeXtreme-7nm`'s `passes` to list OIII
(500.7 nm) before Ha (656.3 nm), and asserts both `L-eXtreme` and `HaO3`
still ship `["Ha", "OIII"]`.

### Covering test

`test_product_canonical_line_order_enforced_regardless_of_source_order` — the
new test above. Placed before
`test_generic_sony_osc_camera_is_mean_of_sony_osc_cameras`.

### Command and output

```
$ python3 -m pytest tools/test_import_qe_research.py -v
tools/test_import_qe_research.py::test_drops_meta_and_research_only_fields PASSED
tools/test_import_qe_research.py::test_osc_camera_inherits_sensor_qe_with_G_mean_and_bayer_key PASSED
tools/test_import_qe_research.py::test_mono_camera_ships_mono_pk_only_and_no_bayer PASSED
tools/test_import_qe_research.py::test_canonical_dual_nb_entries_use_median_fwhm PASSED
tools/test_import_qe_research.py::test_classifier_product_canonicals_present_and_hb_dropped PASSED
tools/test_import_qe_research.py::test_single_line_canonicals_and_broadband_products PASSED
tools/test_import_qe_research.py::test_product_canonical_line_order_enforced_regardless_of_source_order PASSED
tools/test_import_qe_research.py::test_generic_sony_osc_camera_is_mean_of_sony_osc_cameras PASSED
tools/test_import_qe_research.py::test_missing_required_field_fails_loud PASSED
tools/test_import_qe_research.py::test_missing_canonical_filter_fails_loud PASSED
tools/test_import_qe_research.py::test_osc_camera_without_bayer_qe_is_excluded_with_note PASSED
tools/test_import_qe_research.py::test_mono_alias_key_ships_as_mono_pk PASSED
tools/test_import_qe_research.py::test_real_research_file_round_trip PASSED

============================== 13 passed in 0.40s ==============================
```

Re-ran the real-file transform to `/tmp` to confirm the summary line is
unchanged:

```
note: 1 OSC camera(s) excluded — research has no Bayer-split QE; generic_sony_imx_osc applies at runtime with a Process Console warning: ['atik-460ex-color']
wrote /tmp/.../shipped.json: 55 cameras, 96 filters
```

Confirmed all six real dual-NB canonical filters now ship Ha/SII before
OIII: `HaO3 ['Ha', 'OIII']`, `S2O3 ['SII', 'OIII']`, `L-eXtreme ['Ha',
'OIII']`, `L-eNhance ['Ha', 'OIII']`, `L-Ultimate ['Ha', 'OIII']`, `ALP-T
['Ha', 'OIII']`.

### Commit

`88f41e1` — `fix(tools): enforce Ha/SII-before-OIII order for product-mapped
canonical filters`, both trailer lines included.
