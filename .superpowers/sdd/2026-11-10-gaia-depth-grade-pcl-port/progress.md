# HANDOFF (2026-11-12, run stopped on Scott's "lets land this")
- Branch heads (pushed): gdg/phaseF-process = d89feead (Tasks 16-17); gdg/phaseH-nebula = 3bc84301 (merge of phaseF into phaseH; Tasks 16, 17, 21-24 done) + this handoff commit.
- Gate on 3bc84301: container build-modules exit 0 (NukeX 85/85, Planetary 28/28, GDG 33/33), verify-portable exit 0 (4 .so, ubuntu 22.04/debian 12/rocky 9); host ctest 33/33. Note: build-modules.sh builds with -j$(nproc) in the container, not -j8.
- Done: Tasks 16, 17 (reviews clean; ledger in the phaseF worktree .superpowers/sdd/.../progress.md), 21-24 (earlier).
- Task 25: PARTIAL, uncommitted. Left in this worktree's tree AND backed up at ~/projects/backups/gdg-task25-partial-2026-11-12/ (BASE 3bc84301; tracked.patch + untracked.tar.gz + status.txt + task-25-report.md). Restore: `git apply tracked.patch && tar xzf untracked.tar.gz` on 3bc84301.
  - .scp finding (verified by the implementer): `-r=<case>.scp` under --automation-mode runs `GaiaDepthGradeProbe --case=... --out=...` once the probe has CanProcessCommandLines; --force-exit exits 0 in ~6 s. PI exits 0 even when a command throws and the console is not on stdout -> errors routed via GDG_TEST_LAST_ERROR.
  - run-probe-recon.sh: 5/9 pass (scp, CLI files GLB+GIF 36 frames, execute_on refused, ...); failing: read-only string outputs (GetParameterValue API error), emissivity XISF round-trip max|d| 5.7e-4, probe table-column addressing (gaiaDatabaseFilePath), process_category jq (enum elements unavailable). Not done: doc page, release.sh GDG_DOC_IDS, ctest/run-probe.sh/container gate. Exact next step: task-25-report.md "Exact next step".
- Next, in order: finish Task 25 (above) -> review -> Task 26 (brief: task-26-brief.md) -> module ASan pass (host build-asan-module/ with -fsanitize=address,undefined -fno-omit-frame-pointer -j8; run Task 25/26 headless probes via pi-headless with LD_PRELOAD=$(gcc -print-file-name=libasan.so), ASAN_OPTIONS=detect_leaks=0, then a separate leak run filtered to our frames; fix by TDD; delete build-asan-module/).
- Open issues:
  1. Online mode (Task 13 domain): ESA archive caps anonymous async jobs at executionDuration 60 s; Bailer-Jones at default G<18 on the 0.74 deg NAS field exceeds it (G<14: 19 s). Fix = tile the TAP query (changes query+cache semantics) — Scott's scope call. Offline default unaffected.
  2. Deferred minors: Task 16 no_network positive control only with GDG_LIVE_TAP=1; build_adql runs before the cache lookup. Task 17: Browse error path, busy scope exception path and SXT/Duplicate failure-path closes have no observing test (fault injection needed).
  3. Parked from 21-24: R15/R17 minors (see below).
  4. Plan C22 (apparent-coordinate retry, exhaustive matching) not ported — Task 19 checklist.

# SDD ledger — plan: /home/scarter4work/projects/astro-pi/.claude/worktrees/gdg-phaseH-nebula/docs/superpowers/plans/2026-11-10-gaia-depth-grade-pcl-port.md
Scope: Tasks 21-24 (core). Branch gdg/phaseH-nebula = gdg/phaseG-recon ff-> gdg/phaseE-solver (530c8a32).
Tasks 25-26 blocked: Task 16 (RunPrepare/PrepareSession/StarSplit/GaiaDepthGradeProcess) not committed on any branch (checked all refs 2026-11-12).
Spec: docs/superpowers/specs/2026-11-10-gaia-depth-grade-pcl-port-design.md + 2026-11-05-nebula-3d-depth-reconstruction-design.md
Baseline: host build (-DGDG_BUILD_MODULE=ON, ~/PCL 2.13) ctest 24/24 at 530c8a32.

