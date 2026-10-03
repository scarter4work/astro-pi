### Task 18: Delete `StackingMode`, `from_mode`, `output_rgb_mapping`, `is_mono`, `stacking_mode_name`

**State today.** No production code references these (verified: only `channel_config.{hpp,cpp}` and four test files). `is_mono()` reads `mode`, which `from_filter()` never sets — it is already wrong for every v5 config and has no callers.

**Files:**
- Modify: `src/lib/core/include/nukex/core/channel_config.hpp`, `src/lib/core/src/channel_config.cpp`
- Rewrite: `test/unit/core/test_channel_config.cpp`
- Modify: `test/unit/core/test_cube.cpp:7,17,27,35,43`, `test/unit/combine/test_output_assembler.cpp:9,33`, `test/unit/combine/test_spatial_context.cpp:34`

- [ ] **Step 1: Confirm zero production callers**

Run: `grep -rnE "from_mode|output_rgb_mapping|StackingMode|is_mono|stacking_mode_name" src/ | grep -v channel_config`
Expected: no output. Any hit means a missed migration — stop and fix it first.

- [ ] **Step 2: Migrate the tests first (they define the new surface)**

Add to each of the three test files that used `from_mode` a local helper and swap the calls:

```cpp
static ChannelConfig cfg_for(FilterClass cls, const char* name) {
    Filter f;
    f.cls  = cls;
    f.name = name;
    return ChannelConfig::from_filter(f);
}
```

- `test_cube.cpp`: `from_mode(StackingMode::OSC_RGB)` → `cfg_for(FilterClass::BROADBAND_OSC, "OSC")` and the `n_channels == 3` assertion on line 11 becomes `== 4` (R, G, B, synthesised L); `from_mode(StackingMode::MONO_L)` → `cfg_for(FilterClass::BROADBAND_L, "L")`; `from_mode(StackingMode::OSC_HAO3)` → `cfg_for(FilterClass::DUAL_NB_OSC, "HaO3")` and the per-voxel `n_channels == 2` on line 31 becomes `== 3` (`R_HaO3`, `G_HaO3`, `B_HaO3`). Add `#include "nukex/core/filter.hpp"`.
- `test_output_assembler.cpp:9,33` and `test_spatial_context.cpp:34`: `from_mode(StackingMode::MONO_L)` → `cfg_for(FilterClass::BROADBAND_L, "L")`.

Replace `test/unit/core/test_channel_config.cpp` entirely:

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/core/channel_config.hpp"
#include "nukex/core/filter.hpp"

using namespace nukex;

static ChannelConfig cfg_for(FilterClass cls, const char* name) {
    Filter f;
    f.cls  = cls;
    f.name = name;
    return ChannelConfig::from_filter(f);
}

TEST_CASE("ChannelConfig: channel_index_for_name / slot_index / slot_name over a merged LRGB config", "[channel]") {
    ChannelConfig cfg = cfg_for(FilterClass::BROADBAND_L, "L");
    for (const char* c : {"R", "G", "B"}) {
        cfg = ChannelConfig::merge(cfg, cfg_for(FilterClass::BROADBAND_RGB, c));
    }
    REQUIRE(cfg.n_channels == 4);
    REQUIRE(cfg.channel_index_for_name("L") == 0);
    REQUIRE(cfg.channel_index_for_name("R") == 1);
    REQUIRE(cfg.channel_index_for_name("G") == 2);
    REQUIRE(cfg.channel_index_for_name("B") == 3);
    REQUIRE(cfg.channel_index_for_name("Ha") == -1);
    REQUIRE(cfg.slot_index("G") == cfg.channel_index_for_name("G"));
    REQUIRE(cfg.slot_name(3) == "B");
}

TEST_CASE("ChannelConfig: bayer defaults to NONE for mono classes and RGGB for Bayer classes", "[channel]") {
    REQUIRE(cfg_for(FilterClass::BROADBAND_L, "L").bayer        == BayerPattern::NONE);
    REQUIRE(cfg_for(FilterClass::NARROWBAND_SINGLE, "Ha").bayer == BayerPattern::NONE);
    REQUIRE(cfg_for(FilterClass::BROADBAND_OSC, "OSC").bayer    == BayerPattern::RGGB);
    REQUIRE(cfg_for(FilterClass::DUAL_NB_OSC, "HaO3").bayer     == BayerPattern::RGGB);
}
```

- [ ] **Step 3: Delete the symbols**

`channel_config.hpp`: remove `enum class StackingMode`, `stacking_mode_name()`, the `StackingMode mode` member, `output_rgb_mapping[3]`, the `from_mode` declaration, and `bool is_mono() const;`.
`channel_config.cpp`: remove `ChannelConfig::from_mode` and `ChannelConfig::is_mono` definitions.

- [ ] **Step 4: Build + ctest**

Run: `cd build && cmake .. > /dev/null && make -j$(nproc) 2>&1 | grep -E "error" ; ctest 2>&1 | tail -2`
Expected: no errors, all pass.

- [ ] **Step 5: Commit**

```bash
git add src/lib/core test/unit/core test/unit/combine
git commit -m "$(cat <<'EOF'
chore(core): remove StackingMode, from_mode, output_rgb_mapping, is_mono

Dead since Tasks 7-12 routed everything through ChannelConfig::from_filter.
is_mono() read a `mode` field that from_filter never set. Tests that used
from_mode as setup now build configs from Filter values; Bayer-class
channel counts follow from_filter (OSC = R,G,B,L; dual-NB = 3 raw slots).
Spec 4.3 dead-code list.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01NqDedRCQxJy8EbbzWDKMFR
EOF
)"
```

---

