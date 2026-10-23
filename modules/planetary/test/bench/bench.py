#!/usr/bin/env python3
"""bench.py -- the run plan, side-by-side PNGs and summary table for run-stack-bench.sh.

    python3 -I bench.py plan                       # TSV: run, capture, bayer, expect, video
    python3 -I bench.py report <out-dir> [run ...] # per run: compare.json + side-by-side.png;
                                                   # then <out-dir>/summary.md over every run present

run-stack-bench.sh stacks each run with stack-probe into <out-dir>/<run>/ (ours.fits, probe.json,
probe.log, wall.txt) and then calls `report`. This module never stacks and never writes outside
<out-dir>; the NAS is only read.

RUNS. One run per benchmark capture (benchset.BENCHMARK), stacked with the capture's recorded
pattern, except where BAYER_OVERRIDE says otherwise, plus the VARIANTS. The override exists for
the two Lqef captures: the record says RGGB (Scott's Save pattern; both tools' debayer), the
physical evidence (a blue seeing halo; the warm-target rule) says BGGR, and Scott has not
confirmed either. Their side-by-sides use BGGR; the 2026-01-30 Jupiter also runs RGGB as a
colour-comparison variant. The DAN numbers are the same either way to within measurement noise
(an R/B swap only swaps two planes of a Hamilton-Adams demosaic); the variant shows it.

STATUS (Ruling 2). fail: ours' DAN_full, or DAN_interior where judged, is below 0.97 x the best
reference. review: the numbers pass but something sends the capture to Scott regardless -- a
review note from refs.review_notes (limb-only, noise regime unlike every reference, mosaic
fallback) or ALWAYS_REVIEW. pass: neither. Every capture still gets Scott's look at the
side-by-side (Ruling 2's human); "pass" means only that the numbers alone raise nothing.

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
from typing import Dict, List, Optional, Sequence, Tuple

import numpy as np
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import benchset  # noqa: E402
import metric    # noqa: E402
import refs      # noqa: E402
from metric import Box  # noqa: E402

LQEF_NOTE = ("stacked BGGR, not the recorded RGGB: the physical evidence (blue seeing halo, warm-target "
             "rule) points to BGGR while Scott's sidecar says RG; Scott has not confirmed the pattern. "
             "DAN is unchanged by an R/B swap; the colour is Scott's call")
BAYER_OVERRIDE: Dict[str, Tuple[str, str]] = {
    "2026-01-30-jupiter": ("BGGR", LQEF_NOTE),
    "2024-02-01-moon": ("BGGR", LQEF_NOTE + "; both references were debayered RGGB, so their red/blue "
                        "are swapped against ours"),
}
# (run id, capture id, bayer, note)
VARIANTS: List[Tuple[str, str, str, str]] = [
    ("2026-01-30-jupiter-rggb", "2026-01-30-jupiter", "RGGB",
     "colour-comparison variant: the recorded RGGB (Scott's Save pattern, as ASIStudio debayered it)"),
]
ALWAYS_REVIEW = {
    "2026-01-30-jupiter": "always human review (README): ASIStudio's .fit has suppressed power near Nyquist, "
                          "so its noise floor reads small; the ratio understates ours",
    "2026-01-30-jupiter-rggb": "always human review (README), as the BGGR run",
}
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
    runs = []
    for c in benchset.BENCHMARK:
        bayer, note = BAYER_OVERRIDE.get(c.id, (c.bayer or "auto", ""))
        runs.append(Run(c.id, c.id, bayer, c.expect, benchset.nas(c.video), note))
    for rid, cid, bayer, note in VARIANTS:
        c = benchset.by_id(cid)
        runs.append(Run(rid, cid, bayer, c.expect, benchset.nas(c.video), note))
    return runs


def run_by_id(rid: str) -> Run:
    for r in plan():
        if r.id == rid:
            return r
    raise KeyError(rid)


# --- status ----------------------------------------------------------------------------------

def status_of(ours: dict, run_id: str) -> Tuple[str, List[str]]:
    """Ruling 2 from ours' compare row (refs.rows_to_dicts()[0]): (status, reasons)."""
    reasons = []
    full, inner = ours["ratio_to_best_reference"], ours["interior_ratio_to_best_reference"]
    if full is not None and full < refs.PASS_BAR:
        reasons.append(f"DAN_full {full:.3f} x best < {refs.PASS_BAR}")
    if ours["interior_judged"] and inner is not None and inner < refs.PASS_BAR:
        reasons.append(f"DAN_interior {inner:.3f} x best < {refs.PASS_BAR}")
    if reasons:
        return "fail", reasons
    review = list(ours["review"])
    if run_id in ALWAYS_REVIEW:
        review.append(ALWAYS_REVIEW[run_id])
    return ("review", review) if review else ("pass", [])


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
                 out_path: str) -> dict:
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
        full = dct["ratio_to_best_reference"]
        inner = dct["interior_ratio_to_best_reference"]
        score = f"DAN {dct['dan']:.4f} ({full:.3f}x best)"
        score += (f"  int {dct['dan_interior']:.4f} ({inner:.3f}x)" if inner is not None else
                  "  int: not judged (limb-only)")
        if i == 0:
            headers.append([f"OURS ({run.bayer}): {probe['used']}/{probe['measured']} frames, "
                            f"{probe['seconds']:.1f} s", score])
        else:
            headers.append([dct["label"], score, f"{dct['orientation']} ncc {dct['ncc']:.3f}"])
    st, _ = status_of(dicts[0], run.id)
    title = f"{run.id}: {st.upper()} -- same crop, scale and stretch (black per channel, one white at 99.5th pct, gamma 2.2)"
    compose(panel_rows, headers, row_titles, title, out_path)
    return {"shown": shown, "display_levels": [{"black": b, "white": w} for b, w in levels]}


