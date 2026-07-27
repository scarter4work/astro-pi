"""Candidate ranking for the miss path."""

from __future__ import annotations

from autocontrast.db.discover.discover import rank_candidates
from autocontrast.db.discover.gallery import GalleryEntry
from autocontrast.db.discover.index import IndexMatch


def make(entry_id, *, radius=20.0, palette="RGB", sep=0.0):
    entry = GalleryEntry(
        id=entry_id, gallery="esa_hubble",
        detail_url=f"https://example.test/images/{entry_id}/",
        image_url=f"https://cdn.example.test/images/large/{entry_id}.jpg",
        ra_deg=83.8, dec_deg=-5.4, fov_w_arcmin=radius, fov_h_arcmin=radius,
        fov_radius_arcmin=radius, width_px=4000, height_px=4000,
        pixel_scale_arcsec=0.5, object_name="M 42", category="Nebulae",
        entry_type="Observation", palette_class=palette, license="CC BY 4.0",
        attribution="NASA, ESA", published_utc="2006-01-11T16:00:00Z", parsed_ok=True,
    )
    return IndexMatch(entry=entry, separation_arcmin=sep)


def test_palette_compatible_candidates_rank_first():
    """§2.3 flags rather than filters, so a mismatch is demoted, never dropped — it can
    still contribute structure and tone."""
    ranked = rank_candidates(
        [make("mismatch", palette="SHO"), make("match", palette="HOO")],
        query_radius_arcmin=20.0, user_palette_class="HOO",
    )
    assert [c.entry.id for c in ranked] == ["match", "mismatch"]
    assert ranked[0].palette_compatible is True
    assert ranked[1].palette_compatible is False


def test_incompatible_candidates_are_retained_not_discarded():
    ranked = rank_candidates([make("only", palette="SHO")],
                             query_radius_arcmin=20.0, user_palette_class="HOO")
    assert len(ranked) == 1


def test_closest_framing_wins_among_compatible_candidates():
    """A 20-arcmin field is better served by a similarly framed render than by a
    3-arcmin close-up or a 5-degree wide field."""
    ranked = rank_candidates(
        [make("closeup", radius=3.0), make("apt", radius=22.0), make("wide", radius=300.0)],
        query_radius_arcmin=20.0, user_palette_class="RGB",
    )
    assert ranked[0].entry.id == "apt"


def test_framing_ratio_is_symmetric_in_log_space():
    """Half the field and twice the field are equally mismatched."""
    ranked = {c.entry.id: c for c in rank_candidates(
        [make("half", radius=10.0), make("double", radius=40.0)],
        query_radius_arcmin=20.0, user_palette_class="RGB",
    )}
    assert ranked["half"].framing_ratio == ranked["double"].framing_ratio


def test_centering_breaks_ties_between_equally_framed_candidates():
    ranked = rank_candidates(
        [make("offset", radius=20.0, sep=15.0), make("centered", radius=20.0, sep=1.0)],
        query_radius_arcmin=20.0, user_palette_class="RGB",
    )
    assert [c.entry.id for c in ranked] == ["centered", "offset"]


def test_ranking_is_deterministic_for_identical_candidates():
    """Identical candidates must order by id, so runs are reproducible and testable."""
    ranked = rank_candidates([make("bbb"), make("aaa"), make("ccc")],
                             query_radius_arcmin=20.0, user_palette_class="RGB")
    assert [c.entry.id for c in ranked] == ["aaa", "bbb", "ccc"]


def test_unknown_user_palette_treats_nothing_as_compatible():
    """'unknown' means undetermined, so it must not be claimed to match anything —
    that would let a mismatched reference push chroma (§2.1)."""
    ranked = rank_candidates([make("rgb", palette="RGB")],
                             query_radius_arcmin=20.0, user_palette_class="unknown")
    assert ranked[0].palette_compatible is False


def test_empty_input_yields_empty_output():
    assert rank_candidates([], query_radius_arcmin=20.0, user_palette_class="RGB") == []
