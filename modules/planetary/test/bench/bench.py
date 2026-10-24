#!/usr/bin/env python3
"""bench.py -- the run plan, side-by-side PNGs and summary table for run-stack-bench.sh.

    python3 -I bench.py plan                       # TSV: run, capture, bayer, expect, video, WxH
    python3 -I bench.py report <out-dir> [run ...] # per run: compare.json + side-by-side.png;
                                                   # then <out-dir>/summary.md over every run present

run-stack-bench.sh stacks each run with stack-probe into <out-dir>/<run>/ (ours.fits, probe.json,
probe.log, wall.txt) and then calls `report`. This module never stacks and never writes outside
<out-dir>; the NAS is only read.

RUNS. One run per benchmark capture (benchset.BENCHMARK), stacked with the capture's recorded
pattern: RGGB for every colour capture (ZWO ASI585MC / ASI2400MC, camera spec; Scott 2026-10-22),
Auto for the diagnosis cases.

STATUS (Ruling 2 with Ruling P28b). The judged numbers are END_full and, where the interior is judged,
END_interior (metric.py), each as ours over the best reference, against 0.97. Ours is charged its
MEASURED noise -- N = budget x max(1, R(f) / R_cal(f)) per END bin, R the split halves' measured / model,
R_cal the engine's frozen calibration for the target's kind (halves.charge) -- and the references stay
on the photon budget: C1 can only inflate a reference's END, which only lowers ours' ratio, so a
numeric pass is sound and a numeric fail is not unless the reference is known to be unprocessed.
  unmeasured: END is missing for ours or for some reference -- no verdict, loudly;
  review:     the verdict flips between our two frame counts (rule 5); or the numbers fail against a
              reference whose provenance (refs.provenance) is not "none" -- processed or unknown, so C1
              may be what beat us (C1_NOTE); or a fail that does not also hold with ours on the bare
              budget, on both readings (CHARGE_NOTE); or the numbers pass but ours could not be charged
              on both readings (UNCHARGED_NOTE), the split halves lie outside their kind's tolerance in
              either direction (the charge is not confirmed), or a review note applies;
  fail:       END below the bar under both readings, against references whose files record no
              processing (AutoStakkert raw stacks with their .as3), AND below it with ours on the bare
              budget too -- the charge over-charges ours (the halves hold signal they do not share), so
              a fail only under the charge is review (CHARGE_NOTE);
  pass:       END >= 0.97 under the charge on both readings, split halves within their kind's
              tolerance, no review note. Scott's side-by-side is still the spec's final word.
DAN_full / DAN_interior stay in every table as secondary figures; they decide nothing.

IDENTITY (Ruling F2; final review, bench I1). Freeze-before-use is enforced, not assumed. report takes
the identity in use (identity.current: the engine's and the probes' sources, the probe binaries, the
calibration procedure) and gives a run NO VERDICT -- unmeasured, with every reason -- when the frozen
calibration does not describe it (halves.calibration_problems: an identity part that differs or is
missing, calibration.json not committed as it stands, a stale probe), or when the run's directory was
stacked with another identity (its identity.json, written by run-stack-bench.sh; missing = unknown).
Every compare.json carries the identity it was judged with and the calibration's; summary.md prints
both, and flags a record kept from an earlier report whose identity is not the one in use.

SPLIT HALVES (Ruling P28 rule 3). When run-stack-bench.sh stacked the capture's even and odd frames
separately (halfA/halfB .fits, .json, .keff.json in the run's directory, both k over A's box),
report compares their difference with END's model for it (halves.check) against the kind's frozen
tolerance (bench/calibration.json), and derives ours' charge from it.

SIDE-BY-SIDE. One panel per image -- ours, then each reference in benchset order -- all cropped
from the SAME region of ours' grid at each reference's registered integer offset (the crop the
metric used; refs.crop_registered), at the same scale, and with the same display stretch: on the
metric box, each channel's black is its own 1st percentile (after a sigma-1 smooth: a sky
pedestal does not tint the sky), the white is the 99.5th percentile of the black-subtracted
brightest channel with ONE factor for every channel (the target keeps each tool's own colour
balance and no channel clips first), then clip and the row's gamma, the same for every image.
Planets: a context row (the metric box widened by 75 % of its size each side, x2 nearest, gamma
1/2.2 so halos, moons and sky artefacts show) and a zoom row (the metric box, nearest-neighbour to
~320 px so every stacked pixel is visible, LINEAR so a bright disc's belts are not compressed).
Lunar, gamma 1/2.2: an overview row (the region every image covers, area-downsampled to 560 px
wide) and two 1:1 detail crops at x2 nearest, on the two most detailed 192 px tiles of ours that
lie wholly on the target (not the limb), at least 400 px apart.
"""
import argparse
import json
import os
import sys
from dataclasses import dataclass
from typing import List, Optional, Sequence, Tuple

import numpy as np
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import benchset  # noqa: E402
import identity  # noqa: E402
import metric    # noqa: E402
import refs      # noqa: E402
from metric import Box  # noqa: E402

DISPLAY_GAMMA = 1 / 2.2
BLACK_PCT, WHITE_PCT = 1.0, 99.5
PLANET_ZOOM_TARGET = 320       # px, the zoom row's panel side
CONTEXT_GROW = 0.75            # planets: context = box grown by this x its size, each side
LUNAR_OVERVIEW_W = 560
LUNAR_TILE = 192               # 1:1 detail tiles, shown x2
LUNAR_TILE_SEP = 400
HEADER_H = 58
PAD = 6


