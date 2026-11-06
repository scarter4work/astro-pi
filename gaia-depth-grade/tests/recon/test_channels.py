import numpy as np
import pytest

from gaia_depth_grade.recon.channels import (
    extract_channels,
    from_duoband,
    from_mono_sho,
    from_osc_rgb,
    looks_linear,
)


def _linear_master(seed=0, n=128):
    """Sky-floored, right-skewed: faint noise floor plus sparse bright emission."""
    rng = np.random.default_rng(seed)
    img = 0.002 + 0.0005 * rng.standard_normal((n, n))
    yy, xx = np.mgrid[0:n, 0:n]
    for cy, cx, a in [(40, 40, 0.5), (90, 80, 0.3), (60, 100, 0.2)]:
        img += a * np.exp(-((yy - cy) ** 2 + (xx - cx) ** 2) / (2 * 8.0**2))
    return np.clip(img, 0, None).astype(np.float32)


def _stretch(img):
    x = img / img.max()
    return (np.arcsinh(1000 * x) / np.arcsinh(1000)).astype(np.float32)


def test_sho_passthrough_full_confidence():
    ha, oiii, sii = (_linear_master(s) for s in (1, 2, 3))
    cs = from_mono_sho(ha, oiii, sii)
    np.testing.assert_array_equal(cs.ha, ha)
    np.testing.assert_array_equal(cs.oiii, oiii)
    np.testing.assert_array_equal(cs.sii, sii)
    assert cs.ratio_confidence == 1.0
    assert cs.is_linear
    assert cs.ha.dtype == np.float32


def test_duoband_unmix_recovers_planted_lines():
    n = 64
    ha = np.zeros((n, n), np.float32)
    sii = np.zeros((n, n), np.float32)
    ha[:32, :32] = 0.8          # Ha-only quadrant
    sii[32:, 32:] = 0.5         # SII-only quadrant
    oiii = np.full((n, n), 0.3, np.float32)  # common O III background
    cs = from_duoband(ha + oiii, sii + oiii)
    assert cs.ratio_confidence == 0.6
    np.testing.assert_allclose(cs.oiii, oiii, rtol=0.1)
    np.testing.assert_allclose(cs.ha[:32, :32], 0.8, rtol=0.1)
    np.testing.assert_allclose(cs.sii[32:, 32:], 0.5, rtol=0.1)
    assert cs.ha[32:, 32:].max() < 0.03 and cs.sii[:32, :32].max() < 0.03
    assert cs.ha.dtype == cs.oiii.dtype == cs.sii.dtype == np.float32


def test_osc_proxy_maps_channels():
    rng = np.random.default_rng(5)
    rgb = rng.random((16, 20, 3)).astype(np.float32)
    cs = from_osc_rgb(rgb)
    np.testing.assert_allclose(cs.ha, rgb[..., 0])
    np.testing.assert_allclose(cs.oiii, 0.5 * (rgb[..., 1] + rgb[..., 2]), rtol=1e-6)
    assert cs.sii is None
    assert cs.ratio_confidence == 0.25
    assert cs.oiii.dtype == np.float32


def test_mismatched_shapes_raise():
    a = _linear_master(1, 64)
    b = _linear_master(2, 64)
    c = _linear_master(3, 48)
    with pytest.raises(ValueError):
        extract_channels({"ha": a, "oiii": b, "sii": c}, "sho")
    with pytest.raises(ValueError):
        extract_channels({"d1": a, "d2": c}, "duoband")


def test_looks_linear_discriminates():
    assert looks_linear(_linear_master())
    assert not looks_linear(_stretch(_linear_master()))


def test_stretched_input_zeros_ratio_confidence():
    s = [_stretch(_linear_master(k)) for k in (1, 2, 3)]
    cs = extract_channels({"ha": s[0], "oiii": s[1], "sii": s[2]}, "sho")
    assert cs.is_linear is False
    assert cs.ratio_confidence == 0.0


def test_linear_input_keeps_confidence_and_modes():
    m = [_linear_master(k) for k in (1, 2, 3)]
    assert extract_channels({"ha": m[0], "oiii": m[1], "sii": m[2]}, "sho").ratio_confidence == 1.0
    d = extract_channels({"d1": m[0], "d2": m[1]}, "duoband")
    assert d.is_linear and d.ratio_confidence == 0.6
    rgb = np.stack(m, axis=-1)
    o = extract_channels({"rgb": rgb}, "osc")
    assert o.sii is None and o.ratio_confidence == 0.25
    with pytest.raises(ValueError):
        extract_channels({"rgb": rgb}, "bogus")
