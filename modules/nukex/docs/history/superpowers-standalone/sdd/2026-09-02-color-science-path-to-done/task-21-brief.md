### Task 21: E2E — preserve the v4 golden bit-for-bit, add the v5 corpora

**What the April text got wrong.** The manifest is `schema_version: 1` with the three stretch sweeps *inside* the `lrgb_mono_ngc7635` case (`dropdown_sweep`); the golden `test/fixtures/golden/lrgb_mono_ngc7635.json` already holds `primary.{stacked,noise,stretched}` + `sweep.{GHS,MTF,ArcSinh}` hashes. Separate `sweep_*` cases do not exist. The spec's "M27 HaO3" corpus does not exist on disk; the dual-NB corpus is **M16 HaO3** (`~/projects/processing/M16`, 30 frames `*HaO3*`, ZWO ASI2400MC Pro, RGGB), mixed in one directory with 58 `LPro` frames. NGC7635 records filter-wheel *slot numbers* as `FILTER` (`'1'`…`'5'`), so it can only serve the luminance case. Real LRGB-mono with proper `FILTER` names is **M27 2025** (`/mnt/qnap/astro_data/9_1_2025/M27`, ATR585M, L24 R12 G12 B24). Plain OSC is **M27 2023** (`/mnt/qnap/astro_data/4_12_2023/M27`, 33 frames, ASI2400MC, no FILTER). The harness hashes `NukeX_stacked/noise/stretched` and does not check `filter_class_expected`.

**Files:**
- Modify: `tools/validate_e2e.js` (`collectLights`, `runPrimary` tags, golden shape)
- Modify: `test/fixtures/e2e_manifest.json`
- Create: `test/fixtures/golden/{bayer_rgb_m27_2023,bayer_nb_hao3_m16,mono_lrgb_m27_2025}.json` (by regen)

- [ ] **Step 0: Install the dev module + database into PixInsight (no dialog, no sudo)**

`/opt/PixInsight` is owned by the user, and `tools/run_e2e.sh` launches PixInsight with `--default-modules`, which rescans `bin/` and registers whatever signed module is there. PixInsight refuses unsigned modules, so sign first.

```bash
cd /home/scarter4work/projects/nukex5
printf '%s' '[REDACTED-PI-SIGNING-PASSWORD]' > /tmp/.pi_codesign_pass && chmod 600 /tmp/.pi_codesign_pass
cd build && make -j$(nproc) NukeX-pxm 2>&1 | grep -E "error" ; cd ..
tools/release.sh sign
cp build/src/module/NukeX-pxm.so build/src/module/NukeX-pxm.xsgn /opt/PixInsight/bin/
install -D -m 644 share/qe_database.json /opt/PixInsight/share/qe_database.json
ls -la /opt/PixInsight/bin/NukeX-pxm.so /opt/PixInsight/bin/NukeX-pxm.xsgn /opt/PixInsight/share/qe_database.json
```
Expected: `release.sh sign` prints `signed: …NukeX-pxm.so` and `…NukeX-pxm.xsgn`; the three installed files carry today's timestamp. (This is dev staging only; Task 23 proves the updater path delivers the same layout.)

- [ ] **Step 1: Verify the regression floor BEFORE any harness change**

Run: `cd build && make e2e 2>&1 | tail -15`
Expected: `lrgb_mono_ngc7635` primary + all three sweeps report golden match (bit-identical to v4.0.1.0). If any hash differs, STOP: the L-only path must be unchanged by v5 (spec §7.3). Find which commit since `0f1b8fe` moved it (bisect with `make e2e`) and fix before continuing. Do not regen.

- [ ] **Step 2: Harness — frame glob/limit and the composed window**

`tools/validate_e2e.js`:

```js
function collectLights(dir, glob, max_frames) {
   var pats = glob ? [glob] : ["*.fit", "*.fits", "*.FIT", "*.FITS"];
   var all = [];
   for (var i = 0; i < pats.length; i++) {
      var found = searchDirectory(dir + "/" + pats[i], false);
      for (var j = 0; j < found.length; j++) all.push(found[j]);
   }
   all.sort();
   if (max_frames && all.length > max_frames) all = all.slice(0, max_frames);
   return all;
}
```
Update both call sites to `collectLights(tc.light_dir, tc.light_glob, tc.max_frames)`. In `runPrimary`, `var tags = ["stacked", "noise", "stretched"];` → `["stacked", "noise", "stretched", "composed"]` (the loop already skips absent windows). In the golden builder (the function that assigns `out.stacked/noise/stretched` from `primary.pixel_hashes`) add `if (primary.pixel_hashes.composed) out.composed = primary.pixel_hashes.composed.fnv1a_hex;`. Update the header comment (steps 1–5) to mention `NukeX_composed`.

