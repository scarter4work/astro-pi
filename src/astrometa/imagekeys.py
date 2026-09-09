import hashlib
from pathlib import Path
import numpy as np

_CHUNK = 8 * 1024 * 1024
SATURATION_LEVEL = 65000.0


def content_hash(path: Path) -> str:
    h = hashlib.blake2b(digest_size=16)
    with open(path, "rb") as f:
        while chunk := f.read(_CHUNK):
            h.update(chunk)
    return h.hexdigest()


def _block_mean(a: np.ndarray, out_h: int, out_w: int) -> np.ndarray:
    h, w = a.shape
    a = a[: h - h % out_h, : w - w % out_w]
    return a.reshape(out_h, a.shape[0] // out_h,
                     out_w, a.shape[1] // out_w).mean(axis=(1, 3))


def fingerprint(pixels: np.ndarray, size: int = 16) -> str:
    """
    Perceptual dHash of an image: a horizontal-gradient bit pattern over a
    size x size grid, computed after percentile normalisation so the hash
    keys on field content rather than absolute brightness/exposure.

    A degenerate (flat, all-NaN, etc.) image has no gradient information
    and is not an error — it returns an all-zero fingerprint of the same
    bit width as the normal path (size * size bits) so hamming() always
    compares equal-length values.
    """
    a = np.asarray(pixels, dtype=np.float64)
    lo, hi = np.percentile(a, (1.0, 99.0))
    if not np.isfinite(lo) or not np.isfinite(hi) or hi <= lo:
        return "0" * ((size * size + 3) // 4)
    a = np.clip((a - lo) / (hi - lo), 0.0, 1.0)
    small = _block_mean(a, size, size + 1)
    bits = (small[:, 1:] > small[:, :-1]).flatten()
    value = 0
    for bit in bits:
        value = (value << 1) | int(bit)
    return f"{value:0{(bits.size + 3) // 4}x}"


def hamming(a: str, b: str) -> int:
    if len(a) != len(b):
        raise ValueError(
            f"cannot compare fingerprints of different width: "
            f"{len(a)} hex chars vs {len(b)} hex chars"
        )
    return bin(int(a, 16) ^ int(b, 16)).count("1")


def pixel_stats(pixels: np.ndarray) -> tuple[float, float]:
    """
    Returns (background_median, saturated_fraction) for a raw pixel array.

    Unlike fingerprint()'s degenerate-image handling, non-finite pixel
    data here is a hard error rather than a sentinel result: bg_median
    feeds directly into downstream quality thresholds (e.g. session
    culling), and np.median/.mean() would otherwise propagate NaN into
    that value silently — a NaN there makes every later `<` / `>`
    comparison against it silently false instead of raising, which is
    exactly the quiet-wrong-answer failure mode this project's
    no-silent-fallbacks rule exists to prevent.
    """
    a = np.asarray(pixels, dtype=np.float64)
    if not np.isfinite(a).all():
        raise ValueError("pixel_stats: input contains NaN or infinite pixels")
    return float(np.median(a)), float((a >= SATURATION_LEVEL).mean())
