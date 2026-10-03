# SDD ledger — plan: docs/superpowers/plans/2026-09-02-color-science-path-to-done.md

Spec: docs/superpowers/specs/2026-04-26-color-science-overhaul-design.md (read; binding authority).
Branch: v5-color-science (from main @ 0f1b8fe; plan committed as ddc415b).
Workspace: this directory. Build dir: build/ (clean rebuild 2026-09-02, 66/66).

## Pre-flight rulings

- Ruling: implement on branch `v5-color-science` in the existing checkout, not a separate worktree — the build dir (5+ min clean build), the PixInsight install steps and the E2E harness all key off `build/` in this checkout; main stays untouched and is merged at finish — cost if wrong: none to main; a stray uncommitted edit could bleed between tasks (mitigated: each task commits and the tree is checked clean before each dispatch).
- Ruling: Wave 1 tasks (14, 14b, 15, 15b) run SEQUENTIALLY, not as parallel subagents as the plan suggests — the SDD skill forbids parallel implementers in one tree (shared git index + build dir); parallel worktrees would cost four clean builds plus a 4-way merge — cost if wrong: wall-clock only.
- Order chosen: 14 → 15 → 15b → 14b → 16 → 17 → 18 → 19 → 20 → 21 → 22 → 23 → 24.

## Pre-flight conflict scan

| Pair / task | Produces vs consumes | Finding |
|---|---|---|
| 14 ↔ 19 | 14 leaves module filter_classifier/fits_metadata compiled, relinks testlib with nukex4_io; 19 deletes sources + testlib lines | consistent, sequential |
| 14 ↔ 14b | 14's auto-selector test expects mono FILTER='5' → BROADBAND_L + "Unknown filter '5'"; 14b changes only known broadband names | consistent |
| 14b ↔ 17 | both edit filter_classifier.cpp (table vs warning string); 14's test asserts substring "Unknown filter '5'", 17's new text keeps that prefix | consistent, sequential |
| 15 ↔ 16 | script output counts 56 cameras / 96 filters; 16 asserts the same | consistent |
| 15b ↔ 16 | resolve_camera used by 16's probe; NUKEX_QE_CONFIDENCE written by 15b, documented in 17, read in 22 | consistent |
| 15b ↔ 20 | 20's generic-fallback integration test needs ExecuteResult::qe_generic_camera_fallback + fixture generic camera (both from 15b) | consistent, 15b before 20 |
| 15b ↔ 17 | both edit stacking_engine.cpp (lines ~349 vs ~199) | disjoint regions, sequential |
| 18 ↔ 20 | 20's tests use channel_config.slot_index; 18 removes only mode/from_mode/output_rgb_mapping/is_mono | consistent |
| 19 ↔ 20 | both edit test/CMakeLists.txt (testlib block vs test_util block) | disjoint blocks, sequential |
| 16 ↔ 23 | release.sh tars share/; 23 creates repository/updates.xri that release.sh requires | consistent |
| 14 self | test asserts n_bb_l=1,n_bb_osc=1,n_nb_single=2 vs CASE 0→1,1→3,2→4,3→4 | consistent; ctest 66→67 |
| 15 self | BASE fixture now carries L-Ultimate, ALP-T, OIII, SII sources; HaO3 median of 7,3,3,5 = 4.0 | consistent (fixed in self-review) |
| 15b self | ctest binary count unchanged (cases added inside test_qe_database) | consistent |
| 20 self | writer signature has trailing `instrume = ""` so the six-arg calls in existing integration bodies compile | consistent; ctest +1 |
| 19 self | ctest −2 (test_fits_metadata, test_filter_classifier) | consistent; net 66 at end of wave 2 |
| Rubric check | no task mandates an assertion-free test or a duplicated logic block | clean |