# --- report ----------------------------------------------------------------------------------

def _load_json(path):
    with open(path) as fh:
        return json.load(fh)


def report_run(run: Run, run_dir: str) -> dict:
    """compare.json (+ side-by-side.png) for one stacked run; the diagnosis check for a diagnosis
    run. Returns the record summary.md is built from."""
    probe_path = os.path.join(run_dir, "probe.json")
    rec = {"run": run.id, "capture": run.capture, "bayer": run.bayer, "expect": run.expect, "note": run.note}
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
    if run.expect != "stack":
        rec["status"] = "pass" if kind == run.expect else "fail"
        rec["reasons"] = [f"diagnosis {kind} (expected {run.expect}): {probe['diagnosis']['message']}"]
        rec["image_written"] = probe.get("output") is not None
        with open(os.path.join(run_dir, "compare.json"), "w") as fh:
            json.dump(rec, fh, indent=1)
        return rec
    ours = os.path.join(run_dir, "ours.fits")
    if kind != "None" or not os.path.exists(ours):
        rec["status"] = "fail"
        rec["reasons"] = [f"no stack: diagnosis {kind}: {probe['diagnosis']['message']}"]
        return rec
    rows = benchset.compare_capture(run.capture, ours)
    dicts = refs.rows_to_dicts(rows)
    rec["rows"] = dicts
    rec["status"], rec["reasons"] = status_of(dicts[0], run.id)
    rec["side_by_side"] = os.path.join(run_dir, "side-by-side.png")
    rec.update(side_by_side(run, rows, dicts, probe, ours, rec["side_by_side"]))
    with open(os.path.join(run_dir, "compare.json"), "w") as fh:
        json.dump(rec, fh, indent=1)
    return rec


def _tool(label: str) -> str:
    return "ASI" if label.startswith("ASI") else "AS" if label.startswith("AS") else "ours"


def _best(rows: List[dict], tool: str, key: str) -> Optional[dict]:
    cand = [r for r in rows[1:] if _tool(r["label"]) == tool and r[key] is not None]
    return max(cand, key=lambda r: r[key]) if cand else None


def _f(v, fmt="{:.4f}"):
    return "-" if v is None else fmt.format(v)