@dataclass(frozen=True)
class Run:
    id: str
    capture: str
    bayer: str          # stack-probe --bayer
    expect: str
    video: str
    note: str = ""


def plan() -> List[Run]:
    return [Run(c.id, c.id, c.bayer or "auto", c.expect, benchset.nas(c.video)) for c in benchset.BENCHMARK]


def run_by_id(rid: str) -> Run:
    for r in plan():
        if r.id == rid:
            return r
    raise KeyError(rid)


# --- status ----------------------------------------------------------------------------------

def _verdict(full, inner, judged):
    """'fail' (with reasons) or 'pass' of one pair of ratios against the bar."""
    reasons = []
    if full is not None and full < refs.PASS_BAR:
        reasons.append(f"END_full {full:.3f} x best < {refs.PASS_BAR}")
    if judged and inner is not None and inner < refs.PASS_BAR:
        reasons.append(f"END_interior {inner:.3f} x best < {refs.PASS_BAR}")
    return ("fail" if reasons else "pass"), reasons


C1_NOTE = ("C1: a numeric fail is not sound here -- END cannot see noise or sharpening beyond the photon budget "
           "in a reference (metric.py KNOWN LIMIT), and this reference's processing is not known to be none")
CHARGE_NOTE = ("the fail rests on our measured-noise charge: the split halves also hold signal they do not share "
               "(each half's regions keep their own frames), which over-charges ours, so a fail is sound only if "
               "it holds on the bare budget too")
UNCHARGED_NOTE = ("our noise is not measured (no split halves): a pass needs ours charged its measured noise "
                  "(Ruling P28b), so the numbers cannot pass this capture")


def _setters(rows: List[dict], key: str) -> List[dict]:
    """The reference(s) holding the best `key` (END full or interior): what a fail is measured against."""
    vals = [r[key] for r in rows[1:] if r[key] is not None]
    return [r for r in rows[1:] if vals and r[key] == max(vals)]


def status_of(rows: List[dict], run_id: str = "", split_half: Optional[dict] = None) -> Tuple[str, List[str]]:
    """Ruling 2 with Ruling P28b, from the compare rows (refs.rows_to_dicts(), ours first) and the
    split-half record (halves.check): (status, reasons). See STATUS in the module doc."""
    ours = rows[0]
    missing = [f"{r['label']}: {r['end_note'] or 'END missing'}" for r in rows
               if r["end"] is None or (ours["interior_judged"] and r["end_interior"] is None)]
    if missing or ours["end_ratio"] is None:
        return "unmeasured", ["NO VERDICT -- END unmeasurable for " + "; ".join(missing or ["the references"])]
    if ours["end_alt_ratio"] is None:
        return "unmeasured", ["NO VERDICT -- END with our mean-kept-fraction frame count (the flip check) is missing"]
    judged = ours["interior_judged"]
    v, reasons = _verdict(ours["end_ratio"], ours["end_interior_ratio"], judged)
    v_alt, _ = _verdict(ours["end_alt_ratio"], ours["end_interior_alt_ratio"], judged)
    notes = []
    if split_half is not None and split_half.get("status") not in ("agrees", None):
        import halves
        notes.append(f"split halves {split_half.get('status')}: {halves.summary_line(split_half)} -- the noise model and "
                     f"ours' charge are not confirmed on this capture")
    if v != v_alt:
        alt = f"{ours['end_alt_ratio']:.3f}" + (f" / {ours['end_interior_alt_ratio']:.3f}" if judged else "")
        own = f"{ours['end_ratio']:.3f}" + (f" / {ours['end_interior_ratio']:.3f}" if judged else "")
        return "review", [f"verdict flips with our frame count: {v} at the box's noise-power k {ours['end_frames']:.0f} "
                          f"(END {own}), {v_alt} at the mean kept fraction's {ours['end_alt_frames']:.0f} (END {alt})"] \
            + notes + list(ours["review"])
    if v == "fail":
        if ours.get("end_noise_factor") is not None:
            v_bare, _ = _verdict(ours.get("end_uncharged_ratio"), ours.get("end_interior_uncharged_ratio"), judged)
            v_bare_alt, _ = _verdict(ours.get("end_uncharged_alt_ratio"), ours.get("end_interior_uncharged_alt_ratio"), judged)
            if v_bare != "fail" or v_bare_alt != "fail":       # P28c, on both readings (review N2-m1)
                bare = f"{_f(ours.get('end_uncharged_ratio'), '{:.3f}')}" + (
                    f" / {_f(ours.get('end_interior_uncharged_ratio'), '{:.3f}')}" if judged else "")
                bare_alt = f"{_f(ours.get('end_uncharged_alt_ratio'), '{:.3f}')}" + (
                    f" / {_f(ours.get('end_interior_uncharged_alt_ratio'), '{:.3f}')}" if judged else "")
                return "review", reasons + [f"{CHARGE_NOTE} (on the bare budget ours is {bare}; with the mean-kept "
                                            f"k {bare_alt})"] + notes + list(ours["review"])
        setters = _setters(rows, "end") + (_setters(rows, "end_interior") if judged else [])
        unsound = [r for r in setters if (r.get("provenance") or {}).get("state") != "none"]
        if unsound:
            return "review", reasons + [C1_NOTE + ": " + "; ".join(
                f"{r['label']}: {(r.get('provenance') or {}).get('text', 'no provenance')}" for r in unsound)] \
                + notes + list(ours["review"])
        return "fail", reasons + [f"{r['label']}: {r['provenance']['text']}" for r in setters] + notes \
            + list(ours["review"])        # a review note never turns a fail into review (Task 9), but it is shown
    if ours.get("end_noise_factor") is None or ours.get("end_alt_noise_factor") is None:
        return "review", [UNCHARGED_NOTE] + notes + list(ours["review"])
    if split_half is not None and split_half.get("status") == "outside":   # either direction (review I-G)
        return "review", notes + list(ours["review"])
    if ours["review"]:
        return "review", list(ours["review"]) + notes
    return "pass", notes


