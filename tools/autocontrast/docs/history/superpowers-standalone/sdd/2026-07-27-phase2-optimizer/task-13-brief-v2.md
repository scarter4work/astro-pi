### Task 13 (v2): PJSR driver AND the real PixInsight executor

> **SUPERSEDES `task-13-brief.md`.** The original was titled "PJSR driver and the
> PixInsight executor" but its steps built only a driver that ran the sidecar's own
> NumPy search — PixInsight never executed a process. The user ruled that a real
> PixInsightExecutor is required: spec §2.2 says *"Production never optimizes against
> an approximation."* Read v1 for its `envOr`/STATUS_FILE conventions only.

**Files:**
- Create: `pixinsight/autocontrast_optimize.js`
- Modify: `src/autocontrast/sidecar.py` (add `reference_fingerprint` to `_op_analyze`)

---

## The architecture, settled

There is **no Python `PixInsightExecutor` class**, and you must not create one.
`executor.py`'s protocol is `apply(rgb, action) -> rgb` — synchronous, in-process.
The sidecar is spawned as a CHILD of PixInsight and cannot call back into its parent;
an `apply()`-shaped PI executor would need ~540 headless PI launches per run.

**The PixInsight executor IS this PJSR script.** Task 11 shipped spec §2.5's batched
protocol, and the script is the executor half of it:

```
optimize_begin  ──────────►   sidecar: measure baseline, plan batch 1
                ◄──────────   {session_id, baseline_distance, instructions, converged, iteration}
YOU apply N processes in PI, save N candidates
optimize_step   ──────────►   sidecar: measure all N, guardrail, score, prune
                ◄──────────   next batch  |  or  converged + recipe
```

## The instruction contract Task 11 actually shipped

Each instruction (`loop.py:_instruction`) is:

```
{instruction_id, branch_key, action_key, process, params, parent_path, candidate_path}
```

- `instruction_id` is `it{iteration}-br{branch}-ac{position}` — deterministic.
- `process` and `params` come from `Recipe.to_pixinsight_steps()`. Execute the named
  stock PixInsight process with those params, on `parent_path`, and save to
  `candidate_path`.
- Send results back as `produced: [{instruction_id, path}, ...]`.

Rules the sidecar enforces — obey them or you get loud errors:
- An `instruction_id` reported **twice** is an error naming both paths.
- An `instruction_id` the sidecar **never issued** is an error.
- An instruction **absent** from `produced` is treated as an executor failure for
  that candidate ONLY. If PixInsight cannot apply a process, simply omit it and let
  the sidecar log it — do not fabricate an output file, and do not abort the run.

## Three contract requirements you must get right

1. **`optimize_step` requires `image` on EVERY step**, and it must be the same path
   `optimize_begin` was given. The sidecar calls `loop.resume(path, proxy_path=image)`,
   which does an EXACT string comparison and refuses a mismatch. Do not normalize,
   resolve, or rebuild the path between calls — send the identical string.
2. **`pixel_scale_arcsec` must be the image's NATIVE on-sky scale.** `optimize_begin`
   applies the downsample correction itself. `analyze` reports `wcs.pixel_scale_arcsec`
   raw, so wiring `analyze`'s output straight through is CORRECT. A caller that
   pre-corrects would double-correct.
3. **`candidate_suffix`** is a request field defaulting to `.png`, validated at
   `optimize_begin` against `loaders.supported_suffixes()`. See the bit-depth section.

## Bit depth — a real quality decision, not a detail

The default `.png` is **8-bit**. Every candidate would make an 8-bit round trip
through disk. On astro data that is a genuine quality ceiling on the production
search — and it directly undercuts the reason a real PixInsight executor was
requested at all (§2.2: production must not optimize against an approximation).
An 8-bit intermediate IS an approximation.

Task 11 could not fix this: Pillow in this venv cannot write 16-bit RGB
(`TypeError: Cannot handle this data type: (1, 1, 3), <u2`), and `tifffile`,
`imageio`, and `cv2` are all absent.

