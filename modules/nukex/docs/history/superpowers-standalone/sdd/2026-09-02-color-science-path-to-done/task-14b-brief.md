### Task 14b: Classifier — Bayer-aware broadband names (L / L-Pro / UV-IR-cut / CLS on OSC)

**Why.** `known_table()` maps `"l"`/`"luminance"` to `BROADBAND_L` regardless of Bayer, and knows none of the broadband light-pollution names in the research database. Two consequences: an OSC frame tagged `FILTER='L'` gets a one-slot mono config with a Bayer pattern (wrong), and the user's own M16 corpus (`FILTER='LPro'`, RGGB) is rejected at batch start as "not in QE DB" — a hard regression versus v4, which stacked it. Spec §6.3 reserves the loud failure for *unknown* names; these names are known broadband.

**Files:**
- Modify: `src/lib/io/src/filter_classifier.cpp`
- Modify: `test/unit/io/test_filter_classifier.cpp`

**Interfaces:**
- Produces: unchanged `FilterClassifier::classify` signature; new behavior: broadband names resolve to `BROADBAND_OSC` (name `"OSC"`) when `bayer_pattern` is non-empty, else `BROADBAND_L` (name `"L"`).

- [ ] **Step 1: Write the failing tests** (append to `test/unit/io/test_filter_classifier.cpp`)

```cpp
TEST_CASE("FilterClassifier: broadband names on Bayer resolve to BROADBAND_OSC", "[filter_classifier]") {
    FilterClassifier c;
    for (const char* name : {"L", "Luminance", "LPro", "L-Pro", "UV/IR Cut", "CLS-CCD", "LPS-D1", "L1"}) {
        Filter f = c.classify(make_meta(name, "RGGB", "ZWO ASI2400MC Pro"));
        INFO(name);
        REQUIRE(f.cls  == FilterClass::BROADBAND_OSC);
        REQUIRE(f.name == "OSC");
        REQUIRE(c.last_warning().empty());
    }
}

TEST_CASE("FilterClassifier: broadband LPR names on mono resolve to BROADBAND_L named L", "[filter_classifier]") {
    FilterClassifier c;
    for (const char* name : {"LPro", "UV-IR-Cut", "CLS", "LPS-D2"}) {
        Filter f = c.classify(make_meta(name, "", "ASI2600MM"));
        INFO(name);
        REQUIRE(f.cls  == FilterClass::BROADBAND_L);
        REQUIRE(f.name == "L");
        REQUIRE(c.last_warning().empty());
    }
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cd build && make test_filter_classifier_io 2>&1 | grep -E "error" ; ./test/test_filter_classifier_io 2>&1 | tail -3`
Expected: 2 failed test cases (`BROADBAND_L` / `UNKNOWN` where `BROADBAND_OSC` expected; `L` name where `OSC` expected).

- [ ] **Step 3: Implement**

In `src/lib/io/src/filter_classifier.cpp`:

1. Remove the `"l"` and `"luminance"` rows from `known_table()`.
2. Add below `known_table()`:

```cpp
// Broadband names whose class depends on the sensor: OSC on a Bayer frame,
// luminance on a mono frame. Normalised (lowercase, alphanumerics only).
// Sources: research/qe_database_research.json types `luminance` and
// `broadband-LPR`, plus the bare L aliases moved out of known_table().
const std::unordered_set<std::string>& broadband_any_names() {
    static const std::unordered_set<std::string> names = {
        "l", "lum", "luminance",
        "lpro", "lpr", "lps", "lpsd1", "lpsd2", "lpsd3", "lpsv4",
        "uvir", "uvircut", "uvirblock", "irblock", "uvcut",
        "cls", "clsccd",
        "l1", "l2", "l3",           // Astronomik L1/L2/L3 UV-IR block
    };
    return names;
}
```

(add `#include <unordered_set>`.)

3. In `classify`, after `const std::string normalized = normalize_name(meta.filter);` and the empty-name branch, before `lookup_known`:

```cpp
    if (broadband_any_names().count(normalized)) {
        if (is_bayer) {
            out.cls  = FilterClass::BROADBAND_OSC;
            out.name = "OSC";
        } else {
            out.cls  = FilterClass::BROADBAND_L;
            out.name = "L";
        }
        out.bandwidth = BandwidthSpec{550.0, 300.0};
        return out;
    }
```

- [ ] **Step 4: Run the classifier + engine tests**

Run: `cd build && make -j$(nproc) 2>&1 | grep -E "error" ; ctest -R "filter|channel_config_from_filter|engine_config|cache_sig" --output-on-failure 2>&1 | tail -3`
Expected: all pass (the existing "L / R / G / B broadband on mono" case still passes: mono `L` → `BROADBAND_L`).

- [ ] **Step 5: Commit**

```bash
git add src/lib/io/src/filter_classifier.cpp test/unit/io/test_filter_classifier.cpp
git commit -m "$(cat <<'EOF'
fix(io): broadband filter names resolve by sensor type (OSC on Bayer, L on mono)

L / Luminance / L-Pro / LPS / UV-IR-cut / CLS on a Bayer frame are plain
OSC broadband, not a one-slot luminance config with a Bayer pattern, and
not an unknown-FILTER batch rejection. Spec 6.3 keeps the loud failure
for genuinely unknown names; these are known broadband names from the
research DB (types luminance, broadband-LPR).

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01NqDedRCQxJy8EbbzWDKMFR
EOF
)"
```

---