## Task log
- Task 14: implementer BLOCKED mid-task on a verified ODR collision: old `src/module/filter_classifier.cpp` and new `src/lib/core/src/filter.cpp` both define `nukex::filter_class_name(nukex::FilterClass)` → identical mangled symbol; the old body wins in any link holding both (test_stretch_auto_selector 6/7 failing; production NukeX-pxm.so would log old class names). Ruling: (a) split `nukex4_module_testlib` into a legacy OBJECT lib (fits_metadata.cpp + filter_classifier.cpp, for the two old tests only) and the migrated lib (stretch_auto_selector.cpp + stretch_factory.cpp, links nukex4_io); (b) drop `filter_classifier.cpp` from `MODULE_SOURCES` now, files stay on disk for Task 19 — because leaving a reproduced production log-text defect live until Task 19 has no upside, and (b) is a one-line source-list edit with no callers lost — cost if wrong: none functional; Task 19 must also delete the legacy testlib (carry into its dispatch). Pre-flight scan row "Two enums coexist … harmless" was WRONG; corrected here.
- Task 14: implementer DONE (commit c14ec23; 67/67; ODR fix verified by nm/objdump on test binary and NukeX-pxm.so). Review package review-ddc415b..c14ec23.diff; task reviewer dispatched (sonnet).
- Task 14: review spec ✅, no Critical/Important. ⚠️ commit trailers verified by controller (`git log -1 --format=%B c14ec23`): both present.
- Task 14: minor (deferred): src/module/CMakeLists.txt still compiles fits_metadata.cpp (dead in module after this task; Task 19 deletes it).
- Task 14: complete (commits ddc415b..c14ec23, review clean)
- Task 15: BASE c14ec23; implementer dispatched (sonnet).
- Task 14: minor (deferred): RatingDialog/stretch_auto_selector/stretch_factory use 4-space no-spaced-parens style, pre-existing; diff matched local convention (informational, no action). Reviewer Assessment: Approved.
- Task 15: implementer NEEDS_CONTEXT: research camera `atik-460ex-color` (ICX694 OSC) has only a flat "mono" QE curve, no R/G/B; 6 sensors key mono QE as "mono" not "mono_pk". Ruling: (1) OSC cameras with Bayer-split QE at zero wavelengths are EXCLUDED from the shipped DB with a stderr note (spec 6.3 generic fallback covers them at runtime, loudly); partial R/G/B coverage still fails loud; no name special-casing — because inventing R=G=B from a mono curve is fabricated QE and shipping an empty block would surface as a misleading SingularQError; (2) "mono" is accepted as an alias of "mono_pk" — real data under drifted vocabulary. Cost if wrong: Atik 460EX Colour users get generic Sony QE + warning instead of camera-specific values; follow-up = add real ICX694 colour QE to the research file. Downstream corrections: real-file count is now 55 cameras (54 + generic), 96 filters → carry into Task 16 (expected counts) and Task 23 (CHANGELOG "54 cameras plus a generic").
- Task 15: implementer DONE (commit b00ad25; 12/12 pytest; real file → 55 cameras, 96 filters, atik-460ex-color excluded with note; mono alias recovered 8 mono cameras' QE). Review package review-c14ec23..b00ad25.diff; reviewer dispatched (sonnet).
- Task 15: review spec ❌ (1 Important): product-mapped canonical filters (L-eXtreme/L-eNhance/L-Ultimate/ALP-T) not sorted Ha/SII-before-OIII — correct only by source order. ⚠️ RED evidence for the two ruling-driven tests not shown — Ruling: evidence-trail gap only, tests exist and reviewer reproduced 12/12 + real-file run independently; not a spec gap. Fix round 1/5 dispatched to impl-task-15 (FIX_BASE b00ad25).
- Task 15: minor (deferred): `sites.get("mono_pk", sites.get("mono"))` prefers mono_pk when a sensor carries both keys — undocumented, untested, does not occur in today's data.
- Task 15: minor (deferred): no RED run shown for the two ruling-added tests (evidence trail only).
- Task 15: fix round 1/5 (1 addressed pending re-review, 0 open — ordered_for_q_solve() applied to both canonical paths + order test; commit 88f41e1; 13/13). Scoped re-review dispatched (haiku) on review-b00ad25..88f41e1.diff.
- Task 15: fix round 1/5 re-review: 1 addressed, 0 open, no new breakage.
- Task 15: complete (commits c14ec23..88f41e1, review clean after 1 fix round)
- Task 15b: BASE 88f41e1; implementer dispatched (sonnet).
- Task 15b: implementer DONE (commit d1a2daa; 67/67). Review package review-88f41e1..d1a2daa.diff; reviewer dispatched (sonnet).
- Task 15b: review spec ✅ but 2 Important, both plan-mandated (brief reference code): (1) resolve_camera equal-length tie-break depends on unordered_map order; (2) same-document raw keys that normalise identically silently overwrite. Ruling: fix both — the spec's reproducibility premise and the user's "no silent fallbacks" rule outrank the plan's reference code; tie-break = lexicographically smaller key; same-load collision = LoadResult error naming both raw keys, document not half-applied; cross-load override-wins unchanged — cost if wrong: none functional, one more fixture pair. Fix round 1/5 dispatched to impl-task-15b (FIX_BASE d1a2daa).
- Task 15b: minor (deferred): ExecuteResult new field not column-aligned with siblings; console warning text says "(NUKEX_QE_CONFIDENCE)" where spec says "in FITS keywords" (brief drift); mixed-camera guard error now lists resolved keys not raw INSTRUME (intentional).
- Task 15b: fix round 1/5 (2 addressed pending re-review, 0 open; commit 78c08d0; 67/67). Scoped re-review dispatched (sonnet) on review-d1a2daa..78c08d0.diff.
- Task 15b: fix round 1/5 re-review: 2 addressed, 0 open, no new breakage. Out-of-scope (deferred minor): `seen->second != name` guard in parse_and_merge is dead-but-safe (JSON keys are unique per object).
- Task 15b: complete (commits 88f41e1..78c08d0, review clean after 1 fix round)
- Task 14b: BASE 78c08d0; implementer dispatched (haiku).
- Task 14b: implementer DONE (commit 7b33e6c; 67/67; classifier 11/11). Review package review-78c08d0..7b33e6c.diff; reviewer dispatched (haiku).
- Task 14b: review spec ✅, no findings, Approved.
- Task 14b: complete (commits 78c08d0..7b33e6c, review clean)
- WAVE 1 COMPLETE. Task 16: BASE 7b33e6c; implementer dispatched (sonnet). Briefs 16 and 23 patched for the 55-camera count.
- Task 16: implementer DONE_WITH_CONCERNS (commit 0ef2511; 67/67). Concern: share/qe_database.json = 44,225 B vs plan's "60-200 KB" — Ruling: plan estimate was a guess; 4 wavelengths × 55 cameras explains the size; sanity assertions + probe matched — no gap. Review package review-7b33e6c..0ef2511.diff; reviewer dispatched (sonnet).
- Task 16: review spec ✅, no Critical/Important; Approved. ⚠️ ctest claim taken from report (controller saw 67/67 in implementer output).
- Task 16: complete (commits 7b33e6c..0ef2511, review clean)
- Task 17: BASE 0ef2511; implementer dispatched (haiku). Extra site found by controller grep: stacking_engine.cpp:332 per-frame skip message also says "add to qe_overrides.json" — included in dispatch.
- Task 17: implementer DONE (commit 2c6d1ec; 67/67). Review package review-0ef2511..2c6d1ec.diff; reviewer dispatched (haiku).
- Task 17: review spec ✅, no findings, Approved.
- Task 17: complete (commits 0ef2511..2c6d1ec, review clean)
- Task 18: BASE 2c6d1ec; implementer dispatched (sonnet).
- Task 18: implementer DONE (commit 8bcdb29; 67/67; dead-symbol grep empty). Review package review-2c6d1ec..8bcdb29.diff; reviewer dispatched (haiku).
- Task 18: review spec ✅, no findings, Approved.
- Task 18: complete (commits 2c6d1ec..8bcdb29, review clean)
- Task 19: BASE 8bcdb29; implementer dispatched (haiku). Carries Task 14 ruling: delete nukex4_module_legacy_testlib + module CMake ODR comment block.
- Task 19: implementer DONE (commit 35f8fd1; 65/65). Review package review-8bcdb29..35f8fd1.diff; reviewer dispatched (haiku).
- Task 19: controller grep's one remaining CMake hit is `nukex_add_test(test_filter_classifier_io unit/io/test_filter_classifier.cpp …)` — the lib-level classifier test, legitimate, not a leftover.
- Task 19: review spec ✅, no findings, Approved (grep hit confirmed as the lib io test).
- Task 19: complete (commits 8bcdb29..35f8fd1, review clean)
- Task 20: BASE 35f8fd1; implementer dispatched (sonnet). ctest baseline now 65; expect 66 after.
- Ruling (Tasks 21/22): dev module install = `tools/release.sh sign` + `cp NukeX-pxm.{so,xsgn} /opt/PixInsight/bin/` + `install share/qe_database.json /opt/PixInsight/share/` — no PixInsight dialog, no sudo (/opt/PixInsight is user-owned; run_e2e.sh launches PI with --default-modules which rescans bin/). Plan's dialog-based Step 0 replaced in the Task 21 brief. Cost if wrong: none (same mechanism v4 E2E used per CMakeLists comment "sign + copy"). Also: run_e2e.sh wipes /tmp/nukex_e2e at start → Task 22 must copy v5 outputs aside before the v4-baseline run.
- Task 20: implementer BLOCKED after committing its own deliverables (04cdde3; ctest 66/66; phase_a 5/5; phase_b 1 pass / 3 fail / 1 skip). Root cause (verified by implementer probe n=1→0, n=2→0, n=3→ok): KDEFitter::fit returns a zeroed FitResult for n<3 and ModelSelector::select copies it → pixel_selector emits 0.0 → any 1–2-frame channel stacks to zero silently. Ruling: this is a pre-existing production defect (silent zeros, violates no-silent-fallback), fix at the root NOW as inserted Task 20b — KDEFitter n<3 returns the robust location (sample / biweight of pair) with converged=false so FIT_FAILED still flags — rather than padding the tests to ≥3 frames; cost if wrong: the FIT_FAILED flag has no downstream consumer today so the only behaviour change is zero → data value for tiny stacks. Brief written at task-20b-brief.md (controller-authored; not in the plan file).
- Task 20: ruling on the still-gated "negative emission clamped" case: no new writer needed — engineer the negative line through the existing writer: write_synthetic_q_solved_hao3(tmp,16,16,"ASI585MC", 0.8f, -0.04f) keeps every ASI585MC photosite ≥ 0 (B = 0.03·0.8 − 0.50·0.04 = 0.004) while the solve recovers OIII = −0.04 → clamped, counter > 0. To be applied when impl-task-20 resumes after 20b.
- Task 20b: BASE = HEAD after 04cdde3; implementer dispatch next.
- Task 20b: implementer DONE (commit 700d3a0; 66/66; phase_a 5/5; phase_b 4 pass/1 skip; GPU path has no own fit — no mirror needed). Review package review-04cdde3..700d3a0.diff; reviewer dispatched (sonnet). impl-task-20 resumed in parallel (read-only reviewer + one implementer) to un-gate the negative-emission case.
- Task 20: resumed implementer DONE (commit 1a633d3; ctest 66/66; phase_a 5/5; phase_b 5/5, no gating left). Review packages review-35f8fd1..04cdde3.diff + review-700d3a0..1a633d3.diff; reviewer dispatched (sonnet).
- Task 21 pre-check (controller): corpora verified homogeneous — 4_12_2023/M27: 33 lights ASI2400MC RGGB no FILTER; M16 first-12 *HaO3*: 12 lights ASI2400MC HaO3 RGGB; 9_1_2025/M27: 72 lights ATR585M L24/R12/G12/B24 mono.
- Task 20b: review spec ✅, no Critical/Important; GPU parity independently verified (no GPU-side fit). Minor (deferred): n=0 emits estimate 0 with FIT_FAILED — honest and unreachable (engine returns before Phase B on zero frames). Assessment line requested (truncated in transit).
- Task 20b: review Approved. complete (commits 04cdde3..700d3a0, review clean)
- Task 20: review spec ✅ both packages, Approved. minor (deferred): stale file-header bullet in test_phase_b_qsolve.cpp still says the negative case is gated; write_q_solved hardcodes "RGGB" (fixture cameras are all RGGB).
- Task 20: complete (commits 35f8fd1..04cdde3 + 700d3a0..1a633d3, review clean)
- WAVE 2 COMPLETE (16, 17, 18, 19, 20, 20b). ctest 66/66; integration 10/10 opt-in.
- Task 21: BASE 1a633d3; implementer dispatched (sonnet). Harness runs must be backgrounded (10-min foreground cap vs ~20-40 min per make e2e).
- Task 21: implementer BLOCKED at Step 1 (floor gate): PixInsight SIGABRT on NGC7635 frame 1/65. Root cause (verified by controller read of stacking_engine.cpp ~521-545): mono routing branch looks the slot up by frame_filter.name; from_filter() registers BROADBAND_L as "L" regardless of name → "1" (unknown FILTER on mono) AND "L_unnamed" (no FILTER on mono) both abort — a Task-9-era engine defect never exercised because E2E never ran on the v5 branch. Ruling: fix at the root NOW as inserted Task 21b — route by per_frame_cfg.channel_names[0] (the registration mapping), not by changing the classifier's spec-conformant naming; two integration cases pin both spec 6.3 rows — cost if wrong: none functional (registration and lookup now share one source). Step 0's install stands; Task 21 resumes after 21b with a re-sign + re-copy of the fixed build. Brief written at task-21b-brief.md (controller-authored).
- Task 21b: BASE 1a633d3; implementer dispatch next.
- Task 21b: implementer DONE (commit 3c059c9; 66/66; phase_a 7/7; phase_b 5/5; RED = SIGABRT exit 134). Review package review-1a633d3..3c059c9.diff; reviewer dispatched (haiku). Task 21 resumed in parallel (re-sign + re-copy, then Step 1 again).
- Task 21b: review spec ✅, no findings, Approved. complete (commits 1a633d3..3c059c9, review clean)
- Task 21: resumed; floor re-run in progress in background (dev module re-signed + re-copied from 3c059c9 build).
