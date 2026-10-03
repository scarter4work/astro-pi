"""Shared spherical helpers (§5.1 cone-search geometry)."""

from __future__ import annotations

import pytest

from autocontrast.db.skymath import cones_overlap, separation_arcmin


def test_zero_separation_for_identical_positions():
    assert separation_arcmin(83.82, -5.39, 83.82, -5.39) == pytest.approx(0.0, abs=1e-9)


def test_one_degree_of_declination_is_sixty_arcmin():
    assert separation_arcmin(10.0, 0.0, 10.0, 1.0) == pytest.approx(60.0, rel=1e-9)


def test_ra_separation_shrinks_with_cos_dec():
    """One degree of RA subtends less angle away from the equator — the cos(dec)
    factor is exactly what a naive coordinate difference gets wrong."""
    at_equator = separation_arcmin(0.0, 0.0, 1.0, 0.0)
    at_sixty = separation_arcmin(0.0, 60.0, 1.0, 60.0)
    assert at_equator == pytest.approx(60.0, rel=1e-6)
    assert at_sixty == pytest.approx(30.0, rel=1e-3)


def test_ra_wrap_across_zero_is_short_way_round():
    """359.5 -> 0.5 is one degree apart, not 359."""
    assert separation_arcmin(359.5, 0.0, 0.5, 0.0) == pytest.approx(60.0, rel=1e-6)


def test_matches_the_two_real_m42_references():
    """heic0601a vs eso1103a, from data/seed_catalog.json."""
    sep = separation_arcmin(83.7905, -5.4140, 83.82217, -5.39099)
    assert sep == pytest.approx(2.34, abs=0.05)


def test_cones_overlap_at_and_beyond_the_boundary():
    assert cones_overlap(10.0, 4.0, 6.0) is True     # exactly touching
    assert cones_overlap(10.001, 4.0, 6.0) is False   # just past
    assert cones_overlap(0.0, 1.0, 1.0) is True
