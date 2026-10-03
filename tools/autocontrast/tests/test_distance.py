"""Tests for the fingerprint distance combination (§4.5).

These pin the *invariants* — especially the §2.3 palette gate — while leaving the
weighting philosophy (renormalize-on-mismatch or not) to the implementer.
"""

import pytest

from autocontrast.fingerprint.distance import (
    DistanceComponents,
    DistanceWeights,
    combine_distance,
)


def test_matched_palette_is_weighted_sum_of_all_components():
    comps = DistanceComponents(spectrum=0.4, tonal=0.2, chroma=0.6, background=0.1)
    w = DistanceWeights(spectrum=0.4, tonal=0.25, chroma=0.20, background=0.15)
    expected = 0.4 * 0.4 + 0.25 * 0.2 + 0.20 * 0.6 + 0.15 * 0.1
    assert combine_distance(comps, w, palette_match=True) == pytest.approx(expected)


def test_equal_components_and_match_returns_that_value():
    """Weights sum to 1, so four equal components collapse to their common value."""
    comps = DistanceComponents(spectrum=0.3, tonal=0.3, chroma=0.3, background=0.3)
    assert combine_distance(comps, palette_match=True) == pytest.approx(0.3)


def test_chroma_has_zero_influence_on_palette_mismatch():
    """§2.3 hard gate: a wildly different chroma must not change D when palettes
    do not match. This is the non-negotiable invariant."""
    low_chroma = DistanceComponents(spectrum=0.3, tonal=0.2, chroma=0.0, background=0.1)
    high_chroma = DistanceComponents(spectrum=0.3, tonal=0.2, chroma=1e6, background=0.1)

    d_low = combine_distance(low_chroma, palette_match=False)
    d_high = combine_distance(high_chroma, palette_match=False)

    assert d_low == pytest.approx(d_high)


def test_matched_and_mismatched_differ_when_chroma_nonzero():
    """Sanity: the gate actually does something — dropping a real chroma distance
    changes the score."""
    comps = DistanceComponents(spectrum=0.3, tonal=0.2, chroma=0.9, background=0.1)
    d_match = combine_distance(comps, palette_match=True)
    d_mismatch = combine_distance(comps, palette_match=False)
    assert d_match != pytest.approx(d_mismatch)


def test_distance_is_nonnegative():
    comps = DistanceComponents(spectrum=0.1, tonal=0.2, chroma=0.3, background=0.4)
    assert combine_distance(comps, palette_match=True) >= 0
    assert combine_distance(comps, palette_match=False) >= 0