# --- display ---------------------------------------------------------------------------------

def _rgb(img) -> np.ndarray:
    a = np.asarray(img, dtype=np.float64)
    return np.repeat(a[..., None], 3, axis=2) if a.ndim == 2 else a[..., :3]


def display_levels(img, box: Box) -> Tuple[List[float], float]:
    """(black per channel, white range) of an image for display, on `box`: each channel's black
    is its own 1st percentile (sigma-1 smoothed), so a sky pedestal in one channel does not tint
    the sky; the white range is the 99.5th percentile of the black-subtracted brightest channel,
    one factor for every channel, so the target keeps the tool's own colour balance and no
    channel of a coloured target clips before the others."""
    a = _rgb(img)[box.y0:box.y1, box.x0:box.x1]
    sm = ndimage.gaussian_filter(a, (1.0, 1.0, 0))
    black = [float(np.percentile(sm[..., c], BLACK_PCT)) for c in range(3)]
    white = float(np.percentile((sm - np.array(black)).max(axis=2), WHITE_PCT))
    if not white > 0:
        white = 1.0
    return black, white


def stretch(img, levels: Tuple[List[float], float], gamma: float = DISPLAY_GAMMA) -> np.ndarray:
    """uint8 HxWx3: per-channel black, one white range for all channels, clip, then x ** gamma
    (1/2.2 by default; 1 = linear, which keeps a bright disc's belts visible)."""
    black, white = levels
    x = np.clip((_rgb(img) - np.array(black)) / white, 0.0, 1.0) ** gamma
    return np.round(x * 255).astype(np.uint8)


def zoom_nearest(rgb: np.ndarray, k: int) -> np.ndarray:
    return np.repeat(np.repeat(rgb, k, axis=0), k, axis=1) if k > 1 else rgb


def area_down(rgb: np.ndarray, width: int) -> np.ndarray:
    if rgb.shape[1] <= width:
        return rgb
    import cv2
    h = max(1, int(round(rgb.shape[0] * width / rgb.shape[1])))
    return cv2.resize(rgb, (width, h), interpolation=cv2.INTER_AREA)


def intersect(a: Box, b: Box) -> Box:
    return Box(max(a.y0, b.y0), min(a.y1, b.y1), max(a.x0, b.x0), min(a.x1, b.x1))


def grow(box: Box, by: int, within: Box) -> Box:
    return intersect(Box(box.y0 - by, box.y1 + by, box.x0 - by, box.x1 + by), within)


def detail_tiles(lum: np.ndarray, region: Box, tile: int = LUNAR_TILE, sep: int = LUNAR_TILE_SEP,
                 n: int = 2, inside: Optional[np.ndarray] = None) -> List[Box]:
    """The n most detailed tile x tile windows of `lum` inside `region` (band-pass energy,
    difference of Gaussians sigma 1 - 4), at least `sep` px apart (centre to centre). With
    `inside` (a boolean mask on lum's grid, e.g. metric.target_mask), only tiles wholly on it:
    the limb's step would otherwise outscore every crater."""
    sub = lum[region.y0:region.y1, region.x0:region.x1]
    dog = ndimage.gaussian_filter(sub, 1.0) - ndimage.gaussian_filter(sub, 4.0)
    energy = ndimage.uniform_filter(dog * dog, size=tile)
    h2 = tile // 2
    valid = np.zeros_like(energy, dtype=bool)
    if sub.shape[0] > tile and sub.shape[1] > tile:
        valid[h2:sub.shape[0] - h2, h2:sub.shape[1] - h2] = True
    if inside is not None:
        m = np.asarray(inside, dtype=bool)[region.y0:region.y1, region.x0:region.x1]
        # wholly on the mask, and the band-pass's reach (3 x its sigma 4) clear of the limb
        on = ndimage.minimum_filter(m.astype(np.uint8), size=tile + 1 + 2 * 12).astype(bool)
        if (valid & on).any():
            valid &= on
    e = np.where(valid, energy, -np.inf)
    out = []
    for _ in range(n):
        k = int(np.argmax(e))
        if not np.isfinite(e.flat[k]):
            break
        cy, cx = divmod(k, e.shape[1])
        out.append(Box(region.y0 + cy - h2, region.y0 + cy - h2 + tile, region.x0 + cx - h2, region.x0 + cx - h2 + tile))
        yy, xx = np.ogrid[:e.shape[0], :e.shape[1]]
        e[(yy - cy) ** 2 + (xx - cx) ** 2 < sep * sep] = -np.inf
    return out


def _font(size):
    from PIL import ImageFont
    try:
        return ImageFont.load_default(size=size)
    except TypeError:           # Pillow < 10.1
        return ImageFont.load_default()


def _wrap(text: str, width_px: int, char_px: float = 6.6) -> List[str]:
    per = max(10, int(width_px / char_px))
    words, lines, cur = text.split(), [], ""
    for w in words:
        if cur and len(cur) + 1 + len(w) > per:
            lines.append(cur)
            cur = w
        else:
            cur = f"{cur} {w}" if cur else w
    if cur:
        lines.append(cur)
    return lines


