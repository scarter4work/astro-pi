### Task 15b: Camera resolution — INSTRUME normalisation + generic-camera fallback (spec §6.3)

**Why.** `Filter.camera` is the FITS `INSTRUME` string verbatim. Real headers on this machine read `ZWO ASI2400MC Pro`, `Asi294mc Pro`, `ATR585M`. `QEDatabase::has_camera` / `lookup_camera_qe` do an exact `unordered_map::find` on keys like `asi2400mc`. Every real dual-NB stack would fail Phase B with `UnknownCameraError`. Spec §6.3's "unknown INSTRUME → generic Sony IMX OSC values + loud warning" is not implemented anywhere.

**Files:**
- Modify: `src/lib/calibration/include/nukex/calibration/qe_database.hpp`
- Modify: `src/lib/calibration/src/qe_database.cpp`
- Modify: `test/unit/calibration/test_qe_database.cpp`
- Modify: `src/lib/stacker/include/nukex/stacker/stacking_engine.hpp` (`ExecuteResult`)
- Modify: `src/lib/stacker/src/stacking_engine.cpp:346-350`
- Modify: `src/module/NukeXInstance.cpp` (composed-window keywords, near line 641)
- Modify: `test/fixtures/qe/minimal_db.json` (add `generic_sony_imx_osc`)

**Interfaces:**
- Produces: `static std::string QEDatabase::normalize_camera_key(const std::string&)`; `std::string QEDatabase::resolve_camera(const std::string& instrume) const` (normalised DB key or `""`); `static constexpr const char* QEDatabase::kGenericOSCCamera = "generic_sony_imx_osc"`; `bool ExecuteResult::qe_generic_camera_fallback`.
- Consumers: `StackingEngine` (dual-NB camera set), `ChannelDecomposer::build_q` (unchanged — it calls `has_camera`/`lookup_camera_qe`, which now normalise).

- [ ] **Step 1: Write the failing tests** (append to `test/unit/calibration/test_qe_database.cpp`)

```cpp
TEST_CASE("QEDatabase: camera keys normalise (case, punctuation)", "[qe_database]") {
    REQUIRE(QEDatabase::normalize_camera_key("ZWO ASI2400MC Pro") == "zwoasi2400mcpro");
    REQUIRE(QEDatabase::normalize_camera_key("asi_585-MC")        == "asi585mc");
}

TEST_CASE("QEDatabase: lookups are case-insensitive", "[qe_database]") {
    QEDatabase db;
    REQUIRE(db.load_shipped(fixture("minimal_db.json").string()).ok);
    REQUIRE(db.has_camera("asi585mc"));
    REQUIRE(db.has_camera("ASI585MC"));
    REQUIRE(db.lookup_camera_qe("asi585mc", 656.3, Photosite::R) ==
            db.lookup_camera_qe("ASI585MC", 656.3, Photosite::R));
}

TEST_CASE("QEDatabase: resolve_camera maps a real INSTRUME onto a DB key", "[qe_database]") {
    QEDatabase db;
    REQUIRE(db.load_shipped(fixture("minimal_db.json").string()).ok);
    REQUIRE(db.resolve_camera("ASI585MC")            == "asi585mc");   // exact
    REQUIRE(db.resolve_camera("ZWO ASI2600MC Pro")   == "asi2600mc");  // key is a substring
    REQUIRE(db.resolve_camera("asi2600mc-pro")       == "asi2600mc");
    REQUIRE(db.resolve_camera("ATR585M")             == "");           // no key contained
    REQUIRE(db.resolve_camera("ASI585")              == "");           // partial key does not count
    REQUIRE(db.resolve_camera("")                    == "");
}

TEST_CASE("QEDatabase: resolve_camera prefers the longest contained key", "[qe_database]") {
    QEDatabase db;
    REQUIRE(db.load_shipped(fixture("minimal_db.json").string()).ok);
    REQUIRE(db.load_override(fixture("override_camera_pro.json").string()).ok);
    REQUIRE(db.resolve_camera("ZWO ASI2600MC Pro") == "asi2600mcpro");
    REQUIRE(db.resolve_camera("ZWO ASI2600MC")     == "asi2600mc");
}
```

Create `test/fixtures/qe/override_camera_pro.json`:

```json
{
  "schema_version": 1,
  "cameras": {
    "ASI2600MC-Pro": {
      "sensor": "IMX571",
      "type": "OSC",
      "bayer": "RGGB",
      "qe": { "501": { "R": 0.08, "G": 0.89, "B": 0.60 }, "656": { "R": 0.46, "G": 0.05, "B": 0.04 } },
      "confidence": "medium"
    }
  }
}
```

Add `generic_sony_imx_osc` to `test/fixtures/qe/minimal_db.json` `cameras` (a copy of the `ASI585MC` block with `"sensor": "generic"` and `"confidence": "low"`).

- [ ] **Step 2: Run to verify failure**

Run: `cd build && make test_qe_database 2>&1 | grep -E "error" | head -3`
Expected: `'normalize_camera_key' is not a member of 'nukex::QEDatabase'`.

- [ ] **Step 3: Implement in `QEDatabase`**

`qe_database.hpp`, inside `class QEDatabase` public section:

