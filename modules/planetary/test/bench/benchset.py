#!/usr/bin/env python3
"""benchset.py -- the PlanetaryStack benchmark set (README.md explains each choice), the entry
point run-stack-bench.sh calls, and a survey that verifies every reference against its own
video before any stack exists.

    python3 -I benchset.py list | show <id> | compare <id> <ours.fits|ours.tif> [--json]
    python3 -I benchset.py survey <out-dir> [--only ID ...]

Every capture that is stacked is stacked DEBAYERED (stack-probe --bayer <Capture.bayer>):
comparing a raw-CFA mono stack with debayered references is biased by up to 9 % (review round
1, metric.py LIMITS). Capture.bayer is the pattern in the reader's frame layout and
Capture.bayer_source says where it came from (bayer.py measures the green phase on the raw
frames; red/blue from the warm-target rule, a sidecar, or the references' own debayer). The
product itself still stacks an undeclared mosaic as raw mono (Ruling P9); the benchmark
measures the debayered path because that is what a user who sets the pattern gets.

Reference resampling factors are derived from each reference's size against the capture's
frame (refs.factor_from_size); a disagreement with the AutoStakkert file name is recorded in
the reference's label.

The survey reads the first SURVEY_FRAMES frames of each video (ffmpeg, NAS read-only) and
averages them -- a crude native-scale image of the target -- registers each reference to it
(orientation, shift, MEASURED scale) and measures all references of the capture on one box
(refs.compare, anchored on the first reference). It writes <out-dir>/survey.json and a
stretched thumbnail per reference.
"""
import argparse
import json
import os
import subprocess
import sys
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import metric  # noqa: E402
import refs    # noqa: E402

NAS = os.environ.get("PLANETARY_BENCH_NAS", "/mnt/qnap/astro_data")
SURVEY_FRAMES = 32
VIDEOS = "processing-archive-2026-10-03/videos"
LQEF = ("red/blue unverified: the Lqef filter passes only H-beta/OIII and H-alpha/SII, so the "
        "warm-target rule does not apply; an R/B swap leaves the luminance metric unchanged")


@dataclass(frozen=True)
class Capture:
    id: str
    kind: str                     # "planet", "lunar" or "diagnosis"
    video: str                    # relative to NAS
    size: Tuple[int, int]         # (width, height) of a frame
    frames: int
    sidecar: bool                 # <video>.txt beside it
    bayer: Optional[str]          # stack-probe --bayer (reader layout); None = Auto (diagnosis cases)
    bayer_source: str
    references: List[str] = field(default_factory=list)   # relative to NAS
    expect: str = "stack"         # "stack" or the DiagnosisKind name the engine must report
    why: str = ""


