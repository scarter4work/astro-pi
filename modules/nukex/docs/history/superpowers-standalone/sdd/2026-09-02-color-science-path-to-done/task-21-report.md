# Task 21 report — BLOCKED at Step 1 (regression floor gate)

Branch: `v5-color-science`, HEAD at start of task: `1a633d3` (unchanged — no commits made).

## What I implemented

- **Step 0 only.** Built `NukeX-pxm`, signed it, and installed the dev module + QE
  database into `/opt/PixInsight` exactly per the brief. This succeeded cleanly.
- **Step 1 (verify the v4.0.1.0 floor BEFORE any harness change) FAILED** — not
  with a golden hash mismatch, but with a hard process abort. Per my
  instructions this is the most important gate in the task; I stopped there,
  made no harness/manifest/golden edits, and did not proceed to Steps 2–6.

## Step 0 — build, sign, install

```
cd /home/scarter4work/projects/nukex5
printf '%s' '[REDACTED-PI-SIGNING-PASSWORD]' > /tmp/.pi_codesign_pass && chmod 600 /tmp/.pi_codesign_pass
cd build && make -j$(nproc) NukeX-pxm 2>&1 | grep -E "error" ; cd ..
tools/release.sh sign
cp build/src/module/NukeX-pxm.so build/src/module/NukeX-pxm.xsgn /opt/PixInsight/bin/
install -D -m 644 share/qe_database.json /opt/PixInsight/share/qe_database.json
```

Build: no errors, `NukeX-pxm.so` linked cleanly.
`tools/release.sh sign` output:
```
=== Signing module ===
signed: /home/scarter4work/projects/nukex5/build/src/module/NukeX-pxm.so
        /home/scarter4work/projects/nukex5/build/src/module/NukeX-pxm.xsgn
```
Installed files, all timestamped today (2026-09-02 22:45):
```
-rwxr-xr-x 1 scarter4work scarter4work 9911632 Sep  2 22:45 /opt/PixInsight/bin/NukeX-pxm.so
-rw-r--r-- 1 scarter4work scarter4work     704 Sep  2 22:45 /opt/PixInsight/bin/NukeX-pxm.xsgn
-rw-r--r-- 1 scarter4work scarter4work   44225 Sep  2 22:45 /opt/PixInsight/share/qe_database.json
```

Prerequisites verified before starting: `DISPLAY=:0` set; no PixInsight process
running; `/opt/PixInsight/share/` present and empty before install; all three
target corpora present on disk (`4_12_2023/M27` 33 frames, `M16` HaO3+LPro
mixed, `9_1_2025/M27` L/R/G/B ATR585M frames).

## Step 1 — floor verification (FAILED: crash, not a hash mismatch)

Command (backgrounded per instructions, `run_in_background: true`):
```
cd build && make e2e
```
Wall time to failure: **~11 seconds** (started 22:45:38, core dumped
22:45:49) — this is not a slow-running pass; it fails on the very first
frame of the very first case.

Harness output (`build/e2e.log`, verbatim tail):
```
[2026-09-02 22:45:38.646] MESSAGE NukeX v4 — Distribution-Fitted Stacking
[2026-09-02 22:45:38.646] MESSAGE Processing 65 light frame(s)
[2026-09-02 22:45:38.835] MESSAGE Unknown filter '1' for mono frame -- treating as generic luminance. If this is a narrowband filter, rename FILTER to Ha/OIII/SII or add it to a qe_overrides.json selected in the NukeX interface.
[2026-09-02 22:45:40.132] MESSAGE Frames: 65 light
[2026-09-02 22:45:40.134] PHASE_BEGIN Phase A: Loading frames (65 steps)
[2026-09-02 22:45:40.135] PROGRESS 0/65 — Frame 1/65: L_1_Lights_5826_Bin1x1_200s_-5C.fit
[2026-09-02 22:45:40.135] PROGRESS 0/65 —   Unknown filter '1' for mono frame -- treating as generic luminance. ...
[2026-09-02 22:45:40.137] PROGRESS 0/65 —   aligning
[2026-09-02 22:45:40.157] PROGRESS 0/65 —   aligned: ok (stars=200, inliers=200, rms=0.000 px)
[2026-09-02 22:45:40.158] PROGRESS 0/65 —   caching
[2026-09-02 22:45:40.377] PROGRESS 0/65 —   accumulating
Received signal 6
#0 0x7f03a0da24a9 base::debug::CollectStackTrace()
...
#7 0x7f012380599d <unknown>
#8 0x7f01238650e8 <unknown>
#9 0x7f01239700ce <unknown>
#10 0x563aaf54046c pi::MetaProcess::ExecuteGlobal()
#11 0x563aaf5a8051 pi::ProcessInstance::ExecuteGlobal()
#12 0x563aaf20e87a pi::Instance_executeGlobal()
...
/opt/PixInsight/bin/PixInsight.sh: line 46: 1029742 Aborted (core dumped) /opt/PixInsight/bin/PixInsight --automation-mode --force-exit --default-modules -r=.../tools/validate_e2e.js,manifest=.../test/fixtures/e2e_manifest.json
```
Exit code from `make e2e`: 2 (propagated from the `timeout`-wrapped
`PixInsight.sh`, aborted).

