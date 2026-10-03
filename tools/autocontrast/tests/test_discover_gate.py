"""The fail-closed verification gate (G1-G6).

Auto-discovered records enter the store as immediately consensus-eligible
professional_render, so this gate is the ONLY barrier between a parser regression and a
poisoned reference set. Each rule is tested in isolation, not merely end-to-end.
"""

from __future__ import annotations

import pytest

from autocontrast.db.discover.discover import (
    RankedCandidate,
    position_tolerance_arcmin,
    verify_candidate,
)
from autocontrast.db.discover.gallery import GalleryEntry
from autocontrast.db.wcs import WcsResult

QUERY = {"query_ra_deg": 83.82, "query_dec_deg": -5.39, "query_radius_arcmin": 30.0}


def candidate(**overrides):
    defaults = dict(
        id="eso1103a", gallery="eso",
        detail_url="https://www.eso.org/public/images/eso1103a/",
        image_url="https://cdn.eso.org/images/large/eso1103a.jpg",
        ra_deg=83.82217, dec_deg=-5.39099, fov_w_arcmin=35.49, fov_h_arcmin=34.10,
        fov_radius_arcmin=24.61, width_px=8948, height_px=8597,
        pixel_scale_arcsec=0.238, object_name="M 42", category="Nebulae",
        entry_type="Observation", palette_class="RGB", license="CC BY 4.0",
        attribution="ESO/Igor Chekalin", published_utc="2011-01-19T12:00:00Z",
        parsed_ok=True,
    )
    defaults.update(overrides)
    entry = GalleryEntry(**defaults)
    return RankedCandidate(entry=entry, separation_arcmin=0.5, palette_compatible=True,
                           framing_ratio=0.2, centering=0.02)


def solver_returning(result):
    return lambda path: result


AVM_AGREEING = WcsResult(83.82217, -5.39099, 24.61, "avm", True, "WCS from embedded AVM tag",
                         pixel_scale_arcsec=0.238)
# The corrupt values pyavm's force-parse would yield for eso1103a.
AVM_CORRUPT = WcsResult(81.97, -3.74, 24.61, "avm", True, "WCS from embedded AVM tag",
                        pixel_scale_arcsec=0.238)
MANUAL = WcsResult(83.82217, -5.39099, 24.61, "manual", True, "WCS from manual annotation",
                   pixel_scale_arcsec=0.238)
UNSOLVED = WcsResult(None, None, None, "unsolved", False, "no tier produced a WCS")


def test_agreeing_avm_is_accepted(tmp_path):
    result = verify_candidate(candidate(), tmp_path / "img.jpg", **QUERY,
                              wcs_acquirer=solver_returning(AVM_AGREEING))
    assert result.accepted is True
    assert result.wcs.wcs_source == "avm"


def test_g2_rejects_a_corrupt_avm_that_disagrees_with_published_position(tmp_path):
    """REGRESSION: force-parsing eso1103a's AVM yields (81.97, -3.74) against its
    published (83.82, -5.39) — 2.3 degrees apart, roughly 5x tolerance. Two independent
    sources disagreeing means one is wrong and we cannot tell which, so reject rather
    than prefer either."""
    result = verify_candidate(candidate(), tmp_path / "img.jpg", **QUERY,
                              wcs_acquirer=solver_returning(AVM_CORRUPT))
    assert result.accepted is False
    assert "disagree" in result.reason.lower()


def test_g2_accepts_manual_fallback_but_marks_it_unverified(tmp_path):
    """eso1103a's AVM is unparseable, so acquire_wcs falls back to the published
    annotation. That is legitimate, but no independent cross-check happened."""
    result = verify_candidate(candidate(), tmp_path / "img.jpg", **QUERY,
                              wcs_acquirer=solver_returning(MANUAL))
    assert result.accepted is True
    assert result.wcs.wcs_source == "manual"


def test_g2_rejects_an_unsolved_image(tmp_path):
    result = verify_candidate(candidate(), tmp_path / "img.jpg", **QUERY,
                              wcs_acquirer=solver_returning(UNSOLVED))
    assert result.accepted is False
    assert "unsolved" in result.reason.lower() or "no wcs" in result.reason.lower()


def test_g3_rejects_when_the_solved_position_leaves_the_query_cone(tmp_path):
    """The solve is authoritative: if it lands outside the cone, the indexed position
    was wrong, whatever the index said.

    The candidate keeps its real indexed position so G1 PASSES — otherwise this would
    reject at G1 and never reach G3, which is a rule with real teeth: it is what catches
    an index row whose position belongs to a different object than the image does. The
    solve is 'blind' rather than 'avm' for the same reason: an avm source would trip
    G2's drift check first and report 'disagree' instead of 'cone'.
    """
    far = WcsResult(200.0, 40.0, 5.0, "blind", True, "blind solve landed elsewhere",
                    pixel_scale_arcsec=0.238)
    result = verify_candidate(candidate(), tmp_path / "img.jpg", **QUERY,
                              wcs_acquirer=solver_returning(far))
    assert result.accepted is False
    assert "cone" in result.reason.lower()