BENCHMARK: List[Capture] = [
    # --- planets ---------------------------------------------------------------------------
    Capture("2023-02-11-jupiter", "planet", "2_12_2023/2023-02-11-191831-Jupiter-Bin1 13.3F.avi",
            (640, 480), 6516, False, "BGGR",
            "measured, warm-target rule (greens 0.2 % apart, red/blue 53 %); AutoStakkert debayered the same",
            ["2_12_2023/AS_P20/2023-02-11-191831-Jupiter-Bin1 13.3F_lapl5_ap1.png",
             "2_12_2023/AS_P25/2023-02-11-191831-Jupiter-Bin1 13.3F_lapl5_ap1_Drizzle30.png"],
            why="AutoStakkert at native scale (P20) and drizzled (P25)"),
    Capture("2026-01-30-jupiter", "planet", f"{VIDEOS}/2026-01-30-195830-Jupiter-Bin1 Lqef -4.2F.avi",
            (480, 360), 4088, True, "RGGB",
            "Scott's choice (SERViewer Save pattern: Bayer = RG); green phase measured (greens 0.1 % apart); "
            "ASIStudio debayered the same (VSBAYER 0 = RG); the warm-target rule would say BGGR (17.9 %); " + LQEF,
            [f"{VIDEOS}/ASIVideoStack_Output/2026-01-30-195830-Jupiter-Bin1 Lqef -4.2F_Planet_40.fit",
             f"{VIDEOS}/ASIVideoStack_Output/2026-01-30-195830-Jupiter-Bin1 Lqef -4.2F_Planet_50.fit"],
            why="the larger Jupiter (eroded disc >= 20 px: interior judged); current camera"),
    Capture("2023-09-29-saturn", "planet", "9_30_2023/2023-09-29-204604-Saturn-Bin1 LPro 14.9F.avi",
            (1280, 960), 3973, False, "BGGR",
            "measured, warm-target rule (greens 1.2 % apart, red/blue 4.5 % -- the weakest split in the set); "
            "ASIStudio debayered RGGB (VSBAYER 0 = RG)",
            ["9_30_2023/ASIVideoStack_Output/2023-09-29-204604-Saturn-Bin1 LPro 14.9F_Planet_74.fit"],
            why="rings; small target in a large frame"),
    Capture("2023-02-11-mars", "planet", "2_12_2023/2023-02-11-192403-Mars-Bin1 16.5F.avi",
            (320, 240), 13251, False, "RGGB",
            "measured, warm-target rule (greens 1.7 % apart, red/blue 12.7 %); AutoStakkert debayered BGGR "
            "(its R/B 0.93: a blue-shifted Mars)",
            ["2_12_2023/AS_P25/2023-02-11-192403-Mars-Bin1 16.5F_lapl5_ap1_Drizzle30.png"],
            why="smallest disc (limb-only); longest capture"),
    # --- lunar -----------------------------------------------------------------------------
    Capture("2023-02-12-moon", "lunar", "2_12_2023/2023-02-12-024708-Moon-Bin1 14.9F.avi",
            (1920, 1080), 2354, False, "BGGR",
            "measured, warm-target rule (greens 0.1 % apart, red/blue 30.0 %); AutoStakkert debayered the same",
            ["2_12_2023/AS_P20/2023-02-12-024708-Moon-Bin1 14.9F_lapl5_ap1.png",
             "2_12_2023/AS_P50/2023-02-12-024708-Moon-Bin1 14.9F_lapl5_ap2.png",
             "2_12_2023/AS_P25/2023-02-12-024708-Moon-Bin1 14.9F_lapl5_ap2_Drizzle30.png"],
            why="metric validation: AS P20 and P50 both native"),
    Capture("2024-02-01-moon", "lunar", "2_1_2024/2024-02-01-013847-Moon-Bin1 Lqef 14.9F.avi",
            (1920, 1080), 1310, False, "RGGB",
            "green phase measured (greens 0.1 % apart); red/blue as AutoStakkert and ASIStudio both debayered "
            "it (R/B 0.80 = raw site ratio); the warm-target rule would say BGGR (21.4 %); " + LQEF,
            ["2_1_2024/ASIVideoStack_Output/2024-02-01-013847-Moon-Bin1 Lqef 14.9F_Surface_40.fit",
             "2_1_2024/AS_P60/2024-02-01-013847-Moon-Bin1 Lqef 14.9F_lapl5_ap16_Drizzle15.fit"],
            why="both tools on one capture"),
    Capture("2023-08-22-moon", "lunar", "8_23_2023/2023-08-22-213239-FOV-Bin1 LPro 15.8F.avi",
            (1920, 1080), 1352, False, "RGGB",
            "measured, warm-target rule (greens 0.1 % apart, red/blue 20.5 %); AutoStakkert and ASIStudio "
            "debayered the same",
            ["8_23_2023/ASIVideoStack_Output/2023-08-22-213239-FOV-Bin1 LPro 15.8F_Surface_35.fit",
             "8_23_2023/AS_P20/2023-08-22-213239-FOV-Bin1 LPro 15.8F_lapl5_ap4_Drizzle30.fit"],
            why="both tools; named FOV but a lunar limb (checked visually)"),
    Capture("2023-03-05-moon", "lunar", "3_6_2023/moon/2023-03-05-224808-Moon-Bin1 18.3F.avi",
            (1920, 1080), 1705, True, "BGGR",
            "measured, warm-target rule (greens 0.0 % apart, red/blue 46.2 %), = the reader's sidecar "
            "resolution (Bayer RG, Flip=Vert -> BGGR, Ruling 10); AutoStakkert debayered RGGB (its R/B 0.62: "
            "a blue Moon)",
            ["3_6_2023/moon/AS_P20/2023-03-05-224808-Moon-Bin1 18.3F_lapl5_ap4_Drizzle15.fit",
             "3_6_2023/moon/AS_P50/2023-03-05-224808-Moon-Bin1 18.3F_jupiter_lapl5_ap4_Drizzle15.png"],
            why="a second AS P20/P50 pair; sidecar"),
    # --- diagnosis -------------------------------------------------------------------------
    Capture("2026-01-30-saturn-empty", "diagnosis", f"{VIDEOS}/2026-01-30-203627-Saturn-Bin1 Lqef -3.8F.avi",
            (1920, 1080), 21, False, None, "Auto: nothing to stack, no target to measure a pattern on",
            [f"{VIDEOS}/ASIVideoStack_Output/2026-01-30-203627-Saturn-Bin1 Lqef -3.8F_Planet_40.fit"],
            expect="NoTarget", why="frames hold only background; ASIStudio still wrote an image of it"),
    Capture("2023-02-11-venus-clipped", "diagnosis", "2_12_2023/2023-02-11-190452-Venus-Bin1 15.8F.avi",
            (640, 480), 6485, False, None, "Auto: unmeasurable (91 % of the disc saturated); nothing to stack",
            ["2_12_2023/AS_P25/2023-02-11-190452-Venus-Bin1 15.8F_lapl5_ap1_Drizzle30.png",
             "2_12_2023/AS_P50/2023-02-11-190452-Venus-Bin1 15.8F_lapl5_ap1_Drizzle30.png"],
            expect="Clipped", why="every frame >= 25 % saturated (Task 3): a diagnosis, no image"),
]