def summary_md(records: List[dict]) -> str:
    out = ["# PlanetaryStack benchmark -- ours vs AutoStakkert (AS) and ASIStudio (ASI)", "",
           "Generated by `test/run-stack-bench.sh` (bench.py report). DAN_full / DAN_interior: metric.py; "
           "ratio = ours / the best reference (Ruling 2 bar: 0.97 on both, interior only where judged). "
           "AS/ASI columns show each tool's best reference; every reference is in the per-capture table "
           "below and in `<run>/compare.json`. Status: fail = a ratio below 0.97; review = numbers pass but "
           "a note sends it to Scott; pass = the numbers raise nothing. Every capture still needs Scott's "
           "look at `<run>/side-by-side.png`.", ""]
    stacked = [r for r in records if r["expect"] == "stack"]
    counts = {s: sum(1 for r in stacked if r["status"] == s) for s in ("pass", "review", "fail", "not run", "error")}
    out.append(f"Stacked runs: {counts['pass']} pass, {counts['review']} review, {counts['fail']} fail"
               + "".join(f", {counts[k]} {k}" for k in ("not run", "error") if counts[k]) + ".")
    out.append("")
    out.append("| Run | Bayer | Frames used | Time s (wall) | Peak RSS MB | DAN_full ours / AS / ASI | "
               "DAN_interior ours / AS / ASI | Ratio full / interior | Status | Notes |")
    out.append("|---|---|---|---|---|---|---|---|---|---|")
    for r in stacked:
        if r["status"] == "not run" or "rows" not in r:
            out.append(f"| {r['run']} | {r['bayer']} | - | - | - | - | - | - | {r['status']} | "
                       f"{'; '.join(r.get('reasons', []))} |")
            continue
        p, rows, o = r["probe"], r["rows"], r["rows"][0]
        a, s = _best(rows, "AS", "dan"), _best(rows, "ASI", "dan")
        ai, si = _best(rows, "AS", "dan_interior"), _best(rows, "ASI", "dan_interior")
        notes = list(r["reasons"]) + ([r["note"]] if r["note"] else [])
        notes += [f"engine: {w}" for w in p.get("warnings", [])]
        out.append(
            f"| {r['run']} | {r['bayer']} | {p['used']} / {p['measured']} | {p['seconds']:.1f} "
            f"({_f(r['wall_seconds'], '{:.1f}')}) | {p['peakRssMB']:.0f} | "
            f"{o['dan']:.4f} / {_f(a and a['dan'])} / {_f(s and s['dan'])} | "
            f"{_f(o['dan_interior'])} / {_f(ai and ai['dan_interior'])} / {_f(si and si['dan_interior'])} | "
            f"{_f(o['ratio_to_best_reference'], '{:.3f}')} / {_f(o['interior_ratio_to_best_reference'], '{:.3f}')} | "
            f"**{r['status']}** | {'; '.join(notes)} |")
    out += ["", "## Every reference", "",
            "| Run | Image | Factor | DAN_full (ratio) | DAN_interior (ratio) | band SNR | Registration |",
            "|---|---|---|---|---|---|---|"]
    for r in stacked:
        for d in r.get("rows", []):
            reg = (f"{d['orientation']} shift ({d['shift'][0]:+.2f}, {d['shift'][1]:+.2f}) scale {d['scale']:.4f} "
                   f"ncc {d['ncc']:.3f}") if d["orientation"] else "anchor"
            out.append(f"| {r['run']} | {d['label']} | x{d['factor']:g} | {d['dan']:.4f} "
                       f"({_f(d['ratio_to_best_reference'], '{:.3f}')}) | {_f(d['dan_interior'])} "
                       f"({_f(d['interior_ratio_to_best_reference'], '{:.3f}')}) | {d['band_snr']:.0f} | {reg} |")
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
        out.append(f"| {r['run']} | {r['expect']} | {dg['kind']} | {'yes' if r['image_written'] else 'no'} | "
                   f"**{r['status']}** | {dg['message']}{' (' + nums + ')' if nums else ''} |")
    out.append("")
    return "\n".join(out)


def report(out_dir: str, only: Sequence[str] = ()) -> List[dict]:
    records = []
    for run in plan():
        run_dir = os.path.join(out_dir, run.id)
        cached = os.path.join(run_dir, "compare.json")
        if only and run.id not in only and os.path.exists(cached):
            records.append(_load_json(cached))
            continue
        if only and run.id not in only:
            records.append({"run": run.id, "capture": run.capture, "bayer": run.bayer, "expect": run.expect,
                            "note": run.note, "status": "not run"})
            continue
        try:
            rec = report_run(run, run_dir)
        except Exception as e:          # a harness failure is a row, loudly, not a missing row
            rec = {"run": run.id, "capture": run.capture, "bayer": run.bayer, "expect": run.expect,
                   "note": run.note, "status": "error", "reasons": [f"{type(e).__name__}: {e}"]}
            print(f"ERROR {run.id}: {type(e).__name__}: {e}", file=sys.stderr)
        records.append(rec)
        print(f"{run.id}: {rec['status']}" + (f" -- {'; '.join(rec.get('reasons', []))}" if rec.get("reasons") else ""))
    with open(os.path.join(out_dir, "summary.md"), "w") as fh:
        fh.write(summary_md(records))
    return records


def _main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("plan")
    r = sub.add_parser("report")
    r.add_argument("out_dir")
    r.add_argument("runs", nargs="*")
    a = ap.parse_args(argv)
    if a.cmd == "plan":
        for run in plan():
            print("\t".join((run.id, run.capture, run.bayer, run.expect, run.video)))
        return 0
    records = report(a.out_dir, a.runs)
    return 1 if any(r["status"] == "error" for r in records) else 0


if __name__ == "__main__":
    sys.exit(_main())
