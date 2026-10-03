# SDD ledger — plan: docs/superpowers/plans/2026-07-26-p1e-archive-discovery.md

Branch: p1e-archive-discovery (off master)
MERGE_BASE: e13acde1972452a28a0e29ae29d79940dbff469f
Execution order: 1, 2, 4, 3, 5, 6, 7, 8, 9, 10, 11, 12
  (Task 4 precedes Task 3 — Task 3's parse_detail imports Task 4's
   palette_class_from_gallery_bands; recorded caveat in the plan's self-review.)

Task 1: minor (deferred): implementer wrote impl+test in one batch, skipping the observed
  red ModuleNotFoundError state (brief Step 2). Self-disclosed; pure refactor of correct
  code. No code impact.
Task 1: minor (deferred): cones_overlap has no call site yet — intended consumer is
  index.py (Task 6). Correct scoping, not dead code.
Task 1: complete (commits e13acde..7ad809b, review clean — spec OK, quality approved)
Task 2: minor (deferred): detail_url() untested — plan-mandated (in brief's code block but
  not its interface list). Not dead: Task 7's crawler and Task 11's live test both call it.
Task 2: minor (deferred): search_url()'s empty-criteria ValueError branch untested —
  plan-mandated, verbatim brief code.
Task 2: resolved-by-controller: reviewer's "cannot verify live URL shapes from diff" —
  controller probed both sites live on 2026-07-26 and confirmed the asymmetry (Hubble
  requires /page/N/, ESO serves at the bare search path and returns nothing under /page/N/).
  Task 11's live-marked test asserts this programmatically. Not a gap.
Task 2: complete (commits 8fe06bd..fd14175, review clean — spec OK, quality approved)
Task 4: DONE_WITH_CONCERNS — implementer flagged that RGB-classification + strict-equality
  §2.3 gating denies chroma to HaRGB acquisitions. Controller verified: HaRGB user vs
  heic0601a (RGB) -> incompatible. Escalated to owner; owner chose the compatibility-matrix
  fix -> added as Task 12 (plan amended). Task 4's code needs NO rework.
Task 4: complete (commits fd14175..2cbb3f3, review clean — spec OK, quality approved).
  Empty-input asymmetry verified by reviewer: gallery_bands([])->'unknown' via its own
  guard, never delegating to palette_class_from_filters([])->'RGB'. Functions independent.
Task 3: complete (commits dbc66ee..39e0d52, review clean — spec OK, quality approved, zero
  findings). Authorized deviation: parse_filter_bands requires the Band cell itself be
  non-empty. Reviewer independently confirmed the real heic0601a table is 3 columns and ends
  with a malformed <tr> (empty Band/Wavelength, populated Telescope) that the brief's
  `[c for c in cells if c]` leaked in as a spurious 6th band "ACS". Fix validated.
Task 3: note — commit 0dd1916 (opo0205c license fix in data/seed_catalog.json) was made by
  the CONTROLLER, not Task 3's implementer, and is outside Task 3's reviewed diff. The
  reviewer's closing note misattributes it. Recorded here for recovery accuracy.
Task 5: implemented (commit 0dd1916..567b703, 209 passed). Review: spec OK, quality approved
  with 1 Important + 1 Minor.
Task 5: minor (deferred): parse_listing's `\{(.*?)\}` record split assumes flat records and
  pre-escaped apostrophes. Reviewer verified both hold in the two fixtures (brace counts
  match record counts; titles use &#39;), but it is fragile in general — a nested object or a
  genuinely unescaped ' would mis-split silently. Plan-inherited approach, not a defect.
Task 5: fix round 1/5 dispatched — Important: test_navigation_links_are_not_mistaken_for_
  results is vacuous (passes even with the guard deleted; nav links live outside the
  var images block). Also unexercised: malformed record missing id/url, and missing
  width/height yielding None not 0. Fix = synthetic-literal tests for all three paths.
Task 5: fix round 1/5 (1 addressed, 0 open — vacuous nav test replaced with 3 synthetic-
  literal tests; controller mutation-verified all three fail against targeted mutants;
  commits 567b703..24694c9)
Task 5: complete (commits 0dd1916..24694c9, review clean after 1 fix round)

Task 6: implemented, 12/12 task tests, 224 total, lint clean. Plan defect found and fixed
  at source (54bbcdb): RA-wrap test asserted an impossible overlap (12' sep vs 10' radii
  sum); implementer correctly refused to relax the test. Controller ruled: entry radius
  5.0->20.0 plus a new separation_arcmin ~= 12.0 assertion (the discriminating quantity —
  a wrap-broken impl yields 21588').
Task 6: committed by the IMPLEMENTER as 760ebe9 (it committed between the controller's
  status check and the controller's own commit attempt, which then found nothing staged —
  corrected here; the work is the implementer's, not the controller's). Controller verified
  independently: uses skymath (no reimplemented haversine), _COLUMNS derived from
  dataclasses.fields, parsed_ok INTEGER->bool round-trip, separate DB file. Lint clean.
  Implementer's completion report arrived after the controller's wrap-up: status DONE,
  760ebe9, 224 passed (212 + 12 new), ruff clean, no concerns. Agrees with the
  controller's independent verification. task-6-report.md records the RA-wrap ruling.
Task 6: *** REVIEW NOT RUN — FIRST ITEM ON RESUME ***
  Resume command:
    scripts/review-package docs/superpowers/plans/2026-07-26-p1e-archive-discovery.md \
      54bbcdb HEAD
  then dispatch task-reviewer-prompt.md with task-6-brief.md + task-6-report.md.

=== SESSION STOP 2026-07-27 ===
Branch p1e-archive-discovery, 224 tests passing, working tree clean. master untouched.
Done+reviewed: Tasks 1, 2, 4, 3, 5.  Done+unreviewed: Task 6.
Remaining: 7 (crawler+sync), 8 (ranking), 9 (verification gate), 10 (e2e miss path),
           11 (CLI + live drift test), 12 (chroma compatibility relation).
Execution order for the rest: 7, 8, 9, 10, 11, 12.
