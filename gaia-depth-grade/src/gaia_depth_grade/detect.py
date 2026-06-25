from __future__ import annotations

import numpy as np
from astropy.stats import sigma_clipped_stats
from astropy.table import Table
from photutils.detection import DAOStarFinder


def measure_fwhm(image: np.ndarray, x: float, y: float, box: int = 7) -> float:
    half = box // 2
    xi, yi = int(round(x)), int(round(y))
    if xi - half < 0 or yi - half < 0 or xi + half >= image.shape[1] or yi + half >= image.shape[0]:
        return float("nan")
    cut = image[yi - half : yi + half + 1, xi - half : xi + half + 1].astype(float)
    cut = cut - np.median(cut)
    cut[cut < 0] = 0.0
    total = cut.sum()
    if total <= 0:
        return float("nan")
    yy, xx = np.mgrid[0 : cut.shape[0], 0 : cut.shape[1]]
    mx = (xx * cut).sum() / total
    my = (yy * cut).sum() / total
    varx = ((xx - mx) ** 2 * cut).sum() / total
    vary = ((yy - my) ** 2 * cut).sum() / total
    sigma = np.sqrt(max((varx + vary) / 2.0, 0.0))
    return float(2.3548 * sigma)


def detect_stars(image: np.ndarray, fwhm: float, threshold_sigma: float) -> Table:
    _, median, std = sigma_clipped_stats(image, sigma=3.0)
    finder = DAOStarFinder(fwhm=fwhm, threshold=threshold_sigma * std)
    found = finder(image - median)
    if found is None or len(found) == 0:
        return Table(names=("x", "y", "flux", "fwhm"), dtype=(float, float, float, float))
    out = Table()
    # Use x_centroid/y_centroid (photutils 3.0+), fallback to xcentroid/ycentroid for older versions
    x_col = "x_centroid" if "x_centroid" in found.colnames else "xcentroid"
    y_col = "y_centroid" if "y_centroid" in found.colnames else "ycentroid"
    out["x"] = np.asarray(found[x_col], dtype=float)
    out["y"] = np.asarray(found[y_col], dtype=float)
    out["flux"] = np.asarray(found["flux"], dtype=float)
    out["fwhm"] = [measure_fwhm(image, xx, yy) for xx, yy in zip(out["x"], out["y"])]
    return out
