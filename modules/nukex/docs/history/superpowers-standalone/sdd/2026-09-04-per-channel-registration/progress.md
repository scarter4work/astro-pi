# SDD ledger — plan: docs/superpowers/plans/2026-09-04-per-channel-registration.md

Spec: docs/superpowers/specs/2026-09-04-per-channel-registration-design.md (binding authority)
Branch: v5-channel-registration, created from main at 45a801f.
Ruling: created a branch before any implementation — the session was on `main`,
  and the plan's own release rules forbid working there. Cost if wrong: a
  branch to delete.
MERGE_BASE = 45a801f

## Pre-flight scan

Cross-task pairs (shared file or interface):

| pair | produced -> consumed | finding |
|---|---|---|
| T1 -> T2, T5 | `default_reference_channel(int)` | signature matches at all 3 sites |
| T2 -> T3 | `fit_channel` in channel_registration.cpp | **ordering hazard**: T2 defines a stub version, T3 replaces it in place. Must be defined ABOVE `measure_channel_transforms` or T2 will not compile. Already called out in the plan's own self-review. |
| T2 -> T4 | `ChannelTransforms`, `ChannelTransform::apply` | consistent. No include cycle: channel_registration.hpp -> star_detector.hpp -> types.hpp/image.hpp, never homography.hpp |
| T2 -> T5 | `measure_channel_transforms`, `ChannelRegistrationConfig` | consistent |
| T4 -> T5 | 5-arg `warp` overload | consistent |
| T5 -> T6 | `AlignedFrame::channels` | consistent |
| T2, T3, T6 | all append to test_channel_registration.cpp | additive only |
| T2, T6 | both edit channel_registration.hpp/.cpp | T6 is purely additive (`describe_channel_transforms`) |
| T7 | consumes everything; goldens + version header | no code interface |

Within-task self-consistency, checked:

| task | checked | finding |
|---|---|---|
| T1 | test vs config plumbing | consistent |
| T2 | aggregate-init order vs declared member order (s,tx,ty,n_stars,residual,fit); star grid positions vs radius-6 boxes; negligible() arithmetic | all correct, arithmetic verified by hand |
| T3 | isolation-test star count 25+3-6 == n_before-3 | correct |
| T3 | outlier-clip test is actually passable | **simulated**: unclipped error 0.104 px, clipped 0.0001 px, 24/25 kept. Test bites (fails without the clip) and passes with it. |
| T4 | green-untouched assert vs warp's edge behaviour | passes: test image is 0 at the edges the bounds check drops |
| T5 | near-identity path clears `channels`, so the bit-identical assert holds | correct |
| T6 | snprintf output vs the substrings the test greps | "+238 ppm" contains "238 ppm"; no line contains "ch1" |
| T7 | frozen golden verified before the others are re-baselined | correct order |

No conflicts requiring a ruling. Proceeding to Task 1.

## Progress

Task 1: dispatched (sonnet), BASE=45a801f. Briefs for tasks 2-7 pre-extracted.
Task 1: implemented, commit 44266e3, 72/72 suite pass. Review dispatched (sonnet) over 45a801f..44266e3.
Task 1: complete (commits 45a801f..44266e3, review clean, spec compliant)
Task 1: minor (deferred): frame_aligner.hpp:32-33 doc comment still says "channel 0 is used"
  for star detection. Outside Task 1's scope. Task 5 edits this file -- carry it there.
Task 2: dispatched (sonnet), BASE=44266e3.
Task 2: implemented, commit 44645ef, 73/73 suite pass. Warning reported: fit_translation unused (Task 3 wires it). Review dispatched (sonnet) over 44266e3..44645ef.
Task 2: review spec-compliant. Fit maths independently re-derived by the reviewer and
  confirmed correct, including that OLS is centering-invariant so the internal
  re-centering on the sample mean does not change the result.
Task 2: Ruling: the plan-mandated Important finding (fit_translation defined but unused,
  producing a build warning) is accepted as-is rather than papered over with
  [[maybe_unused]]. Task 3 is the immediately following dispatch and consumes the
  function, so the attribute would be added and removed within one task, and an
  attribute left behind after the function is used would itself be wrong. Verified
  no -Werror anywhere in the build, so it cannot break anything.
  VERIFY AT TASK 3: the warning must be gone. If Task 3 does not consume
  fit_translation, this stops being transient and becomes a real finding.
  Cost if wrong: one commit in branch history builds with one warning.