## Pre-flight scan (Tasks 21-24)
| Pair / task | Produces vs consumes | Finding |
|---|---|---|
| 18 -> 21 | CavityModel, ReconConfig render_* fields, reconstruct_cavity, from_mono_sho, ionization_ratio, dust_from_structure, ionizer_from_xy | all present with these names (grep) |
| 3/4 -> 21 | gaussian_filter, percentile | present (Numerics.h) |
| 21 self | dust_occludes asserts out <= 0.5E where dust>=0.5, but out = E*shade*limb*T and limb up to 1+render_limb_gain=1.5 | CONFLICT -> R1 |
| 21 self | degenerate_is_identity: z const -> z_rel 0 -> shade 1; grad 0 -> no g>0 -> limb 1 | consistent |
| 18/7/11 -> 22 | ionizer_table(StarRow), extract_channels, Log, StarRow | present |
| 21 -> 22 | Blister used in tests | ok |
| 18/22 -> 23 | cavity_meta_json, RECON_HONESTY | present / produced by 22 |
| 23 self | grid counts 9x7 d2 -> 20 v / 72 idx; d1 -> 63 | arithmetic checks; 6072x4042 d1 ~1.9 GB < 4 GiB |
| 23 self | size-limit test needs size computed without allocation | R2 |
| 10/21/23 -> 24 | downscale_u8, autostretch_u8, render_cavity_2d, ExportConfig/D | present / produced |
| 24 self | "nearest palette colour through a 32^3 cache" vs gif_roundtrip_exact_for_256_colours | a cache keyed on 5-bit buckets would merge distinct colours -> R3 |
| 24 self | median cut with <=256 distinct colours must yield those colours exactly | R3 |
| Model.h | header comment "row 0 = FITS bottom row" stale per C1 | R4 |

