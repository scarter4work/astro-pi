"""Offline ranking harness — the Phase 0 exit criterion (§10).

Given a reference fingerprint and a set of candidate fingerprints, rank the
candidates by fingerprint distance and report whether the professional renders
separate cleanly from the amateur ones. If they do not — without tuning
gymnastics — the fingerprint definition is wrong and everything downstream is
built on sand (§10). This harness is how we find that out early.

A CLI wrapper (``python -m autocontrast.eval.ranking``) fingerprints a directory
of pro renders and a directory of amateur renders and prints the report.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path

from autocontrast.fingerprint.distance import DistanceWeights, fingerprint_distance
from autocontrast.fingerprint.extract import FingerprintData, extract
from autocontrast.io.loaders import load_raster


@dataclass
class RankEntry:
    name: str
    distance: float


@dataclass
class SeparationReport:
    cleanly_separated: bool
    worst_pro_distance: float
    best_amateur_distance: float
    margin: float  # best_amateur_distance - worst_pro_distance; > 0 means clean separation


@dataclass
class LeaveOneOutReport:
    """Each professional render takes a turn as the reference; the remaining pros
    must still outrank every amateur. Removes any lucky-reference bias from the
    Phase 0 exit criterion."""

    all_clean: bool
    min_margin: float
    per_reference: dict[str, SeparationReport]


def rank_against_reference(
    reference: FingerprintData,
    candidates: dict[str, FingerprintData],
    weights: DistanceWeights = DistanceWeights(),
) -> list[RankEntry]:
    """Rank candidates by ascending fingerprint distance from the reference."""
    entries = [
        RankEntry(name=name, distance=fingerprint_distance(reference, fp, weights))
        for name, fp in candidates.items()
    ]
    entries.sort(key=lambda e: e.distance)
    return entries


def separation_report(
    ranked: list[RankEntry], pro_names: set[str], amateur_names: set[str]
) -> SeparationReport:
    """Do professionals cluster nearer the reference than amateurs?

    Clean separation means the *worst* professional is still closer than the
    *best* amateur — a positive margin between the two groups.
    """
    pro_distances = [e.distance for e in ranked if e.name in pro_names]
    amateur_distances = [e.distance for e in ranked if e.name in amateur_names]
    worst_pro = max(pro_distances)
    best_amateur = min(amateur_distances)
    margin = best_amateur - worst_pro
    return SeparationReport(
        cleanly_separated=margin > 0,
        worst_pro_distance=worst_pro,
        best_amateur_distance=best_amateur,
        margin=margin,
    )


def leave_one_out_report(
    pros: dict[str, FingerprintData],
    amateurs: dict[str, FingerprintData],
    weights: DistanceWeights = DistanceWeights(),
) -> LeaveOneOutReport:
    """Run separation once per professional-as-reference and aggregate."""
    per_reference: dict[str, SeparationReport] = {}
    for reference_name, reference_fp in pros.items():
        others = {n: fp for n, fp in pros.items() if n != reference_name}
        candidates = {**others, **amateurs}
        ranked = rank_against_reference(reference_fp, candidates, weights)
        per_reference[reference_name] = separation_report(
            ranked, pro_names=set(others), amateur_names=set(amateurs)
        )
    margins = [r.margin for r in per_reference.values()]
    return LeaveOneOutReport(
        all_clean=all(r.cleanly_separated for r in per_reference.values()),
        min_margin=min(margins),
        per_reference=per_reference,
    )


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

_RASTER_SUFFIXES = {".png", ".jpg", ".jpeg", ".tif", ".tiff", ".webp"}


def _fingerprint_dir(
    directory: Path, *, pixel_scale_arcsec: float, psf_fwhm_arcsec: float,
    palette_class: str, n_scales: int, max_dim: int | None = None,
) -> dict[str, FingerprintData]:
    fingerprints: dict[str, FingerprintData] = {}
    for path in sorted(directory.iterdir()):
        if path.suffix.lower() not in _RASTER_SUFFIXES:
            continue
        rgb = load_raster(path, max_dim=max_dim)
        fingerprints[path.name] = extract(
            rgb, pixel_scale_arcsec=pixel_scale_arcsec, n_scales=n_scales,
            psf_fwhm_arcsec=psf_fwhm_arcsec, palette_class=palette_class,
        )
    return fingerprints


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Phase 0 fingerprint ranking harness")
    parser.add_argument("--pros", required=True, type=Path, help="dir of professional renders")
    parser.add_argument("--amateurs", required=True, type=Path, help="dir of amateur renders")
    parser.add_argument("--reference", type=Path, default=None,
                        help="single reference render; omit for leave-one-out over --pros")
    parser.add_argument("--palette", default="RGB", help="palette class (default RGB)")
    parser.add_argument("--pixel-scale", type=float, default=1.0, help="nominal arcsec/px")
    parser.add_argument("--psf", type=float, default=2.0, help="nominal PSF FWHM arcsec")
    parser.add_argument("--scales", type=int, default=7, help="starlet scales")
    parser.add_argument("--max-dim", type=int, default=1600,
                        help="downsize longest side before fingerprinting (0 disables)")
    args = parser.parse_args(argv)

    max_dim = args.max_dim or None
    extract_kwargs = dict(pixel_scale_arcsec=args.pixel_scale, psf_fwhm_arcsec=args.psf,
                          palette_class=args.palette, n_scales=args.scales)
    dir_kwargs = dict(**extract_kwargs, max_dim=max_dim)
    pros = _fingerprint_dir(args.pros, **dir_kwargs)
    amateurs = _fingerprint_dir(args.amateurs, **dir_kwargs)

    if args.reference is not None:
        reference = extract(load_raster(args.reference, max_dim=max_dim), **extract_kwargs)
        return _report_single(reference, args.reference.name, pros, amateurs)
    return _report_leave_one_out(pros, amateurs)


def _report_single(reference, reference_name, pros, amateurs) -> int:
    ranked = rank_against_reference(reference, {**pros, **amateurs})
    report = separation_report(ranked, set(pros), set(amateurs))
    print(f"Reference: {reference_name}\n")
    print(f"{'rank':>4}  {'distance':>9}  {'class':<9}  name")
    for i, entry in enumerate(ranked, 1):
        label = "PRO" if entry.name in pros else "amateur"
        print(f"{i:>4}  {entry.distance:>9.4f}  {label:<9}  {entry.name}")
    verdict = "CLEAN" if report.cleanly_separated else "FAILED"
    print(f"\nSeparation: {verdict}  (margin {report.margin:+.4f}; "
          f"worst pro {report.worst_pro_distance:.4f}, best amateur "
          f"{report.best_amateur_distance:.4f})")
    return 0 if report.cleanly_separated else 1


def _report_leave_one_out(pros, amateurs) -> int:
    report = leave_one_out_report(pros, amateurs)
    print(f"Leave-one-out over {len(pros)} pro / {len(amateurs)} amateur renders\n")
    print(f"{'reference (pro)':<28}  {'margin':>9}  verdict")
    for name, sep in report.per_reference.items():
        verdict = "clean" if sep.cleanly_separated else "FAILED"
        print(f"{name:<28}  {sep.margin:>+9.4f}  {verdict}")
    overall = "CLEAN" if report.all_clean else "FAILED"
    print(f"\nExit criterion: {overall}  (min margin {report.min_margin:+.4f})")
    return 0 if report.all_clean else 1


if __name__ == "__main__":
    raise SystemExit(main())
