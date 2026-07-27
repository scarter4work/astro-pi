import numpy as np
import pytest

from autocontrast.optimize.actions import Action
from autocontrast.optimize.executors.numpy_exec import NumpyExecutor


def _img(h=96, w=96):
    y, x = np.mgrid[0:h, 0:w]
    base = 0.35 + 0.15 * np.sin(x / 8.0) * np.cos(y / 11.0)
    return np.clip(np.stack([base, base * 0.9, base * 1.1], axis=-1), 0.0, 1.0)


@pytest.mark.parametrize("kind,level,scale", [
    ("local_contrast", "moderate", 8.0),
    ("local_equalize", "gentle", 8.0),
    ("core_hdr", "moderate", None),
    ("tonal_reshape", "strong", None),
    ("black_point", "gentle", None),
    ("chroma", "moderate", None),
    ("background_neutralize", "", None),
])
def test_every_action_kind_is_executable_and_stays_in_range(kind, level, scale):
    ex = NumpyExecutor()
    src = _img()
    out = ex.apply(src, Action(kind, level, scale, {"layer": 3, "radius_arcsec": 8.0}),
                   pixel_scale_arcsec=1.0)
    assert out.shape == src.shape
    assert np.all(np.isfinite(out))
    assert out.min() >= 0.0 and out.max() <= 1.0


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
