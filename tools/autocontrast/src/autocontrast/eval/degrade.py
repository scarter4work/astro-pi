"""Flattening degradation and the reframed Phase 0 exit test.

The pro-vs-amateur separation test conflates provenance with presentation. What
the Phase 2 optimizer actually needs is that the fingerprint's gradient points
toward depth: applying a flattening degradation to any render must increase its
distance to a deep reference, monotonically with degradation strength. This module
provides a deterministic pixel-level flatten and a monotonicity check over it.

``flatten`` manipulates PIXELS only (blur-blend, contrast compression, floor lift)
— it never touches the fingerprint math, so measuring the result through the
normal pipeline is a non-circular test of the fingerprint's direction.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from scipy.ndimage import gaussian_filter

from autocontrast.fingerprint.distance import fingerprint_distance
from autocontrast.fingerprint.extract import FingerprintData, extract


def flatten(rgb: np.ndarray, strength: float, blur_sigma: float = 3.0) -> np.ndarray:
    """Return a flattened copy of an sRGB image in [0, 1].

    ``strength`` in [0, 1] interpolates from the original (0) to a heavily flat
    render (1) by three flat-render hallmarks: loss of fine structure (blend toward
    a blurred version), reduced global contrast (compress toward the mean), and a
    muddy lifted black floor.
    """
    if strength == 0.0:
        return rgb.copy()

    blurred = np.stack(
        [gaussian_filter(rgb[..., c], sigma=blur_sigma) for c in range(rgb.shape[-1])],
        axis=-1,
    )
    out = (1.0 - strength) * rgb + strength * blurred

    mean = out.mean()
    out = mean + (out - mean) * (1.0 - 0.5 * strength)   # compress contrast
    out = out * (1.0 - 0.1 * strength) + 0.1 * strength  # lift the floor (muddy)
    return np.clip(out, 0.0, 1.0)


def over_sharpen(rgb: np.ndarray, strength: float, blur_sigma: float = 2.0) -> np.ndarray:
    """Over-process via unsharp masking — the "over-cooked attractor" (§6.1):
    plastic texture and halos from pushing local contrast too hard."""
    if strength == 0.0:
        return rgb.copy()
    blurred = np.stack(
        [gaussian_filter(rgb[..., c], sigma=blur_sigma) for c in range(rgb.shape[-1])],
        axis=-1,
    )
    return np.clip(rgb + 2.0 * strength * (rgb - blurred), 0.0, 1.0)


def _luminance(rgb: np.ndarray) -> np.ndarray:
    return rgb.mean(axis=-1, keepdims=True)


def desaturate(rgb: np.ndarray, strength: float) -> np.ndarray:
    """Wash out color toward gray (a flat global loss of chroma structure)."""
    lum = _luminance(rgb)
    return np.clip((1.0 - strength) * rgb + strength * lum, 0.0, 1.0)


def oversaturate(rgb: np.ndarray, strength: float) -> np.ndarray:
    """Push chroma too far — a flat global boost rather than tasteful structure."""
    lum = _luminance(rgb)
    return np.clip(lum + (rgb - lum) * (1.0 + 2.0 * strength), 0.0, 1.0)


# Known-bad degradation axes for the local-minimum exit test. A valid reference
# must move away (D increasing) along every one of them.
DEFAULT_DEGRADATIONS = {
    "flatten": flatten,
    "over_sharpen": over_sharpen,
    "desaturate": desaturate,
    "oversaturate": oversaturate,
}


@dataclass
class FlatteningResult:
    name: str
    distances: list[float]  # distance to the reference at each strength
    monotonic: bool


@dataclass
class FlatteningReport:
    all_monotonic: bool
    per_image: dict[str, FlatteningResult]


def flattening_monotonicity_report(
    reference: FingerprintData,
    images: dict[str, np.ndarray],
    *,
    strengths: list[float],
    extract_kwargs: dict,
    tol: float = 1e-9,
) -> FlatteningReport:
    """Check that flattening moves each image monotonically away from the reference."""
    per_image: dict[str, FlatteningResult] = {}
    for name, rgb in images.items():
        distances = [
            fingerprint_distance(reference, extract(flatten(rgb, s), **extract_kwargs))
            for s in strengths
        ]
        monotonic = all(
            distances[i] <= distances[i + 1] + tol for i in range(len(distances) - 1)
        )
        per_image[name] = FlatteningResult(name=name, distances=distances, monotonic=monotonic)

    return FlatteningReport(
        all_monotonic=all(r.monotonic for r in per_image.values()),
        per_image=per_image,
    )


@dataclass
class AxisResult:
    name: str
    distances: list[float]      # D(reference, degrade(reference, s)) for each strength
    monotonic: bool             # non-decreasing
    strictly_increasing: bool   # every step strictly larger (a true minimum, no plateau)


@dataclass
class LocalMinimumReport:
    """Whether the reference sits at the bottom of the distance valley along every
    degradation axis — the property that makes it a valid optimization target."""

    is_local_minimum: bool
    per_axis: dict[str, AxisResult]


def reference_is_local_minimum(
    reference_rgb: np.ndarray,
    *,
    strengths: list[float],
    extract_kwargs: dict,
    degradations: dict = DEFAULT_DEGRADATIONS,
    tol: float = 1e-9,
) -> LocalMinimumReport:
    """The real Phase 0 exit criterion: degrade the reference along each known-bad
    axis and confirm its fingerprint distance rises monotonically from zero.

    If any degradation *reduces* D, the fingerprint would reward moving away from
    the reference along that axis — the optimizer would be pulled the wrong way.
    """
    reference_fp = extract(reference_rgb, **extract_kwargs)
    per_axis: dict[str, AxisResult] = {}
    for name, degrade in degradations.items():
        distances = [
            fingerprint_distance(reference_fp, extract(degrade(reference_rgb, s), **extract_kwargs))
            for s in strengths
        ]
        monotonic = all(distances[i] <= distances[i + 1] + tol for i in range(len(distances) - 1))
        strictly = all(distances[i] < distances[i + 1] for i in range(len(distances) - 1))
        per_axis[name] = AxisResult(
            name=name, distances=distances, monotonic=monotonic, strictly_increasing=strictly
        )

    return LocalMinimumReport(
        is_local_minimum=all(a.strictly_increasing for a in per_axis.values()),
        per_axis=per_axis,
    )


# --------------------------------------------------------------------------
# CLI — the real Phase 0 exit test over a directory of reference renders.
# --------------------------------------------------------------------------

_RASTER_SUFFIXES = {".png", ".jpg", ".jpeg", ".tif", ".tiff", ".webp"}


def main(argv: list[str] | None = None) -> int:
    import argparse
    from pathlib import Path

    from autocontrast.io.loaders import load_raster

    parser = argparse.ArgumentParser(
        description="Phase 0 exit test: is each reference a strict local minimum of D?"
    )
    parser.add_argument("--references", required=True, type=Path, help="dir of reference renders")
    parser.add_argument("--palette", default="RGB")
    parser.add_argument("--pixel-scale", type=float, default=1.0)
    parser.add_argument("--psf", type=float, default=2.0)
    parser.add_argument("--scales", type=int, default=6)
    parser.add_argument("--max-dim", type=int, default=1200)
    args = parser.parse_args(argv)

    ek = dict(pixel_scale_arcsec=args.pixel_scale, psf_fwhm_arcsec=args.psf,
              palette_class=args.palette, n_scales=args.scales)
    strengths = [0.0, 0.25, 0.5, 0.75, 1.0]
    print(f"Phase 0 exit test — reference as strict local minimum of D")
    print(f"axes: {list(DEFAULT_DEGRADATIONS)}   strengths: {strengths}\n")

    overall = True
    for path in sorted(args.references.iterdir()):
        if path.suffix.lower() not in _RASTER_SUFFIXES:
            continue
        rgb = load_raster(path, max_dim=args.max_dim or None)
        rep = reference_is_local_minimum(rgb, strengths=strengths, extract_kwargs=ek)
        overall &= rep.is_local_minimum
        print(f"{path.name:<20} local_min={rep.is_local_minimum}")
        for name, axis in rep.per_axis.items():
            ds = "  ".join(f"{x:.3f}" for x in axis.distances)
            flag = "" if axis.strictly_increasing else "  <-- NOT strict"
            print(f"    {name:<13} {ds}{flag}")
    print(f"\nALL REFERENCES PASS: {overall}")
    return 0 if overall else 1


if __name__ == "__main__":
    raise SystemExit(main())
