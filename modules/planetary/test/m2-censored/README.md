# M2 censored-estimate measurements (not shipped)

The figures in `src/stack/Accumulate.h` ("Clipping", Limits) for re-review 3's M2 and M6 under Ruling R6 come from
`run.sh` here. Nothing here is built by CMake or run by ctest, and nothing launches PixInsight.

| File | What |
|---|---|
| `run.sh AVI SCRATCH [REV]` | Builds four engines from `git archive` and stacks four SERs with `stack-probe --threads 32`, then prints every measure. The engines are the truth (`TRUTH_REV`, default 3a3092b3), HEAD's rule (`REV`, default 014c89b6), the WIP and the Tobit variant. |
| `wip.patch` | Round 4's WIP Finish, applied to 014c89b6. The bounds of frames no brighter than the reference count at full scale and the rest at their mean; each bound under the estimate counts at it. |
| `tobit.patch` | The WIP plus a Tobit estimate, on when `TOBIT_SIGMA` is set. Bounds are censored at their mean, and sigma is the unsaturated samples' spread floored at `TOBIT_SIGMA`. Each bound counts at E[x given x > its level]. A scratch variant only: its sums are not in `BytesHeld`. |
| `mkser.py` | Converts the pal8 AVI to an 8-bit SER (colour id 8, BAYER_RGGB; stacked with `--bayer RGGB`, the same pattern), with a gain applied to a frame range and a dead site. |
| `cmp.py` | The measures: blue counts at five thresholds, blue mean error per truth band, mean absolute error over truth >= 0.97, the deep core, and dead-site dimming. |

Input: `/mnt/qnap/astro_data/3_6_2023/moon/2023-03-05-224808-Moon-Bin1 18.3F.avi`, 1705 frames. Copy it off the
NAS first; it is only read. The four SERs:

- **base:** the capture as recorded;
- **dead:** blue site (1469, 215) at 5 DN in every frame;
- **g125:** frames 0-255 multiplied by 1.25 and clipped at 255;
- **g125dead:** both edits.

Scratch use is about 15 GB, and a run takes about 15 minutes on 32 threads.
