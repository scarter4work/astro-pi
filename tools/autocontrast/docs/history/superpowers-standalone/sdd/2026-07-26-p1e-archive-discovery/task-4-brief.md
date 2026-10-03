### Task 4: Palette class from gallery filter bands

**Files:**
- Modify: `src/autocontrast/fingerprint/palette.py`
- Test: `tests/test_palette_gallery.py`

**Interfaces:**
- Consumes: nothing (pure).
- Produces: `palette_class_from_gallery_bands(bands: list[str]) -> str`, returning a member
  of the existing `PALETTE_CLASSES`.

**AUTHOR DECISION.** The classification body encodes a domain judgment: how a professional
multi-filter composite maps onto the amateur palette taxonomy. The default below treats a
broadband-dominated composite as `RGB` even when a narrowband layer is blended in, because
that matches how the curated catalog labels `heic0601a` (B, V, **H-alpha**, I, Z → `RGB`).
The alternative — calling it `HaRGB` — is defensible too, and would change which references
pass the §2.3 chroma gate. Adjust the rules if you disagree; the tests encode the default.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_palette_gallery.py
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
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_palette_gallery.py -v`
Expected: FAIL — `ImportError: cannot import name 'palette_class_from_gallery_bands'`

- [ ] **Step 3: Write the implementation**

Append to `src/autocontrast/fingerprint/palette.py`:

```python
# Professional galleries publish photometric band letters and emission-line names rather
# than amateur filter-wheel labels, so they need their own mapping. Broadband letters
# span UV through near-IR; all of them contribute to a broadband colour composite.
_GALLERY_BROADBAND = {
    "U", "B", "V", "R", "I", "Z", "Y", "G", "J", "H", "K", "W", "L", "M",
}

_GALLERY_NARROWBAND = {
    "HA": "Ha", "H-ALPHA": "Ha", "HALPHA": "Ha", "H_ALPHA": "Ha",
    "OIII": "OIII", "O-III": "OIII", "O3": "OIII", "[O III]": "OIII", "[OIII]": "OIII",
    "SII": "SII", "S-II": "SII", "S2": "SII", "[S II]": "SII", "[SII]": "SII",
}


def palette_class_from_gallery_bands(bands: list[str]) -> str:
    """Derive a §4.3 palette class from a gallery's published filter bands.

    Note the asymmetry against :func:`palette_class_from_filters`: an *empty* FITS FILTER
    keyword is positive evidence of a one-shot-colour camera and yields ``RGB``, whereas
    an empty gallery filter table is simply absent metadata and must yield ``unknown``.
    Guessing here would let a mismatched reference push channel ratios (§2.1).

    A broadband-dominated composite is classified ``RGB`` even when a narrowband layer is
    blended in, matching how the curated catalog labels heic0601a (B, V, H-alpha, I, Z).
    """
    if not bands:
        return "unknown"

    broad: set[str] = set()
    narrow: set[str] = set()
    for raw in bands:
        key = str(raw).strip().upper()
        if not key:
            continue
        if key in _GALLERY_NARROWBAND:
            narrow.add(_GALLERY_NARROWBAND[key])
        elif key in _GALLERY_BROADBAND:
            broad.add(key)

    # Two or more broadband filters make a colour composite; incidental narrowband
    # blending does not change the presentation palette.
    if len(broad) >= 2:
        return "RGB"
    if not broad:
        if narrow == {"SII", "Ha", "OIII"}:
            return "SHO"
        if narrow == {"Ha", "OIII"}:
            return "HOO"
    return "unknown"
```

- [ ] **Step 4: Run test to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_palette_gallery.py -v`
Expected: PASS, 18 tests.

- [ ] **Step 5: Confirm the existing palette tests still pass unchanged**

Run: `.venv/bin/python -m pytest tests/test_palette.py -q`
Expected: PASS — the new function must not alter `palette_class_from_filters`.

- [ ] **Step 6: Commit**

```bash
git add src/autocontrast/fingerprint/palette.py tests/test_palette_gallery.py
git commit -m "§4.3: derive palette class from gallery-published filter bands

Both galleries publish a Colours & filters table (heic0601a: B 435, V 555,
H-alpha 658, I 775, Z 850), so a discovered reference's palette is DERIVED, never
guessed or defaulted to RGB.

Key asymmetry, encoded in a test: an empty FITS FILTER keyword means a one-shot-colour
camera and correctly yields RGB, but an absent gallery filter table is just missing
metadata and must yield 'unknown'. Same empty input, opposite right answers, so the
new function does not delegate the empty case to the old one.

Broadband-dominated composites classify as RGB even with a blended narrowband layer,
matching the curated catalog's own label for heic0601a."
```

---

