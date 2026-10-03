### Task 22: Real-data visual validation (M16 HaO3, M27 LRGB-mono, M27 OSC)

Human eyes required for the verdicts. Everything else is scripted. The dev module and database are already installed from Task 21 Step 0.

**Files:**
- Create: `docs/superpowers/specs/2026-04-26-color-science-overhaul/visual-evidence/M16_HaO3_v4.0.1.0_baseline.png`, `M16_HaO3_v5.png`, `M16_HaO3_green_histogram.png`, `M27_LRGB_v5.png`, `M27_OSC_v5.png`
- Create: `docs/superpowers/specs/2026-04-26-color-science-overhaul/visual-evidence/README.md`

- [ ] **Step 1: v4 baseline for M16 HaO3 (scripted; no PixInsight dialogs, no sudo)**

`tools/run_e2e.sh` wipes `/tmp/nukex_e2e` at the start of every run, so first preserve the v5 outputs Task 21 produced, then run the released v4.0.1.0 module on the same 12 M16 HaO3 frames through the harness with a one-case manifest, then restore the v5 dev module.

```bash
cd /home/scarter4work/projects/nukex5
# 1. preserve v5 outputs
rm -rf /tmp/nukex_v5_outputs && cp -r /tmp/nukex_e2e /tmp/nukex_v5_outputs
ls /tmp/nukex_v5_outputs/bayer_nb_hao3_m16/primary/ /tmp/nukex_v5_outputs/mono_lrgb_m27_2025/primary/ /tmp/nukex_v5_outputs/bayer_rgb_m27_2023/primary/
# 2. install the released v4.0.1.0 module (signed .so + .xsgn from the v4 repository tarball)
mkdir -p /tmp/nukex_v4_pkg && tar -C /tmp/nukex_v4_pkg -xzf ~/projects/nukex4/repository/20260425-linux-x64-NukeX.tar.gz
cp /tmp/nukex_v4_pkg/bin/NukeX-pxm.so /tmp/nukex_v4_pkg/bin/NukeX-pxm.xsgn /opt/PixInsight/bin/
# 3. one-case manifest (same 12 frames, same Auto stretch), run, save the result out of the wipe path
cp test/fixtures/e2e_manifest.json /tmp/e2e_manifest.v5.json
python3 - <<'PY'
import json
m = json.load(open('test/fixtures/e2e_manifest.json'))
case = next(c for c in m['cases'] if c['name'] == 'bayer_nb_hao3_m16')
case = dict(case, name='m16_v4_baseline')
m['cases'] = [case]
json.dump(m, open('test/fixtures/e2e_manifest.json', 'w'), indent=2)
PY
(cd build && make e2e 2>&1 | tail -8)
mkdir -p /tmp/nukex_v4_baseline && cp /tmp/nukex_e2e/m16_v4_baseline/primary/stretched.fit /tmp/nukex_v4_baseline/m16_stretched.fit
# 4. restore the committed manifest and the v5 dev module
cp /tmp/e2e_manifest.v5.json test/fixtures/e2e_manifest.json && git diff --exit-code test/fixtures/e2e_manifest.json
cp build/src/module/NukeX-pxm.so build/src/module/NukeX-pxm.xsgn /opt/PixInsight/bin/
```
Expected: the v4 run reports `execute_ok` for `m16_v4_baseline` (golden unchecked); `/tmp/nukex_v4_baseline/m16_stretched.fit` exists; `git diff --exit-code` on the manifest returns 0; `/opt/PixInsight/bin/NukeX-pxm.so` carries the v5 build's timestamp again. Note: the v4 harness's `light_glob`/`max_frames` fields are honoured by the harness JS (which is the v5 tree's `tools/validate_e2e.js`, not the module), so v4 sees exactly the same 12 frames.

- [ ] **Step 2: Render comparison PNGs + the green histogram**

```bash
python3 - <<'EOF'
import numpy as np, matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
from astropy.io import fits
out = "docs/superpowers/specs/2026-04-26-color-science-overhaul/visual-evidence"
def load(p):
    a = fits.getdata(p).astype("float32")
    return np.moveaxis(a, 0, -1) if a.ndim == 3 and a.shape[0] in (3, 4) else a
def png(a, path):
    a = np.clip(a, 0, 1); plt.imsave(path, (a * 255).astype("uint8"))
v4 = load("/tmp/nukex_v4_baseline/m16_stretched.fit")
v5 = load("/tmp/nukex_v5_outputs/bayer_nb_hao3_m16/primary/stretched.fit")
png(v4, f"{out}/M16_HaO3_v4.0.1.0_baseline.png"); png(v5, f"{out}/M16_HaO3_v5.png")
fig, ax = plt.subplots(figsize=(8, 4))
for name, img in (("v4.0.1.0", v4), ("v5", v5)):
    if img.ndim == 3:
        g_excess = img[..., 1] - 0.5 * (img[..., 0] + img[..., 2])
        ax.hist(g_excess.ravel(), bins=200, range=(-0.3, 0.3), histtype="step", label=name)
ax.set_xlabel("G - (R+B)/2 per pixel"); ax.legend(); fig.savefig(f"{out}/M16_HaO3_green_histogram.png", dpi=120)
png(load("/tmp/nukex_v5_outputs/mono_lrgb_m27_2025/primary/stretched.fit"), f"{out}/M27_LRGB_v5.png")
png(load("/tmp/nukex_v5_outputs/bayer_rgb_m27_2023/primary/stretched.fit"), f"{out}/M27_OSC_v5.png")
print("ok")
EOF
```
Expected: five PNGs (8-bit, per `feedback_8bit_png`). Embed them inline in the session for review; do not offer `xdg-open`.

- [ ] **Step 3: Judge against the spec §7.4 bar and record**

Write `visual-evidence/README.md` with one row per image: corpus, frame count, stretch, and the verdict against the bar (M16: no green cast, Ha red / OIII teal, the histogram's v5 curve centred nearer zero than v4's; M27 LRGB: detail preserved, colour natural; M27 OSC: natural colour with synthesised L). Include the `NUKEX_QE_CONFIDENCE` keyword value read from `stretched.fit`'s composed sibling (`fits.getheader("/tmp/nukex_v5_outputs/bayer_nb_hao3_m16/primary/composed.fit")["NUKEX_QE_CONFIDENCE"]`) — expected `database` for M16 (ASI2400MC resolves).

If the M16 bar is not met, the fix is in `src/lib/compose` (palette vectors, Task 6 of the April plan) or in the Q-solve inputs (Task 15's `HaO3` line FWHM or the ASI2400MC QE row) — not in the stretch. Open that as a new task in this document before continuing to Task 23.

- [ ] **Step 4: Commit the evidence**

```bash
git add docs/superpowers/specs/2026-04-26-color-science-overhaul/visual-evidence
git commit -m "$(cat <<'EOF'
docs(visual-evidence): v5 vs v4.0.1.0 on M16 HaO3, M27 LRGB-mono, M27 OSC

Side-by-side stretched outputs and the per-pixel green-excess histogram
for the dual-NB motivating case, with the spec 7.4 verdicts in README.md.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01NqDedRCQxJy8EbbzWDKMFR
EOF
)"
```

---

## Wave 4