def test_g4_rejects_when_no_pixel_scale_can_be_established(tmp_path):
    """§2.2 forbids comparing scales in pixels, so a reference with no angular scale is
    unusable — band-limiting (§4.4) would be meaningless."""
    no_scale = WcsResult(83.82217, -5.39099, 24.61, "manual", True, "manual",
                         pixel_scale_arcsec=None)
    result = verify_candidate(candidate(pixel_scale_arcsec=None, fov_w_arcmin=None),
                              tmp_path / "img.jpg", **QUERY,
                              wcs_acquirer=solver_returning(no_scale))
    assert result.accepted is False
    assert "scale" in result.reason.lower()


def test_g4_uses_published_scale_when_the_solve_supplies_none(tmp_path):
    no_scale = WcsResult(83.82217, -5.39099, 24.61, "manual", True, "manual",
                         pixel_scale_arcsec=None)
    result = verify_candidate(candidate(), tmp_path / "img.jpg", **QUERY,
                              wcs_acquirer=solver_returning(no_scale))
    assert result.accepted is True
    assert result.pixel_scale_arcsec == pytest.approx(0.238, abs=1e-3)


def test_g4_prefers_the_solved_scale_over_the_published_one(tmp_path):
    """§2.2: a real solve must never be overridden by a nominal value."""
    solved = WcsResult(83.82217, -5.39099, 24.61, "avm", True, "avm",
                       pixel_scale_arcsec=0.240)
    result = verify_candidate(candidate(), tmp_path / "img.jpg", **QUERY,
                              wcs_acquirer=solver_returning(solved))
    assert result.pixel_scale_arcsec == pytest.approx(0.240, abs=1e-4)


def test_g5_rejects_a_missing_license(tmp_path):
    """A copyright-asserting credit leaves the license unestablished; ingesting it as
    CC BY 4.0 would attach a false license to the record forever (§5.5)."""
    result = verify_candidate(candidate(license=None), tmp_path / "img.jpg", **QUERY,
                              wcs_acquirer=solver_returning(AVM_AGREEING))
    assert result.accepted is False
    assert "license" in result.reason.lower()


def test_g5_rejects_a_missing_attribution(tmp_path):
    """The galleries state that crediting with the full line is mandatory; we cannot
    honor an obligation we did not capture."""
    result = verify_candidate(candidate(attribution=None), tmp_path / "img.jpg", **QUERY,
                              wcs_acquirer=solver_returning(AVM_AGREEING))
    assert result.accepted is False
    assert "attribution" in result.reason.lower()


def test_g1_rejects_a_candidate_whose_indexed_footprint_misses_the_cone(tmp_path):
    result = verify_candidate(candidate(ra_deg=200.0, dec_deg=40.0, fov_radius_arcmin=1.0),
                              tmp_path / "img.jpg", **QUERY,
                              wcs_acquirer=solver_returning(AVM_AGREEING))
    assert result.accepted is False


def test_tolerance_scales_with_footprint_but_has_a_floor():
    """A quarter of the footprint radius, never below one arcminute."""
    assert position_tolerance_arcmin(24.61) == pytest.approx(6.15, abs=0.01)
    assert position_tolerance_arcmin(0.5) == pytest.approx(1.0, abs=1e-9)


def test_tolerance_is_far_smaller_than_the_known_corruption():
    """The eso1103a force-parse error is ~140 arcmin against a ~6 arcmin tolerance, so
    the regression test above has a wide margin rather than a marginal pass."""
    from autocontrast.db.skymath import separation_arcmin
    error = separation_arcmin(83.82217, -5.39099, 81.97, -3.74)
    assert error > 10 * position_tolerance_arcmin(24.61)


def test_gate_never_raises_on_a_bad_candidate(tmp_path):
    """§12: degraded paths report, they do not except.

    Each candidate is paired with the solve that leaves it genuinely unusable. A
    scale-less entry is NOT unusable on its own — a solve that supplies a scale
    legitimately rescues it, which is what
    test_g4_uses_published_scale_when_the_solve_supplies_none asserts from the other
    side. Pairing it with an all-rescuing solver would assert the opposite of G4's
    stated policy.
    """
    no_scale_anywhere = WcsResult(83.82217, -5.39099, 24.61, "manual", True, "manual",
                                  pixel_scale_arcsec=None)
    cases = [
        (candidate(license=None), AVM_AGREEING),
        (candidate(attribution=None), AVM_AGREEING),
        (candidate(pixel_scale_arcsec=None, fov_w_arcmin=None), no_scale_anywhere),
        (candidate(ra_deg=None, dec_deg=None, fov_radius_arcmin=None), AVM_AGREEING),
        (candidate(), UNSOLVED),
    ]
    for bad, solve in cases:
        result = verify_candidate(bad, tmp_path / "img.jpg", **QUERY,
                                  wcs_acquirer=solver_returning(solve))
        assert result.accepted is False
        assert result.reason
