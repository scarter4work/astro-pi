"""bayer.py -- the CFA pattern of an 8-bit AVI capture, measured on its raw frames.

The benchmark stacks every Bayer capture debayered (stack-probe --bayer PATTERN), so it needs
each capture's pattern in the READER's frame layout: an explicit pattern is applied as it
stands (ser::ResolveBayer: "An explicit choice wins"), and avi_real_check.py proves the reader's
frames equal ffmpeg's rawvideo bytes. The method is avi_real_check.py's Bayer-phase check:
per-site means over the unsaturated bright disc (2x2 sums >= 50 % of their 99.99th percentile,
no site >= 250), pooled over the first frames; the two green sites are the more equal diagonal,
and red is the brighter of the other two (a warm target -- Jupiter, Mars, Saturn, the Moon --
through a CMOS CFA).

Only the GREEN PHASE is evidence. The red/blue call (the warm-target rule) is not a valid source
of the pattern (Scott, 2026-10-22), and through a narrowband filter such as the Lqef it does not
apply at all: the benchmark takes red/blue from the camera spec (benchset.CAMERA_RGGB: every
colour capture in the set is a ZWO ASI585MC or ASI2400MC, both RGGB).
"""
import subprocess
from dataclasses import dataclass

import numpy as np

DISC_FRACTION = 0.5
DISC_PCT = 99.99
SATURATED = 250
MIN_DISC_SUPERPIXELS = 100
MAX_SATURATED_SHARE = 0.8
FRAMES = 20
SITES = ((0, 0), (0, 1), (1, 0), (1, 1))


class Unmeasurable(ValueError):
    pass


@dataclass(frozen=True)
class Measurement:
    pattern: str            # letters of sites (0,0) (0,1) (1,0) (1,1), e.g. "RGGB"
    means: tuple            # per-site means in that order
    superpixels: int        # unsaturated disc superpixels, summed over the frames
    saturated_share: float  # of the bright disc
    green_split: float      # relative difference of the two green sites
    red_blue_split: float   # relative difference of red and blue

    def __str__(self):
        m = " ".join(f"{x:.2f}" for x in self.means)
        return (f"{self.pattern}: site means {m}, greens {100 * self.green_split:.1f} % apart, "
                f"red/blue {100 * self.red_blue_split:.1f} % apart, {self.superpixels} superpixels, "
                f"{100 * self.saturated_share:.0f} % saturated")


def _rel(a, b):
    return abs(a - b) / ((a + b) / 2) if a + b > 0 else 0.0


def measure_frames(frames) -> Measurement:
    """Pattern of raw CFA frames (n x h x w, 8-bit values)."""
    sums, count, bright = np.zeros(4), 0, 0
    for f in np.asarray(frames, dtype=np.float64):
        f = f[:f.shape[0] // 2 * 2, :f.shape[1] // 2 * 2]
        s = [f[dy::2, dx::2] for dy, dx in SITES]
        tot = s[0] + s[1] + s[2] + s[3]
        disc = tot >= DISC_FRACTION * np.percentile(tot, DISC_PCT)
        mask = disc & (np.maximum.reduce(s) < SATURATED)
        sums += [x[mask].sum() for x in s]
        count += int(mask.sum())
        bright += int(disc.sum())
    sat = 1 - count / bright if bright else 1.0
    if count < MIN_DISC_SUPERPIXELS or sat > MAX_SATURATED_SHARE:
        raise Unmeasurable(f"{count} unsaturated disc superpixels (min {MIN_DISC_SUPERPIXELS}), "
                           f"{100 * sat:.0f} % of the disc saturated (max {100 * MAX_SATURATED_SHARE:.0f} %)")
    m = sums / count
    main, anti = _rel(m[0], m[3]), _rel(m[1], m[2])
    greens, others = ((1, 2), (0, 3)) if anti < main else ((0, 3), (1, 2))
    red = others[0] if m[others[0]] > m[others[1]] else others[1]
    blue = others[1] if red == others[0] else others[0]
    letters = ["G"] * 4
    letters[red], letters[blue] = "R", "B"
    return Measurement("".join(letters), tuple(float(x) for x in m), count, sat,
                       _rel(m[greens[0]], m[greens[1]]), _rel(m[red], m[blue]))


def read_frames(path, size, n=FRAMES) -> np.ndarray:
    """The first n frames as ffmpeg decodes them (= the reader's layout), n x h x w uint8."""
    w, h = size
    raw = subprocess.run(["ffmpeg", "-v", "error", "-nostdin", "-i", path, "-frames:v", str(n),
                          "-f", "rawvideo", "-pix_fmt", "gray", "-"], check=True, capture_output=True).stdout
    a = np.frombuffer(raw, np.uint8)
    if a.size == 0 or a.size % (w * h):
        raise ValueError(f"{path}: read {a.size} bytes, not whole {w}x{h} frames")
    return a.reshape(-1, h, w)


def measure_video(path, size, n=FRAMES) -> Measurement:
    return measure_frames(read_frames(path, size, n))
