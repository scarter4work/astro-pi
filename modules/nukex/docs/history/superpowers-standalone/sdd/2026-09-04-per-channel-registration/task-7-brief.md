### Task 7: Re-baseline the goldens and measure the acceptance criterion

Two changes in this plan move Bayer output: star detection now runs on green (Task 1), and the channels are now registered (Tasks 2-5). Both were expected. This task proves the frozen mono golden did **not** move, re-baselines the two that did, and measures the number the spec set as the bar.

**Files:**
- Modify: `test/fixtures/golden/bayer_rgb_m27_2023.json`
- Modify: `test/fixtures/golden/bayer_nb_hao3_m16.json`
- Verify unchanged: `test/fixtures/golden/lrgb_mono_ngc7635.json`
- Create: `tools/measure_channel_registration.py`
- Modify: `src/module/NukeXVersion.h` (version bump)

**Interfaces:**
- Consumes: the whole feature.
- Produces: nothing other code depends on.

- [ ] **Step 1: Run the frozen mono case first, before anything else**

`lrgb_mono_ngc7635` carries `"golden_frozen": true`. A one-channel stack must take the no-op path at every step of this feature, so its golden must be bit-identical. Prove that before touching any other golden — if it moved, the mono no-op is broken and nothing else in this task is meaningful.

```bash
NUKEX_E2E_ONLY=lrgb_mono_ngc7635 cmake --build build --target e2e 2>&1 | tail -40
node tools/validate_e2e.js
```

`run_e2e.sh` takes only `regen` as a positional argument; a single case is
selected with the `NUKEX_E2E_ONLY` environment variable. The full corpus is
four stacks of real data and takes hours, so run one case at a time.

Expected: PASS with no golden drift. If it drifted, stop and find out why. The likely cause is `default_reference_channel` being consulted somewhere with the wrong channel count, or `saturation_fraction` having been changed in Task 1 despite the instruction not to.

- [ ] **Step 2: Run the two Bayer cases and capture the new baselines**

```bash
NUKEX_E2E_ONLY=bayer_rgb_m27_2023 cmake --build build --target e2e 2>&1 | tail -60
NUKEX_E2E_ONLY=bayer_nb_hao3_m16  cmake --build build --target e2e 2>&1 | tail -60
```

These are long runs (budgets are 10800 s and 7200 s in the manifest). Read the console output for the new `channel reg:` lines — they are the first real-data evidence the feature is working, and they must show a plausible correction rather than a rejected one. A field of `identity (nothing measurable)` across every frame means the star SNR gate is too tight for real data; investigate before accepting the baselines.

Also confirm `min_frames_ok_alignment` still holds for both cases. Detecting on green rather than red should if anything **improve** alignment on OSC data, since green has twice the photosites. A drop is a regression, not an expected re-baseline.

- [ ] **Step 3: Update the two golden files**

Regenerate rather than hand-editing hashes:

```bash
NUKEX_E2E_ONLY=bayer_rgb_m27_2023 cmake --build build --target e2e-regen
NUKEX_E2E_ONLY=bayer_nb_hao3_m16  cmake --build build --target e2e-regen
```

Then `git diff test/fixtures/golden/` and read what moved before staging it. A
re-baseline you have not looked at is indistinguishable from a regression you
have accepted. Record in the commit message that this is deliberate and why.

- [ ] **Step 4: Write the acceptance measurement**

Create `tools/measure_channel_registration.py`:

```python
#!/usr/bin/env python3
"""Median red-to-green and blue-to-green star separation in a stacked XISF.

The acceptance criterion for per-channel registration. On the M3 set before
the feature: R-G 0.435 px, B-G 0.058 px. B-G is the floor -- it is centroid
noise, not colour error, because through an L-Quad Enhance green and blue are
both imaging near 500 nm. The bar is R-G below 0.10 px.

Usage: measure_channel_registration.py <stacked.xisf>
"""
import sys
import numpy as np


def read_planes(path):
    """Return (R, G, B) float32 planes from an XISF file."""
    # The project already reads XISF elsewhere; reuse that path rather than
    # writing a third parser. If nothing is importable, convert once with
    # PixInsight and read a FITS instead -- the measurement is what matters,
    # not the container.
    raise NotImplementedError(
        "wire this to the project's existing XISF reader before running")


def centroid(plane, x, y, r=4):
    y0, y1, x0, x1 = y - r, y + r + 1, x - r, x + r + 1
    if y0 < 0 or x0 < 0 or y1 > plane.shape[0] or x1 > plane.shape[1]:
        return None
    box = plane[y0:y1, x0:x1].astype(np.float64)
    bg = min(box[0].min(), box[-1].min(), box[:, 0].min(), box[:, -1].min())
    w = np.clip(box - bg, 0, None)
    if w.sum() <= 0:
        return None
    gy, gx = np.mgrid[y0:y1, x0:x1]
    return (w * gx).sum() / w.sum(), (w * gy).sum() / w.sum()


def main(path):
    R, G, B = read_planes(path)

    # Peaks in green, at the 99.9th percentile -- the same 245-star sample the
    # spec's numbers come from, so the before and after are comparable.
    thresh = np.percentile(G, 99.9)
    ys, xs = np.where(G >= thresh)

    seen, stars = set(), []
    for y, x in zip(ys, xs):
        key = (y // 16, x // 16)      # one star per 16x16 cell, brightest wins
        if key in seen:
            continue
        seen.add(key)
        stars.append((int(x), int(y)))

    for name, plane in (("R-G", R), ("B-G", B)):
        d = []
        for x, y in stars:
            a, b = centroid(G, x, y), centroid(plane, x, y)
            if a and b:
                d.append(np.hypot(b[0] - a[0], b[1] - a[1]))
        d = np.array(d)
        print(f"{name}: n={len(d)}  median={np.median(d):.3f} px  "
              f"mean={d.mean():.3f} px")


if __name__ == "__main__":
    main(sys.argv[1])
```

