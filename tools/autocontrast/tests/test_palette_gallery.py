"""Palette derivation from gallery-published filter bands (§4.3)."""

from __future__ import annotations

import pytest

from autocontrast.fingerprint.palette import (
    PALETTE_CLASSES,
    palette_class_from_filters,
    palette_class_from_gallery_bands,
)


def test_absent_filter_table_is_unknown_not_rgb():
    """The critical asymmetry against palette_class_from_filters: an empty FITS FILTER
    keyword is POSITIVE evidence of a one-shot-colour camera, so that function returns
    RGB. An absent gallery filter table is merely missing metadata, so this one must
    return 'unknown'. Same empty input, opposite correct answers."""
    assert palette_class_from_gallery_bands([]) == "unknown"
    assert palette_class_from_filters([]) == "RGB"


def test_real_hubble_bands_are_rgb():
    """heic0601a: B, V, H-alpha, I, Z — broadband-dominated; catalog declares RGB."""
    assert palette_class_from_gallery_bands(["B", "V", "H-alpha", "I", "Z"]) == "RGB"


def test_real_eso_bands_are_rgb():
    """eso1103a: U, B, V, R, H-alpha."""
    assert palette_class_from_gallery_bands(["U", "B", "V", "R", "H-alpha"]) == "RGB"


def test_pure_narrowband_triplet_is_sho():
    assert palette_class_from_gallery_bands(["S-II", "H-alpha", "O-III"]) == "SHO"


def test_pure_narrowband_pair_is_hoo():
    assert palette_class_from_gallery_bands(["H-alpha", "O-III"]) == "HOO"


@pytest.mark.parametrize("alias", ["H-alpha", "Halpha", "Ha", "H-Alpha"])
def test_h_alpha_aliases_are_recognised(alias):
    assert palette_class_from_gallery_bands([alias, "O-III"]) == "HOO"


@pytest.mark.parametrize("alias", ["O-III", "OIII", "O3", "[O III]"])
def test_oiii_aliases_are_recognised(alias):
    assert palette_class_from_gallery_bands(["H-alpha", alias]) == "HOO"


def test_single_broadband_filter_is_not_a_colour_palette():
    """One filter cannot make a colour composite; claiming RGB would be a guess."""
    assert palette_class_from_gallery_bands(["V"]) == "unknown"


def test_single_narrowband_filter_is_unknown():
    assert palette_class_from_gallery_bands(["H-alpha"]) == "unknown"


def test_unrecognised_bands_are_unknown_never_guessed():
    """§2.1: a mis-derived palette lets a mismatched reference push channel ratios."""
    assert palette_class_from_gallery_bands(["Radio 21cm"]) == "unknown"
    assert palette_class_from_gallery_bands(["", "  "]) == "unknown"


def test_result_is_always_a_declared_palette_class():
    for bands in ([], ["V"], ["B", "V"], ["H-alpha", "O-III"], ["nonsense"]):
        assert palette_class_from_gallery_bands(bands) in PALETTE_CLASSES