def compose(rows_of_panels: Sequence[Sequence[np.ndarray]], headers: Sequence[Sequence[str]],
            row_titles: Sequence[str], title: str, out_path: str) -> None:
    """Grid PNG: one column per image (header lines on top), one row per crop kind."""
    from PIL import Image, ImageDraw
    ncol = len(headers)
    col_w = [max(r[c].shape[1] for r in rows_of_panels) for c in range(ncol)]
    col_w = [max(w, 200) for w in col_w]
    wrapped = [[ln for t in h for ln in _wrap(t, col_w[c])] for c, h in enumerate(headers)]
    head_h = max(HEADER_H, 16 * max(len(w) for w in wrapped) + 6)
    row_h = [max(p.shape[0] for p in r) for r in rows_of_panels]
    title_h = 26
    W = PAD + sum(w + PAD for w in col_w)
    H = title_h + head_h + sum(h + 18 + PAD for h in row_h) + PAD
    canvas = Image.new("RGB", (W, H), (24, 24, 24))
    d = ImageDraw.Draw(canvas)
    d.text((PAD, 5), title, fill=(255, 255, 255), font=_font(15))
    f = _font(12)
    x = PAD
    for c in range(ncol):
        for i, ln in enumerate(wrapped[c]):
            d.text((x, title_h + 2 + 16 * i), ln, fill=(235, 235, 235) if c else (255, 214, 120), font=f)
        x += col_w[c] + PAD
    y = title_h + head_h
    for r, panels in enumerate(rows_of_panels):
        d.text((PAD, y + 2), row_titles[r], fill=(170, 170, 170), font=f)
        y += 18
        x = PAD
        for c, p in enumerate(panels):
            canvas.paste(Image.fromarray(p), (x, y))
            x += col_w[c] + PAD
        y += row_h[r] + PAD
    canvas.save(out_path)


def side_by_side(run: Run, rows: List["refs.Row"], dicts: List[dict], probe: dict, ours_path: str,
                 out_path: str, status: str) -> dict:
    """Write the side-by-side PNG; returns what it showed (regions, levels) for compare.json."""
    c = benchset.by_id(run.capture)
    ours = refs.load_image(ours_path)
    lum = metric.luminance(ours)
    box = Box(*rows[0].result.box)
    images = [ours]
    regs = [None]
    region = Box(0, lum.shape[0], 0, lum.shape[1])
    for row in rows[1:]:
        ref = refs.load_reference(row.path, frame_size=c.size)
        images.append(ref.image)
        regs.append(row.registration)
        region = intersect(region, refs.overlap_box(row.registration, ref.image.shape, lum.shape))

    def crop(i, b: Box):
        return images[i][b.y0:b.y1, b.x0:b.x1] if regs[i] is None else refs.crop_registered(images[i], regs[i], b)

    levels = [display_levels(crop(i, box), Box(0, box.y1 - box.y0, 0, box.x1 - box.x0)) for i in range(len(images))]
    panel_rows, row_titles = [], []
    if c.kind == "planet":
        bh, bw = box.y1 - box.y0, box.x1 - box.x0
        ctx = grow(box, int(round(CONTEXT_GROW * max(bh, bw))), region)
        panel_rows.append([zoom_nearest(stretch(crop(i, ctx), levels[i]), 2) for i in range(len(images))])
        row_titles.append(f"context x2, gamma 2.2 (ours' pixels y {ctx.y0}-{ctx.y1}, x {ctx.x0}-{ctx.x1})")
        k = max(1, int(round(PLANET_ZOOM_TARGET / max(bh, bw))))
        panel_rows.append([zoom_nearest(stretch(crop(i, box), levels[i], 1.0), k) for i in range(len(images))])
        row_titles.append(f"metric box x{k} nearest, LINEAR (y {box.y0}-{box.y1}, x {box.x0}-{box.x1})")
        shown = {"context": list(ctx), "zoom": list(box), "zoom_factor": k}
    else:
        panel_rows.append([area_down(stretch(crop(i, region), levels[i]), LUNAR_OVERVIEW_W) for i in range(len(images))])
        row_titles.append(f"overview, area-downsampled (y {region.y0}-{region.y1}, x {region.x0}-{region.x1})")
        tiles = detail_tiles(lum, intersect(box, region), inside=metric.target_mask(lum))
        for t in tiles:
            panel_rows.append([zoom_nearest(stretch(crop(i, t), levels[i]), 2) for i in range(len(images))])
            row_titles.append(f"1:1 detail x2 nearest (y {t.y0}-{t.y1}, x {t.x0}-{t.x1})")
        shown = {"overview": list(region), "tiles": [list(t) for t in tiles]}

    headers = []
    for i, dct in enumerate(dicts):
        score = (f"END {dct['end']:.2f} ({_f(dct['end_ratio'], '{:.3f}')}x best)" if dct["end"] is not None
                 else "END: unmeasurable")
        score += (f"  int {dct['end_interior']:.2f} ({_f(dct['end_interior_ratio'], '{:.3f}')}x)"
                  if dct["end_interior"] is not None else
                  ("  int: not judged (limb-only)" if not dct["interior_judged"] else "  int: unmeasurable"))
        dan = f"DAN {dct['dan']:.4f} ({_f(dct['ratio_to_best_reference'], '{:.3f}')}x), k {_f(dct['end_frames'], '{:.0f}')}"
        if i == 0:
            headers.append([f"OURS ({run.bayer}): {probe['used']}/{probe['measured']} frames, "
                            f"{probe['seconds']:.1f} s", score, dan])
        else:
            headers.append([dct["label"], score, dan, f"{dct['orientation']} ncc {dct['ncc']:.3f}"])
    title = f"{run.id}: {status.upper()} -- same crop, scale and stretch (black per channel, one white at 99.5th pct, gamma 2.2)"
    compose(panel_rows, headers, row_titles, title, out_path)
    return {"shown": shown, "display_levels": [{"black": b, "white": w} for b, w in levels]}