def by_id(cid: str) -> Capture:
    for c in BENCHMARK:
        if c.id == cid:
            return c
    raise KeyError(cid)


def nas(rel: str) -> str:
    return os.path.join(NAS, rel)


def reference_paths(c: Capture) -> List[str]:
    return [nas(p) for p in c.references]


def compare_capture(cid: str, ours) -> List["refs.Row"]:
    """THE call for run-stack-bench.sh: our debayered stack (FITS/TIFF path or array) against
    this capture's references on one box, reference factors from their size. Row 0 is ours.
    A diagnosis capture has no stack to compare and is refused."""
    c = by_id(cid)
    if c.expect != "stack":
        raise ValueError(f"{cid} is a diagnosis case (expect {c.expect}); there is no stack to compare")
    return refs.compare(ours, reference_paths(c), anchor_label="ours", frame_size=c.size)


def video_mean(c: Capture, n: int = SURVEY_FRAMES) -> np.ndarray:
    w, h = c.size
    raw = subprocess.run(["ffmpeg", "-v", "error", "-nostdin", "-i", nas(c.video), "-frames:v", str(n),
                          "-f", "rawvideo", "-pix_fmt", "gray", "-"],
                         check=True, capture_output=True).stdout
    frames = np.frombuffer(raw, np.uint8)
    if frames.size % (w * h) or frames.size == 0:
        raise ValueError(f"{c.video}: read {frames.size} bytes, not whole {w}x{h} frames")
    return frames.reshape(-1, h, w).astype(np.float64).mean(axis=0)


def thumbnail(img, path, max_side=640):
    from PIL import Image
    lum = metric.luminance(img)
    d = max(1.0, max(lum.shape) / max_side)
    small = refs.area_resample(lum, d) if d > 1 else lum
    lo, hi = np.percentile(small, 0.5), np.percentile(small, 99.9)
    out = np.clip((small - lo) / max(hi - lo, 1e-12), 0, 1) ** (1 / 2.2)
    Image.fromarray((out * 255).astype(np.uint8)).save(path)