The `read_planes` stub is deliberate and must be filled in by whoever runs this — wiring it to the project's existing XISF path is a two-line change, and guessing at that path here would be worse than saying so.

- [ ] **Step 5: Measure the M3 set**

Re-stack `/home/scarter4work/projects/processing/M3` with the new build and run the script on `NukeX_stacked`.

| | |
|---|---|
| before | R-G 0.435 px |
| floor | B-G 0.058 px |
| **bar** | **R-G median below 0.10 px** |

Also check green's median star FWHM against the previous stack. Green is never resampled, so it must be unchanged. A change there means the reference channel is being warped, which would be a bug in Task 5's near-identity path.

- [ ] **Step 6: Version bump, sign, package**

Per the release workflow in CLAUDE.md, in this order, none skipped:

In `src/module/NukeXVersion.h`, currently 5.0.1.0, bump
`NUKEX_MODULE_VERSION_BUILD` to 1 and set `NUKEX_MODULE_RELEASE_YEAR`,
`_MONTH` and `_DAY` to today. Then:

```bash
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
cmake --build build --target package     # signs the module and the XRI
```

`package` signs the module, builds the tarball, updates the SHA1 in
`updates.xri` and signs that too. The version bump and the package files are
committed together, in one commit, and only then pushed.

Do **not** `sudo` anything into `/opt/PixInsight`. That directory is owned by `scarter4work` and group-writable; a root-owned file there makes PixInsight's own updater fail with `Permission denied` and leaves a half-applied update. If the E2E harness needs a module in `<PI>/bin`, install it without elevation.

- [ ] **Step 7: Commit**

```bash
git add test/fixtures/golden/bayer_rgb_m27_2023.json \
        test/fixtures/golden/bayer_nb_hao3_m16.json \
        tools/measure_channel_registration.py \
        src/module/
git commit -m "test(e2e): re-baseline the Bayer goldens for channel registration

Both Bayer cases move for two reasons, both intended: star detection now
runs on green rather than channel 0, and the colour channels are now
registered to each other within each frame.

lrgb_mono_ngc7635 does NOT move and was verified first. It is frozen, a
one-channel stack takes the no-op path at every step of this feature, and
it is the anchor proving nothing else drifted.

Acceptance measured on the M3 set: median red-to-green separation, against
0.435 px before and a 0.058 px floor."
```

---

## Self-Review

**Spec coverage.** Every section of the design document maps to a task:

| spec section | task |
|---|---|
| Where it sits (measure between homography and warp) | 5 |
| Green is the reference (detector + registration) | 1, 2 |
| The model (uniform scale + translation, no rotation) | 2 |
| Interface (`ChannelTransform`, `ChannelTransforms`, measure) | 2 |
| `warp` overload applying `H · A_c` | 4 |
| Failure ladder, all six rows | 3 (rows 1-4), 2 (isolation), 5 (near-identity skip) |
| Testing: synthetic and exact | 2 |
| Testing: the ladder | 3 |
| Testing: integration through the engine | 5 |
| Testing: acceptance on real data | 7 |
| Goldens move / frozen mono does not | 7 |
| Decided: independent per-channel fits | 2 (each channel fitted in its own loop iteration) |

**The near-identity skip lands in Task 5 rather than 3**, deliberately: it needs the frame's dimensions to know the corner radius, and `measure_channel_transforms` does not see a decision about resampling.

**The spec's isolation row is implemented as its own filter, and an earlier draft of this plan was wrong about that.** That draft argued `StarDetector::Config::exclusion_radius` already keeps detections apart, so the catalog arrives isolated by construction and no separate pass was needed. It does not: `exclusion_radius` defaults to 5 and the centroid box is 13 wide, so the detector permits exactly the neighbours that hurt. Simulating a field where 30% of stars had a companion 7.6 px away put the fit error at 0.029 px using every star and 0.006 px using only the isolated ones. `min_neighbour_separation` exists because that claim was tested and failed.

**Type consistency.** `ChannelTransform`, `ChannelTransforms`, `ChannelRegistrationConfig`, `measure_channel_transforms`, `describe_channel_transforms`, `default_reference_channel`, `ChannelTransform::Fit`, `ChannelTransform::apply`, `ChannelTransform::max_displacement`, `ChannelTransforms::negligible`, `FrameAligner::Config::register_channels`, `FrameAligner::Config::channel_config`, `AlignedFrame::channels` — each is defined in exactly one task and spelled identically at every later use. The aggregate initialisation `ChannelTransform{1.000238, 0.31, -0.12, 187, 0.043, Fit::Affine}` used in the Task 3 and Task 6 tests matches the member order declared in Task 2: `s, tx, ty, n_stars, residual, fit`.

**One ordering hazard, called out here because it is easy to trip on.** Task 2's implementation calls `fit_channel`, which Task 3 replaces. Task 2 must define the simple version in the anonymous namespace *above* `measure_channel_transforms`, or it will not compile. Task 3's step 3 replaces that definition in place rather than adding a second one.