Task 2: minor (deferred): no test covers min_neighbour_separation, min_star_snr, or the
  below-min_stars_affine fallback. All three are plan-mandated gaps -- Task 3's tests
  cover exactly these three paths, so expect this to close there rather than at merge.
Task 2: complete (commits 44266e3..44645ef, review clean, 1 plan-mandated finding ruled on)
Task 2: minor (deferred): channel_registration.cpp:372 -- the 1e-9 denominator guard's
  magnitude is uncommented. Cosmetic.
Task 3: dispatched (sonnet), BASE=44645ef.
Task 3: implemented, commit 1121569, 13/13 cases, 73/73 suite. RED matched the predicted
  failures exactly. fit_translation warning CONFIRMED GONE -- the Task 2 ruling's
  verification condition is satisfied, that finding is closed rather than carried.
Task 3: review dispatched (sonnet) over 44645ef..1121569.
Task 3: complete (commits 44645ef..1121569, review clean, no Important findings)
Task 3: reviewer verified structurally that no path falls through to a partial correction,
  that plausible() gates all three fitted-return rungs, and that n_stars/residual are set
  on every non-identity return (no stale counts reported to the user as fact).
Task 3: minor (deferred): only 2 of the 3 gaps carried from Task 2 actually closed.
  Neighbour-isolation and sparse-star fallback are now tested; the PER-STAR SNR GATE
  still has no boundary test. The "star invisible in one channel" test erases signal
  entirely, which is a different condition from present-but-below-threshold.
  This is a brief-level gap, not an implementer deviation. Carry to final review.
Task 3: minor (deferred): two untested corners -- (a) clip drops survivors below
  min_stars_affine, where the code reports the UNCLIPPED fit rather than degrading to
  translation-only; (b) MAD-degenerate sigma==0 with a real outlier, where clipping is
  skipped entirely. Both are plan-mandated behaviour, both backstopped by plausible().
Task 4: dispatched (sonnet), BASE=1121569.
Task 4: implemented, commit 6a08f01. test_homography 9/9 cases (52661 assertions),
  73/73 suite, module .so links. Review dispatched (sonnet) over 1121569..6a08f01,
  with the coordinate-ordering memory-safety check named as its first priority.
Task 4: Ruling: pre-existing non-ASCII characters in unrelated homography.cpp comments
  (lines 79/80/101/318) are left alone. The ASCII rule exists because PCL reads a
  const char* as ISO-8859-1, so it binds string literals reaching the Process Console,
  not comments, and these predate this branch. Cost if wrong: nothing at runtime;
  a cosmetic cleanup someone does later.
Task 4: review spec-compliant, no Critical, no Important. The memory-safety check passed
  on traced code, not on the report's claim: the channel transform sits strictly after
  sx/sy are derived from H_inv and strictly before both the isfinite and bounds checks,
  so a coordinate pushed out of frame by the channel shift is rejected before any read.
Task 4: reviewer also established the empty-transforms test is a STRUCTURAL guarantee
  rather than a spot check -- with empty transforms apply_ct is false on every channel,
  so the two overloads execute identical instructions on any input, not just the tested
  one. And the circular-include risk is closed: nothing in channel_registration.hpp's
  transitive include chain reaches homography.hpp (types.hpp DEFINES HomographyMatrix
  rather than including the header that uses it).
Task 4: minor (deferred): a continuation-line indent shifted by one space in the 4-arg
  warp signature. Reviewer judged it net-positive and needing no action.
Task 4: complete (commits 1121569..6a08f01, review clean, no Important findings)
Task 5: dispatched (sonnet), BASE=6a08f01. Carrying Task 1's deferred minor into it --
  frame_aligner.hpp's doc comment still claims star detection uses channel 0, and this
  is the task that edits that file.