def survey(c: Capture, out_dir: str) -> dict:
    os.makedirs(out_dir, exist_ok=True)
    rec = {"id": c.id, "kind": c.kind, "video": nas(c.video), "sidecar_on_disk": os.path.exists(nas(c.video) + ".txt"),
           "sidecar_expected": c.sidecar, "bayer": c.bayer, "references": []}
    mean = video_mean(c)
    thumbnail(mean, os.path.join(out_dir, f"{c.id}__video-mean.png"))
    for i, path in enumerate(reference_paths(c)):
        ref = refs.load_reference(path, frame_size=c.size)
        entry = {"path": path, "label": ref.info.label, "factor_used": ref.factor,
                 "native_shape": list(ref.image.shape)}
        thumbnail(ref.image, os.path.join(out_dir, f"{c.id}__ref{i}.png"))
        try:
            reg = refs.register(ref.image, mean)
            entry["to_video"] = {"orientation": reg.orientation, "shift": list(reg.shift),
                                 "scale": reg.scale, "ncc": reg.ncc}
        except refs.ScaleMismatch as e:
            entry["to_video"] = {"error": str(e), "measured_scale": e.measured}
        except refs.RegistrationFailed as e:
            entry["to_video"] = {"error": str(e)}
        rec["references"].append(entry)
    paths = reference_paths(c)
    if len(paths) >= 2:
        try:
            rec["compare"] = refs.rows_to_dicts(refs.compare(paths[0], paths[1:], frame_size=c.size))
        except ValueError as e:
            rec["compare"] = {"error": str(e)}
    else:
        ref = refs.load_reference(paths[0], frame_size=c.size)
        box = metric.target_box(ref.image)
        r = metric.measure(ref.image, box, interior=metric.interior_window(ref.image, box))
        rec["compare"] = [{"label": ref.info.label, "dan": r.value, "dan_interior": r.interior,
                           "interior_diameter": r.interior_diameter, "band_snr": r.band_snr, "box": list(r.box)}]
    return rec


def _main(argv=None):
    ap = argparse.ArgumentParser(description="PlanetaryStack benchmark set")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("survey")
    s.add_argument("out_dir")
    s.add_argument("--only", nargs="*")
    sub.add_parser("list", help="id, kind, expect, bayer (or auto), video path (tab-separated)")
    sh = sub.add_parser("show", help="one capture as JSON")
    sh.add_argument("id")
    cp = sub.add_parser("compare", help="our stack against the capture's references")
    cp.add_argument("id")
    cp.add_argument("ours", help="our debayered stack, FITS or TIFF")
    cp.add_argument("--json", action="store_true")
    args = ap.parse_args(argv)
    if args.cmd == "list":
        for c in BENCHMARK:
            print(f"{c.id}\t{c.kind}\t{c.expect}\t{c.bayer or 'auto'}\t{nas(c.video)}")
        return 0
    if args.cmd == "show":
        c = by_id(args.id)
        print(json.dumps({"id": c.id, "kind": c.kind, "video": nas(c.video), "size": c.size,
                          "frames": c.frames, "sidecar": c.sidecar, "bayer": c.bayer,
                          "bayer_source": c.bayer_source, "expect": c.expect, "why": c.why,
                          "references": reference_paths(c)}, indent=2))
        return 0
    if args.cmd == "compare":
        d = refs.rows_to_dicts(compare_capture(args.id, args.ours))
        if args.json:
            print(json.dumps(d, indent=2, default=float))
        else:
            refs.print_rows(d)
        return 0
    results = []
    for c in BENCHMARK:
        if args.only and c.id not in args.only:
            continue
        print(f"== {c.id}", flush=True)
        rec = survey(c, args.out_dir)
        for r in rec["references"]:
            print(f"   {r['label']:<48} {r['to_video']}", flush=True)
        print(f"   compare: {json.dumps(rec['compare'], default=float)[:2000]}", flush=True)
        results.append(rec)
    with open(os.path.join(args.out_dir, "survey.json"), "w") as f:
        json.dump(results, f, indent=2, default=float)
    return 0


if __name__ == "__main__":
    sys.exit(_main())