- [ ] **Step 3: Manifest**

Replace the three `*_placeholder` cases in `test/fixtures/e2e_manifest.json` with (keep `lrgb_mono_ngc7635` byte-for-byte):

```json
    {
      "name": "bayer_rgb_m27_2023",
      "filter_class_expected": "BROADBAND_OSC",
      "light_dir": "/mnt/qnap/astro_data/4_12_2023/M27",
      "flat_dir": "",
      "primary_stretch": 0,
      "primary_stretch_label": "Auto",
      "finishing_stretch": 0,
      "finishing_stretch_label": "None",
      "wall_time_budget_s": 1500,
      "min_frames_ok_alignment": 30
    },
    {
      "name": "bayer_nb_hao3_m16",
      "filter_class_expected": "DUAL_NB_OSC",
      "light_dir": "/home/scarter4work/projects/processing/M16",
      "light_glob": "*HaO3*.fit",
      "max_frames": 12,
      "flat_dir": "",
      "primary_stretch": 0,
      "primary_stretch_label": "Auto",
      "finishing_stretch": 0,
      "finishing_stretch_label": "None",
      "wall_time_budget_s": 1500,
      "min_frames_ok_alignment": 12,
      "visual_bar": "no green cast in the nebula; Ha red, OIII teal; NUKEX_QE_CONFIDENCE = database"
    },
    {
      "name": "mono_lrgb_m27_2025",
      "filter_class_expected": "BROADBAND_L + BROADBAND_RGB",
      "light_dir": "/mnt/qnap/astro_data/9_1_2025/M27",
      "flat_dir": "",
      "primary_stretch": 0,
      "primary_stretch_label": "Auto",
      "finishing_stretch": 0,
      "finishing_stretch_label": "None",
      "wall_time_budget_s": 1800,
      "min_frames_ok_alignment": 72
    },
    {
      "name": "bayer_nb_s2o3_placeholder",
      "skip": true,
      "skip_reason": "no S2O3 corpus on this machine"
    },
    {
      "name": "lrgbsho_placeholder",
      "skip": true,
      "skip_reason": "no mixed L+R+G+B+HaO3 corpus on this machine"
    }
```
Set `"schema_version": 2` and update `description` to `"v5 E2E corpus: preserved v4 LRGB-mono floor + OSC, dual-NB (M16 HaO3) and LRGB-mono (M27 2025) cases"`.

- [ ] **Step 4: Verify run (new cases unchecked), then regen**

Run: `cd build && make e2e 2>&1 | tail -25`
Expected: `lrgb_mono_ngc7635` still matches; the three new cases execute OK (`execute_ok`, `alignment_all_ok: true`, within budget) and report `golden_check.checked: false` (no golden yet). Any `executeGlobal false` is a real defect — read `build/e2e.log` for the module's message (typical: QE database missing → Step 0 was skipped; unknown camera → Task 15b regressed).

Then: `make e2e-regen 2>&1 | tail -10 && git status --short test/fixtures/golden`
Expected: three new golden files; `lrgb_mono_ngc7635.json` **unmodified** (`git diff --exit-code test/fixtures/golden/lrgb_mono_ngc7635.json` → exit 0). If it shows a diff, the regen has overwritten the floor — `git checkout` it and go back to Step 1.

- [ ] **Step 5: Verify against the new goldens once more**

Run: `cd build && make e2e 2>&1 | grep -E "golden|status" | head -20`
Expected: four cases `golden match`, two skipped.

- [ ] **Step 6: Commit**

```bash
git add tools/validate_e2e.js test/fixtures/e2e_manifest.json test/fixtures/golden
git commit -m "$(cat <<'EOF'
test(e2e): v5 corpora (OSC M27-2023, dual-NB M16 HaO3, LRGB-mono M27-2025); v4 floor preserved

lrgb_mono_ngc7635 (primary + GHS/MTF/ArcSinh sweeps) is byte-identical
to the v4.0.1.0 golden: the L-only path is untouched by the overhaul.
New goldens hash NukeX_stacked/noise/stretched/composed. The harness
gains light_glob + max_frames so the M16 directory's HaO3 frames can be
selected apart from its LPro frames. The spec's "M27 HaO3" set does not
exist on disk; M16 HaO3 is the dual-NB motivating corpus.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01NqDedRCQxJy8EbbzWDKMFR
EOF
)"
```

---

