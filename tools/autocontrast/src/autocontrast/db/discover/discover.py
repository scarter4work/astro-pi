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

from ..ingest import ingest_reference_auto
from ..records import ReferenceRecord
from ..skymath import cones_overlap, separation_arcmin
from ..store import ConeMatch, FingerprintStore
from ..wcs import BlindSolver, WcsResult, acquire_wcs
from .crawl import MAX_IMAGE_BYTES, Fetcher
from .gallery import GalleryEntry
from .index import GalleryIndex, IndexMatch


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


def verify_metadata(
    entry: GalleryEntry,
    *,
    query_ra_deg: float,
    query_dec_deg: float,
    query_radius_arcmin: float,
) -> VerifyResult | None:
    """Gates G5 and G1 — everything decidable from indexed metadata alone.

    Returns the rejection, or ``None`` when the entry clears both.

    Split out from :func:`verify_candidate` so the orchestrator can run these BEFORE
    downloading. A gallery render can be a quarter-gigabyte; fetching one and only then
    rejecting it for a missing credit line spends a free public archive's bandwidth on
    a verdict already available from the index.
    """
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
    return None


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

    rejection = verify_metadata(
        entry,
        query_ra_deg=query_ra_deg, query_dec_deg=query_dec_deg,
        query_radius_arcmin=query_radius_arcmin,
    )
    if rejection is not None:
        return rejection

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


@dataclass
class CandidateReport:
    """Why one candidate was accepted or rejected — §12's surfacing requirement."""

    id: str
    accepted: bool
    reason: str


@dataclass
class DiscoveryOutcome:
    ingested: bool
    record: ReferenceRecord | None
    considered: list[CandidateReport]
    detail: str


@dataclass
class AcquisitionOutcome:
    """``discovered`` is ``None`` when the store already held a match, so no discovery
    was attempted."""

    matches: list[ConeMatch]
    discovered: DiscoveryOutcome | None
    detail: str


def _cached_image(fetcher: Fetcher, url: str, cache_dir: Path, entry_id: str) -> Path:
    """Download ``url`` into ``cache_dir`` unless already present."""
    cache_dir.mkdir(parents=True, exist_ok=True)
    suffix = Path(url).suffix or ".jpg"
    path = cache_dir / f"{entry_id}{suffix}"
    if not path.exists():
        path.write_bytes(fetcher.get_bytes(url, max_bytes=MAX_IMAGE_BYTES))
    return path


