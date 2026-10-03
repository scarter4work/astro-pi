"""Tests for palette-class derivation from FITS FILTER keywords (§4.3).

"On the user's side, palette class is auto-derived from FITS headers — the FILTER
keyword written by the ZWO EFW makes this deterministic. Never prompt for what the
headers already state." (§4.3)
"""

import pytest

from autocontrast.fingerprint.palette import PALETTE_CLASSES, palette_class_from_filters


def test_dual_band_ha_oiii_is_hoo():
    """The user's own rig writes FILTER='HaO3' (an Ha+OIII dual-band filter)."""
    assert palette_class_from_filters(["HaO3"]) == "HOO"


@pytest.mark.parametrize("name", ["HaOIII", "L-eXtreme", "L-Ultimate", "Duo-Band"])
def test_other_dual_band_filter_names_are_hoo(name):
    assert palette_class_from_filters([name]) == "HOO"


def test_separate_ha_and_oiii_is_hoo():
    assert palette_class_from_filters(["Ha", "OIII"]) == "HOO"


def test_sii_ha_oiii_is_sho():
    assert palette_class_from_filters(["SII", "Ha", "OIII"]) == "SHO"


def test_rgb():
    assert palette_class_from_filters(["R", "G", "B"]) == "RGB"


def test_lrgb():
    assert palette_class_from_filters(["L", "R", "G", "B"]) == "LRGB"


def test_ha_plus_rgb_is_hargb():
    assert palette_class_from_filters(["Ha", "R", "G", "B"]) == "HaRGB"


def test_luminance_only():
    assert palette_class_from_filters(["L"]) == "L-only"


def test_no_filter_on_a_colour_camera_is_rgb():
    """An OSC/DSLR shot with no filter wheel writes no FILTER keyword."""
    assert palette_class_from_filters([]) == "RGB"


def test_unrecognised_filter_is_unknown_not_a_guess():
    """§12: never silently guess. An unknown filter must surface as 'unknown', which
    (per §2.3) contributes structural+tonal only — never chroma."""
    assert palette_class_from_filters(["Wratten25"]) == "unknown"


def test_derivation_is_case_and_whitespace_insensitive():
    assert palette_class_from_filters([" hao3 "]) == "HOO"


def test_all_results_are_valid_palette_classes():
    for filters in (["HaO3"], ["R", "G", "B"], ["L"], ["nonsense"]):
        assert palette_class_from_filters(filters) in PALETTE_CLASSES
