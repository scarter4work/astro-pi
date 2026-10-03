### Task 6: Report it in the Process Console

A user reading the console must be able to tell that channel registration happened, which rung of the ladder each channel landed on, and how big the correction was. A silent correction is indistinguishable from a bug.

**Files:**
- Modify: `src/lib/stacker/src/stacking_engine.cpp` (the Phase A alignment log, around line 652)
- Test: `test/unit/alignment/test_channel_registration.cpp` (append — the formatter is pure and testable)

**Interfaces:**
- Consumes: `AlignedFrame::channels`, `ChannelTransform::fit`, `ChannelTransform::max_displacement`.
- Produces: `std::string nukex::describe_channel_transforms(const ChannelTransforms&, double radius)` declared in `channel_registration.hpp`.

- [ ] **Step 1: Write the failing test**

Append to `test/unit/alignment/test_channel_registration.cpp`:

```cpp
// --- console reporting ---------------------------------------------------

TEST_CASE("describe_channel_transforms names the rung and the size",
          "[channel_registration]") {
    ChannelTransforms ct;
    ct.cx = 399.5; ct.cy = 399.5; ct.reference_channel = 1;
    ct.per_channel.resize(3);
    ct.per_channel[0] = ChannelTransform{1.000238, 0.31, -0.12, 187, 0.043,
                                         ChannelTransform::Fit::Affine};
    ct.per_channel[1].fit = ChannelTransform::Fit::Identity;
    ct.per_channel[2] = ChannelTransform{1.0, 0.02, -0.01, 190, 0.031,
                                         ChannelTransform::Fit::TranslationOnly};

    const std::string s = describe_channel_transforms(ct, 565.0);

    // The rung, the size of the correction, and the star count all have to be
    // there: a user seeing a moved golden needs to know why from the log.
    CHECK(s.find("ch0") != std::string::npos);
    CHECK(s.find("affine") != std::string::npos);
    CHECK(s.find("238 ppm") != std::string::npos);
    CHECK(s.find("n=187") != std::string::npos);
    CHECK(s.find("ch2") != std::string::npos);
    CHECK(s.find("translation") != std::string::npos);
    CHECK(s.find("ch1") == std::string::npos);   // the reference is not reported

    // ASCII only. PCL reads const char* as ISO-8859-1, so a stray UTF-8 byte
    // reaches the Process Console as mojibake.
    for (unsigned char c : s) CHECK(c < 0x80);
}

TEST_CASE("describe_channel_transforms says so when there is nothing to say",
          "[channel_registration]") {
    CHECK(describe_channel_transforms(ChannelTransforms{}, 565.0).empty());
}
```

- [ ] **Step 2: Run and watch it fail**

```bash
cmake --build build -j$(nproc) --target test_channel_registration 2>&1 | tail -20
```

Expected: compile failure — `describe_channel_transforms` is not declared.

- [ ] **Step 3: Declare and implement the formatter**

In `channel_registration.hpp`, after `measure_channel_transforms`:

```cpp
/// One line describing what channel registration did, for the Process
/// Console. Empty when there was nothing to report.
///
/// `radius` is where the displacement is quoted -- normally the frame corner,
/// which is where a scale term is largest and where a user looking at their
/// stars will notice.
///
/// ASCII only: PCL reads const char* as ISO-8859-1, so a UTF-8 character here
/// reaches the console as mojibake.
std::string describe_channel_transforms(const ChannelTransforms& ct,
                                        double radius);
```

Add `#include <string>` to the header.

In `channel_registration.cpp`:

```cpp
std::string describe_channel_transforms(const ChannelTransforms& ct,
                                        double radius) {
    if (ct.empty()) return {};

    std::string out;
    for (size_t ch = 0; ch < ct.per_channel.size(); ch++) {
        if (static_cast<int>(ch) == ct.reference_channel) continue;
        const ChannelTransform& t = ct.per_channel[ch];

        char buf[192];
        switch (t.fit) {
        case ChannelTransform::Fit::Affine:
            std::snprintf(buf, sizeof(buf),
                          "ch%zu affine %+.0f ppm t=(%+.2f,%+.2f) "
                          "n=%d res=%.3fpx max=%.3fpx",
                          ch, (t.s - 1.0) * 1e6, t.tx, t.ty,
                          t.n_stars, t.residual, t.max_displacement(radius));
            break;
        case ChannelTransform::Fit::TranslationOnly:
            std::snprintf(buf, sizeof(buf),
                          "ch%zu translation t=(%+.2f,%+.2f) n=%d res=%.3fpx",
                          ch, t.tx, t.ty, t.n_stars, t.residual);
            break;
        case ChannelTransform::Fit::Identity:
            std::snprintf(buf, sizeof(buf),
                          "ch%zu identity (nothing measurable)", ch);
            break;
        }
        if (!out.empty()) out += "; ";
        out += buf;
    }
    return out;
}
```

Add `#include <cstdio>` and `#include <string>` to the .cpp.

- [ ] **Step 4: Run and watch it pass**

```bash
cmake --build build -j$(nproc) --target test_channel_registration && ./build/test/test_channel_registration
```

Expected: PASS.

- [ ] **Step 5: Emit it from the engine**

In `src/lib/stacker/src/stacking_engine.cpp`, immediately after the `obs.advance(0, "  aligned: " + status + ...)` block closes (the `else` branch ending around line 680), add:

```cpp
        // Channel registration, when there was any. Silence here means the
        // frame's channels already agreed to better than 0.01 px at the
        // corner, which is the near-identity skip doing its job.
        if (!aligned.channels.empty()) {
            const double corner = std::hypot(aligned.image.width()  / 2.0,
                                             aligned.image.height() / 2.0);
            const std::string desc =
                describe_channel_transforms(aligned.channels, corner);
            if (!desc.empty()) obs.advance(0, "  channel reg: " + desc);
        }
```

Add `#include "nukex/alignment/channel_registration.hpp"` to the engine's includes if it is not already reachable through `frame_aligner.hpp` (it is, but name it explicitly — this file uses the symbol directly).

- [ ] **Step 6: Build and run the suite**

```bash
cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure
```

Expected: all green.

- [ ] **Step 7: Commit**

```bash
git add src/lib/alignment/include/nukex/alignment/channel_registration.hpp \
        src/lib/alignment/src/channel_registration.cpp \
        src/lib/stacker/src/stacking_engine.cpp \
        test/unit/alignment/test_channel_registration.cpp
git commit -m "feat(stacker): report channel registration in the console

Names the rung, the size in ppm and pixels, the star count and the
residual, per channel. Silence means the near-identity skip took the
plain path. ASCII only -- PCL reads const char* as ISO-8859-1."
```

---