# --- report ----------------------------------------------------------------------------------

def _load_json(path):
    with open(path) as fh:
        return json.load(fh)


@dataclass
class Context:
    """What a report judges with (IDENTITY in the module doc): the identity in use, the frozen
    calibration and why it cannot judge (empty: it can)."""
    used: dict
    calibration: dict
    calibration_problems: List[str]

    @staticmethod
    def make(stack_probe=None, region_probe=None) -> "Context":
        import halves
        used = identity.current(stack_probe, region_probe)
        cal = halves.load_calibration()
        return Context(used, cal, halves.calibration_problems(cal, used))

    def stamp(self, rec: dict) -> dict:
        rec["identity"] = {p: self.used.get(p) for p in identity.PARTS}
        rec["calibration_identity"] = self.calibration.get("identity")
        rec["calibration_generated"] = self.calibration.get("generated")
        return rec

    def run_problems(self, run_dir: str) -> List[str]:
        """Why the run's stacks were not made with the identity in use (its identity.json)."""
        path = os.path.join(run_dir, "identity.json")
        stacked = _load_json(path) if os.path.exists(path) else None
        return [f"the run was stacked with another identity: {m}" for m in identity.mismatches(stacked, self.used)]


def _no_verdict(rec: dict, problems: List[str]) -> dict:
    """A record judged against a calibration or stacks that do not describe the engine in use: NO VERDICT."""
    if problems:
        was = rec.get("status")
        rec["status"] = "unmeasured"
        rec["reasons"] = (["NO VERDICT -- " + "; ".join(problems) + (f" (the numbers alone would say {was})" if was else "")]
                          + list(rec.get("reasons", [])))
    return rec


def report_run(run: Run, run_dir: str, ctx: Optional[Context] = None) -> dict:
    """compare.json (+ side-by-side.png) for one stacked run; the diagnosis check for a diagnosis
    run. Returns the record summary.md is built from. ctx: what it is judged with (Context.make() when
    None)."""
    ctx = ctx or Context.make()
    probe_path = os.path.join(run_dir, "probe.json")
    rec = ctx.stamp({"run": run.id, "capture": run.capture, "bayer": run.bayer, "expect": run.expect, "note": run.note})
    if not os.path.exists(probe_path):
        rec["status"] = "not run"
        return rec
    probe = _load_json(probe_path)
    rec["probe"] = probe
    wall = os.path.join(run_dir, "wall.txt")
    rec["wall_seconds"] = float(open(wall).read().split()[0]) if os.path.exists(wall) else None
    rec["video_bytes"] = os.path.getsize(run.video) if os.path.exists(run.video) else None
    src = os.path.join(run_dir, "source.txt")
    rec["source"] = open(src).read().strip() if os.path.exists(src) else None
    kind = probe["diagnosis"]["kind"]
    run_problems = ctx.run_problems(run_dir)
    if run.expect != "stack":
        rec["status"] = "pass" if kind == run.expect else "fail"
        rec["reasons"] = [f"diagnosis {kind} (expected {run.expect}): {probe['diagnosis']['message']}"]
        rec["image_written"] = probe.get("output") is not None
        return _write_record(_no_verdict(rec, run_problems), run_dir)
    ours = os.path.join(run_dir, "ours.fits")
    if kind != "None" or not os.path.exists(ours):
        rec["status"] = "fail"
        rec["reasons"] = [f"no stack: diagnosis {kind}: {probe['diagnosis']['message']}"]
        return _write_record(_no_verdict(rec, run_problems), run_dir)
    c = benchset.by_id(run.capture)
    import noise
    try:
        frame_noise = noise.measure_video(run.video, c.size)
        rec["photon_transfer"] = str(frame_noise[0])
    except noise.NoiseUnmeasurable as e:   # loud: every row's end_note carries it, the status is unmeasured
        frame_noise = rec["photon_transfer"] = f"UNMEASURABLE: {e}"
    keff_path = os.path.join(run_dir, "keff.json")
    keff = _load_json(keff_path) if os.path.exists(keff_path) else None
    import halves
    sh = split_half_check(run_dir, frame_noise, halves.kind_of(c.kind))
    if sh is not None:
        rec["split_half"] = sh
    measured = sh is not None and sh.get("status") in ("agrees", "outside")
    rows = benchset.compare_capture(run.capture, ours, probe=probe, frame_noise=frame_noise, keff=keff,
                                    noise_factor=sh["factor"] if measured else None,
                                    noise_factor_alt=sh["factor_alt"] if measured else None)
    dicts = refs.rows_to_dicts(rows)
    rec["rows"] = dicts
    rec["status"], rec["reasons"] = status_of(dicts, run.id, sh)
    _no_verdict(rec, ctx.calibration_problems + run_problems)
    rec["side_by_side"] = os.path.join(run_dir, "side-by-side.png")
    rec.update(side_by_side(run, rows, dicts, probe, ours, rec["side_by_side"], rec["status"]))
    return _write_record(rec, run_dir)


def _write_record(rec: dict, run_dir: str) -> dict:
    """compare.json: every record a report makes is kept, whatever its status (final review, bench I2)."""
    if os.path.isdir(run_dir):
        with open(os.path.join(run_dir, "compare.json"), "w") as fh:
            json.dump(rec, fh, indent=1)
    return rec


