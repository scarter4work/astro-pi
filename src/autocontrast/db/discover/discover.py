"""Miss-path orchestration: cone search -> rank -> fetch -> verify -> ingest (§5.2).

Ordering and gating policy live here. The verification gate is safety-critical: because
auto-discovered records enter the store as immediately consensus-eligible
``professional_render`` (§5.4), this gate is the only barrier between a parser regression
and a poisoned reference set. It fails closed.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from ..skymath import cones_overlap, separation_arcmin
from ..wcs import BlindSolver, WcsResult, acquire_wcs
from .gallery import GalleryEntry
from .index import IndexMatch


@dataclass
class RankedCandidate:
    entry: GalleryEntry
    separation_arcmin: float
    palette_compatible: bool
    framing_ratio: float   # |log(entry_radius / query_radius)|; 0 is a perfect match
    centering: float       # separation as a fraction of the entry footprint radius


def rank_candidates(
    matches: list[IndexMatch],
    *,
    query_radius_arcmin: float,
    user_palette_class: str,
) -> list[RankedCandidate]:
    """Order overlapping candidates best-first.

    Policy, in precedence order:

    1. **Palette compatibility.** §2.3 flags rather than filters, so an incompatible
       render is demoted, never dropped — it can still contribute structure and tone.
       ``unknown`` on either side is never treated as compatible: it means undetermined,
       and claiming a match would let a mismatched reference push chroma (§2.1).
    2. **Framing aptness.** ``|log(ratio)|`` so that half the field and twice the field
       are equally penalised. Band-limiting (§4.4) absorbs residual scale differences,
       but a similarly framed render is a more apt style target than a close-up.
    3. **Centering.** How centrally the query sits inside the reference footprint.
    4. **Id**, so identical candidates order reproducibly.
    """
    ranked: list[RankedCandidate] = []
    for match in matches:
        entry = match.entry
        compatible = (
            user_palette_class == entry.palette_class
            and user_palette_class != "unknown"
            and entry.palette_class != "unknown"
        )
        radius = entry.fov_radius_arcmin or 0.0
        framing = (
            abs(math.log(radius / query_radius_arcmin))
            if radius > 0 and query_radius_arcmin > 0 else math.inf
        )
        centering = match.separation_arcmin / radius if radius > 0 else math.inf
        ranked.append(RankedCandidate(
            entry=entry, separation_arcmin=match.separation_arcmin,
            palette_compatible=compatible, framing_ratio=framing, centering=centering,
        ))

    ranked.sort(key=lambda c: (
        not c.palette_compatible, round(c.framing_ratio, 6), round(c.centering, 6), c.entry.id
    ))
    return ranked


# G2 tolerance: a quarter of the footprint radius, floored at one arcminute so tiny
# fields still get a workable window. For eso1103a (24.61') that is ~6.2', while the
# known AVM force-parse corruption is ~140' out — an order of magnitude of margin.
_TOLERANCE_FRACTION = 0.25
_TOLERANCE_FLOOR_ARCMIN = 1.0

WcsAcquirer = Callable[[Path], WcsResult]


def position_tolerance_arcmin(fov_radius_arcmin: float | None) -> float:
    """Allowed disagreement between a parsed AVM and the published position."""
    if not fov_radius_arcmin:
        return _TOLERANCE_FLOOR_ARCMIN
    return max(_TOLERANCE_FRACTION * fov_radius_arcmin, _TOLERANCE_FLOOR_ARCMIN)


@dataclass
class VerifyResult:
    accepted: bool
    reason: str
    wcs: WcsResult | None
    pixel_scale_arcsec: float | None


def verify_candidate(
    candidate: RankedCandidate,
    image_path: str | Path,
    *,
    query_ra_deg: float,
    query_dec_deg: float,
    query_radius_arcmin: float,
    blind_solver: BlindSolver | None = None,
    wcs_acquirer: WcsAcquirer | None = None,
) -> VerifyResult:
    """Run gates G1-G6 against a fetched candidate. Fails closed; never raises.

    ``wcs_acquirer`` is injectable so the gate is testable without real image files; it
    defaults to :func:`acquire_wcs` with the published position as the manual annotation.
    """
    entry = candidate.entry
    path = Path(image_path)

    # ---- G5: license and attribution (§5.5). Checked first: it needs no I/O, and a
    # reference we cannot attribute is unusable however good its position.
    if not entry.license:
        return VerifyResult(
            False,
            f"{entry.id}: license could not be established (credit asserts copyright, or "
            "the gallery default does not apply). Refusing to ingest under a license we "
            "cannot support (§5.5).",
            None, None,
        )
    if not entry.attribution:
        return VerifyResult(
            False,
            f"{entry.id}: no attribution captured; the galleries require the full credit "
            "line, and §5.5 makes it travel with every record.",
            None, None,
        )

    # ---- G1: the indexed footprint must overlap the query cone.
    if not entry.has_position:
        return VerifyResult(False, f"{entry.id}: no indexed position.", None, None)
    indexed_sep = separation_arcmin(query_ra_deg, query_dec_deg, entry.ra_deg, entry.dec_deg)
    if not cones_overlap(indexed_sep, query_radius_arcmin, entry.fov_radius_arcmin):
        return VerifyResult(
            False,
            f"{entry.id}: indexed footprint does not overlap the query cone "
            f"({indexed_sep:.1f}' apart).",
            None, None,
        )

    # ---- G2: independent WCS cross-check.
    if wcs_acquirer is None:
        manual = {
            "ra_deg": entry.ra_deg,
            "dec_deg": entry.dec_deg,
            "fov_radius_arcmin": entry.fov_radius_arcmin,
            "pixel_scale_arcsec": entry.pixel_scale_arcsec,
        }

        def wcs_acquirer(p: Path) -> WcsResult:
            return acquire_wcs(p, blind_solver=blind_solver, manual=manual)

    wcs = wcs_acquirer(path)
    if not wcs.solved:
        return VerifyResult(False, f"{entry.id}: unsolved — {wcs.detail}", wcs, None)

    if wcs.wcs_source == "avm":
        tolerance = position_tolerance_arcmin(entry.fov_radius_arcmin)
        drift = separation_arcmin(wcs.ra_deg, wcs.dec_deg, entry.ra_deg, entry.dec_deg)
        if drift > tolerance:
            return VerifyResult(
                False,
                f"{entry.id}: parsed AVM and published position disagree by "
                f"{drift:.1f}' (tolerance {tolerance:.1f}'). One of them is wrong and "
                "which is unknowable here, so the candidate is rejected rather than "
                "trusting either.",
                wcs, None,
            )

    # ---- G3: the solved position is authoritative and must still be in the cone.
    solved_sep = separation_arcmin(query_ra_deg, query_dec_deg, wcs.ra_deg, wcs.dec_deg)
    solved_radius = wcs.fov_radius_arcmin or entry.fov_radius_arcmin
    if not cones_overlap(solved_sep, query_radius_arcmin, solved_radius):
        return VerifyResult(
            False,
            f"{entry.id}: solved position falls outside the query cone "
            f"({solved_sep:.1f}' apart); the indexed position was wrong.",
            wcs, None,
        )

    # ---- G4: an angular pixel scale is mandatory (§2.2). A real solve wins over the
    # published nominal value.
    scale = wcs.pixel_scale_arcsec or entry.pixel_scale_arcsec
    if scale is None:
        return VerifyResult(
            False,
            f"{entry.id}: no pixel scale from the solve or from published metadata; "
            "§2.2 forbids comparing scales in pixels.",
            wcs, None,
        )

    # ---- G6: palette is derived, never guessed. 'unknown' is permitted — it forfeits
    # chroma via the §2.3 gate, which is the honest outcome, not a failure.
    verified = "cross-checked against published position" if wcs.wcs_source == "avm" \
        else f"accepted via {wcs.wcs_source} (no independent cross-check)"
    return VerifyResult(True, f"{entry.id}: {verified}.", wcs, float(scale))