def discover_reference(
    store: FingerprintStore,
    index: GalleryIndex,
    *,
    ra_deg: float,
    dec_deg: float,
    search_radius_arcmin: float,
    palette_class: str,
    fetcher: Fetcher,
    cache_dir: str | Path,
    blind_solver: BlindSolver | None = None,
    wcs_acquirer: WcsAcquirer | None = None,
    top_k: int = 1,
    psf_fwhm_arcsec: float = 2.0,
    n_scales: int = 7,
    max_dim: int | None = 1600,
) -> DiscoveryOutcome:
    """Cone-search the index, then fetch, verify, and ingest the best candidate(s).

    ``palette_class`` is the *user image's* palette, used only for §2.3 ranking. A
    discovered record always stores the render's own derived palette.

    ``top_k`` defaults to 1, matching §5.4: seed a position with one best professional
    reference, and let consensus arrive as more accumulate.
    """
    cache = Path(cache_dir)
    matches = index.cone_search(ra_deg, dec_deg, search_radius_arcmin)
    if not matches:
        return DiscoveryOutcome(
            False, None, [],
            f"No candidate in the gallery index covers ({ra_deg:.4f}, {dec_deg:.4f}) "
            f"within {search_radius_arcmin:.1f}'. Sync the index, or add a curated "
            "catalog entry.",
        )

    ranked = rank_candidates(
        matches, query_radius_arcmin=search_radius_arcmin, user_palette_class=palette_class
    )

    reports: list[CandidateReport] = []
    ingested: ReferenceRecord | None = None

    for candidate in ranked:
        entry = candidate.entry
        if not entry.image_url:
            reports.append(CandidateReport(entry.id, False, "no CDN image URL published"))
            continue

        # Decide everything the index can decide before spending the archive's
        # bandwidth: an unlicensed or out-of-cone candidate costs zero bytes.
        rejection = verify_metadata(
            entry, query_ra_deg=ra_deg, query_dec_deg=dec_deg,
            query_radius_arcmin=search_radius_arcmin,
        )
        if rejection is not None:
            reports.append(CandidateReport(entry.id, False, rejection.reason))
            continue

        try:
            image_path = _cached_image(fetcher, entry.image_url, cache, entry.id)
        except Exception as exc:  # network/size failure for THIS candidate only
            reports.append(CandidateReport(entry.id, False, f"download failed: {exc}"))
            continue

        verdict = verify_candidate(
            candidate, image_path,
            query_ra_deg=ra_deg, query_dec_deg=dec_deg,
            query_radius_arcmin=search_radius_arcmin,
            blind_solver=blind_solver, wcs_acquirer=wcs_acquirer,
        )
        reports.append(CandidateReport(entry.id, verdict.accepted, verdict.reason))
        if not verdict.accepted:
            continue

        outcome = ingest_reference_auto(
            store, image_path,
            psf_fwhm_arcsec=psf_fwhm_arcsec,
            palette_class=entry.palette_class,
            source_type="professional_render",
            provenance={
                "source_url": entry.detail_url,
                "license": entry.license,
                "attribution": entry.attribution,
                "gallery": entry.gallery,
                "discovered": True,
            },
            manual={
                "ra_deg": entry.ra_deg,
                "dec_deg": entry.dec_deg,
                "fov_radius_arcmin": entry.fov_radius_arcmin,
                "pixel_scale_arcsec": verdict.pixel_scale_arcsec,
            },
            pixel_scale_arcsec=verdict.pixel_scale_arcsec,
            id=f"{entry.gallery}:{entry.id}",
            n_scales=n_scales, max_dim=max_dim,
        )
        if not outcome.ingested:
            reports[-1] = CandidateReport(entry.id, False, f"ingest declined: {outcome.detail}")
            continue

        ingested = outcome.record
        if sum(1 for r in reports if r.accepted) >= top_k:
            break

    if ingested is None:
        return DiscoveryOutcome(
            False, None, reports,
            f"{len(ranked)} candidate(s) considered, none usable: "
            + "; ".join(f"{r.id} ({r.reason})" for r in reports),
        )
    return DiscoveryOutcome(
        True, ingested, reports,
        f"Ingested {ingested.id} from the {ingested.provenance['gallery']} gallery.",
    )


def acquire_reference(
    store: FingerprintStore,
    index: GalleryIndex,
    *,
    ra_deg: float,
    dec_deg: float,
    search_radius_arcmin: float,
    palette_class: str,
    fetcher: Fetcher,
    cache_dir: str | Path,
    blind_solver: BlindSolver | None = None,
    wcs_acquirer: WcsAcquirer | None = None,
    top_k: int = 1,
    psf_fwhm_arcsec: float = 2.0,
    n_scales: int = 7,
    max_dim: int | None = 1600,
) -> AcquisitionOutcome:
    """The Phase 1 exit criterion in one call: look up, and on a miss discover.

    Returns existing matches untouched when the store already covers the position — no
    network, no discovery.
    """
    matches = store.cone_search(ra_deg, dec_deg, search_radius_arcmin, palette_class)
    if matches:
        return AcquisitionOutcome(
            matches, None,
            f"{len(matches)} reference(s) already stored for this position.",
        )

    discovered = discover_reference(
        store, index, ra_deg=ra_deg, dec_deg=dec_deg,
        search_radius_arcmin=search_radius_arcmin, palette_class=palette_class,
        fetcher=fetcher, cache_dir=cache_dir, blind_solver=blind_solver,
        wcs_acquirer=wcs_acquirer, top_k=top_k, psf_fwhm_arcsec=psf_fwhm_arcsec,
        n_scales=n_scales, max_dim=max_dim,
    )
    return AcquisitionOutcome(
        store.cone_search(ra_deg, dec_deg, search_radius_arcmin, palette_class),
        discovered,
        discovered.detail,
    )
