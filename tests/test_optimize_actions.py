# tests/test_optimize_actions.py
import math
import pytest

from autocontrast.optimize.actions import (
    Action, available_actions, layer_for_scale, scale_for_layer,
)


def test_layer_and_scale_are_inverse():
    # energy.py fixes plane i at 2**i * pixel_scale arcsec.
    assert scale_for_layer(3, 1.0) == 8.0
    assert layer_for_scale(8.0, 1.0) == 3


def test_action_key_is_stable_and_distinguishing():
    a = Action(kind="local_contrast", level="moderate", scale_arcsec=8.0, params={})
    b = Action(kind="local_contrast", level="strong", scale_arcsec=8.0, params={})
    assert a.key == a.key
    assert a.key != b.key


def test_no_action_is_offered_below_the_psf_limit():
    # 1.0"/px with a 4.0" PSF: bands at 1" and 2" are unresolvable and must
    # never be proposed (SS2.2 -- chasing them sharpens noise into artifacts).
    actions = available_actions(
        pixel_scale_arcsec=1.0, psf_fwhm_arcsec=4.0, n_scales=7,
        palette_compatible=True, applied_kinds=frozenset(),
    )
    scaled = [a for a in actions if a.scale_arcsec is not None]
    assert scaled, "expected some scale-denominated actions"
    assert all(a.scale_arcsec >= 4.0 for a in scaled)


def test_chroma_actions_are_withheld_when_palette_is_incompatible():
    gated = available_actions(
        pixel_scale_arcsec=1.0, psf_fwhm_arcsec=2.0, n_scales=7,
        palette_compatible=False, applied_kinds=frozenset(),
    )
    assert not any(a.kind == "chroma" for a in gated)

    admitted = available_actions(
        pixel_scale_arcsec=1.0, psf_fwhm_arcsec=2.0, n_scales=7,
        palette_compatible=True, applied_kinds=frozenset(),
    )
    assert any(a.kind == "chroma" for a in admitted)


def test_once_only_actions_are_not_reoffered():
    once = available_actions(
        pixel_scale_arcsec=1.0, psf_fwhm_arcsec=2.0, n_scales=7,
        palette_compatible=True, applied_kinds=frozenset({"star_split", "background_neutralize"}),
    )
    assert not any(a.kind in ("star_split", "background_neutralize") for a in once)


def test_scale_below_pixel_scale_is_rejected_outright():
    with pytest.raises(ValueError):
        layer_for_scale(0.1, 1.0)
