# SDD ledger — plan: docs/superpowers/plans/2026-07-27-phase2-optimizer.md

Branch: phase2-optimizer
Merge base: 9b3c44b (master)
Plan committed: a1bb239

Task 1: implementer DONE (commit b6c8169, 6 passed)
Task 1: review — spec OK, quality approved; 2 Important, 1 Minor, 1 warn
  - Important: Action.strength silent fallback on bad level (actions.py:42) -> fix round 1
  - Important: once-only test vacuous, no positive assertion -> fix round 1
  - Minor (deferred): Action.strength absent from brief's declared Produces list; reconcile in design doc
  - warn resolved by controller: Action cannot self-validate PSF limit (PSF unknown at construction;
    later tasks build bare Actions in tests). Invariant holds via available_actions construction path.
    NO code change. Downstream tasks must not assume Action self-validates.
Task 1: fix round 1/5 dispatched (resumed original implementer)
Task 1: fix round 1/5 (2 addressed, 0 open; commits b6c8169..d01489a)
Task 1: complete (commits 5501752..d01489a, review clean, 9 passed)
Task 2: implementer DONE (commit 657678a, 5 passed / 14 cumulative, no threshold adjustments)
Task 2: review — spec OK, quality approved; 1 Important, 1 Minor, 1 warn
  - Important: check_noise_floor fails OPEN when base sigma <= 0 (guardrails.py:65) -> fix round 1
  - Minor (deferred): _gray uses unweighted channel mean, not luminance weighting (guardrails.py:47).
    Accepted; Tasks 3-4 share the helper, consistency > weighting choice. Revisit only if it distorts a metric.
  - warn resolved by controller: exact float == in clipped_fraction is sound. Cross-task invariant —
    NumpyExecutor (Task 6) clips every output, io/loaders clips on load, so clipped pixels land exactly
    on 0.0/1.0. INVARIANT later tasks must preserve: every candidate reaching guardrails is np.clip'd.
Task 2: fix round 1/5 dispatched (resumed original implementer)
Task 2: fix round 1/5 (1 addressed, 0 open; commits 657678a..0088758)
Task 2: complete (commits d01489a..0088758, review clean, 6 passed)
Plan corrected: fail-open check_noise_floor synced to shipped code so re-runs can't reintroduce it.
Task 3: implementer DONE_WITH_CONCERNS (commit 801c2d6, 9 passed / 316 repo-wide)
  - fixture adjusted with justification: star count bound 30-40 -> 20-40, measured 28. Cause diagnosed
    (noiseless synthetic -> sigma 0 -> threshold ~0 -> adjacent stars merge into one component). ACCEPTED.
  - CONTROLLER RULING (base.count == 0 keeps its PASS): not a repeat of Task 2. Task 2's base<=0 was an
    assessment failure dressed as a pass; zero stars is a legitimate state. Design doc lines 268-269 are
    decisive — guardrails always run on the RECOMBINED image, never the starless layer, so the starless
    layer never reaches the guardrail. Remaining case is a genuinely starless input; fail-closed would
    block legitimate work. Defect is the SILENCE, not the pass -> fix round 1 makes the verdict say
    "not assessed" while still passing.
  - CARRY FORWARD TO TASK 10: passing verdicts with a non-empty reason must be surfaced in the run log,
    or this "not assessed" notice dies inside the verdict object and never reaches the user.
  - KNOWN PROPERTY (recorded, no fix): detect_stars degenerates on noiseless input — sigma 0 collapses
    the threshold to background+~0 and it detects everything above the median. Real astro data always
    has noise; low practical risk. On record for whoever tunes detection later.
Task 3: fix round 1/5 dispatched (resumed original implementer)
Task 3: fix round 1/5 complete (commit 5881ec8; 10 passed, 317 repo-wide, nothing deselected)
  Note: controller's mid-flight snapshot showed the edit uncommitted and untested and sent a resume
  message; the implementer had work in progress. Final state verified independently by controller:
  clean tree, both files in one commit, test asserts detect_stars(blank).count==0 before the verdict.
  No discrepancy in the shipped result.
Task 3: full review dispatched over 370ea8d..5881ec8 (both commits)
Task 3: review — spec OK, quality approved, NO Critical/Important. Both rulings verified implemented.
  - Minor (deferred): 3 blank lines before StarStats instead of PEP8's 2 (guardrails.py:123-125)
  - Minor (deferred): detect_stars degenerate noiseless mode documented in report + test comment but
    NOT in source (guardrails.py:133-139). detect_stars is reused by later tasks; a reader of
    guardrails.py alone won't know. Worth a one-line source comment. -> final review to triage.
Task 3: complete (commits 370ea8d..5881ec8, review clean, 10 passed)
Task 4: implementer DONE_WITH_CONCERNS (commit 3d35367, 14 passed)
  - implementer found + fixed a real BRIEF defect: reason string lacked "hue" while the brief's own test
    asserted it. Fixed the IMPLEMENTATION not the test (right direction). -> sync into plan doc at close.
Task 4: review — spec OK, quality NOT APPROVED. 1 Critical, 1 Minor.
  - CRITICAL: test_hue_invention_passes_when_color_only_intensifies is vacuous. Source and candidate map
    to the IDENTICAL bins ([8,8],[9,8]); mass in empty bins = 0.0 exactly. Verified INDEPENDENTLY by the
    controller as well as the reviewer. It rules out only the crudest mutation (flag-all-mass); it cannot
    catch any error in support/boundary logic, which is what the guardrail exists to get right.
  - Minor (bundled into fix round, not deferred): new imports sit mid-file rather than in the top block.
  - Documented as intentional (bundled): neutral source blocks colorization entirely (per SS2.1).
  - Deferred minors: _channel_ratios is blind to spatially-varying drift (global 3-vector mean; a gradient
    preserving global means passes undetected) — already honest in the docstring. Also no dedicated test
    for the means-sum-to-zero div guard (guard is a visible branch, low risk). -> final review to triage.
  - OPEN DESIGN QUESTION pending measurement: if real cross-boundary mass trips the check, hue invention
    is too strict and would veto legitimate ColorSaturation moves. Fix is a design change (dilate source
    support / distance-to-nearest-supported-bin), NOT a threshold raise. Controller rules if it fires.
Task 4: fix round 1/5 dispatched (resumed original implementer)
Task 4: DESIGN QUESTION FIRED — measured by controller: source chroma near a bin edge, 1.3x boost puts
  0.0054 mass in "unsupported" cells (limit 1e-3 -> TRIPS); 1.8x puts 0.952. The cell-based check
  rejects ordinary saturation increases and would veto every chroma move on real data.
  USER RULING: reimplement on HUE ANGLE. Spec revised + committed (72d52a7), new section 5.1 is the
  authority. Rationale: a*/b* position cannot separate "same colour, more of it" from "colour that was
  never there" (saturation moves radially into a new cell); hue ANGLE can. Matches SS2.1's own wording.
  Raising epsilon explicitly REJECTED by user: 1.8x needs epsilon > 0.95, at which point the guardrail
  detects no realistic fabrication (the SS3.7 tune-around-a-finding failure).
  Acceptance set for the guardrail (all must be tested): 1.3x/1.8x/2.2x boost PASS; 3.0x (clips) TRIP;
  green-on-neutral TRIP. Plus: minimum-chroma floor required; neutral source still blocks colourisation.
Task 4: fix round 1/5 redirected to the hue-angle reimplementation
Task 4: fix round 2 (bdc5020) implemented hue-angle w/ 3deg bins -- PASSED its own fixture, FAILED
  controller's independent fixture (1.3x/1.8x/2.2x all tripped on a fixture verified 0% out-of-gamut).
  ROOT CAUSE: saturation applied in RGB, but CIELAB hue angle is NONLINEAR in RGB, so legitimate boosts
  genuinely drift hue: 1.3x=+0.84deg, 1.8x=+2.35, 2.2x=+3.63, 3.0x=+6.39 (none clipping). The
  implementer's flat-hue fixture drifted 0.000deg, hiding it. 3deg bins were fitted between its two
  measured points (0.000 and 3.578) -- over-fitting; held on one fixture, collapsed on another.
  CONTROLLER ERROR ACKNOWLEDGED: the "3.0x must TRIP" acceptance criterion was MINE and was
  unachievable — 3.0x unclipped sits at 6.39deg, same band as 1.8x/2.2x. It asked the fabrication
  guardrail to do the clipping guardrails' job.
  USER RULING #2: angular TOLERANCE, default 20deg. Spec corrected + committed (0bbef50).
  Measured gap: legit drift 0.84-6.39deg; green-into-red 116.28deg; neutral source supports no hue.
  20deg is ~3x above drift, ~6x below invention. Bounded drift is why angle is safe where a*/b* cells
  were not — radial movement is unbounded, angular drift is not.
  REVISED ACCEPTANCE SET: 1.3x/1.8x/2.2x/3.0x-unclipped PASS; green-into-red TRIP; green-on-neutral TRIP.
  Clipping explicitly NOT this guardrail's job (check_shadow/highlight_clipping own it).
  LESSON FOR REMAINING TASKS: verify implementer fixtures independently with a DIFFERENT fixture shape;
  a flat/uniform fixture can hide behaviour a spatially-varying one exposes.
Task 4: fix round 3/5 dispatched (angular tolerance)
Task 4: fix round 3 complete (commit 24730d8; 19 passed, 326 repo-wide).
  Controller verified INDEPENDENTLY on 24730d8, not from the report:
   - repro script clean, no MISMATCH: 1.3x/1.8x/2.2x/3.0x all PASS at 0.000% out-of-gamut;
     green-into-red TRIPS. hue_tolerance_deg genuinely consulted (guardrails.py:312).
   - adversarial probe with fixture shapes neither party used: multi-hue source spanning 271.9deg
     passes 1.5x and 2.5x; green injected into that source's unoccupied hue GAP correctly TRIPS
     (proves the tolerance still discriminates on a wide-hue source rather than passing everything);
     desaturation passes; wraparound fixture near 0/360 passes 2.0x.
  Note: two message crossings during this task. Controller's "incomplete" messages described states
  that were superseded in flight. Shipped result verified correct at HEAD in both cases.
Task 4: scoped re-review dispatched over 3d35367..24730d8 (5 commits: 3 code, 2 spec)
Task 4: re-review — ADDRESSED, all 7 checks (a)-(g) traced end to end. Tests confirmed non-vacuous
  (spatially-varying fixtures; unclipped verified by assertion not assumption). New breakage: none.
  - CARRY FORWARD TO TASK 6: the hue-drift model + bin choice are calibrated against a HAND-BUILT
    proxy for ColorSaturation, since the real NumpyExecutor does not exist yet. Re-check the six
    acceptance cases against the real executor once Task 6 lands. Not a defect in this diff.
Task 4: complete (commits 3d35367..24730d8, review clean, 19 passed / 326 repo-wide, 3 fix rounds)
Plan corrected: superseded a*/b* hue block retired, points to spec 5.1.
Task 5: implementer DONE (commit 9fdb920, 5 passed). Controller probes before review:
  - immutability holds under the pattern that matters: two branches extending a SHARED prefix leave
    the prefix at len 1; keys differ; order-sensitive both ways (differs AND equals itself).
  - PROCESS_FOR_KIND covers all 8 emittable kinds -> no KeyError at render time. VERIFIED.
  - applied_kinds dedups repeated kinds at different scales. VERIFIED.
  - LATENT HAZARD (cross-task, recorded not blocking): Action.params is compare=False, so two Actions
    differing ONLY in params compare EQUAL and produce the SAME recipe key -- yet render to DIFFERENT
    PixInsight steps (demonstrated: layer 3 vs layer 99). Harmless today because params are derived
    from (kind, level, scale) + the session's pixel scale, which is constant within a run, so equal
    keys imply equal params. Breaks if a recipe is deserialized under a DIFFERENT pixel scale, or if a
    later task hand-constructs Actions. -> watch in Tasks 9 (session resume) and 13 (replay at full
    res). A recipe that reproduces a different image than the beam scored would be a SS12 failure.
