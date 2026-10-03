### Task 21b: Engine — route mono frames by the registered slot name, not the raw filter name

**Why.** `stacking_engine.cpp` (mono routing branch, `case FilterClass::BROADBAND_L / NARROWBAND_SINGLE / BROADBAND_RGB`, ~lines 521-545) looks the voxel slot up with `frame_filter.name` and calls `std::abort()` when it is absent. `ChannelConfig::from_filter` (`channel_config.cpp`) registers `BROADBAND_L` under the literal `"L"` regardless of `Filter.name`. The classifier legitimately produces BROADBAND_L frames whose name is not `"L"`: `"L_unnamed"` (spec §6.3: no FILTER keyword on mono) and the raw FILTER value (spec §6.3: unknown FILTER on mono, e.g. NGC7635's wheel-slot `'1'`). Both abort PixInsight on the first frame. Task 21's regression-floor run (`lrgb_mono_ngc7635`, FILTER='1') caught it: SIGABRT at frame 1/65. The per-frame `ChannelConfig per_frame_cfg = ChannelConfig::from_filter(frame_filter)` computed earlier in the same loop (line ~368) already holds the registered names — route by those.

**Files:**
- Modify: `src/lib/stacker/src/stacking_engine.cpp` (mono routing branch only; `per_frame_cfg` is in scope in the same per-frame loop — verify, and if it is not, compute `ChannelConfig::from_filter(frame_filter)` once at the top of the routing switch)
- Modify: `test/integration/test_phase_a_router.cpp` (two new cases)

**Interfaces:** unchanged. `ChannelConfig::from_filter(f).channel_names[0]` is `"L"` for BROADBAND_L, `f.name` for BROADBAND_RGB and NARROWBAND_SINGLE — the same mapping `merge()` registered, so lookup and registration can no longer diverge.

- [ ] **Step 1: Write the failing tests** — append to `test/integration/test_phase_a_router.cpp` (same includes/`using` as the file's existing cases):

```cpp
TEST_CASE("Phase A: missing FILTER on mono (L_unnamed) routes into the L slot",
          "[.integration][phase_a]") {
    auto tmp = fs::temp_directory_path() / "phase_a_mono_unnamed.fits";
    test_util::write_synthetic_mono(tmp.string(), 16, 16, "ASI2600MM", /*filter*/"", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({tmp.string()}, {}, nullptr);

    REQUIRE(result.ok);
    int L_idx = result.cube->channel_config.slot_index("L");
    REQUIRE(L_idx != -1);
    REQUIRE(result.cube->at(8, 8).welford[L_idx].mean == Catch::Approx(0.5f).margin(0.05f));
}

TEST_CASE("Phase A: unknown FILTER on mono routes into the L slot (spec 6.3, e.g. a wheel slot number)",
          "[.integration][phase_a]") {
    auto tmp = fs::temp_directory_path() / "phase_a_mono_unknown.fits";
    test_util::write_synthetic_mono(tmp.string(), 16, 16, "ATR585M", /*filter*/"1", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({tmp.string()}, {}, nullptr);

    REQUIRE(result.ok);
    int L_idx = result.cube->channel_config.slot_index("L");
    REQUIRE(L_idx != -1);
    REQUIRE(result.cube->at(8, 8).welford[L_idx].mean == Catch::Approx(0.5f).margin(0.05f));
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cd build && make test_phase_a_router 2>&1 | grep -E "error" ; ./test/integration/test_phase_a_router "[integration]" 2>&1 | tail -4; echo "exit=$?"`
Expected: the binary aborts (SIGABRT from the mono routing branch) while running one of the two new cases — Catch2 prints the case name before the abort; exit code is non-zero. That crash IS the red.

- [ ] **Step 3: Implement**

In the mono routing branch replace

```cpp
                const std::string& slot_name = frame_filter.name;
```
with
```cpp
                // Route by the slot merge() actually registered for this
                // frame's class — from_filter() maps BROADBAND_L to "L"
                // regardless of Filter.name (which may be "L_unnamed" or a
                // raw unknown FILTER value such as a wheel-slot number),
                // and R/G/B or Ha/OIII/SII to the name itself. Looking up
                // frame_filter.name directly aborted on every mono frame
                // whose FILTER was not literally "L".
                const std::string& slot_name = per_frame_cfg.channel_names[0];
```
and update the branch's leading comment ("The slot name comes from the filter — …") to say the slot name comes from `from_filter()`. If `per_frame_cfg` is not in scope at that point, add `const ChannelConfig per_frame_cfg = ChannelConfig::from_filter(frame_filter);` immediately before the routing `switch`.

- [ ] **Step 4: Verify**

Run:
```bash
cd build && make -j$(nproc) 2>&1 | grep -E "error" ; ctest 2>&1 | tail -2
./test/integration/test_phase_a_router "[integration]" 2>&1 | tail -3
./test/integration/test_phase_b_qsolve  "[integration]" 2>&1 | tail -3
```
Expected: `100% tests passed, 0 tests failed out of 66`; phase_a `All tests passed (… 7 test cases)`; phase_b 5/5.

- [ ] **Step 5: Commit**

```bash
git add src/lib/stacker/src/stacking_engine.cpp test/integration/test_phase_a_router.cpp
git commit -m "$(cat <<'EOF'
fix(stacker): route mono frames by the registered slot, not the raw filter name

from_filter() registers BROADBAND_L under "L" whatever Filter.name says,
but the mono routing branch looked the slot up by frame_filter.name and
aborted when absent. Every mono frame whose FILTER was not literally "L"
crashed PixInsight on frame 1: no FILTER keyword ("L_unnamed") and unknown
names such as NGC7635's wheel-slot '1' — both spec 6.3 rows. Caught by the
v4 regression-floor E2E run. Route by per_frame_cfg.channel_names[0], the
same mapping merge() used, so lookup and registration cannot diverge; two
integration cases pin both spec rows.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01NqDedRCQxJy8EbbzWDKMFR
EOF
)"
```