Ruling R1: render2d_dust_occludes runs with render_limb_gain = 0 (shade<=1, so out<=0.5E is the exact invariant of the occlusion term); the default-gain case asserts out <= 0.5*(1+render_limb_gain)*E. — the plan's bound ignores the limb term it also specifies — if wrong: the test is slightly weaker than intended.
Ruling R2: Glb exposes a pure size function (e.g. glb_size_bytes(nx, ny, decimate, has_dust)) used by build_glb's pre-allocation check and by glb_size_limit_error. — the test must size 6072x4042 without allocating — cost: one extra public function.
Ruling R3: the 32^3 cache must not change results: either memoise on the full 24-bit colour or use the grid only to prune candidates before an exact squared-distance search (lowest index on ties). Median cut splits until each box holds one distinct colour or 256 boxes exist; a box's colour = its pixels' mean, so <=256 distinct colours round-trip exactly. — the plan's exactness tests demand it — cost: none.
Ruling R4: Task 21 corrects Model.h's frame comment to C1 (row 0 = PixInsight top row) while adding render_limb_gain. — C1 superseded it — cost: none.
Ruling R5: Tasks 25-26 not started: they consume Task 16 RunPrepare/PrepareSession/GradeOptions/SplitWithStarXTerminator and the GaiaDepthGradeProbe command line; Task 16 exists only as uncommitted work in the phaseF-process worktree.
Task 21: dispatched implementer (opus) BASE=530c8a32
Task 21: review approved (spec ok, 0 crit/imp, 5 minor). Fixing minors 1,2,4 (limb test, sky margin, stale frame comments Image.h/Sky.h) per no-deferring rule; minor 3 = plan-specified q normaliser, carried to Task 22 brief; minor 5 accepted (documented ramp).
Task 21: container build-modules.sh exit 0 (GDG ctest 25/25), verify-portable PASS on 510cd1cd
Ruling R6: no scale floor on the limb normaliser q (Task 21 concern) — the plan specifies q over g>0; every reconstruct path incl. stretched-input degraded yields geometry_prior's bowl (z spans [0,1]), so rounding-flat z is unreachable from reconstruct — if wrong: a hand-built near-flat model gets spurious rim brightening in render only.
Task 21: complete (commits 530c8a3..bd4f128, review clean after 1 minor-fix round; re-review approved; nit 'by default' wording accepted)
Task 22: dispatched implementer (sonnet) BASE=bd4f128c
Task 22: implementer DONE_WITH_CONCERNS e57d971d (26/26). Ruling R7: lum rounds to f32 after each addition — numpy float32 array arithmetic does exactly that — cost if wrong: <=1 ulp lum differences. Process note: implementer wrote impl before tests, RED captured with a stub swap.
Task 22: review approved (0 crit/imp, 7 minor); fix round 1 sent: minors 1-7 (incl. scale guards -> throw). Ruling R8: non-finite/<=0 deg_per_px or distance_pc throws invalid_argument — no-silent-fallback rule — cost if wrong: Task 25 must map 0=none before the core (it already specifies that).
Ruling R9: Task 23 adds a PCL-free src/core/AtomicFile.{h,cpp} (write path+'.tmp', fsync, rename; 'cannot write <path>: <strerror>'; tmp removed on failure) used by write_glb and, in Task 24, write_gif — the plan specifies identical atomic semantics for both; one helper avoids a duplicated logic block — cost: one file not in the plan's file list. OnlineCache keeps its own mkstemp/0600 writer (different contract).
Task 22: complete (commits bd4f128..d48c299, review clean after 1 minor-fix round; re-review approved)
Task 23: dispatched implementer (opus) BASE=d48c299b
Note: no ASan/UBSan/TSan runtime on host or in build image astro-pi-build:e5496228143a (checked 2026-11-12); plan's 'ASan before release' remains open — outside Tasks 21-24 scope, reported.
Task 23: implementer DONE 590f75ad (27/27); review approved (0 crit/imp, 6 minor); fix round 1 sent (all 6). Ruling R10: AtomicFile temp = mkstemp '<path>.tmp.XXXXXX', fchmod 0644, fsync dir — plan's fixed '.tmp' name lets two writers to one path publish a partial file, violating Review Focus G2-4 — cost: temp name differs from the plan text.
Task 23 Step 4 manual look: blister.glb loads in three.js r160 GLTFLoader (extras.units 'px'), emissive surface + dust lane render; screenshot task-23-blister-glb.png in this dir.
Task 23: round 2 (mkostemp O_CLOEXEC, 88a12c15) — controller read the 1-line diff directly. Task 23: complete (commits d48c299..88a12c1, review clean after 2 minor-fix rounds)
Task 24: dispatched implementer (opus) BASE=88a12c15
Ruling R11 (Task 24): expose R from Glb (e.g. `double cavity_radius_px(const CavityModel&)` in Glb.h, used by build_glb and orbit; Task 26's metrics also need it) instead of recomputing — DRY — cost: one public function.
Ruling R12 (Task 24): splat ties (equal z at one target) keep the lower source x; a row with no filled target pixel is 0 (only possible when every pixel shifts out of frame); z_apex samples the apex rounded and clamped to the grid; LZW output is cross-checked with independent decoders (ffmpeg, ImageMagick) during development, recorded in the report (test-only, not committed) — the in-repo GifRead decoder could share an encoder misconception — cost: none.
Task 24: implementer DONE 3ac71c69 (30/30; ffmpeg+ImageMagick decode identical). Ruling R13: quantization fixture = 256x64 smooth gradient with 16384 distinct colours using all 256 entries (R=x, G=64+y+x/4, B=255-x), bound <=4 unchanged — the plan names no fixture; a field tiling the full 256x256 plane puts any 256 axis-aligned boxes at exactly 4.0, an infeasible margin — cost if wrong: the test is less demanding than a full-plane field. Note for Task 25: call write_orbit_gif (streams frames), never orbit_views on full masters (~21 GB).
Task 24 visual: blister_orbit.gif 36 frames 128x96; frames 9/27 show the core shifting against the rim in opposite directions. Top-centre black notch = dust_from_structure (Task 18 golden port) at the rim/edge contact, not orbit — carry to Task 26 real-data judgement.
Task 24: review approved (0 crit/imp, 5 minor; reviewer re-verified ffmpeg vs ImageMagick byte-identical on 36 frames); fix round 1 sent (all 5, incl. shape guards in orbit/build_glb/render2d).
Task 24: complete (commits 88a12c1..cf1c1d3, review clean after 1 minor-fix round; re-review approved)
Container gate on cf1c1d35: build-modules.sh exit 0, GDG ctest 30/30 (NukeX 85/85, Planetary 28/28), verify-portable PASS (ubuntu 22.04, debian 12, rocky 9). Pushed cf1c1d35. Final whole-branch review (opus) dispatched over 530c8a32..cf1c1d35.
Final review (opus): With fixes — 0 critical, 1 important (no cavity_meta_from_json for Task 25 ModelViews), 8 minor. Full-size 6072x4042 measured: reconstruct 2.7 s / 3.96 GB peak; write_glb d=1 1.865 GB file; write_orbit_gif 4.4 s +2.3 GB.
Ruling R14: fix wave = Important 1 (strict cavity_meta_from_json + round-trip test), Minor 1 (render rejects non-finite), Minor 3 (apex must be finite and within the grid expanded by its own size, clear error in glb/orbit; frame-content assertion), Minor 4 (AtomicFile mode = 0666 & ~umask read thread-safely from /proc/self/status), Minor 6 (Orbit.h warns orbit_views holds all frames; Task 25 uses write_orbit_gif). — cheap and clear — cost: small API additions.
Ruling R15: not fixed — Minor 2 (core error prefixes; Task 25 pre-validates parameters so core field names don't reach users), Minor 5 (dir-fsync-after-publish documented; unique temps left only on kill), Minor 7 (STF on full-size frame 0 is the plan's text), Minor 8 (chmod tests assume non-root; every sanctioned runner is non-root via keep-id) — cost if wrong: a root CI fails 3 tests; users may see core field names if Task 25 skips pre-validation.
Ruling R16: R6 restated as a Task 25 assumption — ModelViews renders user-supplied z; the limb normaliser has no floor, acceptable for models written by --model — cost: spurious rim brightening on hand-made near-flat z views.
Final fix wave: 9ab626f6 + b812880e (Guards rename to keep Validate.* for Task 26), host ctest 31/31; scoped re-review Approved (3 harmless minors parked: orbit double finiteness scan, \u accepts uppercase hex, test helper throws on malformed umask). Ruling R17: residual minors parked — no correctness impact — cost: one extra O(N) scan in orbit.
INCIDENT: container gate on b812880e FAILED test_recon_meta (5e-324 -> from_chars result_out_of_range under Rocky 9 runtime libstdc++); b812880e was pushed before the gate result was checked (my && chain did not gate on ctest). Fix dispatched to fix-wave agent: shared portable exact parser for Cavity meta + TapText.
FINAL: 0671d42e pushed; container build-modules exit 0 (GDG 32/32, NukeX 85/85, Planetary 28/28), verify-portable PASS, host ctest 32/32. Workspace kept (not deleted) so rulings R1-R17 survive for Tasks 25-26.

## Session 2026-11-12b: Tasks 25-26 (+ ASan pass)
Tasks 16-17 done on gdg/phaseF-process (ledger: /home/scarter4work/projects/astro-pi/.claude/worktrees/agent-a8753ceab04475dfd/.superpowers/sdd/2026-11-10-gaia-depth-grade-pcl-port/progress.md), pushed d89feead. R5 resolved.
Merged gdg/phaseF-process into gdg/phaseH-nebula: 3bc84301 (conflicts: core CMakeLists source list union; test_display/test_tap both sides' new cases kept). Host -DGDG_BUILD_PROBES=ON build 0 warnings, ctest 33/33.
Task 16 names for Task 25: RunPrepare, PrepareSession (StaleReason()/Target(), not IsStale(View)), GradeOptions, SplitWithStarXTerminator.
Ruling R21: Task 25's scp_launch_reaches_probe failing is NOT "stop and report" (plan text): Scott (2026-11-12) ruled fix the root cause, no workaround, no PJSR fallback — cost: none.
Pending after Task 26 (coordinator): ASan+UBSan host build build-asan-module/ of GaiaDepthGrade-pxm, run Task 25/26 headless probes with LD_PRELOAD libasan (detect_leaks=0; separate leak run filtered to our frames), fix by TDD, delete build-asan-module/.
Merge 3bc84301 gate: container build-modules exit 0 (NukeX 85/85, Planetary 28/28, GDG 33/33), verify-portable exit 0 (all 4 .so), no probe/test strings in shipped GDG .so. Pushed.
Task 25: dispatched implementer (opus) BASE=3bc84301
