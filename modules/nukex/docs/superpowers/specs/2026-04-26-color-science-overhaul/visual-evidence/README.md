# v5.0.0.0 real-data visual evidence

Recorded 2026-09-04 against spec §7.4. Every image is an 8-bit PNG rendered
from the FITS the E2E harness saved, at the pixel values the committed goldens
hash, box-averaged down to 2048 px on the long edge so the evidence stays
committable. Averaging preserves colour balance exactly, which is what these
images are here to show.

The corpora are the ones that exist on this machine. The spec's "M27 HaO3" set
does not; the dual-narrowband corpus is M16 HaO3.

| image | corpus | frames | window |
|---|---|---|---|
| `M16_HaO3_v4.0.1.0_baseline.png` | M16 HaO3, ZWO ASI2400MC Pro, RGGB | 12 of 30 | stretched, Auto |
| `M16_HaO3_v5.png` | same frames | 12 | stretched, Auto |
| `M16_HaO3_green_histogram.png` | same frames, per-pixel green excess | 12 | stretched |
| `M16_HaO3_v5_composed_autoscaled.png` | same frames | 12 | composed, auto-scaled |
| `M27_OSC_v5.png` | M27 2023, ASI2400MC, no FILTER | 33 | stretched, Auto |

The first two images are the same picture. That is the finding, not an
oversight — see the M16 verdict.

## The v4 baseline is a rebuild, not the released binary

The released v4.0.1.0 binary cannot be used here: it links `libglog.so.0` and
this machine has only `libglog.so.2`, so PixInsight reports
`NukeX is not defined`. The baseline is **v4.0.1.0 source rebuilt with today's
toolchain**, which is the better control anyway — same compiler, same
libraries, so the only variable left is the v5 code.

Both runs also align to the same reference frame. v4 takes the first frame;
v5's rule keeps the first frame whenever it is viable, and here it is. So the
only variable between the two runs is the colour-science code — which turns
out to leave the stacked and stretched windows untouched. See the M16 verdict.

## M27 LRGB-mono is absent, deliberately

The spec asks for a third image, M27 2025 LRGB-mono. There is none to show:
NukeX refuses multi-filter mono batches as of this release. That configuration
produced one populated channel out of four and three exactly-zero ones, because
`FrameCache` is keyed on post-debayer geometry and Phase B assumes every
channel shares one frame set. See `test/integration/test_lrgb_mono.cpp` and the
`skip_reason` on that E2E case.

## Verdicts

### M27 OSC — passes the bar

Natural colour: the star field shows a real range of stellar colour, blue
through orange, with no cast in the background. The Dumbbell is visible at
centre. Per-channel medians on the stretched output are R 0.109, G 0.134,
B 0.129 — a mild green lean that is ordinary for un-white-balanced OSC data.

`NUKEX_QE_CONFIDENCE = database`, so the ASI2400MC resolved rather than
falling back to the generic Sony OSC row. `NUKEX_GAMUT_CLIPPED = 126` of
24,543,024 pixels.

### M16 HaO3 — the comparison the spec asked for cannot be made, and that is the result

The spec asks to put v4's stretched output beside v5's and show the green cast
gone. Running it produces something more interesting: **the two are
bit-identical.**

| window | v4.0.1.0 | v5.0.0.0 |
|---|---|---|
| `stacked` | `394f47de5901188e` | `394f47de5901188e` |
| `stretched` | `54e8428f21f41fe6` | `54e8428f21f41fe6` |
| per-pixel green excess, median | +0.00079 | +0.00079 |

(SHA-256 over the float32 pixel buffer, first 16 hex digits.)

Phase A does route these frames into Ha/OIII slots instead of plain R/G/B, so
the routing genuinely changed — but the stacked pixel values are the same
debayered channels in the same order, so the stack does not move, and neither
does the stretch applied to it. Neither version shows a green cast here: the
median green excess is +0.0008 in both, which is neutral.

**The colour science is additive.** v4 produced three windows; v5 produces four.
`NukeX_composed` is new, and it is the only place the emission-line palette
appears. Judging v5's colour from the stretched window compares the wrong
image.

So the honest verdict against the §7.4 bar: **"no green cast" passes, and it
passed in v4 too on this corpus. "Ha red, OIII teal" does not pass** — see
`M16_HaO3_v5_composed_autoscaled.png` and the second observation below.

That the M16 stretched output is unchanged also means this corpus cannot
settle the v4.0.0.7 green-cast complaint, which was raised on M27 narrowband
data. Whatever that complaint was about, it was not the code path these 12
frames exercise.

## Two observations that are not verdicts

**The auto-stretch leaves both corpora dark.** M16's stretched output has a
median of 0.084 and a 99.9th percentile of 0.101; the nebula sits in the top
0.01%. This is *not* a v5 regression: the NGC7635 stretched output is
bit-identical to v4's (`5ca3caef`), so the stretch path did not move in this
release. It is the same tendency recorded after the first real-data stack on
v4.0.0.7 ("a bit faint"), more pronounced on narrowband. A tuning question,
and a separate one.

**The dual-narrowband composite is red on magenta, with no green at all.**
`M16_HaO3_v5_composed_autoscaled.png` is the composed window scaled between
the background median and the 99.99th percentile, because the window is linear
and renders black otherwise. It shows M16 with real structure — the pillars
are there — rendered entirely in Ha red on a magenta background. `NukeX_composed`
on M16 has 10 non-zero green pixels out of 24,543,024 and reports
`NUKEX_GAMUT_CLIPPED = 24,516,516`. The same release on the plain-OSC corpus —
same camera, same frame size — clips **126**. So this is specific to the
emission-palette path, not the composer in general. `ColorComposer::compose`
weights the palette by raw signal and never divides by the total, so chroma
scales with absolute brightness: on data whose 99.99th percentile is 0.04,
essentially the whole frame lands below visibility and what little chroma
there is comes from Ha, which swamps OIII. The palette itself is innocent —
OIII's Lab entry yields positive green at every luminance. Whether the missing
normalisation is a dropped division or a deliberate choice not to colour noise
is a call for the author, so nothing was changed.

## Provenance

- Module: NukeX 5.0.0.0, clean build, `NUKEXVER` in every FITS header.
- Frames: `NFRAMES 12` / `NFAILALN 0` for M16, `33` / `0` for M27 OSC.
- Rendered by `tools/`-adjacent throwaway script from
  `~/.cache/nukex_e2e_keep/<case>/primary/stretched.fit`, the same files the
  goldens were hashed from.
