# RC-Astro PI wrappers — progress ledger (branch feat/rc-astro-pi-wrappers)
Task 1: complete (commits f2ff78c..96ba84e, review clean — engine + headless harness)
  Minors deferred to final review:
   - RCAstroLib cleanup sweep is non-recursive (flat temp dirs by design; low risk)
   - Task-1 RED evidence only covered the #include-missing failure mode
   - loud-failure proof used uncommitted diag scripts (not re-verifiable from repo)
  Env facts discovered (now in plan Global Constraints): bare processEvents(),
   bare searchDirectory(), console.* not on stdout under --automation-mode,
   applyInPlace() takes ownership/closes result window.

Task 2: complete (commit 704a3fc — runCli + NDJSON parsing, GPU CLI integration test)
  RED: RCAstro.runCli is not a function (confirmed before implementation)
  GREEN: PASS t_lib_runcli in /tmp/rc_t2_result.log; progress events seen: true
  Note: brief's Step 4 stdout-grep for "Using gpu" does NOT work as written —
   console.* never reaches stdout under --automation-mode (Task 1 finding applies
   to runCli's own console.writeln too, not just findBinary's). GPU usage was
   instead verified with a one-off scratchpad script that captured the device
   NDJSON event directly via onEvent: {"device":"gpu","name":"NVIDIA GeForce RTX
   5070 Ti","provider":"CUDA"}, elapsed ~2.8s (matches expected ~3s GPU runtime,
   not a CPU-speed fallback). Script was not committed (throwaway verification).
Task 2: complete (commits 704a3fc..4714111, review clean — runCli + NDJSON, loud errors)
  Verified: GPU asserted in test (RTX 5070 Ti / CUDA); negative path returns REAL
   rc-astro reason ("not a valid XISF file (bad signature)"), not a generic code.
  Minors deferred to final review:
   - strayText accumulates non-JSON stdout for process lifetime (harmless; gated off success path)
   - a line starting with '{' that fails JSON.parse is silently dropped
  New env fact: for a SIGABRT-killed child, PJSR ExternalProcess routes the abort
   text through onStandardOutputDataAvailable (stdout), NOT stderr.
Task 3: complete (commit 1d6cfe2, review clean — BlurXTerminator (CLI))
  MAJOR CATCH: applyInPlace() was broken for ALL tools — PixelMath.SameAsTarget is
   undefined; must be PixelMath.prototype.SameAsTarget. Fixed in RCAstroLib.jsh.
   (Task 1's reviewer predicted this: applyInPlace had no test coverage.)
  Accepted deviation (controller): the engine fix is bundled in the BXT commit rather
   than split out. Reviewer called it "not a functional defect — bookkeeping"; an
   interactive rebase isn't available here and the gain is nil. Documented, not churned.
  Resolved minor: --ml-version 4/2 ARE valid (models downloaded: BlurXTerminator.4.onnx
   and .2.onnx). Open minor: --ash still emitted under --correct-only (unverified vs CLI).
  New env facts (now in plan): Label is a native global (no pjsr/Label.jsh — it does not
   exist); duplicate quoted #include of the same file silently kills the script.
Task 4: complete (commit eae2426, review clean — StarXTerminator (CLI))
  DISCOVERY PAID OFF: real stars file is "<base>-stars.xisf" (HYPHEN), not the
   "_stars.xisf" the plan assumed. Guessing would have silently produced no stars image.
  Minors deferred to final review:
   - RCAstroSXT.js:56 `if (outputStars && File.exists(starsP))` SILENTLY skips the stars
     window if the file is missing though CLI reported ok --> violates the standing
     "no silent fallbacks that hide real errors" rule. FIX IN FINAL WAVE.
   - no test asserts unscreen=true + outputStars=false emits no --unscreen (structural only)
   - repeat runs could collide on _starless/_stars window ids
Task 5: complete (commit 2b1f241, review clean — NoiseXTerminator (CLI))
  Implementer corrected the plan's BUGGY SectionBar draft (duplicate onToggleSection,
   direct .visible= write) to the canonical PI pattern (setSection + hide/show +
   isExpanded in the toggle-end handler); reviewer independently verified against
   SectionBar.jsh + AdP/AlignByCoordinates.js + AdP/MosaicPlanner.js. Correct.
  Minor deferred: the 8 advanced flags are argv-asserted but never round-tripped
   through a real rc-astro run (flag presence tested, not flag semantics).
Task 6: complete (commit 30d38d9, review clean — install.sh + README + full suite)
  FULL SUITE: all 5 headless tests PASS (t_lib_roundtrip, t_lib_runcli, t_bxt, t_sxt, t_nxt).
Fix wave: complete (commit 5b19c1f) — all accumulated Important + notable Minor findings:
  - PI menu is "Script" (SINGULAR), not "Scripts" — README + install.sh corrected (verified
    against PI's own ScriptCodeSigning doc + bundled scripts; nothing in /opt/PixInsight
    uses the plural for the top-level menu).
  - SXT no longer SILENTLY skips a requested-but-missing stars window -> RCAstro.fail (loud).
  - runCli no longer silently drops malformed JSON lines -> folded into strayText + logged.
  - README: SXT device row lists cpu; cuDNN >=9.13 claim now cited (NVIDIA release notes +
    observed CUDNN_FE HEURISTIC_QUERY_FAILED / smVersion:1200 symptom).
  Re-verified: t_lib_runcli PASS, t_sxt PASS.
STILL OPEN (manual, needs a human at the GUI): interactive dialog check — dialogs cannot
  run under --automation-mode, so all three dialogs are code-reviewed but never executed.
Fix waves B/C/D (fc24b4b, 5915c01, f309ade) — closed ALL whole-branch review findings.
  Real bugs the broad review caught that per-task gates could not:
   - CRITICAL: applyInPlace discarded PixelMath.executeOn()'s bool -> a FAILED apply
     silently reported success (a 12-panel mosaic would come back unprocessed + "OK").
   - saveAs RE-BOUND the user's real image window to the temp file, which cleanup then
     DELETED -> Ctrl+S on their original would have written to a dead path. (Confirmed
     empirically, then fixed with an off-screen copy.)
   - SXT's _starless/_stars windows had NO astrometric solution -> useless for mosaics.
   - BXT --correct-only was BROKEN: CLI hard-rejects --ash/--ansr/--nsr with it. The
     test even encoded the rejected argv and still passed. Now emits only --correct-only
     and is proven end-to-end against the real binary.
   - fail() popped a MODAL MessageBox -> would hang a headless pipeline forever.
   - preview targets threw a raw TypeError (View.mainView does not exist).
FINAL: READY TO MERGE (opus whole-branch reviewer, verified vs real rc-astro 0.9.10).
  All 5 headless tests PASS on real GPU runs.
Low-severity follow-ups (NOT blockers, deliberately deferred):
  - SXT isModified-skip path relies on PI auto-uniquifying a taken id; no committed test.
  - runCli timeout doesn't cover the isStarting loop.
  - RCAstro.timeoutSeconds not documented in README's headless section.
STILL REQUIRES A HUMAN: interactive GUI check (dialogs cannot run under --automation-mode).
