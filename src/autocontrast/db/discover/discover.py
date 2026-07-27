"""Miss-path orchestration: cone search -> rank -> fetch -> verify -> ingest (§5.2).

Ordering and gating policy live here. The verification gate is safety-critical: because
auto-discovered records enter the store as immediately consensus-eligible
``professional_render`` (§5.4), this gate is the only barrier between a parser regression
and a poisoned reference set. It fails closed.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

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