Task 5: Ruling: the brief's test fixture is changed, deviating from "use the code verbatim".
  The implementer root-caused a failing test with an independent diagnostic calling only
  StarDetector/StarMatcher/HomographyComputer -- no Task 5 code -- and found 13 matches of
  which only 6 had the correct offset, below min_matches=8, so alignment failed before
  channel registration was reached.
  WHY: the fixture placed stars on an exactly periodic 5x5 lattice. Triangle-similarity
  matching is degenerate on a lattice, because many triangles are congruent to a triangle
  in a different cell, so the matcher pairs stars across cells. Real star fields are never
  periodic. This is a defect in the fixture I wrote, not in StarMatcher and not in the
  align() rewrite. StarMatcher and HomographyComputer are NOT to be touched.
  DECIDED: break the periodicity with a deterministic (no-RNG) sinusoidal offset, and move
  the grid origin inward at the same time -- plain jitter on the old origin would have
  pushed the star that the reference-frame tests measure partly outside their fixed
  centroid window and biased the result. Exact constants supplied and checked: 75 px pitch,
  measured star stays inside its window, one star per window, furthest star clear of the
  frame edge and the 6 px centroid margin, adjacent stars 57 px apart against a 13 px
  isolation threshold, both frames differing only by the global shift.
  Cost if wrong: a test fixture that is less representative than intended. The production
  code is untouched by this ruling.
Task 5: implemented, commit be9c546 (includes the ruled fixture fix, documented in the
  commit body). 11/11 test_frame_aligner cases, 73/73 suite. Review dispatched on OPUS
  rather than sonnet -- this is the diff whose failure mode does not surface as a test
  failure, so it warrants the capability.
Task 5: minor (deferred): the failed-alignment branch has no dedicated test. The
  implementer flagged this itself rather than claiming it proven. It is a narrow mirror
  of the tested reference-frame branch. Carry to final review.
Task 5: review traced all four named risks from control flow and all four pass.
  (1) All three image-producing paths apply the correction, and they share ONE
      channels_matter gate computed above every branch -- so the failure mode this task
      exists to prevent cannot be reintroduced by editing a single arm.
  (2) Mono no-op is byte-identical, not approximate: the 4-arg warp IS the 5-arg warp
      with empty transforms, same function, so no resample can be introduced.
  (3) The near-identity skip cannot skip a frame that needs correction. max_displacement
      ADDS the scale and translation terms rather than combining them, so it is an upper
      bound that cannot cancel below the true displacement, and the corner radius used
      exceeds the true max radius from the fit centre. Both approximations err safe.
  (4) Reference-frame output geometry cannot disagree: adopt_reference sets the stored
      dimensions from the same frame, and the pre-change behaviour was also the frame's
      own dimensions.
Task 5: reviewer proved the reference channel is not resampled BY CONSTRUCTION rather than
  within tolerance -- identity transform, identity H, so the bilinear weights reduce to
  1 and 0 and the interior is bit-identical. The acceptance criterion's "green FWHM must
  be unchanged" is therefore structural, not a tolerance to be checked.
Task 5: Ruling: the reviewer's one warning item, that only the controller could confirm
  the 0.058 px M3 figure quoted in a code comment, is RESOLVED, not deferred. I measured
  it earlier in this session on the user's 53-frame stack, 245 stars at the 99.9th
  percentile in green, and it is recorded in the design document. It is the centroid-noise
  floor rather than a colour error, because through that filter green and blue both image
  near 500 nm.
Task 5: fix round 1/5 dispatched (resumed original implementer), 2 Important findings.
Task 5: Ruling: finding 1, warping the reference frame zeroes its last row and column,
  is fixed IN warp by clamping the base index rather than by special-casing the identity
  path. Bilinear needs the +1 neighbour, and the correct handling is to clamp to the last
  valid cell rather than refuse the sample, so this is a defect in warp itself and the fix
  belongs there. Special-casing the identity path would have been a workaround for a bug
  left in place. At the edge fx reaches 1.0, which weights the +1 neighbour fully, so the
  edge value is exact rather than approximated. Cost if wrong: warp's edge behaviour
  changes for every frame, which moves the Bayer goldens slightly further than the feature
  alone would -- they are being re-baselined in Task 7 regardless.
