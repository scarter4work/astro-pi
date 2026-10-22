#!/usr/bin/env python3
"""benchset.py -- the PlanetaryStack benchmark set (README.md explains each choice) and a
survey that verifies every reference against its own video before any stack exists.

    python3 -I benchset.py survey <out-dir> [--only ID ...]

For each capture the survey reads the first SURVEY_FRAMES frames of the video (ffmpeg, NAS
read-only) and averages them -- a crude native-scale image of the target -- then loads each
reference (refs.load_reference with the factor recorded below), registers it to that image
(orientation, shift, MEASURED scale) and measures all references of the capture on one box
(refs.compare, anchored on the first reference). It writes <out-dir>/survey.json and a
stretched thumbnail per reference. A reference whose scale is not 1 fails loudly.
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


@dataclass(frozen=True)
class Capture:
    id: str
    kind: str                     # "planet", "lunar" or "diagnosis"
    video: str                    # relative to NAS
    size: Tuple[int, int]         # (width, height) of a frame
    frames: int
    sidecar: bool                 # <video>.txt beside it (Bayer pattern known)
    mosaic: bool                  # no sidecar: our stack is raw CFA as mono -> metric mosaic mode
    references: List[Tuple[str, Optional[float]]] = field(default_factory=list)  # (rel path, factor)
    expect: str = "stack"         # "stack" or the DiagnosisKind name the engine must report
    why: str = ""


BENCHMARK: List[Capture] = [
    # --- planets ---------------------------------------------------------------------------
    Capture("2023-02-11-jupiter", "planet", "2_12_2023/2023-02-11-191831-Jupiter-Bin1 13.3F.avi",
            (640, 480), 6516, False, True,
            [("2_12_2023/AS_P20/2023-02-11-191831-Jupiter-Bin1 13.3F_lapl5_ap1.png", None),
             ("2_12_2023/AS_P25/2023-02-11-191831-Jupiter-Bin1 13.3F_lapl5_ap1_Drizzle30.png", None)],
            why="metric-validation capture: AS at native scale (P20) and drizzled x3 (P25)"),
    Capture("2026-01-30-jupiter", "planet",
            "processing-archive-2026-10-03/videos/2026-01-30-195830-Jupiter-Bin1 Lqef -4.2F.avi",
            (480, 360), 4088, True, False,
            [("processing-archive-2026-10-03/videos/ASIVideoStack_Output/2026-01-30-195830-Jupiter-Bin1 Lqef -4.2F_Planet_40.fit", None),
             ("processing-archive-2026-10-03/videos/ASIVideoStack_Output/2026-01-30-195830-Jupiter-Bin1 Lqef -4.2F_Planet_50.fit", None)],
            why="Bayer known (Scott's Save pattern: RG) -> colour vs ASIStudio; current camera"),
    Capture("2023-09-29-saturn", "planet", "9_30_2023/2023-09-29-204604-Saturn-Bin1 LPro 14.9F.avi",
            (1280, 960), 3973, False, True,
            [("9_30_2023/ASIVideoStack_Output/2023-09-29-204604-Saturn-Bin1 LPro 14.9F_Planet_74.fit", None)],
            why="rings (Cassini division when seeing allows); large frame, drifting disc"),
    Capture("2023-02-11-mars", "planet", "2_12_2023/2023-02-11-192403-Mars-Bin1 16.5F.avi",
            (320, 240), 13251, False, True,
            [("2_12_2023/AS_P25/2023-02-11-192403-Mars-Bin1 16.5F_lapl5_ap1_Drizzle30.png", None)],
            why="small disc in a 320x240 frame, 13 251 frames (long capture)"),
    # --- lunar -----------------------------------------------------------------------------
    Capture("2023-02-12-moon", "lunar", "2_12_2023/2023-02-12-024708-Moon-Bin1 14.9F.avi",
            (1920, 1080), 2354, False, True,
            [("2_12_2023/AS_P20/2023-02-12-024708-Moon-Bin1 14.9F_lapl5_ap1.png", None),
             ("2_12_2023/AS_P50/2023-02-12-024708-Moon-Bin1 14.9F_lapl5_ap2.png", None),
             ("2_12_2023/AS_P25/2023-02-12-024708-Moon-Bin1 14.9F_lapl5_ap2_Drizzle30.png", None)],
            why="metric validation: AS P20 and P50 both native, plus P25 drizzled x3"),
    Capture("2024-02-01-moon", "lunar", "2_1_2024/2024-02-01-013847-Moon-Bin1 Lqef 14.9F.avi",
            (1920, 1080), 1310, False, True,
            [("2_1_2024/ASIVideoStack_Output/2024-02-01-013847-Moon-Bin1 Lqef 14.9F_Surface_40.fit", None),
             ("2_1_2024/AS_P60/2024-02-01-013847-Moon-Bin1 Lqef 14.9F_lapl5_ap16_Drizzle15.fit", 3.0)],
            why="both AS and ASIStudio; the AS file is named Drizzle15 but is x3 (measured)"),
    Capture("2023-08-22-moon", "lunar", "8_23_2023/2023-08-22-213239-FOV-Bin1 LPro 15.8F.avi",
            (1920, 1080), 1352, False, True,
            [("8_23_2023/ASIVideoStack_Output/2023-08-22-213239-FOV-Bin1 LPro 15.8F_Surface_35.fit", None),
             ("8_23_2023/AS_P20/2023-08-22-213239-FOV-Bin1 LPro 15.8F_lapl5_ap4_Drizzle30.fit", None)],
            why="both AS and ASIStudio; LPro filter; named FOV but a lunar surface capture"),
    Capture("2023-03-05-moon", "lunar", "3_6_2023/moon/2023-03-05-224808-Moon-Bin1 18.3F.avi",
            (1920, 1080), 1705, True, False,
            [("3_6_2023/moon/AS_P20/2023-03-05-224808-Moon-Bin1 18.3F_lapl5_ap4_Drizzle15.fit", 3.0),
             ("3_6_2023/moon/AS_P50/2023-03-05-224808-Moon-Bin1 18.3F_jupiter_lapl5_ap4_Drizzle15.png", None)],
            why="Bayer known (sidecar RG, ASI2400MC); a second AS P20/P50 pair"),
    # --- diagnosis -------------------------------------------------------------------------
    Capture("2026-01-30-saturn-empty", "diagnosis",
            "processing-archive-2026-10-03/videos/2026-01-30-203627-Saturn-Bin1 Lqef -3.8F.avi",
            (1920, 1080), 21, False, True,
            [("processing-archive-2026-10-03/videos/ASIVideoStack_Output/2026-01-30-203627-Saturn-Bin1 Lqef -3.8F_Planet_40.fit", None)],
            expect="NoTarget",
            why="frames hold only background; ASIStudio still wrote an image of it"),
    Capture("2023-02-11-venus-clipped", "diagnosis", "2_12_2023/2023-02-11-190452-Venus-Bin1 15.8F.avi",
            (640, 480), 6485, False, True,
            [("2_12_2023/AS_P25/2023-02-11-190452-Venus-Bin1 15.8F_lapl5_ap1_Drizzle30.png", None),
             ("2_12_2023/AS_P50/2023-02-11-190452-Venus-Bin1 15.8F_lapl5_ap1_Drizzle30.png", None)],
            expect="Clipped", why="every frame >= 25 % saturated (Task 3): a diagnosis, no image"),
]


def by_id(cid: str) -> Capture:
    for c in BENCHMARK:
        if c.id == cid:
            return c
    raise KeyError(cid)


def nas(rel: str) -> str:
    return os.path.join(NAS, rel)


def reference_specs(c: Capture) -> List[Tuple[str, Optional[float]]]:
    """(absolute path, factor) for refs.compare / refs.load_reference."""
    return [(nas(p), f) for p, f in c.references]


def compare_capture(cid: str, ours) -> List["refs.Row"]:
    """THE call for run-stack-bench.sh: our stack (FITS/TIFF path or array) against this
    capture's references, on one box, in mosaic mode when the capture has no Bayer sidecar.
    Row 0 is ours. A diagnosis capture has no stack to compare and is refused."""
    c = by_id(cid)
    if c.expect != "stack":
        raise ValueError(f"{cid} is a diagnosis case (expect {c.expect}); there is no stack to compare")
    return refs.compare(ours, reference_specs(c), mosaic=c.mosaic, anchor_label="ours")


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
           "sidecar_expected": c.sidecar, "references": []}
    mean = video_mean(c)
    thumbnail(mean, os.path.join(out_dir, f"{c.id}__video-mean.png"))
    for i, (path, factor) in enumerate(reference_specs(c)):
        entry = {"path": path, "factor_given": factor}
        ref = refs.load_reference(path, factor)
        entry.update(label=ref.info.label, factor_used=ref.factor, native_shape=list(ref.image.shape))
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
    specs = reference_specs(c)
    if len(specs) >= 2:
        try:
            rows = refs.compare(specs[0], specs[1:])
            rec["compare"] = refs.rows_to_dicts(rows)
        except ValueError as e:
            rec["compare"] = {"error": str(e)}
    else:
        r = metric.measure(refs.load_reference(*specs[0]).image)
        rec["compare"] = [{"label": refs.parse_reference_name(*specs[0]).label, "dan": r.value,
                           "band_snr": r.band_snr, "box": list(r.box)}]
    return rec


def _main(argv=None):
    ap = argparse.ArgumentParser(description="PlanetaryStack benchmark set")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("survey")
    s.add_argument("out_dir")
    s.add_argument("--only", nargs="*")
    sub.add_parser("list", help="id, kind, expect, mosaic, video path (tab-separated)")
    sh = sub.add_parser("show", help="one capture as JSON")
    sh.add_argument("id")
    cp = sub.add_parser("compare", help="our stack against the capture's references")
    cp.add_argument("id")
    cp.add_argument("ours", help="our stack, FITS or TIFF")
    cp.add_argument("--json", action="store_true")
    args = ap.parse_args(argv)
    if args.cmd == "list":
        for c in BENCHMARK:
            print(f"{c.id}\t{c.kind}\t{c.expect}\t{int(c.mosaic)}\t{nas(c.video)}")
        return 0
    if args.cmd == "show":
        c = by_id(args.id)
        print(json.dumps({"id": c.id, "kind": c.kind, "video": nas(c.video), "size": c.size,
                          "frames": c.frames, "sidecar": c.sidecar, "mosaic": c.mosaic,
                          "expect": c.expect, "why": c.why,
                          "references": [{"path": p, "factor": f} for p, f in reference_specs(c)]}, indent=2))
        return 0
    if args.cmd == "compare":
        d = refs.rows_to_dicts(compare_capture(args.id, args.ours))
        if args.json:
            print(json.dumps(d, indent=2, default=float))
        else:
            for r in d:
                ratio = f"  {r['ratio_to_best_reference']:.3f} of best ref" if r["ratio_to_best_reference"] else ""
                print(f"{r['label']:<48} DAN {r['dan']:.6f}  SNR {r['band_snr']:9.1f}{ratio}")
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