def split_half_check(run_dir: str, frame_noise, kind: str):
    """halves.check for the run (None when run-stack-bench.sh did not stack the halves). A failure
    of the check's own inputs is a loud record (ours is then uncharged, so no pass), never a gap."""
    import halves
    import noise
    try:
        return halves.check(run_dir, frame_noise, kind)
    except (refs.RegistrationFailed, refs.ScaleMismatch, noise.NoiseUnmeasurable, OSError, ValueError) as e:
        return {"status": "error", "reason": f"{type(e).__name__}: {e}"}


def _tool(label: str) -> str:
    return "ASI" if label.startswith("ASI") else "AS" if label.startswith("AS") else "ours"


def _best(rows: List[dict], tool: str, key: str) -> Optional[dict]:
    cand = [r for r in rows[1:] if _tool(r["label"]) == tool and r[key] is not None]
    return max(cand, key=lambda r: r[key]) if cand else None


def _f(v, fmt="{:.4f}"):
    return "-" if v is None else fmt.format(v)


def _pair(full, inner):
    return f"{_f(full, '{:.3f}')} / {_f(inner, '{:.3f}')}"


def _identity_text(ident) -> str:
    if not ident:
        return "none recorded"
    return ", ".join(f"{p} {identity._short(ident.get(p))}" for p in identity.PARTS)