Task 5: review — spec OK, quality NOT APPROVED. 2 Important, 3 Minor.
  - Important: order-sensitivity test asserts only INEQUALITY; a key of id(self) would pass. -> round 1
  - Important: round-trip test compares only .key, which ignores params (Action.key omits params and
    params is compare=False), so a from_dict DROPPING params would pass. -> round 1
    NOTE: reviewer reached this independently via test coverage; controller reached the same root cause
    via cross-task probing (equal keys -> different rendered steps, layer 3 vs 99). Two symptoms, one
    cause. The round-trip test is the natural place to hold the SS12 line, hence fixing it properly.
  - Minor (bundled into round 1): bare unmessaged KeyError at recipe.py:74, inconsistent with the
    well-messaged ValueError from .strength. Unreachable today (all 8 kinds mapped, verified).
  - Minor (deferred): from_dict does no validation -- a corrupted persisted recipe parses fine and only
    fails at render time. Loud, but late.
  - Minor (deferred): applied_kinds test is shallow (one action, one kind; no repeated-kind collapse).
  - warn (out of scope): to_pixinsight_steps output is never checked against a real PixInsight script.
    -> becomes testable at Task 13; carry it there.
Task 5: fix round 1/5 dispatched (resumed original implementer)
Task 4 CARRY-FORWARD RESOLVED EARLY (pre-checked against Task 6's brief):
  The hue guardrail was calibrated against a "hand-built proxy" for ColorSaturation. That proxy is in
  fact IDENTICAL to the real NumpyExecutor chroma op in the plan: gray + (out-gray)*(1.0+s), the same
  RGB-space formula used in the controller's calibration fixtures.
  Magnitudes: gentle s=0.25 -> 1.25x, moderate 0.50 -> 1.50x, strong 0.85 -> 1.85x. All sit inside the
  measured PASSING band (1.3x=0.84deg, 1.8x=2.35deg, 2.2x=3.63deg vs a 20deg tolerance).
  CONCLUSION: the hue guardrail cannot veto any chroma action the executor can produce. Still confirm
  empirically once Task 6 lands, but the concern is answered.
Task 5: fix round 1/5 (3 addressed, 0 open; commits 9fdb920..f0a3a3b)
  Controller MUTATION-TESTED finding 2 independently: replaced from_dict with a version silently
  dropping params -> test_round_trips_through_dict now FAILS (it passed before the fix). Assertion bites.
  Also mutation-tested Task 2's fix: reintroducing the fail-open noise check is caught by
  test_noise_floor_fails_closed_on_degenerate_baseline. Both high-stakes fixes verified by regression,
  not assumption.
  (Re-reviewer cited recipe.py:57-58 for a test assertion that lives in the test file — citation slip,
  substance independently verified.)
Task 5: complete (commits 4166859..f0a3a3b, review clean, 5 passed)
Task 6: implementer DONE (commit 415fed3, 10 passed).
  - Large-layer analysis: implementer found MORE than the controller did. Controller found only that
    plane energy decays to float noise so the boost becomes a no-op. Implementer found the real hazard:
    a trous kernel is 4*2^layer+1 wide and scipy convolve1d is DIRECT (non-FFT), so cost grows
    exponentially with layer -- measured 0.109s/0.441s/1.791s at layers 12/14/16, and layer 30 hung
    past 120s with no error. Mechanism confirmed by controller from starlet.py _dilate.
  - CONTROLLER RULING: add the guard. Not theoretical -- `layer` rides inside Action.params, and Task 9
    SERIALIZES recipes while Task 13 REPLAYS them at full res. A recipe persisted under one geometry and
    reloaded under another is exactly how an out-of-range layer arrives. Compounds the recorded
    compare=False hazard: nothing upstream would flag the params mismatch either.
    A silent HANG is worse than the silent fallbacks SS12 forbids -- a fallback at least returns.
    REJECT, do not clamp: clamping would apply a different action than the recipe records, breaking the
    SS12 audit guarantee. Guard must run BEFORE starlet_transform or it inherits the hang.
Task 6: fix round 1/5 dispatched (layer guard)
Task 6: fix round 1/5 complete (commit 03fca7e; 12 passed).
  Guard: max_layer = floor(log2(min(H,W))), raises ValueError naming layer/dims/max, NO clamping.
  Controller verified INDEPENDENTLY at 03fca7e:
   - raises in 0.0000s on layer 40 vs a 96x96 image (was >120s hang). Error text is actionable.
   - in-range layer 3 still works, changes the image, output stays within [0,1].
   - MARGIN CONFIRMED: available_actions emits layers 1-6; a 1600px proxy supports up to 10, so the
     guard cannot reject legitimate production work. (A guard that fixed the hang by blocking real
     actions would just trade one failure for a quieter one.)
  PROCESS NOTE: controller's "not landed" audit was accurate when run (HEAD 415fed3, no guard); the
  commit landed afterwards. Third message crossing this session. Treat such audits as SNAPSHOTS, not
  accusations — but keep making them: one genuinely incomplete round (Task 3) was caught this way.
Task 6: review dispatched over f0a3a3b..03fca7e (2 commits)
Task 6: review — spec OK, quality APPROVED. All 6 global constraints confirmed (clip on every branch,
  no mutation, unknown-kind raises, chroma formula byte-identical, star_split no-op, no new deps).
  Guard's 4 properties all hold (runs before starlet_transform, rejects not clamps, actionable message,
  bound has margin).
  - UPGRADED Minor->Important by controller, -> fix round 2: test_every_action_kind_is_executable
    asserts only shape/finite/range, so an IDENTITY executor passes for 6 of 7 kinds. Discriminatory
    power rests on one test. Upgraded because chroma's exact behaviour is load-bearing for Task 4's
    guardrail calibration -- a chroma branch silently degrading to a no-op would fail NOTHING today.
  - EXPLICITLY DROPPED (decided, not deferred; no ticket per the no-stub-tickets rule): n_scales=7 is
    hardcoded at every call site with no clamp to image size, so _max_supportable_layer only stays >=6
    when the short side is >=64px -- today's margin is coincidental, not enforced. DROPPED because the
    search runs on a ~1600px proxy and real renders are far larger, AND the guard already converts the
    case into a loud actionable failure; the loop kills only that candidate and logs the reason. The
    system degrades visibly. Adequate SS12 behaviour.
Task 6: fix round 2/5 dispatched (test discriminatory power)
Task 6: fix round 2/5 (1 addressed, 0 open; commits 03fca7e..18c98b9). Test-only.
  Controller MUTATION-TESTED independently: injected a silent identity regression into local_contrast,
  chroma, core_hdr and background_neutralize in turn -- ALL CAUGHT. Made star_split do something --
  also caught, so its no-op is genuinely PINNED, not incidentally satisfied.
  (Controller's first mutation run reported "NOTHING caught" -- that was a HARNESS BUG: passed 3 args
  to a 4-param parametrized test and the except-TypeError swallowed it. Corrected. Lesson: a negative
  mutation result has two causes -- weak tests, or the mutation never ran. Verify the case executed.)
Task 6: complete (commits f0a3a3b..18c98b9, review clean, 13 passed / 344 repo-wide)
BRANCH SHAPE at Task 6 close: 22 commits = 17 implementation + 5 spec/plan corrections. ~1 in 4
  commits exists because building the thing disproved something the documents asserted.
Task 7: implementer DONE_WITH_CONCERNS (commit fb73d64, 7 passed). Found a real bug in the brief's own
  test, but WIDENED the assertion to accept core_hdr -- which accommodated a degeneracy instead of
  exposing it. Controller measured: top_k=3 returned THREE core_hdr levels, 1 distinct kind.
  DEFECT (controller-found): all structural remedies tied on ONE aggregate spectrum gap -> alphabetical
  tiebreak -> core_hdr fills top_k forever (it is not once-only). local_contrast/local_equalize NEVER
  proposed at any band. Beam width 3 explored 1 option; the SS6.2 band axis went unused. Tiebreak was
  also lexicographic on the formatted key (16.000 < 2.000 < 32.000).
  NOT a design change: spec SS3.5 ALREADY said "energy deficit at a given arcsec band -> local_contrast
  at that band". The PLAN's code collapsed it to an aggregate. Spec sharpened + committed 338d067.
Task 7: fix round 1/5 -> commit 33ba86c. Per-band ranking via band_limit. Monopoly broken: 3 distinct
  kinds. Implementer also found (correctly, and flagged rather than hiding) that per-band scoring ALONE
  is insufficient: len(LEVELS)==3==top_k, so whichever group ranks #1 fills top_k by itself. Added
  round-robin across groups, gentlest first. ACCEPTED.
Task 7: SECOND monopoly of the same shape, controller-measured: round-robin never reaches a 2nd pass
  (always more groups than slots), so levels seen = ['gentle'] at top_k 3, 6 AND 9. moderate/strong
  structurally unreachable; SS6.2's magnitude axis became as decorative as the band axis had been.
  USER RULING: magnitude chosen by GAP SIZE, one level per (kind, band) group. Spec committed e4d2611.
  Rejected: gentle-only + compounding (risks iteration cap, fails SS10 'measurably improves'); raising
  top_k until round-robin wraps (~15-20, ~5x cost, blows runtime budget, still arbitrary).
  PATTERN WORTH REMEMBERING: the action space is a PRODUCT (kind x band x magnitude) but top_k is a flat
  slice of a ranked list. Fixing ties on one axis relocates the degeneracy to whichever axis is still
  tied. Escape = make each dimension carry information (magnitude DERIVED from gap), not enumerate it.
  -> WATCH Task 8: prune() dedups by recipe key, same flat-slice-of-ranked-list shape.
Task 7: ESCALATED to a fresh implementer on a stronger model. Original agent twice responded to the
  round-2 instruction by re-verifying round 1 instead (and stalled ~3h between messages) -- the
  "cannot see its own problem" signal. Round 1 work (33ba86c) is good and preserved.
Task 7: fix round 2 -> a714b93 (original agent, gap-to-level rewrite) then 60c91c6 (fresh agent,
  recalibrated boundaries). Both agents ran concurrently for a period -- controller's escalation and the
  original's completion crossed. Controller issued a STOP to the fresh agent, then REVERSED it after
  measuring. Lesson: adjudicate on MERIT before ordering a stop, not on commit order.
  BOUNDARY CONFLICT, resolved by controller measurement (180 positive gap values, all 45 pairs of the
  10 real images in data/references + data/amateur):
   - a714b93 justified 0.12/0.25 by "an empirically empty range between ~0.17 and ~0.36". NOT EMPTY:
     56 of 180 values sit inside 0.15-0.40. Largest consecutive gap is 0.0569 and lies in the extreme
     tail (0.4213->0.4782), not the body. Its 45-value survey from 5 fixture pairs was too small to see
     them. NOT fabrication -- a real survey whose sample could not support its claim.
   - controller tertiles p33.3=0.0823 / p66.7=0.1499 independently matched the fresh agent's
     0.0754 / 0.1526. Final: GAP_GENTLE_MAX=0.075, GAP_MODERATE_MAX=0.15.
   - balance: 0.075/0.15 -> ~28/39/33; 0.12/0.25 -> ~54/33/13 (skewed back toward the bug).
  Also landed: mode-change pass-through now keys on `level == ""` rather than ONCE_ONLY membership --
  "at most once" and "no magnitude axis" are different properties that merely coincide today.
  CONTROLLER RULING - NO SQUASH: a714b93's message keeps its overturned claim; 60c91c6 records the
  correction. A history showing "claim -> larger survey -> recalibration" is more honest than one that
  never erred. Branch is local-only so no external SHA dependency.