`/tmp/nukex_e2e_meta.txt` and `/tmp/nukex_e2e_console.log` were **never
written** — the crash is a hard `SIGABRT` inside the module's C++ code
during `ExecuteGlobal`, which the PJSR harness's try/catch cannot intercept
(it isn't a JS exception; the whole process aborts). No pixel hash was ever
computed for `lrgb_mono_ngc7635`, so there is no golden diff to show — the
harness never got that far.

**No golden was regenerated. No golden file was touched.**

### Symbolized backtrace (via `coredumpctl` + `gdb`, read-only diagnostics)

A core dump was captured (`coredumpctl list` → `SIGABRT ... /opt/PixInsight/bin/PixInsight`, 93.7M). Symbolizing frame 3 against `NukeX-pxm.so`:

```
#3  nukex::StackingEngine::execute(...) [clone .cold] () from NukeX-pxm.so
#4  pcl::NukeXInstance::ExecuteGlobal() () from NukeX-pxm.so
#5  pcl::ProcessContextDispatcher::ExecuteProcessGlobal(void*) () from NukeX-pxm.so
#6  pi::MetaProcess::ExecuteGlobal(void*) const ()
#7  pi::ProcessInstance::ExecuteGlobal() ()
```

### Root cause (read-only source investigation, no edits made)

`src/lib/stacker/src/stacking_engine.cpp:523-538` (the `BROADBAND_L` /
`NARROWBAND_SINGLE` / `BROADBAND_RGB` mono-accumulation branch):
```cpp
const std::string& slot_name = frame_filter.name;
int slot_idx = cube.channel_config.slot_index(slot_name);
if (slot_idx < 0) {
    std::abort(); // missing mono slot — dispatch-table bug
}
```

`src/lib/io/src/filter_classifier.cpp:131-141` — the "unknown FILTER value
on a mono frame" fallback (introduced in `efac5f2a`, "feat(io):
FilterClassifier with 5-class taxonomy + tiered unknown handling",
2026-04-27, **before** `0f1b8fe** and before the v4.0.1.0 golden was
captured on 2026-04-25 — i.e. the classifier itself postdates the golden
snapshot and is new machinery this v5 branch introduced):
```cpp
} else {
    out.cls       = FilterClass::BROADBAND_L;
    out.name      = meta.filter;              // <-- raw, unrecognized string
    out.bandwidth = BandwidthSpec{550.0, 300.0};
    last_warning_ = "Unknown filter '" + meta.filter + "' for mono frame -- "
                    "treating as generic luminance. ...";
}
```
NGC7635's FITS `FILTER` header is a filter-wheel *slot number* (`'1'`), not
a name, and slot numbers never match anything in `known_table()` or
`broadband_any_names()` — so this fallback fires, and `out.name` ends up
literally `"1"`.

But `src/lib/core/src/channel_config.cpp` (the `merge()` that builds the
slot table) registers the mono BROADBAND_L slot **only** under the
canonical literal `"L"`:
```
17:        case FilterClass::BROADBAND_L:
19:            cfg.channel_names[0] = "L";
```

So `StackingEngine::execute()` looks up `slot_index("1")`, gets `-1`
(the table only has `"L"`), and hits the deliberate `std::abort()` guard
that the code comments describe as catching a "dispatch-table
programmer error." This is exactly that — the classifier's fallback
path and `merge()`'s slot-naming convention disagree for any mono frame
with an unrecognized `FILTER` value, and NGC7635 is precisely that case.

`git log --oneline 0f1b8fe..HEAD -- src/lib/io/src/filter_classifier.cpp`
shows only two later touches to this file (`7b33e6c`, `2c6d1ec`, both
today, 2026-09-02) — neither touches the unknown-filter fallback branch
(lines 131-141); `7b33e6c`'s diff (inspected) only reworks the
*recognized*-broadband-name path (`broadband_any_names()`), not the
unrecognized-name fallback. So the bug has been latent since `efac5f2a`
(2026-04-27) and nothing in the v5 branch's later work exercised or fixed
this exact fallback combination until this E2E floor check hit it.

**This is not a hash regression the harness could "STOP and compare" on —
the module never survives to compute a hash.** Per my brief's Step 1
instruction I would normally bisect and fix; per my task's explicit
override ("If Step 1 ... already shows a golden mismatch ... STOP there
and report BLOCKED ... do not regen, do not proceed to Step 2") and the
"do not edit engine code in this task" instruction, I stopped here without
touching `stacking_engine.cpp`, `filter_classifier.cpp`, or
`channel_config.cpp`.

## Files changed

None in the repository. (`/opt/PixInsight/bin/NukeX-pxm.{so,xsgn}` and
`/opt/PixInsight/share/qe_database.json` were installed per Step 0 — these
are outside the git tree, dev-staging only, as the brief describes.)

## Self-review

- Completeness: N/A — blocked before Step 2 (harness edits never made).
- Evidence: crash log, symbolized backtrace, and root-cause source citations
  all captured above; PixInsight process confirmed exited (no dangling
  instance) after the abort.
- Discipline: no engine/module/harness/manifest/golden file was edited or
  regenerated. No subagent was spawned. No bisect across commits was
  performed (would require rebuilding at each candidate commit, which is a
  fix-hunting activity outside this task's scope per instructions).

## Issues / concerns

1. **Blocking regression**: any mono (non-Bayer) FITS light frame whose
   `FILTER` header value isn't in `filter_classifier.cpp`'s known tables
   (e.g. a raw filter-wheel slot number like NGC7635's `'1'`) now crashes
   the entire module with `SIGABRT`/`std::abort()` instead of stacking as
   generic luminance. This affects the very case (`lrgb_mono_ngc7635`) that
   is supposed to be the immutable v4 regression floor for this whole
   overhaul (spec §7.3), so the floor is currently **not intact**.
2. The fix is almost certainly a one-line change in
   `filter_classifier.cpp`'s unknown-mono-filter fallback: set
   `out.name = "L"` (matching `channel_config.cpp`'s registration) while
   keeping `meta.filter` only in the warning message text — but I did not
   make this change; it's an engine-code fix outside this task's scope.
3. Until that's fixed and Step 1 passes clean, Task 21 cannot proceed to
   Steps 2–6 (harness edits, manifest changes, new corpora, or the commit).
