# Task 18 Report: Delete StackingMode, from_mode, output_rgb_mapping, is_mono, stacking_mode_name

## What I implemented

Followed the brief's step order exactly:

1. **Step 1 (pre-check):** `grep -rnE "from_mode|output_rgb_mapping|StackingMode|is_mono|stacking_mode_name" src/ | grep -v channel_config` returned empty, confirming no production callers outside `channel_config.{hpp,cpp}`.

2. **Step 2 (migrate tests first):**
   - `test/unit/core/test_cube.cpp`: added `#include "nukex/core/filter.hpp"` and a local `cfg_for(FilterClass, const char*)` helper. Replaced all four `from_mode(...)` call sites:
     - `from_mode(StackingMode::OSC_RGB)` → `cfg_for(FilterClass::BROADBAND_OSC, "OSC")`, with the construction test's `n_channels == 3` assertion updated to `== 4` (R, G, B, synthesised L).
     - `from_mode(StackingMode::MONO_L)` (two call sites) → `cfg_for(FilterClass::BROADBAND_L, "L")`.
     - `from_mode(StackingMode::OSC_HAO3)` → `cfg_for(FilterClass::DUAL_NB_OSC, "HaO3")`, with the per-voxel `n_channels == 2` assertion updated to `== 3`.
     - `from_mode(StackingMode::OSC_RGB)` (const-access test) → `cfg_for(FilterClass::BROADBAND_OSC, "OSC")` (assertion there wasn't channel-count based, so unchanged).
   - `test/unit/combine/test_output_assembler.cpp`: added the same `cfg_for` helper and `#include "nukex/core/filter.hpp"`; both `from_mode(StackingMode::MONO_L)` call sites → `cfg_for(FilterClass::BROADBAND_L, "L")`.
   - `test/unit/combine/test_spatial_context.cpp`: added the same helper and include; the one `from_mode(StackingMode::MONO_L)` call site → `cfg_for(FilterClass::BROADBAND_L, "L")`.
   - `test/unit/core/test_channel_config.cpp`: replaced entirely with the brief's verbatim two-test-case rewrite (merged-LRGB channel indexing/slot naming, and Bayer-pattern defaults across filter classes).

3. **Step 3 (delete symbols):**
   - `channel_config.hpp`: removed `enum class StackingMode`, `stacking_mode_name()`, the `mode` member, `output_rgb_mapping[3]` (and its "(removed in Task 18)" placeholder comment), the `from_mode` declaration, and `bool is_mono() const;`.
   - `channel_config.cpp`: removed the `ChannelConfig::from_mode` and `ChannelConfig::is_mono` definitions. Left `channel_index_for_name`, `from_filter`, `merge`, and `slot_index` untouched.

4. **Step 4 (build + test):** see Testing below.

5. **Step 5 (commit):** committed exactly the brief's message (verbatim body + both trailer lines).

## What I tested and results

- `grep -rnE "from_mode|output_rgb_mapping|StackingMode|is_mono|stacking_mode_name" src/ test/` → empty, both before symbol deletion (test-only migration confirmed) and after (full completeness check).
- `cmake ..` → reconfigured cleanly, no new warnings/errors relevant to this change.
- `make -j$(nproc)` → 0 build errors (grepped for "error" in full build log — zero matches).
- `ctest` (full suite) → **100% tests passed, 0 failed, out of 67** (matches the brief's expected same binary count).
- `ctest -R "test_channel|test_cube|test_output_assembler|test_spatial_context"` (targeted re-run of the four affected binaries plus two others matched by the regex, `test_channel_config_from_filter` and `test_channel_decomposer`) → 6/6 passed.

Output was pristine in all runs — no warnings surfaced during grep/build/test that needed follow-up.

## Files changed

- `src/lib/core/include/nukex/core/channel_config.hpp` (25 deletions, symbol removal)
- `src/lib/core/src/channel_config.cpp` (67 deletions, definition removal)
- `test/unit/core/test_channel_config.cpp` (rewritten per brief, 81 lines → smaller two-test-case file)
- `test/unit/core/test_cube.cpp` (migrated 4 call sites + 2 assertion updates)
- `test/unit/combine/test_output_assembler.cpp` (migrated 2 call sites)
- `test/unit/combine/test_spatial_context.cpp` (migrated 1 call site)

Commit: `8bcdb29` — "chore(core): remove StackingMode, from_mode, output_rgb_mapping, is_mono"

## Self-review findings

- **Completeness:** Final grep for `from_mode|output_rgb_mapping|StackingMode|is_mono|stacking_mode_name` across `src/` and `test/` returns nothing — confirmed clean.
- **Quality:** Reviewed the finished `channel_config.hpp`/`.cpp` in full — no stray comments, dead includes, or leftover references. `ChannelConfig` now only carries `n_channels`, `channel_names`, `bayer`, and the `from_filter`/`merge`/`channel_index_for_name`/`slot_index`/`slot_name` surface.
- **Discipline:** Diff touches exactly the six files the brief named, nothing more (`git diff --stat` confirmed before commit).
- **Testing:** Build and full ctest run both pristine; 67/67 passing, matching brief's expected same binary count.

## Issues or concerns

None. The migration matched the brief's specified assertion changes exactly (OSC construction case 3→4, dual-NB per-voxel case 2→3), and no other assertions needed touching.