Task 7: full suite 357 passed, nothing deselected. Review dispatched over 18c98b9..60c91c6.
Task 7: both agents stood down cleanly. Final state verified: HEAD 60c91c6, tree clean, 357 passed.
  THIRD independent survey (controller) agreed with the fresh agent's: p33.3 0.0823 vs 0.0754,
  p66.7 0.1499 vs 0.1526, 56/180 vs 58/227 inside the claimed-empty range. Fresh agent correctly noted
  the controller's "largest break" (0.0569) sits in the EXTREME TAIL (0.4213->0.4782), so it is not a
  usable threshold either -- a usable break would have to fall in the BODY. "No natural break" survives.
  SQUASH RULING (controller, final): NO. a714b93 keeps its overturned claim and 60c91c6 records the
  correction. A history showing "claim -> larger survey -> recalibration" is more honest than one that
  never erred. Branch is local-only, so no external SHA depends on it.
  *** PROCESS HAZARD (raised by the original implementer, and it is right) ***
  TWO AGENTS EDITED ONE SHARED WORKING TREE CONCURRENTLY. It resolved only because both happened to
  treat the tree as append-only and checked state before writing; it could easily have clobbered work.
  RULE FOR TASKS 8-14: never run two implementers on the same task at once. If an escalation is ever
  needed again, either (a) confirm the original is truly dead first, or (b) dispatch the replacement
  with isolation:"worktree" so it cannot collide. The controller caused this by escalating on a stall
  signal that turned out to be message-crossing.
