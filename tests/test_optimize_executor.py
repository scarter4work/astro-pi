import numpy as np
import pytest

from autocontrast.optimize.actions import Action
from autocontrast.optimize.executors.numpy_exec import NumpyExecutor


def _img(h=96, w=96):
    y, x = np.mgrid[0:h, 0:w]
    base = 0.35 + 0.15 * np.sin(x / 8.0) * np.cos(y / 11.0)
    return np.clip(np.stack([base, base * 0.9, base * 1.1], axis=-1), 0.0, 1.0)


@pytest.mark.parametrize("kind,level,scale,expect_change", [
    ("local_contrast", "moderate", 8.0, True),
    ("local_equalize", "gentle", 8.0, True),
    ("core_hdr", "moderate", None, True),
    ("tonal_reshape", "strong", None, True),
    ("black_point", "gentle", None, True),
    ("chroma", "moderate", None, True),
    ("background_neutralize", "", None, True),
    # star_split is a MODE change (SS6.2), not a pixel operation: the numpy
    # executor is a deliberate no-op here, and that's the behaviour under test.
    ("star_split", "", None, False),
])
def test_every_action_kind_is_executable_and_stays_in_range(kind, level, scale, expect_change):
    ex = NumpyExecutor()
    src = _img()
    out = ex.apply(src, Action(kind, level, scale, {"layer": 3, "radius_arcsec": 8.0}),
                   pixel_scale_arcsec=1.0)
    assert out.shape == src.shape
    assert np.all(np.isfinite(out))
    assert out.min() >= 0.0 and out.max() <= 1.0

    diff = float(np.abs(out - src).mean())
    if expect_change:
        # A branch that silently degraded to a passthrough would satisfy every
        # assertion above; this is the one that actually catches it.
        assert diff > 1e-6, f"{kind} did not visibly change the image (mean abs diff {diff:.3e})"
    else:
        assert np.array_equal(out, src), f"{kind} is a mode change and must be a pixel no-op"


def test_executor_does_not_mutate_its_input():
    ex, src = NumpyExecutor(), _img()
    before = src.copy()
    ex.apply(src, Action("tonal_reshape", "strong", None, {}), pixel_scale_arcsec=1.0)
    assert np.array_equal(src, before)


def test_stronger_local_contrast_moves_the_image_further():
    ex, src = NumpyExecutor(), _img()
    gentle = ex.apply(src, Action("local_contrast", "gentle", 8.0, {"layer": 3}),
                      pixel_scale_arcsec=1.0)
    strong = ex.apply(src, Action("local_contrast", "strong", 8.0, {"layer": 3}),
                      pixel_scale_arcsec=1.0)
    assert np.abs(strong - src).mean() > np.abs(gentle - src).mean()


def test_unknown_action_kind_is_a_loud_error():
    with pytest.raises(ValueError, match="unknown action"):
        NumpyExecutor().apply(_img(), Action("teleport", "strong", None, {}),
                              pixel_scale_arcsec=1.0)


def test_local_contrast_rejects_a_layer_beyond_what_the_image_supports():
    # A 96x96 image supports up to layer floor(log2(96)) == 6; 7 is one past the edge.
    ex, src = NumpyExecutor(), _img(h=96, w=96)
    with pytest.raises(ValueError, match="exceeds what a 96x96 image supports"):
        ex.apply(src, Action("local_contrast", "moderate", 8.0, {"layer": 7}),
                 pixel_scale_arcsec=1.0)


def test_local_contrast_accepts_a_layer_at_the_supported_boundary():
    # layer == max_layer is the last legitimate value; it must still execute normally.
    ex, src = NumpyExecutor(), _img(h=96, w=96)
    out = ex.apply(src, Action("local_contrast", "moderate", 8.0, {"layer": 6}),
                   pixel_scale_arcsec=1.0)
    assert out.shape == src.shape
    assert np.all(np.isfinite(out))
    assert not np.array_equal(out, src)
