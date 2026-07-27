"""§2.3 chroma compatibility as a family relation (added after Task 4).

Palette equality was too strict: professional broadband composites blend narrowband
layers, so heic0601a (B, V, H-alpha, I, Z -> 'RGB') would deny chroma to an HaRGB
acquisition. Compatibility now groups palettes whose colour presentation is mutually
informative.
"""

from __future__ import annotations

import pytest

from autocontrast.fingerprint.palette import PALETTE_CLASSES, palette_chroma_compatible


@pytest.mark.parametrize("palette", ["RGB", "LRGB", "HaRGB", "HaOIII-RGB", "SHO", "HOO"])
def test_a_chroma_bearing_palette_is_compatible_with_itself(palette):
    assert palette_chroma_compatible(palette, palette) is True


@pytest.mark.parametrize(
    "a, b",
    [
        ("RGB", "HaRGB"),        # the motivating case: heic0601a vs an Ha+RGB acquisition
        ("HaRGB", "RGB"),        # symmetric
        ("RGB", "LRGB"),
        ("LRGB", "HaRGB"),
        ("RGB", "HaOIII-RGB"),
    ],
)
def test_broadband_palettes_share_chroma(a, b):
    """A blended narrowband layer does not change a broadband colour presentation."""
    assert palette_chroma_compatible(a, b) is True


@pytest.mark.parametrize(
    "a, b",
    [
        ("SHO", "HOO"),          # both narrowband, but gold/teal vs red/teal
        ("HOO", "SHO"),
        ("RGB", "SHO"),
        ("RGB", "HOO"),
        ("HOO", "LRGB"),
    ],
)
def test_narrowband_and_broadband_do_not_share_chroma(a, b):
    """§2.3's whole point: a mismatched palette must never push channel ratios (§2.1)."""
    assert palette_chroma_compatible(a, b) is False


def test_monochrome_is_never_chroma_compatible_even_with_itself():
    """An L-only image has no chroma, so chroma guidance is meaningless."""
    assert palette_chroma_compatible("L-only", "L-only") is False
    assert palette_chroma_compatible("L-only", "RGB") is False


def test_unknown_is_never_chroma_compatible_even_with_itself():
    """'unknown' means undetermined, not 'a palette that happens to match'."""
    assert palette_chroma_compatible("unknown", "unknown") is False
    assert palette_chroma_compatible("unknown", "RGB") is False
    assert palette_chroma_compatible("RGB", "unknown") is False


def test_relation_is_symmetric_across_every_declared_palette_pair():
    for a in PALETTE_CLASSES:
        for b in PALETTE_CLASSES:
            assert palette_chroma_compatible(a, b) is palette_chroma_compatible(b, a)


def test_unrecognised_palette_names_are_not_compatible():
    assert palette_chroma_compatible("nonsense", "nonsense") is False
    assert palette_chroma_compatible("", "RGB") is False