Task 5: Ruling: finding 2, erasing result.channels on the near-identity skip, is a defect
  in MY PLAN and is corrected. The Fit enum exists so a degraded frame is distinguishable
  from a clean one; erasing the measurement makes a frame that gave up report identically
  to a frame with nothing to fix, and Task 6 is the reporting task that consumes it. The
  measurement is now kept and only its application declined. Cost if wrong: none
  identified -- no pixel-level behaviour changes.
Task 5: OUT OF SCOPE DEFECT FOUND, do not fix here: the stacker feeds every warped pixel
  into the Welford accumulator and histogram with NO no-data guard
  (stacking_engine.cpp:528-536), so out-of-frame zeros from any warp count as real
  samples. This contaminates the overlap border of every dithered stack, not just one
  edge. Much larger than this plan. Promote to memory as FIRST UP at closeout.
Task 5: review verdict Approved, with the 2 Important findings already in fix round 1.
Task 5: Ruling: two Minor findings were folded into the in-flight round rather than
  deferred, because a round was already dispatched so it costs one dispatch either way,
  and one of them closes a coverage gap on a path this task actually changed.
  (a) The failed-alignment path gets a real test. The implementer flagged the gap and
      dismissed it as "mirrors a tested branch"; I accepted the flag and rejected the
      dismissal. It is the only path whose output geometry is the frame's own dimensions,
      and one of only two that changed from clone to warp.
  (b) A stray mid-file include in the frame aligner test moves to the top.
Task 5: minor (deferred to final review): the detection channel and the channel-registration
  reference channel are derived from two separate expressions. If a caller ever sets
  StarDetector::Config::channel, the catalog would hold e.g. red positions while the
  correction pins to green, and nothing would complain. Nothing sets it today outside one
  detector test. Held for the whole-branch review so the detector interaction is judged
  as a unit rather than piecemeal.
Task 5: fix round 1/5 (2 addressed, 0 open; commit be9c546..d64723a). Edge test watched
  RED first (0.0f vs 0.226804122f) before the warp clamp went in.
Task 5: Ruling: accepted the implementer's deviation from my literal instruction. I asked
  for `channels_matter ? result.channels : ChannelTransforms{}` at all three warp call
  sites; it applied the ternary at one and argued the other two are reachable only from
  inside a channels_matter-gated branch, making the ternary provably dead there. It is
  right, and it said so rather than quietly complying. Cost if wrong: none -- the
  behaviour is identical either way.
Task 5: fix round 2/5 dispatched. The round-1 addendum (failed-alignment test, stray
  mid-file include) did not land -- VERIFIED AGAINST THE TREE, not the report: 11 TEST_CASEs
  unchanged and the include still at line 170. My message most likely arrived mid-round.
  Re-sent both.
Task 5: fix round 2/5 done. Addendum landed and VERIFIED AGAINST THE TREE: 12 TEST_CASEs
  (was 11), stray include gone. New test uses a 3-star frame against a 25-star reference,
  under the matcher's 8-match minimum; the implementer ran it with -s to confirm
  alignment_failed is genuinely true rather than assuming it, and the corrected offset
  measures 0.00017 px against a 0.06 band.
  The implementer amended d64723a into 46ad8ac rather than adding a commit. Safe here --
  the branch has no upstream and nothing had been pushed.
Task 5: scoped re-review dispatched over be9c546..46ad8ac, covering all 4 findings plus
  two things I want independently checked: (a) whether the accepted dead-ternary claim is
  actually true of the code, since if either call site IS reachable with channels_matter
  false then finding 2 is not addressed and my acceptance was wrong; (b) whether the warp
  clamp is correct for a NON-identity homography too, since that change now affects every
  warped frame rather than only the reference.
Task 5: fix round 2/5 re-review: all 4 findings ADDRESSED, no new Critical/Important
  breakage. My accepted dead-ternary deviation was independently VERIFIED TRUE: both
  remaining call sites are reachable only under channels_matter == true, so the ternary
  is a tautology there. The clamp was confirmed correct for a non-identity homography
  too -- at sx == sw-1 the clamp forces fx = 1.0, giving full weight to the sw-1 sample,
  so the edge is the exact source value rather than a blend, and x and y resolve
  independently so the corner is exact as well.