Task 7: controller checked for a THIRD degeneracy across all 25 real reference->amateur pairs, top_k=3:
   single-KIND slates : 0/25   -> degeneracy (a) gone everywhere, not just on the test fixture
   single-LEVEL slates: 7/25   -> legitimate (all three top gaps land in one bucket), NOT a monopoly.
                                  Before the fix this would have been 25/25, all gentle.
   No third axis has collapsed.
  OBSERVATION TO WATCH (not a defect): in 16 of 25 real pairs top_k=3 contains NO scale-denominated
  action at all -- slates fill with background_neutralize / black_point / core_hdr / tonal_reshape.
  On real amateur-vs-pro pairs the dominant gaps are TONAL and BACKGROUND, not per-band structural.
  Plausibly correct, but it means the band axis is lightly exercised on real data -- for a different
  reason than before (genuinely smaller gaps, not a ranking bug). The loop should self-correct as tone
  and background gaps close and structural gaps rise in relative terms.
  -> CHECK AT TASK 10 (does the loop actually reach structural actions on later iterations?) and at
     TASK 14 (does the exit criterion's "measurably improves" half depend on structural work happening?)
Task 7: review — spec OK, quality APPROVED, NO Critical/Important. All 6 global constraints confirmed
  (zero direct Action( construction; palette gate both directions; deterministic sort on dict keys not
  hash order; numeric tiebreak with no action.key in the sort; mode-change level "" passes through and
  .strength does not raise; no new deps). Degeneracies (a) and (b) confirmed gone BY CONSTRUCTION with
  regression tests verified to fail against the prior commits. No third degeneracy. Calibration comment
  honest -- no trace of the overturned claim in source or the final commit message.
  - Minor (deferred): gap_for's 0 <= layer < len(deficits) bounds guard is defensive against a layer
    computed outside n_scales (menu-build uses max(ref.psf, target.psf) while _band_deficits uses the
    target's own bands). Untested edge case; consistent with the documented "neither short nor matched
    -> 0" convention, not a silent-failure violation. -> final review to triage.
Task 7: complete (commits 18c98b9..60c91c6, review clean, 13 in-file / 357 repo-wide)
Task 8: dispatched (single implementer only, per the concurrency rule). Controller pre-measured the
  distance-tie question on real data (eso1103a vs an amateur render, 5 candidate actions):
    smallest separation between candidate distances = 9.701e-04, NO exact ties.
  So ties among GENUINE actions are unlikely -- 3 orders above float noise.
  BUT one case guarantees an exact tie: star_split is a deliberate no-op in NumpyExecutor, so its
  candidate is pixel-identical to the parent and scores an IDENTICAL distance. It survives dedup
  (different recipe key) while exploring nothing -- a branch consuming a beam slot for no information.
  -> compare against the implementer's own analysis; decide at Task 10 where mode-change routing lives.
Task 8: implementer DONE (commit 0df60c3, 5 brief tests + full suite 362, no regressions).
  Tie analysis: no structural collapse possible -- dedup by recipe.key precedes the width slice, so
  distinct recipes survive a tie together and identical recipes collapse. Residual: stable sort favours
  the earlier input on a tie; implementer located this in the LOOP's candidate ordering (Task 10), not
  in prune. Matches controller's measurement (no exact ties among genuine actions, min separation 9.7e-4).
  Controller verified all 6 properties independently, incl. the decisive one: 3 duplicates crowding the
  input front still let a distinct branch reach the beam -> dedup genuinely precedes the slice.
Task 9 PRE-CHECK (controller): the Session schema DOES persist pixel_scale_arcsec + psf_fwhm_arcsec, so
  a resumed session knows its original geometry. But from_dict only reads them back -- nothing verifies
  the proxy being resumed against still matches. The recorded params/layer hazard therefore survives in
  a milder form: a session reused against a different image deserialises cleanly and fails later.
  Task 6's layer guard is the backstop (loud named ValueError, not a hang) -- already accepted as
  adequate. -> ASK Task 9's implementer to analyse whether a cheap load-time consistency check is
  worth adding; do NOT mandate it.
Task 8: review — spec OK, quality APPROVED, NO material findings. Dedup-before-slice confirmed at
  beam.py:59-66 (sort live -> check seen -> append -> width check). Tie analysis judged sound; the
  star_split case correctly assigned to the LOOP (prune cannot know an action was a no-op) with a
  suggested remedy of intelligent tie-breaking on candidate ordering. -> CARRY TO TASK 10.
Task 8: complete (commits 60c91c6..0df60c3, review clean, 5 in-file / 362 repo-wide)
Task 10 PRE-CHECK (controller, against the plan's own code) — BOTH carry-forwards confirmed as real gaps:
  1. PASSING VERDICTS ARE NEVER LOGGED. advance() appends to guardrail_log only inside `if failed:`
     (plan lines ~1937-1943). Task 3's fix -- the zero-star branch passing with reason "star integrity
     not assessed: baseline had no detectable stars" -- therefore dies inside the verdict object and
     never reaches the user. That fix is currently COSMETIC, exactly as predicted when it was ruled on.
     -> Task 10 must surface passing verdicts that carry a non-empty reason.
  2. CANDIDATE ORDERING decides ties. candidates.append(...) runs in proposal order (plan line ~1950)
     and prune sorts stably, so a distance tie breaks on whichever action the proposer emitted first.
     This is where the star_split no-op tie lands: star_split is pixel-identical to its parent, scores
     an identical distance, survives dedup (different recipe key), and consumes a beam slot having
     explored nothing. -> Task 10 owns the remedy (both the Task 8 implementer and its reviewer agreed).
Task 9: implementer DONE (commit 094133f; 4 in-file, 366 repo-wide, nothing deselected).
  Proactively strengthened the brief's round-trip test per the flagged params hazard (non-empty
  distinctive params + to_pixinsight_steps equality) and added a 3-branch mixed-alive beam test.
  Controller MUTATION-TESTED all three failure modes -- ALL CAUGHT:
    params dropped by Recipe.from_dict -> both round-trip tests fail
    guardrail_log dropped by load      -> guardrail-log test fails
    beam truncated to 1 branch         -> both round-trip tests fail
  CONTROLLER RULING (load-time consistency check): NOT ADDED. Implementer's reasoning accepted and is
  better than the question: Session records no image DIMENSIONS, so load() cannot verify image identity
  from pixel_scale alone -- any check there is inference dressed as verification, and it would couple a
  pure serialization module to image I/O. The guard belongs where the information lives: Task 10's
  resume call site holds both the session and the actual proxy path and can do an EXACT path comparison.

=== TASK 10 MUST CARRY THESE THREE (all confirmed against the plan's own code) ===
  A. SURFACE PASSING VERDICTS THAT CARRY A REASON. advance() logs only inside `if failed:`, so Task 3's
     "star integrity not assessed" pass never reaches the user. That fix is cosmetic until Task 10
     consumes it.
  B. CANDIDATE ORDERING / star_split NO-OP. Candidates are appended in proposal order and prune sorts
     stably, so ties break arbitrarily. star_split is pixel-identical to its parent, scores an identical
     distance, survives dedup (different recipe key), and burns a beam slot exploring nothing.
  C. EXACT proxy_path COMPARISON ON RESUME (from Task 9's ruling above).
Task 9: review — spec OK, quality APPROVED, NO findings. All 18 fields verified field-by-field through
  save/load. Recipe serialization delegated to Recipe.to_dict/from_dict, no duplicate logic. load()
  raises on missing/malformed JSON. Params hazard CLOSED (non-empty distinctive params, asserts both
  to_pixinsight_steps equality and direct params equality).
Task 9: complete (commits 0df60c3..094133f, review clean, 4 in-file / 366 repo-wide)
Task 10: implementer DONE_WITH_CONCERNS (commit f1c8e41; 16 in-file, 382 repo-wide, none deselected).
  All three carried requirements (A)(B)(C) implemented AND verified failing against the brief's code first.
  (B) approach ACCEPTED: discards any candidate where np.array_equal(produced, parent) -- the PROPERTY,
      never the action name. Catches star_split's no-op, a decayed-band local_contrast, and any future
      no-op; PixInsight's real star_split (which does change pixels) still survives. NOT recorded in the
      recipe -- an entry that changed no pixels would be a lie in the SS12 audit artifact. Endorsed.
  Measured: already-good fixture D = -5.4e-20 (genuinely at the valley floor); flatten(ref,0.6)
      D 0.154184 -> 0.024127.
  DESIGN CHANGE it flagged rather than shipping silently: a branch contributes top_k LIVE candidates,
      not top_k attempts. Argument: "a discard that also costs search breadth is a score term wearing a
      different hat, which SS7 forbids." Controller verified the pattern independently -- on a flattened
      eso1103a, 4 of 10 proposed actions tripped, and rank 2 (tonal_reshape, the BEST improver at
      -0.050) tripped noise+shadow. Trips are the common case.
  CONTROLLER MEASURED THE COST at the real 1600px proxy (which the implementer could not see):
      menu 50 actions; per candidate 2.54s (exec 0.18 + guardrails 0.83 + FINGERPRINT 1.53 dominates).
      attempts=9/iter -> 7.6 min | unbounded=150/iter -> 127 min | bounded 3x top_k=27/iter -> 22.9 min
  USER RULING: BOUNDED RETRY, max_attempts = 3 * top_k. Spec committed eb2ac3e (SS3.3 new subsection).
      Cap binding must be SURFACED -- silent truncation would read as "explored fully" when it did not.
  OTHER CONCERNS RULED: np.ptp fix correct (NumPy 2.5 removed the ndarray method); iteration_cap possibly
      under-sized (D still falling at 20) -> NOTED as an open SS3.7 calibration question for Tasks 12/14,
      not changed here; 15 of 19 recipe actions being local_contrast -> NOTED, only live tests can say if
      15 stacked MultiscaleLinearTransform steps is a sane PI recipe -> CARRY TO TASK 13; starless
      reference fixture meant star_integrity was never exercised in assessing mode -> implementer added a
      starry fixture, kept.
Task 10: fix round 1/5 dispatched (bound the retry at 3x top_k, surface when it binds)
Task 10: fix round 1/5 landed (commit 9c5e19c, bounded retry at max_attempts = 3 x top_k).
  Tree clean, full suite 387 passed, nothing deselected.
  - FOURTH calibration question (raised by the Task 10 implementer, not the controller): max_attempts'
    own trip rate was measured on a 160px SYNTHETIC fixture -- 413 attempts over 20 iterations
    (~20.7/iter against the 27 worst case), cap binding 3 times. Trip rates at the real 1600px proxy
    will differ. Re-measure before trusting 3 x top_k as the right bound.
  Task 10 detail: 21 tests (16 + 5 new). Cap-binds verified with a real logged event using `noted`
    (not `failed`, since nothing was discarded -- the branch stopped looking) keyed by branch rather
    than action. max_attempts derived via __post_init__ so it tracks top_k, and round-trips through
    Session save/load (tested -- the cross-task contract holds). Bound is FREE on this fixture: same
    19-action recipe, D 0.154184 -> 0.024127 unchanged.
=== SESSION PAUSED HERE ===
NEXT ACTION ON RESUME: Task 10 still needs its TASK REVIEW (never dispatched — the session ended
  after the fix round). Build the package over 094133f..9c5e19c and review before closing Task 10.
  Then Tasks 11-14.
THREE OPEN CALIBRATION QUESTIONS carried to Tasks 12-14 (do NOT tune around them, SS3.7):
  - iteration_cap=20 may be under-sized; D was still falling at iteration 20 on a flattened render.
  - a recipe came back 15-of-19 local_contrast; only live PI tests can say if 15 stacked
    MultiscaleLinearTransform steps is sane.
  - on real amateur-vs-pro pairs, 16 of 25 slates proposed NO scale-denominated action; confirm the
    loop reaches structural work on later iterations.
=== SESSION RESUMED (2026-07-28) ===
Controller verified on resume: tree CLEAN, HEAD 9c5e19c, Task 10's fix round IS committed.
User re-confirmed subagent-driven execution for the remainder.
Task 10: task review DISPATCHED (opus) over 094133f..9c5e19c -- the task's FIRST and ONLY review,
  covering both the implementation (f1c8e41) and the fix round (9c5e19c). Reviewer was given the
  three carried requirements (A)(B)(C) plus the fourth (bounded-retry surfacing, spec eb2ac3e),
  since none of them appear in task-10-brief.md.
TASK 11 BRIEF DEFECTS found by the controller BEFORE dispatch (brief predates later rulings):
  1. LOAD-BEARING: the brief's `_op_optimize_step` calls `Session.load(path)` directly, which
     BYPASSES `loop.resume(session_file, *, proxy_path)` -- the function Task 10 built to satisfy
     carried requirement (C). The sidecar's optimize_step IS the resume call site named in Task 9's
     ruling, so shipping the brief verbatim would leave (C) as dead code at the exact seam it exists
     for. RULING: the ledger governs the brief. optimize_step must call `resume(...)` and therefore
     must require the image path in its request. This widens the op contract -> CARRY TO TASK 13
     (the PJSR driver must send `image` on every step, not only on begin).
  2. Brief's test helper uses `f.ptp()` -- removed from ndarray in NumPy 2.5, the same breakage
     Task 10 already hit. Must be `np.ptp(f)`.
  3. Brief re-imports FingerprintData and load_raster inside sidecar.py; both are ALREADY imported
     at sidecar.py:34 and :36. Cosmetic, but the implementer should not add duplicate imports.
USER RULINGS (2026-07-28), both expanding scope beyond the briefs:
  - Task 13 must ALSO deliver a real PixInsightExecutor -- production must not optimize against the
    NumpyExecutor approximation (spec SS2.2: "Production never optimizes against an approximation").
  - Task 15 (SS3.6 full-resolution validation checkpoint) IS in scope for this branch.
CONTROLLER FINDING that follows from those rulings -- ESCALATED TO THE USER, plan vs spec conflict:
  Spec SS2.5 (lines 108-128) already specifies the production protocol, and Task 11's brief
  CONTRADICTS it:
    spec:  optimize_begin -> session_id + instruction BATCH 1; PI applies N processes and saves N
           candidates; optimize_step -> sidecar MEASURES those N, guardrails, scores, prunes,
           returns BATCH 2 or converged. The sidecar never applies pixels in production.
    brief: optimize_step calls advance(session, NumpyExecutor(), ...) -- the WHOLE iteration runs
           server-side in Python. That is the CI path only.
  Consequence: "PixInsightExecutor" CANNOT be an Executor subclass. executor.py's protocol is
  `apply(rgb, action, *, pixel_scale_arcsec) -> rgb`, a synchronous in-process call; but the sidecar
  is spawned as a CHILD of PixInsight and cannot call back into its own parent. The production
  executor is the BATCHED PROTOCOL of SS2.5, not an object implementing Executor.
  This makes Task 11 a design task, not the transcription its brief describes, and it changes the
  op contract that Tasks 13/14/15 consume. NOT DISPATCHED pending the user's decision.
CONTROLLER ANALYSIS of the two candidate shapes for "a real PixInsightExecutor" (resolves the
escalation above WITHOUT needing a further user decision -- cost evidence is decisive):
  (B) an apply()-shaped PixInsightExecutor that shells out to a headless PI per candidate. Matches
      executor.py's Protocol and matches loop.py:214, where Task 10's author explicitly anticipated
      "the PixInsight executor's star_split does change the image and must survive this check".
      REJECTED ON COST: apply() is synchronous and one-candidate-at-a-time, so this pays a full PI
      process startup per candidate. At the measured ~27 candidates/iteration x 20 iterations that is
      ~540 headless PI launches -- hours of startup alone, on top of the measured 2.54s/candidate.
      Batching the launches to avoid it just reconstructs (A).
  (A) spec SS2.5's batched protocol. CHOSEN. PI (the parent process, already running) executes;
      the sidecar measures, guardrails, scores, prunes.
  NON-OBVIOUS CONSEQUENCE the split must handle: advance() walks the ranked menu until it has top_k
  LIVE candidates (loop.py:184-190), and liveness is only knowable AFTER guardrails run on produced
  pixels. A batch therefore cannot know up front how many candidates to request. An iteration whose
  guardrails trip needs a SUPPLEMENTARY batch -- so "one batch per iteration" is the nominal case,
  not an invariant. max_attempts (3 x top_k) remains the bound across all batches of one iteration,
  and the cap-binding event must still surface (requirement D).
  Task 10's shipped advance() is NOT deleted: it stays as the offline/CI path that run_to_convergence
  and Tasks 12/14 drive with NumpyExecutor. The batched ops are a second entry point over the same
  proposer, guardrails, scoring and pruning -- no duplicated logic.
Task 10: review LANDED (report at task-10-review.md). Spec compliance FAILED, quality NEEDS FIXES.
  NO Critical. All FOUR extra-brief requirements verified IMPLEMENTED with tests both directions:
    (A) loop.py:238-244 noted-vs-failed distinguished by FIELD not string; tests :287 / :314
    (B) loop.py:203-224 keyed on np.array_equal(produced, parent), NOT the action name; tests :483 / :506.
        Reviewer independently verified the premise at beam.py:68 -- sort IS stable, dedup IS by recipe
        key, so the no-op really would have taken the top slot on an already-good image.
    (C) loop.py:99 plain string != with no normalization; tests :532 / :544 / :560
    (D) loop.py:263-279 emits noted:["attempt_cap"] keyed by BRANCH; bound at beam.py:44-48; attempt
        counted BEFORE the executor runs (loop.py:190); tests :344 / :353 / :366 / :404 / :417
  CONTROLLER ADJUDICATION of the 3 Important findings:
  1. "resume() has no producer -- nothing calls session.save()" -> NOT A TASK 10 DEFECT. Task 11's brief
     already calls session.save(path) in BOTH ops; the loop staying pure while the SIDECAR owns session
     persistence is the correct seam, and run_to_convergence is the offline/CI path where in-memory is
     right. The reviewer flagged it as possibly Task 11's to own -- it is. CARRIED TO TASK 11 as an
     explicit acceptance requirement so it cannot be lost. The genuine defect inside this finding is the
     `# checkpoint (SS6.1)` comment at loop.py:300 labelling an in-memory assignment as a checkpoint
     -> folded into fix round 1.
  2. "_ranked_menu verbatim-duplicates propose.py:135-145" -> REAL, ENTERS THE FIX LOOP. Failure mode is
     specific: _ranked_menu builds its own menu only to learn len(menu) and ask for the WHOLE ranking; if
     propose.py drifts, len(menu) under-counts, propose_actions truncates, and the loop silently loses the
     TAIL of the ranking -- the exact tail SS3.3's bounded retry exists to reach (local_contrast@2" ranks
     FIFTH and is the action that closes the gap). Fix: propose_actions(top_k=None) = whole ranking.
  3. "the spec's own budget argument does not reach its own conclusion" -> ESCALATED TO USER, not fixed by
     a subagent. Spec lines 181-195 reject the unbounded retry because ~127 min exceeds a stated 15-minute
     budget, then adopt a policy whose own table row says ~23 min worst case -- also over, by ~50%. Same
     reasoning reproduced at beam.py:33-38. This is a defect in reasoning I committed in eb2ac3e, and the
     budget number is the user's call. MUST be settled BEFORE Task 12 calibrates against it.
  8 Minor findings recorded in task-10-review.md -> hand to the FINAL whole-branch review to triage.
Task 10: fix round 1/5 dispatched (fresh implementer, sonnet -- the original is from a prior session and
  cannot be resumed). Scope: finding 2 + the lying checkpoint comment. Explicitly forbidden from touching
  the spec, beam.py's rationale, or any tunable (SS3.7).
USER RULING on Important finding 3 (the budget): ATTACK THE COST DRIVER, keep max_attempts = 3 x top_k.
  Rationale: 15 min vs 23 min is a symptom; the disease is 2.54s/candidate of which fingerprint
  extraction is 1.53s. Narrowing the cap to fit the budget would reintroduce the exact failure SS3.3
  was written to prevent (local_contrast@2" ranks FIFTH and is the action that closes the gap).
  => NEW TASK 16: fingerprint extraction cost. Two candidate wins identified by the controller,
     BOTH TO BE MEASURED FIRST, not assumed:
       (a) advance() re-measures every live branch's parent each iteration (loop.py:176), but that
           parent was a CANDIDATE last iteration and was already fingerprinted then. One redundant
           full extraction per branch per iteration.
       (b) guardrails (0.83s) compute an MRS-noise starlet transform and extract() (1.53s) computes a
           starlet transform OF THE SAME IMAGE. Two decompositions where one may serve both.
     HARD CONSTRAINT on Task 16: it must be BEHAVIOR-PRESERVING. Any drift in a fingerprint invalidates
     every calibrated threshold and the SS10 exit criterion with it. Equality of fingerprints before/after
     must be PROVEN on real data, not asserted. This is an optimization, not a re-design of the metric.
     Task 16 also owns the SPEC FIX: SS3.3 lines 181-195 and beam.py:33-38 must state a budget their own
     argument actually reaches, with the post-optimization measured numbers.
  SEQUENCING: Task 16 lands AFTER Task 11 (Task 11 refactors advance(), which is where win (a) lives --
  doing perf first would be redone) and BEFORE Task 12 (which calibrates against runtime and runs
  run_to_convergence on real data). Revised order: 10 -> 11 -> 16 -> 12 -> 13 -> 14 -> 15.
REVISED REMAINING SCOPE (was 4 tasks, now 7 -- all three expansions are user rulings, not drift):
  Task 11  sidecar ops, rebuilt on spec SS2.5's BATCHED protocol (not the brief's server-side advance)
  Task 16  fingerprint extraction cost + the spec's budget reasoning        [NEW - user ruling]
  Task 12  offline end-to-end on real reference data
  Task 13  PJSR driver AND a real PixInsightExecutor                        [EXPANDED - user ruling]
  Task 14  the SS10 exit criterion on real data
  Task 15  SS3.6 full-resolution validation checkpoint                      [NEW - user ruling]
=== HANDOFF TO THE USER (2026-07-28) ===
User is taking over implementation. NO further implementer subagents dispatched by the controller.
Task 11's implementer was NOT dispatched. Its rewritten brief is task-11-brief-v2.md (see below).
Task 10: fix round 1/5 landed (commit df577e7, +3 tests, suite 390 passed, nothing deselected).
  Finding 2 (_ranked_menu duplication) fixed via propose_actions(top_k=None); the lying
  `# checkpoint (SS6.1)` comment fixed at loop.py:290-291 and now names the sidecar as the source of
  durability. Scoped re-review dispatched over 9c5e19c..df577e7; verdict recorded below.
HANDOFF RESCINDED (same session, user's next message): "i want you to get this done".
  Controller RESUMES subagent-driven execution through Tasks 11, 16, 12, 13, 14, 15.
  Running to completion; stopping only for a genuine blocker or a plan/spec conflict that is the
  user's to rule on. The handoff block above is superseded and is retained only as a record.
Task 10: fix round 1/5 re-review LANDED — ALL FINDINGS ADDRESSED, no new breakage.
  1. _ranked_menu duplication ADDRESSED (loop.py:110-125 is now a one-line delegation to
     propose_actions(..., top_k=None); propose.py:109-186 widened top_k to `int | None`).
  2. Lying checkpoint comment ADDRESSED (loop.py:289-291 now names Session.save/the sidecar as the
     source of durability).
  All 3 scope limits honored: no session.save() in loop.py; spec and beam.py untouched; NO tunable
  VALUE changed (only top_k's type signature widened -- config values pass through unchanged).
  PURITY SETTLED FROM THE DIFF, no run needed: the only changed line in propose_actions' body is the
  break condition, `if len(proposed) >= top_k` -> `if top_k is not None and len(proposed) >= top_k`.
  For any caller passing an int the guard is always True, so the condition reduces to EXACTLY the old
  expression -- logically identical, not merely similar. Everything upstream (available_actions,
  gap_for, group_key/sorted(groups) ordering, _level_for_gap bucket selection) is untouched, so
  ordering, applied_kinds filtering, per-band ranking and magnitude assignment are unaffected.
  REGRESSION TEST VALIDATED as a real mechanical catch, not a vacuous call-through:
  test_ranked_menu_has_no_menu_construction_of_its_own patches loop_mod.available_actions out from
  under the code; against the PRE-FIX duplicate that starves the local reconstruction to [] and fails
  both assertions (report shows `assert 0 > 0`), post-fix the delegation is unaffected.
  Second opinion test also checked: test_top_k_none_returns_the_whole_ranking_untruncated asserts
  top_k=None == top_k=1000 AND full[:3] == top_k=3, so the bounded call is a strict PREFIX of the
  unbounded one -- same ranking, not a second computation that happens to agree.
Task 10: complete (commits 094133f..df577e7, review clean, 21+3 in-file / 390 repo-wide).
  NOTE for the final whole-branch review: 8 Minor findings parked in task-10-review.md.
  PROCESS NOTE: two reviewer subagents went idle without delivering before reporting on a later nudge.
  A duplicate re-reviewer dispatched as insurance was stopped once the original delivered. Future
  dispatches require the report be WRITTEN TO A FILE as it goes, so a lost reply does not lose the work.
Task 11: dispatching against task-11-brief-v2.md (NOT the superseded task-11-brief.md).
CONTROLLER DECISION (user said "just keep working" -- taken as leave to decide):
  TASK 17 SCHEDULED on this branch -- PJSR release compliance. Verified on disk 2026-07-28: the
  defect is TOTAL, not partial. No repository/ dir, no updates.xri, no packages, and tools/ holds
  only capture_gallery_fixtures.sh -- no sign.sh, no build-packages.sh, no install-local.sh.
  autocontrast_analyze.js has a #feature-id but NO #define VERSION. (solve_spike.js's
  `#define VERSION "6.4.2"` is ImageSolver's, not ours -- do not mistake it for our versioning.)
  Brief written: task-17-brief.md. Sequenced AFTER Task 13 so it can version both shipping scripts
  in one pass. Reference implementation to copy conventions from: ~/projects/EZ-suite-bsc/EZ-Stretch-BSC/.
  Brief forbids pushing, forbids committing any secret, and requires an honest BLOCKED over a faked
  signature or stubbed SHA1.
REVISED ORDER: 11 -> 16 -> 12 -> 13 -> 14 -> 17 -> 15.
Task 11: implementer dispatched (opus -- design task, not transcription). Given the v2 brief, the
  four surviving requirements (A)(B)(C)(D), the no-duplication constraint with df577e7 cited as the
  precedent, and instructed to write its report to disk AS IT GOES.
Task 11: implementer DONE_WITH_CONCERNS (commit 547ad1e; 21 new tests, full suite 411 passed
  (baseline 390), nothing deselected, no warnings). Agent went idle without replying, but the
  write-report-to-disk-as-you-go instruction WORKED -- commit and 11.6KB report both on disk, tree
  clean. Controller verified from git, not from a status line.
  Adopted the brief's suggested simplification: persist per-branch cursor/kept/attempts, recompute the
  deterministic menu per batch. Shared seam is `ingest_candidate` (claim to be VERIFIED by review).
  REAL BUG IT FOUND WHILE IMPLEMENTING: first cut mirrored advance()'s `while may_issue(config)` in
  _plan_batch. Wrong -- `kept` cannot advance during PLANNING because nothing has been produced yet,
  so it ran until attempts hit max_attempts and issued 9 instructions where the beam wanted 3. Now
  bounded at min(top_k - kept, max_attempts - attempts). Caught by its own test
  test_the_first_batch_never_exceeds_the_beam_width_of_the_root.
  SCOPE NOTE: the diff also modifies recipe.py (+70), which the brief's file list did NOT name.
  Flagged to the reviewer to judge as necessary-or-creep.
  FOUR CONCERNS RAISED -- controller triage:
  1. *** CARRY TO TASK 13, AND IT COSTS A DEPENDENCY *** candidate_suffix defaults to .png = 8-BIT.
     Every candidate makes an 8-bit round trip through disk, a real quality ceiling on the PRODUCTION
     search, not a test artifact. Cause: Pillow in this venv cannot write 16-bit RGB at all
     (TypeError on <u2 for both TIFF and PNG) and tifffile/imageio/cv2 are NOT installed, so the test
     cannot even play PixInsight at 16 bits. Deliberately exposed as a request+session field so the
     PJSR task can raise it without touching the sidecar -- but Task 13 must ADD A 16-BIT READER
     DEPENDENCY to do so. Implementer correctly flagged rather than silently adding a dependency.
  2. OPEN GAP, unowned: the SS12 "linear input is a loud error" constraint has NOTHING to inherit --
     grepped, no stretch/linearity check exists anywhere in src/. optimize_begin does not enforce it.
     NOT added because any linearity detector is a threshold and SS3.7 forbids introducing a tunable
     without exit-criterion runs that do not exist yet. Reasoning ACCEPTED. Needs an owner before
     Phase 2 is called complete -> raise with Task 14.
  3. CARRY TO TASK 13: optimize_begin corrects pixel_scale_arcsec by downsample_factor(), matching
     _op_analyze. So the request must carry the NATIVE on-sky scale. Wiring analyze's raw
     wcs.pixel_scale_arcsec straight through is CORRECT; a pre-corrected caller would DOUBLE-correct.
     Must be stated in the PJSR bridge contract.
  4. No BeamConfig accepted from the request -- sidecar always uses defaults, on the reading that a
     request field would make every tunable adjustable by whoever writes the JSON (SS3.7). ACCEPTED;
     its attempt-cap test reaches the bound through BEHAVIOR (PixInsight produces nothing) rather than
     by setting config, which is the better test.
Task 11: task review dispatched (opus) over df577e7..547ad1e. Reviewer told the structural check is
  THE priority -- whether advance() now routes through the same seam or a second copy of the
  guardrail/score/prune logic exists -- with df577e7 cited as the precedent for that failure class.
Task 11: review LANDED (task-11-review.md, 27KB). Task quality APPROVED with one Important to fix.
  NO Critical. Spec compliance ✅ on both ops, verified field-by-field against the v2 brief.
  STRUCTURAL VERDICT -- THE thing this review existed to settle: NO SECOND COPY. advance() and the
  batched path genuinely share one search through the `ingest_candidate` seam. The df577e7 failure
  class was NOT reintroduced. Two structural no-duplication tests judged real.
  ALL FOUR CARRIED REQUIREMENTS STILL HOLD ON THE BATCHED PATH (not merely on the old advance() path):
    (A) HOLDS  (B) HOLDS  (C) HOLDS  (D) HOLDS
    plus: attempt counted on ISSUE not success ✅; max_attempts bounds across ALL batches ✅.
  recipe.py (+70, unlisted in the brief) judged in scope, not creep.
  IMPORTANT #1 -> fix round 1: `candidate_suffix` accepted UNVALIDATED at
    sidecar.py:_op_optimize_begin (`req.get("candidate_suffix", ".png")`). The suffix reaches
    PixInsight, but the sidecar reads candidates back via _optimize_loader -> load_image -> Pillow.
    The OBVIOUS choice for a PJSR caller is `.xisf`, PixInsight's NATIVE format, which Pillow cannot
    open. Failure mode is a MISDIAGNOSIS, not silence: every candidate raises inside _ingest_batch,
    is logged failed:["executor"], the branch spends its attempts, _close_iteration finds zero
    survivors, and the run ends with "every branch was discarded by guardrails or executor failure".
    The read errors ARE in guardrail_log, but the top-line diagnosis blames PixInsight and the
    guardrails for a ONE-FIELD CONFIG MISTAKE. Fix: validate at optimize_begin against what
    load_image actually handles, naming the suffix and the supported set. This also gives concern #1
    (the 8-bit ceiling) a home -- whoever adds a 16-bit reader extends that same set.
  Fixer instructed to DERIVE the supported set from io/loaders.py rather than hardcode a list that
  can drift -- explicitly citing df577e7 as the precedent for why.
  Minor findings (incl. #2: `iteration` vs `iterations` in one response dict, differing by one while
  a batch is in flight) -> DEFERRED to the final whole-branch review to triage.
Task 11: fix round 1/5 landed (commit ec6c662; 413 passed, baseline 411 + 2 new, nothing deselected,
  no warnings). candidate_suffix now validated at optimize_begin against a NEW supported_suffixes()
  in loaders.py, built from _FITS_SUFFIXES (load_image's OWN dispatch constant, reused not copied)
  unioned with Pillow's Image.registered_extensions(). An unreadable suffix such as .xisf now raises
  immediately naming the suffix and the supported set. New test confirmed failing pre-fix via
  git stash (resp["ok"] was True for .xisf), then passing.
  Scoped re-review dispatched over 547ad1e..ec6c662. Told to verify the set is GENUINELY DERIVED
  rather than a fresh hardcoded list that happens to look right, and to give a view on whether
  Image.registered_extensions() being environment-dependent is a strength (tracks reality) or a
  hazard (a suffix accepted on one machine is refused on another).
Task 11: fix round 1/5 re-review LANDED -- ADDRESSED, no new breakage.
  Validation sits at sidecar.py:255-267, BEFORE session.candidate_suffix is set, BEFORE
  loop.begin_batch, and BEFORE session.save -- so a bad suffix leaves no session behind.
  DERIVATION CONFIRMED GENUINE, not a hardcoded lookalike: _FITS_SUFFIXES (loaders.py:72) is ONE
  module-level set that BOTH load_image's dispatch (loaders.py:132) and supported_suffixes()
  reference directly -- same object, no second copy, cannot drift. The raster half,
  Image.registered_extensions(), is exactly the plugin table Image.open() consults inside
  load_raster, so supported_suffixes() and load_image cannot disagree BY CONSTRUCTION. Verified
  empirically in-venv: .xisf absent; .png/.tif/.tiff/.jpg/.jpeg/.fits/.fit/.fts present.
  RULING ON THE ENVIRONMENT-DEPENDENCE QUESTION the controller raised: registered_extensions() being
  build-dependent is the CORRECT tradeoff, not a hazard. The question the fix must answer is "can
  THIS sidecar process read this back," and a hardcoded list would falsely claim portability a given
  Pillow build may not have, and need manual upkeep. Real consequence: a suffix accepted in dev could
  be refused in prod if Pillow builds differ. -> CARRY TO TASK 17 as a one-line packaging note. Not
  a defect.
  TEST JUDGED GENUINE, not a tautology: asserts on handle_request's observable contract (ok False,
  .xisf and candidate_suffix named in the error, NO session JSON left in work_dir) through the real
  PixInsight-facing entry point. Re-reviewer independently traced that loop.begin() is pure in-memory
  and writes nothing to work_dir before the raise, confirming the "no session left behind" assertion
  is meaningful rather than coincidental.
Task 11: complete (commits df577e7..ec6c662, review clean, 21+2 in-file / 413 repo-wide).
  Minor findings parked in task-11-review.md for the final whole-branch review.
Task 13: brief v2 written (task-13-brief-v2.md), superseding task-13-brief.md. Built against the
  ACTUAL shipped instruction contract {instruction_id, branch_key, action_key, process, params,
  parent_path, candidate_path} with deterministic ids it{N}-br{N}-ac{N}. Assigns Task 13 the 16-bit
  problem explicitly, with an honest-8-bit escape hatch, and requires `noted` entries reach the
  console (else Tasks 3 and 10's work is discarded at the last step).
Task 16: dispatching (fingerprint extraction cost + the spec's budget reasoning).
CONTROLLER PRE-FLIGHT on Tasks 12 and 14's data prerequisites (checked on disk 2026-07-28, BEFORE
dispatch, so neither task burns a round discovering it):
  - data/fingerprints.sqlite EXISTS. data/discovery_cache/ holds 35 real .jpg renders.
  - provenance has NO `cache_file` key. Real keys: source_url, license, attribution, ingested_utc,
    wcs_source. Task 12's brief Step 2/3 already anticipates this ("inspect an actual record first")
    and the resolution is that cached renders are named `<record_id>.jpg`
    (data/discovery_cache/eso1103a.jpg confirmed present).
  - `esa_hubble:opo9545a1` is NOT in this store; only `eso1103a` is. The Phase 1 live run that
    ingested opo9545a1 used a FRESH store, not this one. BOTH task-12-brief.md and task-14-brief.md
    list opo9545a1 FIRST in their fallback, so both will silently fall through to eso1103a.
    CONSEQUENCE: eso1103a is broadband RGB while the user's M42 print is HOO, so the palette gate
    drops chroma (SS2.3) and the comparison runs on structure+tone only. That is legitimate behavior,
    but it must be STATED in the test rather than discovered as a surprise -- and it means the exit
    criterion is NOT exercising the chroma path unless opo9545a1 is re-ingested first.
  - *** TASK 14 BRIEF DEFECT, load-bearing *** test_declines_on_a_cached_professional_render takes
    sorted(Path("data/discovery_cache").glob("*.jpg"))[0] -- which is eso0104a.jpg -- and optimizes it
    against ESO1103A's fingerprint. Two DIFFERENT objects. The test's own docstring claims it is "a
    professional render measured against its own fingerprint", which it is not. A large distance
    between unrelated targets is CORRECT, so the `improved is False` assertion could fail for a reason
    that says nothing about the fail-safe -- or pass for the wrong reason. FIX BEFORE DISPATCHING
    TASK 14: measure a render against ITS OWN stored fingerprint (use the reference's own cached
    file, `data/discovery_cache/{rid}.jpg`), or ingest the second reference so a genuine
    same-object pair exists.
Task 16: implementer DONE (commit 8d8f727; 422 passed, baseline 413 + 9 in tests/test_fingerprint_cost.py,
  nothing deselected, no warnings, NO existing test modified). Agent again went idle without replying;
  commit + 15KB report both verified on disk by the controller.
  MEASURED RESULT: fingerprint.extract 1.497s -> 0.919s (-38.6%); a SCORED candidate 2.719s -> 2.136s
  (-21.4%); the per-branch-per-iteration parent re-measure 1.497s -> ELIMINATED. Three consecutively
  measured real iterations: 75.26s -> 53.21s (-29%), with distance IDENTICAL at every iteration
  (0.1177 / 0.1058 / 0.0866). Extraction counts confirm the mechanism exactly: 3 fewer per iteration,
  one per live branch (width=3). Both hypotheses (a) and (b) CONFIRMED, not refuted.
  *** THE MORE VALUABLE HALF: re-measuring found THREE OF FOUR INPUTS to the old SS3.3 argument were
  WRONG, in both directions ***
    1. The menu holds 14-22 actions, NOT 50. Actions are constructed band-limited (SS2.2/SS4.4), so the
       menu is bounded by the number of RESOLVABLE WAVELET PLANES, not by the catalog of action kinds.
       This makes the UNBOUNDED arm far cheaper than claimed: ~28.6 min, not ~127 min.
    2. A DISCARDED candidate never pays for an extraction -- ingest_candidate runs guardrails BEFORE it
       fingerprints (1.217s vs 2.136s). The old arithmetic charged every attempt the full price, which
       is exactly what produced the over-budget ~23 min figure. The cap binds precisely WHEN CANDIDATES
       ARE CHEAPEST -- the opposite of what the old argument assumed.
    3. A branch cannot burn every attempt at full price; it stops at top_k kept, so the worst branch is
       top_k-1 scored plus the rest discarded, not max_attempts scored.
  CORRECTED BUDGET: bounded retry ~12.8 min worst case vs the 15-minute budget -- GENUINELY MET, and met
  by attacking cost with NO tunable adjusted. Unbounded stays rejected on its own merits at ~28.6 min.
  So SS3.3's CONCLUSION was right all along; only its NUMBERS were wrong.
  Two qualifications the implementer put IN THE SPEC rather than glossing: (i) the budget covers the
  SIDECAR's work only -- in production the executor is PixInsight over a file round trip (SS2.5), which
  is additive and UNMEASURED, so these are a FLOOR not a ceiling; (ii) it is a worst case in two
  independent senses at once, and the cost model over-predicts three real measured iterations by 9-12%,
  erring toward pessimism, which is the right direction for a budget.
  THREE CONCERNS -- controller triage:
  1. GUARDRAILS ARE NOW THE LARGEST SIDECAR COST (0.857s vs the fingerprint's 0.919s), with ~0.42s/candidate
     (~20%) of measured redundancy the implementer deliberately did NOT touch (guardrails.py is outside its
     file list and is a reviewed SS7 surface): mrs_noise_sigma called 4x per evaluation where 2 would do
     (0.229s), _gray 4x (0.093s), and check_hue_invention recomputing _hue_mass_by_bin(source) +
     rgb_to_lab(source) + _channel_ratios(source) for EVERY candidate in the whole run though `source` is
     invariant (~0.26s, needs a cross-candidate cache = a real architectural change to a SS7 module).
     Quantified so the decision is informed. NOT load-bearing -- the budget is met at 12.8 min without it.
     CONTROLLER RULING: PARKED, not scheduled. Restraint was correct; SS7 is the surface that stops the tool
     fabricating, and a 20% saving does not justify unrequested surgery on it when the budget is already met.
     -> raise to the user at branch close as optional follow-on work.
  2. The new test file costs ~108s of the suite's 233s, inherent to proving equivalence by running the loop
     to convergence twice. Trade FLAGGED rather than the proof quietly trimmed. ACCEPTED.
  3. PRE-EXISTING, NOT THIS TASK'S: under `-W error`, test_degrade / test_ingest / test_optimize_loop fail on
     ResourceWarning: unclosed database -- an unclosed sqlite3.Connection in the fingerprint store. Verified
     present on the PRE-change code by the same run. Clean under the project's actual `pytest -q` invocation.
     A REAL resource leak, not a test artifact. -> CARRY TO THE FINAL WHOLE-BRANCH REVIEW.
Task 16: task review dispatched (opus). Told the equivalence proof is the headline -- specifically whether the
  test compares genuinely INDEPENDENT old and new paths or the new path against itself, and whether "exact"
  equality is exact or a tolerance dressed as equality (any tolerance = a SS3.7 calibration decision reserved
  to the user). Also told to verify spec claims 1-3 against the CODE, not the prose.
Task 16: review LANDED (task-16-review.md, 26KB). Task quality APPROVED. NO Critical, NO Important.
  Reviewer independently RE-DERIVED the whole corrected budget table from the measured unit costs and
  reproduced every figure (12.79 / 15.49 / 6.41 / 9.65 / 28.61 / 31.38 min) -- the arithmetic follows
  from the corrected inputs. Spec claims 2 and 3 verified TRUE against the code; claim 1's NUMBER
  (14-22 actions) correct, its stated REASON only half right -> Minor.
  No tunable adjusted (SS3.7 satisfied). No existing test assertion altered -- confirmed by the reviewer
  that no existing test file appears in the diff at all.
  FOUR MINOR findings -> deferred to the final whole-branch review. The one worth naming:
  M1 -- THE EQUIVALENCE TEST CANNOT SEE THE ONE LINE OF ARITHMETIC THAT ACTUALLY MOVED.
    tests/test_fingerprint_cost.py calls energy_spectrum, which post-diff DELEGATES to the same helper
    the new path calls, so BOTH sides of the comparison run the changed arithmetic. The substitution
    `2.0 ** np.arange(n_scales)` -> `2.0 ** np.arange(len(planes))` is invisible to the proof -- and
    that expression IS the SS2.2 scale convention the task was forbidden to change. Asserted only
    relative to itself.
  CONTROLLER VERIFIED M1 DIRECTLY rather than accepting the Minor rating, because it touches a NAMED
  GLOBAL CONSTRAINT: starlet_transform returns planes of shape (n_scales, H, W) with the residual
  returned SEPARATELY as a second value (starlet.py:48-49, 59, 65). So len(planes) == n_scales holds
  BY CONSTRUCTION and cannot drift. THE SUBSTITUTION IS SAFE. M1's risk is real-but-unrealized:
  correct today, unproven by the test. Correctly rated Minor. Recorded so nobody re-derives it.
  (Review cited energy.py:161; the file is 77 lines and the line is energy.py:63 -- a diff-relative
  offset, immaterial, but it is why the controller checked rather than trusting the citation.)
Task 16: complete (commits ec6c662..8d8f727, review clean, 9 in-file / 422 repo-wide).
Task 12: dispatching, carrying the controller's pre-flight data findings so it does not rediscover them.
Task 12: implementer DONE (commit 4bc020c; 3 in-file / 425 repo-wide, baseline 422, nothing deselected,
  no warnings). Agent went idle MID-TASK with the file untracked and the report marked IN PROGRESS;
  controller saw the exact stopping point from the on-disk report and RESUMED it rather than
  re-dispatching, so the 43s test run and the analysis were not repeated. Fourth silent-idle this
  session and the first that cost anything.
  Reference resolved eso1103a, as pre-flighted. Cache path derived from discover.py's OWN naming
  convention ({entry_id}{suffix}), not from provenance and not hardcoded.
  MEASURED -- the first real evidence of SS10 behavior on cached reference data:
    flattened (strength=0.6): baseline 0.14296 -> best 0.08643 over 7 iterations, distance_history
      MONOTONE DECREASING at every step. recipe {black_point:3, local_contrast:2, chroma:1}.
      112 guardrail_log entries.
    undegraded: DECLINED. best == baseline == 0.017309, 1 iteration, EMPTY recipe, 10 log entries.
  *** CONTROLLER FINDING, sent to the reviewer as the named risk ***
  The flattened run converged via "every branch was discarded by guardrails or executor failure" --
  SS6.3 condition 3, the EXHAUSTION path -- while distance was STILL FALLING monotonically at the final
  step (0.08794 -> 0.08643). So the loop stopped because everything was rejected, WHILE IT WAS STILL
  IMPROVING. Consequences: (i) the measured best 0.08643 is a FLOOR on the loop's capability, not a
  measure of it; (ii) iteration_cap was never the binding constraint -- guardrails or the applied_kinds
  filter were. Two candidate causes with very different implications: guardrails rejecting nearly
  everything on NumpyExecutor output, vs the applied_kinds filter exhausting a 14-22 entry band-limited
  menu after 6 actions. Reviewer asked to judge which, and whether the TEST asserts anything about the
  convergence reason or silently accepts an exhaustion-terminated run as a healthy convergence.
  THE THREE OPEN CALIBRATION QUESTIONS -- status after this task:
  1. iteration_cap=20: STILL OPEN. Never approached (7 and 1 iterations). This fixture STRUCTURALLY
     cannot test it -- both runs converge early. Task 14 is where it gets exercised.
  2. local_contrast dominance (was 15/19): NOT REPRODUCED. Here 2 of 6; black_point dominates instead.
     A second data point, neither confirmation nor refutation.
  3. "no scale-denominated action" (was 16/25): NOT REPRODUCED as a pure case -- local_contrast is
     scale-denominated and IS present.
  NEW COVERAGE GAP, documented by the implementer in the module docstring (verify): the palette gate's
  chroma-drop path (SS2.3) is NOT exercised anywhere in this file. eso1103a is broadband RGB and every
  test compares it against its OWN fingerprint, so the palette ALWAYS matches. -> CARRY TO TASK 14.
  IMPORTANT FOR READING TASK 14: the undegraded baseline (0.0173) sits WELL BELOW the flattened run's
  best-achieved distance (0.0864). The criterion is therefore measurable IMPROVEMENT, not recovery to
  the pristine floor. Stated explicitly in the report at the controller's request.
Task 12: task review dispatched (sonnet) over 8d8f727..4bc020c.
Task 12: review LANDED. Task quality APPROVED. NO Critical. ONE Important (a WRITEUP fix, not code).
  Spec ✅. All three controller corrections verified honored IN THE DIFF TEXT. Cache-path convention
  independently traced to discover.py's _cached_image (lines 263-270, {entry_id}{suffix}) -- matches,
  not guessed. No pytest.skip anywhere, only pytest.fail carrying the Phase 1 remediation command.
  Palette-gate gap documented in the FILE, not just the report. Diff additive-only, no tunable touched.
  SS7.2 bullet 3 (condition-3 termination) already unit-tested synthetically at
  test_optimize_loop.py:210, so no coverage gap there.
=== THE EXHAUSTION MECHANISM, SETTLED -- reviewer RE-INSTRUMENTED the run standalone ===
  It is `max_attempts = 9`. NOT applied_kinds, NOT iteration_cap. Both of the controller's candidate
  causes were WRONG.
    - The ranked menu for this fixture holds 18 distinct (priority, band, kind) groups. max_attempts
      = 3 x top_k = 9, so EACH BRANCH ATTEMPTS ONLY HALF THE MENU before giving up.
    - applied_kinds withholds only ONCE_ONLY kinds (star_split, background_neutralize), NEITHER used
      here -- the full 18-group menu was available, UNSHRUNK, every iteration.
    - At iteration 7 all 3 branches hit the cap holding 0/3, with guardrail failures (noise_floor,
      star_integrity, shadow_clipping, highlight_clipping) escalating each iteration. Genuine
      guardrail pressure -- but 9 of 18 groups per branch were NEVER ATTEMPTED.
    - iteration_cap = 20 was never binding (7 and 1 iterations).
  => 0.08643 is a FLOOR SET BY THE BOUNDED RETRY, not evidence of the best achievable recipe.
     Reading "improved: true" as "found the optimum" would be WRONG.
*** TENSION THIS CREATES WITH TASK 16 -- FOR THE USER AFTER TASK 14, NOT NOW ***
  SS3.3 bounded the retry at 3 x top_k to avoid an unbounded cost believed to be ~127 min. Task 16
  RE-MEASURED that arm at ~28.6 min. So the cost justification for the bound is far weaker than when
  it was adopted -- and the bound is now demonstrably leaving HALF THE MENU unexplored under guardrail
  pressure, which is the very failure SS3.3 was written to prevent (the argument that motivated it was
  that local_contrast@2" ranks FIFTH and a short cap never reaches it).
  DO NOT ACT ON THIS YET. SS3.7 forbids adjusting a tunable without re-running all exit-criterion
  tests, and Task 14 IS that run. Establish the criterion under CURRENT tunables first, then decide on
  the evidence. Raise to the user at Task 14's close.
  CONTROLLER ACTIONS TAKEN:
  - task-14-brief-v2.md correction 4 REWRITTEN to name max_attempts as the binding tunable and to
    require reporting how many menu groups went unattempted when a run ends by exhaustion. (My own
    first draft of that correction repeated the iteration_cap misattribution.)
  - task12-impl resumed to correct the misattribution in its report only. No code, no tunable.
Task 14: brief v2 written and corrected (task-14-brief-v2.md), superseding task-14-brief.md, whose
  test_declines_on_a_cached_professional_render optimizes eso0104a.jpg against ESO1103A's fingerprint
  -- two different objects, contradicting its own docstring.
  Pre-flighted: /mnt/qnap/astro_data/prints/M42_hoo_2_18_23.jpg EXISTS (30 MB, Feb 2023).
Task 12: Important (writeup) ADDRESSED. task-12-report.md now attributes the exhaustion to
  max_attempts=9 against an 18-group menu, rules out applied_kinds, states iteration_cap=20 as never
  binding and still OPEN, and states the consequence plainly: 0.08643 is a FLOOR set by the bounded
  retry, not evidence of the best achievable recipe. No code and no tunable changed.
  (No commit: .superpowers/ is gitignored -- confirmed by the implementer via git check-ignore. The
  ledger and reports are scratch by design; git history is the durable record of CODE.)
Task 12: complete (commits 8d8f727..4bc020c, review clean, 3 in-file / 425 repo-wide).
Task 13: implementer dispatched (opus). It is the FIRST task to put real PixInsight processes in the
  loop, and it OWNS two things nothing else can produce:
    - the PixInsight file-round-trip cost, which Task 16's ~12.8 min budget explicitly excludes and
      calls "additive and unmeasured" -- so Task 13's wall-clock closes that gap;
    - the bit-depth decision (add a 16-bit reader, or ship .png and DECLARE it in both the report and
      the script's console output -- a declared 8-bit path is acceptable, a silent one is not).
  Expected headless result: PASS improved=false on the user's finished print. If it returns
  improved=true that is a REAL FINDING about the fingerprint or proposer -- record the proposed recipe,
  touch no threshold.
Task 13: implementer COMPLETE (commit 56935e3; 431 passed, baseline 425 + 6 new 16-bit TIFF tests,
  nothing deselected). A REAL headless PixInsight run was performed against the user's finished M42
  print. Nothing pushed.
Task 13: review LANDED (task-13-review.md, 21KB). APPROVED with follow-ups. NO Critical. Contract
  honored on all eight points; SS12's surface genuinely real.
=== THE FAIL-SAFE JUDGMENT -- reviewer traced it end to end and REJECTED the implementer's own defence
    while reaching a STRONGER conclusion ===
  SS10's fail-safe is INTACT; this run simply never exercised it.
    loop.py:89-97 best initialized to the ROOT branch (the proxy itself) at baseline_distance;
    loop.py:307 best replaced only by a candidate beating it by epsilon_improve;
    loop.py:703 improved = best.distance < baseline_distance - epsilon_improve.
    Both sides measured on the SAME footing -- sidecar.py:247,305 build ONE _optimize_loader(max_dim)
    used for the baseline AND every candidate. Finding 2's resampler divergence shifts baseline and
    every candidate by identical construction, so it CANNOT manufacture an improved=true.
  THE IMPLEMENTER'S DEFENCE WAS REJECTED: comparing a processed proxy against an unprocessed
    Pillow-downsampled master is precisely the incomparability Finding 2 itself identifies, so it
    cannot be evidence; and two GLOBAL tone/colour operations would not undo resampling artifacts in
    FINE STRUCTURE anyway.
  THE CORRECT EXPLANATION: the brief's improved=false expectation was a PREDICTION ABOUT THE IMAGE,
    not a requirement on the CODE. It assumed a finished print sits at the valley floor. IT DOES NOT --
    the print measures D ~ 0.23-0.25 from eso1103a. With that much headroom a 4.3% reduction from two
    conservative global operations is unremarkable, and DECLINING WOULD HAVE BEEN THE WRONG ANSWER.
  Two run facts sharpen it: the beam COLLAPSED (SS6.3 condition 3) rather than converging flat, and the
    attempt cap bound on ALL THREE iterations. The search never had breadth.
USER RULING (2026-07-28): REFRAME SS10 ON EVIDENCE, exactly as the Phase 0 exit criterion was handled.
  task-14-brief-v2.md rewritten accordingly:
    - decline case is now an image AT THE VALLEY FLOOR (a cached render vs its OWN fingerprint, D~0),
      which is what genuinely tests SS3.2's structural fail-safe;
    - improve case unchanged (flattened copy, same reference, same thresholds);
    - one-threshold-set pairing unchanged;
    - the user's finished print is MEASURED AND RECORDED but NOT asserted on, and written up as an
      explicit CALIBRATION FINDING FOR PHASE 3 with numbers: is D~0.24 right for a competent amateur
      print against a pro render of the same object, or an artifact of normalization / palette gating /
      cross-instrument comparison? Phase 2 does not fail for leaving that open, but it must be written
      down so Phase 3 inherits it rather than rediscovering it.
FOUR IMPORTANT findings -> Task 13 fix round 1 dispatched (opus):
  I-1 *** REAL BUG *** buildProxy resamples BEFORE promoting sample format
      (autocontrast_optimize.js:281-299), so on an 8-bit source PI resamples in an 8-BIT BUFFER and the
      later promotion just multiplies by 257. Measured on the run's own proxy.tif: 250 distinct values,
      ALL exact multiples of 257. The proxy is the baseline D AND the root parent of every branch -- so
      the file's own header comment justifying 16-bit candidates because "an 8-bit intermediate is an
      approximation" is contradicted by the one array everything descends from. A 3.3x bicubic downsize
      averages ~11 source pixels, so the lost precision is real information (same resize at native depth:
      ~15000 distinct levels vs 250). Fix is two lines; NOT a SS3.7 change. MUST RE-RUN -- this shifts
      baseline D and may NARROW Finding 2's 9% divergence, i.e. part of that 9% may be an 8-bit artifact.
  I-2 the report's 16-bit "proof" is not evidence: (value % 256 != 0) is AUTOMATIC for any 8-bit-promoted
      image since v*257 mod 256 == v mod 256. It returned 100% on provably 8-bit data. The 16-bit
      DECISION still stands (candidates: 499 levels, 495 not multiples of 257) but the evidence must be
      valid. Replace with distinct-level or % 257.
  I-3 load_raster's bit-depth heuristic (loaders.py:261-265) is now a LIVE SILENT-CORRUPTION path:
      divisor inferred from arr.max(), so a genuinely 16-bit candidate with max <= 255 comes out 257x
      TOO BRIGHT with no log and no error (SS12 forbids exactly that). Measured: uint16 constant 200
      (true 0.003052) loads as 0.784314. Pre-existing code, but THIS diff gave it a live caller.
      Fix: carry np.iinfo(dtype).max from _decode_tiff instead of inferring from data.
  I-4 a FALSE SAFETY CLAIM in a comment (autocontrast_optimize.js:424-427): "no more than 1% of pixels
      can be clipped by construction... refusing to need it". Every black_point@-/strong in the real run
      was discarded by shadow_clipping at 2.44 / 2.54 / 3.97 / 2.81% -- 2-4x the claimed bound. Behavior
      is safe (the guardrail worked); the CLAIM is false, and a construction-level bound in a comment is
      exactly what a later change will lean on.
OTHER LIVE-RUN FINDINGS (recorded, deliberately NOT fixed -- all SS3.7 calibration territory):
  - LocalHistogramEqualization's radius is bounded [16,512] px and RAISES rather than clamps, so at the
    print's proxy scale (4.665"/px) the four finest bands of the SS6.2 ladder are STRUCTURALLY
    unreachable. 6 of 9 executor failures. Will recur on essentially every image.
  - StarXTerminator SIGSEGVs headless EVERY time (3 of 3), so star_split cannot be applied headless at
    all on this install. Run continued correctly.
  - attempt_cap bound on EVERY iteration of the real run (1/3, 1/3, 0/3 live). THIRD independent
    confirmation that max_attempts=9 is the binding constraint -- now on REAL PixInsight output, not the
    NumPy approximation.
Task 15: brief written (task-15-brief.md). REFRAMED IN LIGHT OF TASK 13: SS3.6 was planned as a
  nice-to-have gap-filler, but Task 13's Finding 2 makes it the branch's most load-bearing check --
  the SAME image measures D=0.2264 downsampled by Pillow and D=0.2467 downsampled by PixInsight's
  Resample, a 9% shift in the number the whole system is built around, from nothing but the resampler.
  The entire search runs on a proxy and NOTHING currently checks whether proxy-derived decisions hold
  at full resolution. That is precisely what SS3.6 is for.
  ARCHITECTURAL CONSTRAINT written into the brief: advance() can replay locally through its Executor,
  but the BATCHED path cannot -- SS2.5 makes PixInsight the executor and the sidecar cannot call back
  into its parent, so a full-res replay needs its OWN instruction round-trip. Both paths must share ONE
  implementation of replay/measure/compare/log, per the Task 11 precedent (one search, two entry points,
  shared seam). A second copy is the worst outcome; df577e7 and Task 11's review both turned on that
  failure class.
  validation_interval and divergence_tolerance are NEW tunables, not adjustments -- introducing them
  does not violate SS3.7, but defaults must be justified and the per-checkpoint cost measured
  (Task 13 measured the print at 5318x3975 = ~127 MB per candidate at 16-bit, which is exactly why
  SS3.6 is PERIODIC and not per-iteration).
  Test requirement from the plan's own words: a deliberately scale-sensitive action MUST show
  divergence between an 800px proxy and the 1600px original -- plus the converse, that a
  scale-INSENSITIVE recipe must NOT trip the flag. A detector that always fires is as useless as one
  that never does.
ALL REMAINING BRIEFS NOW WRITTEN: 14 (v2, reframed on the user's ruling), 15, 17.
Task 13: fix round 1/5 landed (commit 7635c42; 433 pass). SECOND real headless PixInsight run performed.
  *** THE 9% RESAMPLER DIVERGENCE WAS NOT MOSTLY AN 8-BIT ARTIFACT ***
  With a genuine full-depth proxy the divergence narrowed only 9.0% -> 8.3%. About ONE TWELFTH was
  8-bit rounding; the rest is REAL and irreducible -- two legitimate resamplers genuinely disagree by
  ~8% on a fine-structure metric. This is a PERMANENT property of the system that both Task 14 and
  Task 15 must account for, not a bug awaiting a fix.
  | metric                         | before   | after    |
  | proxy baseline D               | 0.2467   | 0.24534  |
  | master D (Pillow downsample)   | 0.2264   | 0.2264 (UNCHANGED -- the control held)
  | Finding 2 divergence           | 9.0%     | 8.3%     |
  | final D                        | 0.2361   | 0.22820  |
  | discarded / noted / exec fails | 25/3/9   | 41/5/15  |
  NOTABLE: off a full-depth proxy the optimizer reached D=0.2282 against the master's own 0.2264 --
  it recovered nearly the ENTIRE proxy penalty, from a LARGER margin than before (-7.0% vs -4.3%).
  improved=true reproduced. That STRENGTHENS rather than undermines the reviewer's fail-safe analysis:
  the search really is finding genuine improvements against the reference, and the print really does
  sit ~0.23 away from eso1103a.
  The master D staying UNCHANGED at 0.2264 is meaningful evidence the fix was correctly scoped -- a
  PJSR-side proxy change cannot touch the sidecar's own Pillow downsample of the master.
  Fixer SELF-REPORTED one scope excursion (a Pillow-branch line in I-3), "argued, measured, and easy to
  revert", rather than hiding it. Correct behavior; handed to the re-reviewer to judge.
  Findings 1, 3 and 4 from the original run (LHE radius floor, StarXTerminator SIGSEGV, and the
  improved=true calibration question) are UNCHANGED and remain open exactly as filed.
Task 13: scoped re-review dispatched (sonnet) over 56935e3..7635c42.
*** CONTROLLER CORRECTION -- the 8-bit proxy was STARVING THE SEARCH, not just its precision ***
  I earlier recorded Task 13's first live run as a THIRD independent confirmation that max_attempts=9
  binds hard (1/3, 1/3, 0/3 live). That evidence was taken against the ACCIDENTALLY 8-BIT PROXY and
  OVERSTATED the case. At full depth the same run gives:
    candidates applied 18 -> 30; the beam held TWO live branches (br0 AND br1) in iterations 2-3 where
    the old run was br0 throughout; iteration 1's attempt cap reports "2 of 3 live" vs "1 of 3".
  Task 12's evidence is UNAFFECTED (NumpyExecutor, in-memory, no disk round trip). But ANY reading of
  beam breadth or attempt-cap frequency taken against the old proxy is UNDERSTATED, and the
  max_attempts case is weaker on real PI than I recorded. Memory file corrected too.
  ALSO: Task 16's budget must use 44.7 s / 30 candidates, NOT 31.6 s / 18.
  ALSO: the winning recipe got SIMPLER and BETTER -- CurvesTransformation tonal_reshape@-/strong ALONE
  (old: BackgroundNeutralization + CurvesTransformation), reaching a LOWER distance.
  I-1 fix confirmed on the new proxy.tif: 62443 distinct levels, 62198 not multiples of 257 --
  against 250 / 0 before. That is the valid form of the check I-2 said was missing.
  SCOPE EXCURSION (self-reported, well argued -- re-reviewer to rule): the fixer applied I-3's
  full-scale lookup to the PILLOW branch as well as _decode_tiff. Reason: fixing only TIFF would make
  a near-black uint8 .tif normalize by 255 while a byte-identical .png did not (the old rule skips
  division entirely when max <= 1.0, so a uint8 image of constant 1 returned 1.0 -- WHITE).
  loaders.py's own docstring says the post-decode path is shared "so the two decoders cannot come to
  disagree about what a loaded image is" -- a one-branch fix would install exactly that disagreement.
  Blast radius measured: for uint8 the rules differ only when max(arr) <= 1; no test changed behavior;
  the master's D is 0.2264 in BOTH runs, so the JPEG path is byte-for-byte unaffected.
  ruff on touched files: 1 pre-existing E702 before and after; repo-wide 28 before, 28 after.
Task 13: fix round 1/5 re-review LANDED -- ALL FOUR ADDRESSED, no new breakage.
  I-1 ADDRESSED (autocontrast_optimize.js:270, setSampleFormat moved above the Resample block at 272).
    Re-reviewer INDEPENDENTLY RE-DERIVED proxy.tif from the fixer's own /tmp artifact: 62443 distinct
    levels, 62198 not multiples of 257 -- matches the report exactly.
  I-2 ADDRESSED (task-13-report.md:73-94, the vacuous %256 claim struck IN PLACE, replaced with
    distinct-level / %257 evidence -- no invalid check left in the record).
  I-3 ADDRESSED (loaders.py:144-166,278,287-297; two new tests at test_raster.py:163-191 assert EXACT
    scaled values -- genuine proof, not path exercise).
  I-4 ADDRESSED (autocontrast_optimize.js:411-427, comment corrected, action code below unchanged).
  All four scope limits honored -- verified by grepping the diff: band ladder/LHE untouched, no tunable
  touched, loop.py and sidecar.py ABSENT from the diff (so improved=true was not tampered with), and
  the prior review's Minor findings M-1..M-4 left alone.
  SCOPE EXCURSION RULED JUSTIFIED -- KEEP IT (loaders.py:278). Fixing only _decode_tiff would make the
  two decoders disagree, which loaders.py's own docstring forbids. Blast radius provably narrow (only
  uint8/uint16 arrays with max <= 1), 433/433 unchanged, master D unchanged at 0.2264 across both runs.
  RE-RUN VERIFIED AGAINST THE ACTUAL ARTIFACTS ON DISK -- status.txt, report.txt, 30 cand-*.tif and
  proxy.tif all match the report's numbers to FULL PRECISION, including the 41/15/5 discard /
  executor-failure / noted counts. The master-D-unchanged control reasoning holds.
  improved=true reproducing with a LARGER margin (-7.0%) off a full-depth proxy is CONSISTENT WITH and
  STRENGTHENS the prior review's fail-safe conclusion, and the fixer's own data shows why: the 8-bit
  proxy was starving search breadth (1 branch / 18 candidates -> 2 branches / 30 candidates).
  CONTROLLER CORRECTION TO ITS OWN EARLIER NOTE: "about a twelfth of Finding 2 was an 8-bit artifact"
  is loose. The actual reduction 0.0202 -> 0.0189 is 6.4% of the original, closer to a FIFTEENTH.
  Conclusion unchanged, and if anything stronger -- even less of the 8% was fixable.
  No new breakage. ruff clean on loaders.py; the pre-existing E702 at test_raster.py:217 is untouched
  and unrelated.
Task 13: complete (commits 4bc020c..7635c42, review clean, 433 repo-wide).
  STILL OPEN from its live runs, all deliberately unfixed as SS3.7 calibration territory:
  LHE radius floor [16,512]; StarXTerminator SIGSEGV headless; the D~0.24 amateur-vs-pro calibration
  question (now Task 14's to RECORD, not assert); and the ~8% irreducible resampler divergence, which
  Task 14 must name and Task 15 exists to detect.
Task 14: dispatching against task-14-brief-v2.md (REFRAMED per the user's ruling), NOT task-14-brief.md.
=== SESSION LANDED (user: "just land it for tonight") ===
STATE: HEAD 7635c42, tree CLEAN, 433 tests passing, nothing deselected, NOTHING PUSHED.
Task 14 was dispatched and then STOPPED before it produced anything -- no commit, no report file, no
  test file. Nothing was lost and nothing half-finished is on disk. It must be dispatched FRESH.
COMPLETE THIS SESSION (each through implement -> review -> fix -> re-review):
  Task 10 (094133f..df577e7) | Task 11 (df577e7..ec6c662) | Task 16 (ec6c662..8d8f727)
  Task 12 (8d8f727..4bc020c) | Task 13 (4bc020c..7635c42)
REMAINING, ALL BRIEFED AND READY TO DISPATCH AS-IS:
  Task 14 -> task-14-brief-v2.md   (the SS10 gate. REFRAMED on the user's ruling. NOT task-14-brief.md,
                                    whose decline test compares two DIFFERENT objects.)
  Task 17 -> task-17-brief.md      (PJSR release compliance -- the defect is TOTAL: no repository/,
                                    no updates.xri, no sign.sh/build-packages.sh/install-local.sh,
                                    no #define VERSION in either shipping script.)
  Task 15 -> task-15-brief.md      (SS3.6 full-res validation checkpoint. Task 13's ~8% irreducible
                                    resampler divergence is what makes this load-bearing.)
  Then: the FINAL WHOLE-BRANCH REVIEW over 9b3c44b..HEAD (currently 40 commits, 33 files,
  +9390/-29), on the most capable model, pointed at the parked Minor findings in
  task-10-review.md / task-11-review.md / task-13-review.md.
BRANCH SCOPE: merge-base 9b3c44b. loop.py is the largest file at 719 lines (it holds BOTH advance()
  and Task 11's batched entry point) -- flag to the final reviewer.
  ASYMMETRY WORTH NAMING TO THE FINAL REVIEWER: pixinsight/autocontrast_optimize.js is the most
  environment-dependent component and has NO automated coverage -- it can only run under real
  PixInsight. Its entire verification is two headless runs recorded in task-13-report.md.
PROCESS NOTE FOR NEXT SESSION: every subagent this session went idle WITHOUT replying; all of them
  still delivered because the dispatch required the report be WRITTEN TO DISK AS IT GOES. Keep that
  instruction in every dispatch. Verify disk state (git log + report file) rather than trusting a
  status line -- one agent left work uncommitted and was resumed precisely instead of re-dispatched.
