# Task 6 report — install script, README, full-suite verification

## Status: DONE

## What was done

1. **`install.sh`** — created exactly per the brief (prints script dir, the
   3-step Feature Scripts registration procedure, and whether `rc-astro` is on
   `PATH`). `chmod +x` applied. Verified by running it directly:

   ```
   Scripts live in: /home/scarter4work/PixInsightScripts/RC-Astro

   One-time registration in PixInsight:
     1. SCRIPT menu > Feature Scripts...
     2. Add  ->  select: /home/scarter4work/PixInsightScripts/RC-Astro
     3. Done. Then find them under:  Scripts > RC-Astro > *(CLI)

   rc-astro binary: /usr/local/bin/rc-astro
   ```

2. **`README.md`** — created covering: purpose (Blackwell/sm_120 TensorFlow
   JIT-compile RAM-exhaustion crash in the compiled plugins, replaced by the
   ONNX Runtime `rc-astro` CLI at ~2.7 s/panel on GPU); the three tools with
   their full parameter/flag tables (drawn directly from `RCAstroBXT.js`,
   `RCAstroSXT.js`, `RCAstroNXT.js`) and their `Scripts > RC-Astro > *(CLI)`
   menu location; output behavior (BXT/NXT modify the target view in place and
   are undoable; SXT never touches the original and creates `<id>_starless` +
   optional `<id>_stars` as new windows); error-handling policy (loud
   MessageBox + console, no silent fallback); install steps referencing
   `install.sh` + the Feature Scripts UI step; headless test run commands;
   the cuDNN >= 9.13 GPU requirement (this box: 9.24, below 9.13 falls back to
   CPU silently); and a link to
   `docs/2026-07-11-rc-astro-pi-wrappers-design.md`.

3. **Full five-test headless suite** — run per the brief's Step 3 loop,
   unmodified, against the existing (untouched) Tasks 1-5 code. All five
   genuinely passed. Verbatim suite output:

   ```
   == t_lib_roundtrip OK ==
   == t_lib_runcli OK ==
   == t_bxt OK ==
   == t_sxt OK ==
   == t_nxt OK ==
   ```

   Verbatim contents of each result log (for the record):

   ```
   --- /tmp/rc_t1_result.log ---
   binary: /usr/local/bin/rc-astro
   PASS t_lib_roundtrip

   --- /tmp/rc_t2_result.log ---
   progress events seen: true
   device used: gpu (NVIDIA GeForce RTX 5070 Ti)
   negative-path errorMsg: terminate called after throwing an instance of 'rcastro::Error'
   what():  '/home/scarter4work/pixinsight-swap/rc-astro-1783811169617-315214/not_really_xisf.xisf' is not a valid XISF file (bad signature)
   PASS t_lib_runcli

   --- /tmp/rc_t3_result.log ---
   argv[default ss/sn, autoPSF=true]: ["IN.xisf","--ss","0.250","--sn","0.900","--ash","0.100","--ansr","--device","gpu","--output","OUT.xisf"]
   argv[autoPSF=false, psfRadius=1.5]: ["IN.xisf","--ss","0.250","--sn","0.900","--ash","0.100","--no-ansr","--nsr","1.50","--device","gpu","--output","OUT.xisf"]
   argv[correctOnly=true]: ["IN.xisf","--correct-only","--ash","0.100","--no-ansr","--nsr","1.50","--device","gpu","--output","OUT.xisf"]
   argv[mlVersion=4]: ["IN.xisf","--ss","0.250","--sn","0.900","--ash","0.100","--ansr","--ml-version","4","--device","gpu","--output","OUT.xisf"]
   PASS t_bxt

   --- /tmp/rc_t4_result.log ---
   wrote: /home/scarter4work/pixinsight-swap/rc-astro-1783811187264-439048/image.xisf
   wrote: /home/scarter4work/pixinsight-swap/rc-astro-1783811187264-439048/image_starless.xisf
   wrote: /home/scarter4work/pixinsight-swap/rc-astro-1783811187264-439048/image_starless-stars.xisf
   argv[outputStars=false]: ["IN.xisf","--device","gpu","--output","OUT.xisf"]
   argv[outputStars=true, unscreen=true]: ["IN.xisf","--output-stars","--unscreen","--device","gpu","--output","OUT.xisf"]
   argv[mlVersion=11]: ["IN.xisf","--output-stars","--unscreen","--ml-version","11","--device","gpu","--output","OUT.xisf"]
   PASS t_sxt

   --- /tmp/rc_t5_result.log ---
   argv[useAdvanced=false]: ["IN.xisf","--dn","0.900","--it","2","--fs","5.00","--device","gpu","--output","OUT.xisf"]
   argv[useAdvanced=true]: ["IN.xisf","--dn","0.900","--it","2","--fs","5.00","--di","0.500","--dc","0.600","--dhf","0.700","--dlf","0.800","--dihf","0.100","--dilf","0.200","--dchf","0.300","--dclf","0.400","--device","gpu","--output","OUT.xisf"]
   argv[mlVersion=3]: ["IN.xisf","--dn","0.900","--it","2","--fs","5.00","--di","0.500","--dc","0.600","--dhf","0.700","--dlf","0.800","--dihf","0.100","--dilf","0.200","--dchf","0.300","--dclf","0.400","--ml-version","3","--device","gpu","--output","OUT.xisf"]
   PASS t_nxt
   ```

   Note: the "negative-path errorMsg" line in `t_lib_runcli`'s log is an
   intentionally-exercised failure path *within* that test (feeding a bad
   XISF file to confirm `runCli` surfaces the real C++ exception text) — the
   test itself still asserts and reports `PASS`. It is not a suite failure.

## Not done / explicitly deferred

- **Step 4 (manual interactive registration + UI check)** is manual-only per
  the brief ("manual, once") and was not performed as part of this automated
  task — it requires opening the PixInsight GUI interactively, which is
  outside what this agent can drive. `install.sh` output above confirms the
  registration instructions are correct and `rc-astro` is discoverable;
  someone with hands on the PI GUI still needs to do the one-time Feature
  Scripts `Add` and the three dialog-open/Undo/error-MessageBox checks.

## Files touched

- Added `/home/scarter4work/PixInsightScripts/RC-Astro/install.sh` (new)
- Added `/home/scarter4work/PixInsightScripts/RC-Astro/README.md` (new)
- No existing file (`RCAstroLib.jsh`, `RCAstroBXT.js`, `RCAstroSXT.js`,
  `RCAstroNXT.js`, any `test/*`) was modified.

## Commit

Committed on `feat/rc-astro-pi-wrappers` per Step 5 of the brief.