```cpp
    // Key used for the spec-6.3 unknown-INSTRUME fallback. Shipped by
    // tools/import_qe_research.py as the mean of Sony-sensor OSC cameras.
    static constexpr const char* kGenericOSCCamera = "generic_sony_imx_osc";

    // Lowercase, alphanumerics only. Applied to every camera key on load and
    // to every camera argument on lookup, so "ASI585MC" == "asi585mc".
    static std::string normalize_camera_key(const std::string& raw);

    // Maps a FITS INSTRUME value onto a DB camera key: exact normalised match
    // first, else the longest DB key contained in the normalised INSTRUME
    // ("ZWO ASI2400MC Pro" -> "asi2400mc"). Returns "" when nothing matches;
    // callers decide between failing loud and kGenericOSCCamera.
    std::string resolve_camera(const std::string& instrume) const;
```

`qe_database.cpp`:

```cpp
std::string QEDatabase::normalize_camera_key(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if (std::isalnum(c)) out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

std::string QEDatabase::resolve_camera(const std::string& instrume) const {
    const std::string key = normalize_camera_key(instrume);
    if (key.empty()) return {};
    if (cameras_.count(key)) return key;
    std::string best;
    for (const auto& kv : cameras_) {
        if (kv.first.size() > best.size() && key.find(kv.first) != std::string::npos) {
            best = kv.first;
        }
    }
    return best;
}
```

(add `#include <cctype>`.) In `parse_and_merge`, store `cameras_[normalize_camera_key(name)] = std::move(cam);`. In `has_camera`, `confidence`, and `lookup_camera_qe`, look up `normalize_camera_key(name)` instead of `name`. Filters are untouched (canonical names are exact by construction).

- [ ] **Step 4: Run the calibration tests**

Run: `cd build && make test_qe_database test_channel_decomposer 2>&1 | grep -E "error" ; ctest -R "qe_database|decomposer" --output-on-failure 2>&1 | tail -3`
Expected: 2/2 pass (decomposer's `build_q("ASI585MC", …)` keeps working through normalisation).

- [ ] **Step 5: Engine — resolve at the dual-NB camera set, fall back loudly**

`stacking_engine.hpp`, in `ExecuteResult` after `n_frames_rejected_filter`:

```cpp
        bool qe_generic_camera_fallback = false; // spec 6.3: INSTRUME not in QE DB, generic Sony OSC QE used
```

`stacking_engine.cpp`: next to the existing `dual_nb_cameras` declaration add `std::set<std::string> unknown_instrume_warned;`. Replace lines 347–350 with:

```cpp
        // Track DUAL_NB_OSC cameras for the Q-solve mixed-camera guard.
        // Resolve the raw INSTRUME onto a DB key here so the guard compares
        // DB identities ("asi2400mc"), not header spellings.
        if (frame_filter.cls == FilterClass::DUAL_NB_OSC) {
            std::string key = qe_database_->resolve_camera(frame_filter.camera);
            if (key.empty()) {
                key = QEDatabase::kGenericOSCCamera;
                result.qe_generic_camera_fallback = true;
                if (unknown_instrume_warned.insert(frame_filter.camera).second) {
                    obs.message("Camera '" + frame_filter.camera +
                                "' unknown; using generic Sony IMX OSC QE values. "
                                "Output marked as low-confidence (NUKEX_QE_CONFIDENCE).");
                }
            }
            dual_nb_cameras.insert(key);
        }
```

Ensure `result` is the `ExecuteResult` being built in that scope (it is — the same object that later receives `result.derived`). Add `#include <set>` if absent.

- [ ] **Step 6: Module — mark the composed window**

In `src/module/NukeXInstance.cpp`, directly after the `NUKEX_GAMUT_CLIPPED` keyword append (≈ line 646):

```cpp
      cw_ka.Append( pcl::FITSHeaderKeyword(
          "NUKEX_QE_CONFIDENCE",
          result.qe_generic_camera_fallback ? "generic-fallback" : "database",
          "QE source for Phase B: camera entry or generic Sony OSC fallback" ) );
```

- [ ] **Step 7: Full build + ctest**

Run: `cd build && cmake .. > /dev/null && make -j$(nproc) 2>&1 | grep -E "error" ; ctest 2>&1 | tail -3`
Expected: `100% tests passed` (same binary count; the new cases run inside test_qe_database).

- [ ] **Step 8: Commit**

```bash
git add src/lib/calibration src/lib/stacker src/module/NukeXInstance.cpp test/unit/calibration test/fixtures/qe
git commit -m "$(cat <<'EOF'
fix(calibration): resolve FITS INSTRUME onto QE DB keys; generic OSC fallback per spec 6.3

QEDatabase keyed cameras by exact string while Filter.camera carries the
raw INSTRUME ("ZWO ASI2400MC Pro", "Asi294mc Pro"); every real dual-NB
stack would have thrown UnknownCameraError at Phase B. Keys are now
normalised (lowercase alphanumerics) on load and lookup, and
resolve_camera() maps an INSTRUME onto the longest contained key. When
nothing matches, the engine uses generic_sony_imx_osc, warns once per
INSTRUME in the Process Console, sets ExecuteResult::qe_generic_camera_fallback,
and the module writes NUKEX_QE_CONFIDENCE='generic-fallback' on the
composed window.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01NqDedRCQxJy8EbbzWDKMFR
EOF
)"
```

---

## Wave 2