Task 5: complete (commits 6a08f01..46ad8ac, 2 fix rounds, all findings addressed)
Task 5: minor (deferred): the width()<2 guard has no dedicated test.
Task 5: minor (deferred): a per-channel shift can still push one channel's edge pixel out
  of frame, leaving that channel zero at the extreme edge. This is arguably correct --
  there IS no data there for that channel, and inventing some would be worse -- but it
  compounds with the stacker's missing no-data guard already logged above.
Task 6: dispatched (sonnet), BASE=46ad8ac, WITH A RULED AMENDMENT to the brief.
Task 6: Ruling: the Task 5 finding-2 fix invalidated the brief's engine snippet, and I am
  amending rather than letting it ship. AlignedFrame::channels is now populated whenever a
  colour frame was measured, applied or not, so the brief's `if (!channels.empty())` guard
  -- previously false on a skipped frame -- is now true for every frame. As written it
  would print a per-frame console line reporting a few ppm that was never applied, and the
  brief's own comment claiming "silence means the skip did its job" would become false.
  DECIDED: (a) promote kNegligibleChannelShiftPx from frame_aligner.cpp's anonymous
  namespace into channel_registration.hpp, ONE definition -- two copies that drift would
  make the console lie about what was applied; (b) the engine reports when the correction
  was applied OR when the fit gave up on any non-reference channel. The second condition
  is the point: a frame that could not be measured is negligible BY CONSTRUCTION and would
  otherwise be indistinguishable from a frame with nothing wrong, which are opposite
  situations. Cost if wrong: console either too quiet or too noisy; no pixel effect.
Task 6: implemented, commit 5b46ead. 16 cases / 173 assertions, 73/73 suite.
  Amendment implemented; kNegligibleChannelShiftPx grep-confirmed to have exactly one
  definition, referenced by both frame_aligner.cpp and stacking_engine.cpp.
Task 6: Ruling: the implementer found pre-existing em-dashes in stacking_engine.cpp and
  flagged them as out of scope. I am fixing them in this task instead. I verified the
  extent myself: 3 hits, of which lines 563 and 664 are LIVE console strings shipping
  mojibake to users in the released build, on the unknown-filter and blown-out-frame
  paths. It is the exact defect class this task's own test guards against, in a file this
  task already edits, and it is a three-character change. Project rules forbid filing a
  surfaced follow-up as a note rather than dispatching, promoting or dropping it, and
  there is standing guidance that removing an artefact without fixing its cause is how a
  defect returns -- the cause here was simply that nobody grepped. Also asked for a
  repo-wide grep including ESCAPED byte sequences, which hide from a naive search.
  Cost if wrong: a three-character diff in a file already under review.
Task 6: em-dashes fixed, commit amended to c62ba8a, 73/73. The implementer went further
  than the grep I asked for and wrote a string-literal-aware scanner that distinguishes
  literals from comments and catches \xHH, octal and \uXXXX escapes. That was the right
  call: this codebase legitimately uses non-ASCII in comments, so a byte-level grep over
  whole files is all false positives.
Task 6: review dispatched (sonnet) over 46ad8ac..c62ba8a.
Task 6: Ruling: the scanner found TWO MORE live shipping non-ASCII bugs outside this
  task's files, and I dispatched a fix rather than filing them.
  (1) channel_decomposer.cpp:35 -- em-dash in the singular-Q-matrix exception message.
  (2) NukeXProgress.cpp:117 -- six HEX-ESCAPED U+2550 box characters in the banner
      printed at the start of EVERY phase of EVERY run. Being escaped is exactly why it
      survived; a search for visible characters cannot see it. This is the most visible
      of all of them and it has been shipping.
  Both verified by me directly before dispatching. They go in their OWN commit, not
  folded into Task 6, so Task 6's review diff stays clean. Cost if wrong: a small
  independent commit that is easy to revert.
Task 6: complete (commits 46ad8ac..c62ba8a, review clean, no Critical/Important findings)
Task 6: reviewer independently confirmed the reference-skip in the gave-up condition is
  NECESSARY, not just present: measure_channel_transforms leaves the reference entry as
  Identity by construction, so omitting the skip would have made the gate true on every
  colour frame and the console would have reported every frame.
Task 6: minor (deferred): the `if (!desc.empty())` guard is dead given `applied || gave_up`.
  Implementer flagged it itself and left it as brief-inherited defensive code.