def summary_md(records: List[dict], ctx: Optional[Context] = None) -> str:
    import halves
    ctx = ctx or Context.make()
    cal = ctx.calibration["kinds"]
    AGREEMENT_TEXT = "; ".join(f"{k} {v['tolerance'][0]:.2f}-{v['tolerance'][1]:.2f} x" for k, v in cal.items())
    out = ["# PlanetaryStack benchmark -- ours vs AutoStakkert (AS) and ASIStudio (ASI)", "",
           "Generated by `test/run-stack-bench.sh` (bench.py report). Every colour capture is stacked "
           "**RGGB** (ZWO ASI585MC / ASI2400MC camera spec, Scott 2026-10-22); the side-by-side colours "
           "follow from that, the luminance metrics do not change with it.", "",
           "**The judged metric is END** (metric.py, Ruling P28): sharpness at equal noise -- the band "
           "detail a matched Wiener restoration recovers, every image judged against one noise model, the "
           "capture's per-frame noise (noise.py, from its raw frames) over the image's effective frame "
           "count k (ours: the noise-power k over the box, from the engine's plan; a reference: its "
           "percentage x the frames). Ratio = ours / the best reference; the bar is 0.97 on END_full and, "
           "where the interior is judged, END_interior. DAN_full / DAN_interior (the previous yardstick) "
           "are secondary columns and decide nothing.", "",
           "**Ruling P28b:** ours is charged its MEASURED noise (the split halves over the engine's frozen "
           "calibration, per END bin: budget x max(1, R/R_cal)); the references stay on the photon budget, "
           "since their noise cannot be measured from their image (C1: END cannot see noise or sharpening "
           "beyond the budget). C1 can only raise a reference's END, so a numeric **pass** is sound; a "
           "numeric fail against a reference whose processing is not known to be none is **review**.", "",
           "Status: **unmeasured** = END is missing for some image (no verdict; the reason is in the notes); "
           "**pass** = END >= 0.97 under ours' measured-noise charge on both readings of our k, the split "
           "halves within their kind's tolerance, no review note; **fail** = END below 0.97 under both "
           "readings, and also with ours on the bare budget on both readings (P28c), against references "
           "whose files record no processing; **review** = the rest, with the reasons (a verdict that "
           "flips between the readings, a fail that does not hold on the bare budget, a fail against a "
           "reference of unknown or processed provenance, a pass with ours uncharged, a pass whose split "
           "halves lie outside the tolerance in either direction, a review note on a pass). Every capture "
           "still needs Scott's look at `<run>/side-by-side.png`.", ""]
    stacked = [r for r in records if r["expect"] == "stack"]
    kinds = ("pass", "review", "fail", "unmeasured", "not run", "error")
    counts = {s: sum(1 for r in stacked if r["status"] == s) for s in kinds}
    out.append("Stacked runs: " + ", ".join(f"{counts[k]} {k}" for k in kinds if counts[k] or k in kinds[:3]) + ".")
    out.append("")
    # Ruling F2: what this report judged with, and whether the frozen calibration describes it.
    out.append(f"Judged with: {_identity_text(ctx.used)}. Calibration frozen {ctx.calibration.get('generated')} "
               f"against: {_identity_text(ctx.calibration.get('identity'))}.")
    if ctx.calibration_problems:
        out.append("")
        out.append("**NO VERDICT on any stacked run: the calibration does not describe what is in use** -- "
                   + "; ".join(ctx.calibration_problems) + ".")
    stale = [r["run"] for r in records if r.get("stale")]
    if stale:
        out.append("")
        out.append(f"**{len(stale)} record(s) kept from an earlier report were judged with another identity or "
                   f"calibration** ({', '.join(stale)}): their status stands until they are reported again.")
    out.append("")
    out.append("| Run | Bayer | Frames used | k ours: box (mean kept) | Time s (wall) | Peak RSS MB | END_full ours / AS / ASI | "
               "END_interior ours / AS / ASI | END ratio full / int | mean-kept k: ratio full / int | Split halves | "
               "DAN ratio full / int (secondary) | Status | Notes |")
    out.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in stacked:
        flag = " (stale)" if r.get("stale") else ""
        if r["status"] in ("not run", "error") or "rows" not in r:
            out.append(f"| {r['run']} | {r['bayer']} | - | - | - | - | - | - | - | - | - | - | **{r['status']}**{flag} | "
                       f"{'; '.join(list(r.get('stale', [])) + list(r.get('reasons', [])))} |")
            continue
        p, rows, o = r["probe"], r["rows"], r["rows"][0]
        a, s = _best(rows, "AS", "end"), _best(rows, "ASI", "end")
        ai, si = _best(rows, "AS", "end_interior"), _best(rows, "ASI", "end_interior")
        notes = list(r.get("stale", [])) + list(r["reasons"]) + ([r["note"]] if r["note"] else [])
        sh = r.get("split_half") or {}
        split = (f"{sh['status']} x{sh['judged']['high']:.2f}" if sh.get("status") in ("agrees", "outside")
                 else sh.get("status", "not stacked"))
        notes += [f"engine: {w}" for w in p.get("warnings", [])]
        out.append(
            f"| {r['run']} | {r['bayer']} | {p['used']} / {p['measured']} | {_f(o['end_frames'], '{:.0f}')} "
            f"({_f(o['end_alt_frames'], '{:.0f}')}) | {p['seconds']:.1f} ({_f(r['wall_seconds'], '{:.1f}')}) | "
            f"{p['peakRssMB']:.0f} | {_f(o['end'], '{:.2f}')} / {_f(a and a['end'], '{:.2f}')} / {_f(s and s['end'], '{:.2f}')} | "
            f"{_f(o['end_interior'], '{:.2f}')} / {_f(ai and ai['end_interior'], '{:.2f}')} / "
            f"{_f(si and si['end_interior'], '{:.2f}')} | {_pair(o['end_ratio'], o['end_interior_ratio'])} | "
            f"{_pair(o['end_alt_ratio'], o['end_interior_alt_ratio'])} | {split} | "
            f"{_pair(o['ratio_to_best_reference'], o['interior_ratio_to_best_reference'])} | "
            f"**{r['status']}**{flag} | {'; '.join(notes)} |")
    out += ["", "## Every image", "",
            "Per image: its frame count k (references: stacking percentage x the capture's frames -- "
            "AutoStakkert's global percentage, approximate per alignment point), the noise budget "
            "sigma_frame^2 / k, END and its ratio, and DAN (secondary).", "",
            "| Run | Image | Factor | k | Budget | Our charge per bin | END_full (ratio) | END_interior (ratio) | DAN_full (ratio) | DAN_interior (ratio) | Provenance | Registration | END note |",
            "|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for r in stacked:
        for d in r.get("rows", []):
            reg = (f"{d['orientation']} shift ({d['shift'][0]:+.2f}, {d['shift'][1]:+.2f}) scale {d['scale']:.4f} "
                   f"ncc {d['ncc']:.3f}") if d["orientation"] else "anchor"
            chg = " ".join(f"{v:.2f}" for v in d["end_noise_factor"]) if d.get("end_noise_factor") else "-"
            prov = (d.get("provenance") or {})
            prov_t = f"{prov.get('state')}: {prov.get('text')}" if prov else "-"
            out.append(f"| {r['run']} | {d['label']} | x{d['factor']:g} | {_f(d['end_frames'], '{:.0f}')} | "
                       f"{_f(d['end_noise_budget'], '{:.3g}')} | {chg} | {_f(d['end'], '{:.2f}')} ({_f(d['end_ratio'], '{:.3f}')}) | "
                       f"{_f(d['end_interior'], '{:.2f}')} ({_f(d['end_interior_ratio'], '{:.3f}')}) | {d['dan']:.4f} "
                       f"({_f(d['ratio_to_best_reference'], '{:.3f}')}) | {_f(d['dan_interior'])} "
                       f"({_f(d['interior_ratio_to_best_reference'], '{:.3f}')}) | {prov_t} | {reg} | {d['end_note'] or ''} |")
    out += ["", "## Per-frame noise and the split-half check (Ruling P28 rule 3; validation, not input)", "",
            "Photon transfer: the raw frames' noise variance v(m) = a m + c (noise.py). Split halves: our "
            "engine stacked the even and the odd frames separately; their difference D is compared with "
            "END's model for it, sigma_frame^2 (1/k_A + 1/k_B) / 4 (each half's noise-power k over the box), "
            "as measured / model over 0.21-0.30 cycles/px, after a least-squares gain and sub-pixel refinement "
            "of the halves' registration. The tolerance per target kind, from the engine on synthetic captures "
            f"of known noise (halves.calibrate, frozen in bench/calibration.json), is {AGREEMENT_TEXT}. Ours' "
            "charge per END bin is max(1, R/R_cal).", "",
            "| Run | Photon transfer | Split halves: measured / model |", "|---|---|---|"]
    for r in stacked:
        sh = r.get("split_half")
        out.append(f"| {r['run']} | {r.get('photon_transfer', '-')} | {halves.summary_line(sh)} |")
    out += ["", "## Timing (ours; AutoStakkert's time on the same file is not available)", "",
            "| Run | Read from | Video GB | Frames | Engine s | Wall s | s per GB | Read GB | Read MB/s | Peak RSS MB | Phases (s) |",
            "|---|---|---|---|---|---|---|---|---|---|---|"]
    for r in records:
        if "probe" not in r:
            continue
        p = r["probe"]
        gb = r["video_bytes"] / 1e9 if r.get("video_bytes") else None
        phases = ", ".join(f"{k} {v:.2f}" for k, v in p["phaseSeconds"].items())
        out.append(f"| {r['run']} | {r.get('source') or '-'} | {_f(gb, '{:.2f}')} | {p['frames']} | {p['seconds']:.1f} | "
                   f"{_f(r['wall_seconds'], '{:.1f}')} | {_f(p['seconds'] / gb if gb else None, '{:.1f}')} | "
                   f"{p['bytesRead'] / 1e9:.2f} | {p['readMBps']:.0f} | {p['peakRssMB']:.0f} | {phases} |")
    out += ["", "## Diagnosis cases", "", "| Run | Expected | Got | Image written | Status | Message |",
            "|---|---|---|---|---|---|"]
    for r in records:
        if r["expect"] == "stack":
            continue
        if "probe" not in r:
            out.append(f"| {r['run']} | {r['expect']} | - | - | {r['status']} | |")
            continue
        dg = r["probe"]["diagnosis"]
        nums = ", ".join(f"{k} {v}" for k, v in dg["numbers"].items())
        why = list(r.get("stale", [])) + ([r["reasons"][0]] if r["status"] == "unmeasured" and r.get("reasons") else [])
        out.append(f"| {r['run']} | {r['expect']} | {dg['kind']} | {'yes' if r['image_written'] else 'no'} | "
                   f"**{r['status']}**{' (stale)' if r.get('stale') else ''} | {dg['message']}{' (' + nums + ')' if nums else ''}"
                   f"{' -- ' + '; '.join(why) if why else ''} |")
    out.append("")
    return "\n".join(out)


def stale_record(rec: dict, ctx: Context) -> List[str]:
    """Why a record kept from an earlier report was not judged with what is in use now (its identity
    or its calibration's): it stands, flagged, until its run is reported again."""
    out = [f"judged with another identity: {m}" for m in identity.mismatches(rec.get("identity"), ctx.used)]
    if rec.get("calibration_identity") != ctx.calibration.get("identity"):
        out.append(f"judged with another calibration (frozen {rec.get('calibration_generated') or 'unknown'}, "
                   f"now {ctx.calibration.get('generated')})")
    return out


def report(out_dir: str, only: Sequence[str] = (), ctx: Optional[Context] = None) -> List[dict]:
    """Every run's record, then summary.md. `only`: report these runs again; every other run keeps its
    last record (its compare.json, written on every path -- a fail, an error, a diagnosis), flagged
    when it was judged with another identity or calibration, so a partial re-report never erases a
    verdict (final review, bench I2). A run with no record at all is "not run"."""
    ctx = ctx or Context.make()
    records = []
    for run in plan():
        run_dir = os.path.join(out_dir, run.id)
        cached = os.path.join(run_dir, "compare.json")
        if only and run.id not in only:
            if os.path.exists(cached):
                rec = _load_json(cached)
                rec["stale"] = stale_record(rec, ctx)
            else:
                rec = {"run": run.id, "capture": run.capture, "bayer": run.bayer, "expect": run.expect,
                       "note": run.note, "status": "not run"}
            records.append(rec)
            continue
        try:
            rec = report_run(run, run_dir, ctx)
        except Exception as e:          # a harness failure is a row, loudly, not a missing row
            rec = _write_record(ctx.stamp({"run": run.id, "capture": run.capture, "bayer": run.bayer, "expect": run.expect,
                                           "note": run.note, "status": "error", "reasons": [f"{type(e).__name__}: {e}"]}),
                                run_dir)
            print(f"ERROR {run.id}: {type(e).__name__}: {e}", file=sys.stderr)
        records.append(rec)
        print(f"{run.id}: {rec['status']}" + (f" -- {'; '.join(rec.get('reasons', []))}" if rec.get("reasons") else ""))
    with open(os.path.join(out_dir, "summary.md"), "w") as fh:
        fh.write(summary_md(records, ctx))
    return records


def _main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("plan")
    bx = sub.add_parser("box", help="the compare box of a run's stack (y0,y1,x0,x1), for stack-region-probe --keff-box")
    bx.add_argument("run")
    bx.add_argument("ours")
    tb = sub.add_parser("target-box", help="metric.target_box of a stack (y0,y1,x0,x1); with a second stack, "
                        "that box moved into the second stack's grid (halves.box_in_b)")
    tb.add_argument("fits")
    tb.add_argument("other", nargs="?")
    r = sub.add_parser("report")
    r.add_argument("out_dir")
    r.add_argument("runs", nargs="*")
    r.add_argument("--stack-probe", default=identity.DEFAULT_PROBES[0], help="the stack-probe the runs were stacked with")
    r.add_argument("--region-probe", default=identity.DEFAULT_PROBES[1], help="the stack-region-probe likewise")
    a = ap.parse_args(argv)
    if a.cmd == "box":
        b = benchset.compare_capture(run_by_id(a.run).capture, a.ours)[0].result.box
        print(f"{b.y0},{b.y1},{b.x0},{b.x1}")
        return 0
    if a.cmd == "target-box":
        A = metric.luminance(refs.load_image(a.fits))
        b = metric.target_box(A)
        if a.other:
            import halves
            b = halves.box_in_b(A, metric.luminance(refs.load_image(a.other)), b)
        print(f"{b.y0},{b.y1},{b.x0},{b.x1}")
        return 0
    if a.cmd == "plan":
        for run in plan():
            w, h = benchset.by_id(run.capture).size
            print("\t".join((run.id, run.capture, run.bayer, run.expect, run.video, f"{w}x{h}")))
        return 0
    records = report(a.out_dir, a.runs, Context.make(a.stack_probe, a.region_probe))
    return 1 if any(r["status"] == "error" for r in records) else 0


if __name__ == "__main__":
    sys.exit(_main())