**Your task owns it.** Add a 16-bit-capable reader as a project dependency (prefer
`tifffile` for 16-bit TIFF — smallest, most focused option), extend
`loaders.supported_suffixes()` and `load_image` so the new suffix is genuinely
readable, and have the script pass that suffix. Requirements:
- The suffix must round-trip: PixInsight writes it, `load_image` reads it back with
  the bit depth preserved. **Prove that with a test**, not an assertion.
- Extend the EXISTING derivation in `loaders.py`; do not add a parallel list. Task 11's
  fix (`ec6c662`) deliberately derived that set so it could not drift.
- If 16-bit turns out not to be achievable cleanly, **report it and ship `.png`** with
  the limitation stated plainly in your report and in the script's console output.
  A working 8-bit path that says so is acceptable; a silent 8-bit path is not.

## The script itself

Follow `pixinsight/autocontrast_analyze.js` conventions exactly: `envOr()` for every
path, `STATUS_FILE` reporting (headless PI writes nothing useful to stdout),
`ExternalProcess.execute` for the sidecar. Run form:
`PixInsight.sh -n --automation-mode -r=pixinsight/autocontrast_optimize.js --force-exit`

Behavior:
1. Resolve a reference for the field via the proven `analyze` op. Add
   `result["reference_fingerprint"] = best.record.fingerprint.to_dict()` to
   `_op_analyze` — the script needs the fingerprint dict and `analyze` does not
   currently return it.
2. `optimize_begin`, then loop: execute the batch's instructions in PixInsight, save
   candidates, `optimize_step`, repeat until `converged`.
3. Report every guardrail-discarded candidate with its reason, AND every `noted`
   entry (`attempt_cap`, "star integrity not assessed", …). §12 requires degraded
   paths surface — a `noted` entry that never reaches the console is the fix Task 3
   and Task 10 built being thrown away at the last step.
4. Write the recipe to `RECIPE_FILE`.
5. **When `improved` is false, say DECLINED plainly and state the file was not
   modified.** On an already-good image that is the CORRECT outcome, not a failure.

## Step: run it headlessly on a real print

```bash
mkdir -p /tmp/autocontrast_optimize
AUTOCONTRAST_IMAGE=/mnt/qnap/astro_data/prints/M42_hoo_2_18_23.jpg \
/opt/PixInsight/bin/PixInsight.sh -n --automation-mode \
  -r=pixinsight/autocontrast_optimize.js --force-exit
cat /tmp/autocontrast_optimize/status.txt
```

Expected: `PASS improved=false` — a finished print should be DECLINED. If it reports
`improved=true`, that is a **real finding, not a test to adjust**: record the recipe
it proposed and report it. Do not touch any threshold (§3.7).

Report the real wall-clock time of the run. The measured proxy cost is 2.54s per
candidate before PixInsight's own process time; the live number is unknown and
matters to Task 16's budget.

## Global constraints

- Branch `phase2-optimizer`. Do not commit to master. **Do not push.**
- No AI anywhere in this plan.
- Guardrail violations discard candidates; never a score term (§7).
- No test may be deselected to make the suite green.
- **No tunable may be adjusted** (§3.7).
- Every degraded path logs and surfaces (§12). No silent fallbacks.
- Sidecar response contract unchanged.
- Headless PI writes nothing useful to stdout — report via STATUS_FILE.

## Verification

- `.venv/bin/python -m pytest tests/test_sidecar.py tests/test_optimize_sidecar.py -v`
- `.venv/bin/python -m pytest -q` — full count, nothing deselected. Baseline 413.
- The headless run above, with its real `status.txt` contents pasted.

## Report

Write to `.superpowers/sdd/2026-07-27-phase2-optimizer/task-13-report.md` AS YOU GO.
Reply with only: status, commit SHA(s), test summary, the headless result, wall-clock,
and concerns.