Task 7: Ruling: the acceptance criterion is changed from an absolute pixel bar to a RATIO,
  committed as 98c0170, BEFORE dispatching the multi-hour run rather than after it fails.
  I validated the measurement script against the shipped pre-fix stack still on the
  Desktop and got 0.354 px red-green with an 0.081 px blue-green floor, where the spec
  said 0.435 and 0.058. The data did not change -- the estimator and the star sample did,
  while the spec was being drafted (1047 stars with the final radius-6 median-background
  2-iteration estimator, versus 245 stars with an earlier one).
  That is precisely the fragility of an absolute threshold: it encodes whichever estimator
  measured it, and the original 0.10 px bar sat only 0.04 above a floor that has since
  moved to 0.081. A run could have missed a bar it was never really compared against.
  Blue is a control -- identical centroid noise, almost no colour error to correct -- so
  the ratio isolates the effect and survives an estimator change. 4.4x today, bar 1.5x.
  Cost if wrong: the bar is looser or tighter than intended in a way the reported absolute
  numbers still expose, since both are printed.
Task 7: dispatched (sonnet), BASE=98c0170. Measurement script pre-validated by me against
  real data and handed over working, rather than as the stub the plan left.
ascii-fix: complete, commit 5b11692. Both live bugs fixed. The agent's literal-aware
  scanner reports zero remaining non-ASCII inside string literals across src/, and I
  independently confirmed it with two separate greps: none inside literals, and no
  escaped high bytes anywhere. The banner is now "=== %s (%d steps) ===". The ~65 files
  still carrying non-ASCII do so only in comments, which is harmless.
  Note this was found only because the scan covered ESCAPED bytes. A character-level
  search cannot see \xe2\x95\x90, and that is the one that was on every user's screen.
Branch state verified clean: 9 commits on v5-channel-registration from 45a801f.

## Open-items closeout (user instruction: stop leaving open items)

Ruling: the stub memory file project_stacker_no_data_guard.md was DELETED. Writing it
  violated the project rule against filing "X should look at this" entries. The defect is
  being fixed now instead.

Stacker no-data defect: ROOT-CAUSED AND MEASURED before any fix, per the Iron Law.
  Code: the four accumulation loops walk every pixel and call route_sample_idx, which
  calls welford.update() and histogram.update() unconditionally. A warp leaves 0 outside
  the source's coverage, and 0 is a legal pixel value, so nothing downstream can tell a
  measurement of darkness from an absence of data.
  Measured on the user's shipped M3 stack (6072x4042, 53 frames): 268802 exactly-zero
  pixels; the ring at depth 1 sits at 48.6% of interior brightness; contamination reaches
  48 px deep all round, which is the session's dither and drift excursion. The visible
  black edge is the least of it -- depths 3 to 48 are merely too dark and look plausible.
  Design ruled: explicit per-channel CoverageMask, NOT a NaN or negative sentinel. I
  traced the consumers first: the aligned image goes to a disk cache via write_frame and
  to compute_frame_median, so a sentinel would leak into the cache and into Phase B.
  Per-channel is required rather than nice-to-have, because channel registration can push
  one plane out of the source while the others stay in.
  Dispatched in an ISOLATED WORKTREE so Task 7's multi-hour E2E run is not disturbed.

Deferred minors: NOT deferred to memory or to a final review. Queued to run immediately
  after the stacker fix merges, because they touch the same files (frame_aligner.cpp,
  stacking_engine.cpp, homography.*) and parallel worktrees would collide:
  1. channel_registration.cpp: the 1e-9 denominator guard's magnitude is uncommented.
  2. No boundary test for the per-star SNR gate. The existing test erases signal entirely,
     which is a different condition from present-but-below-threshold.
  3. No test for the clip-drops-below-min_stars_affine corner, nor the MAD-degenerate
     sigma==0 corner.
  4. No test for warp's width()<2 guard.
  5. The detection channel and the registration reference channel are derived from two
     separate expressions; if a caller ever sets StarDetector::Config::channel they
     disagree silently. Fix by deriving both from one place.
  6. stacking_engine.cpp: the `if (!desc.empty())` guard is dead given `applied || gave_up`.
