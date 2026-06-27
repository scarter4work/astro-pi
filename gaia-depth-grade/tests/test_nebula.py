import numpy as np
from astropy.table import Table

from gaia_depth_grade.nebula import (
    gaia_depth_budget,
    render_nebula,
    structural_depth,
)


def _nebula_with_dust_lane():
    """A diffuse glow (bright) with a dark dust lane (near) and empty sky border."""
    h = w = 120
    yy, xx = np.mgrid[0:h, 0:w]
    glow = 0.4 * np.exp(-((xx - 60) ** 2 + (yy - 60) ** 2) / (2 * 40.0**2))
    lane = 0.25 * np.exp(-((xx - 60) ** 2) / (2 * 4.0**2))  # vertical dark stripe
    img = np.clip(glow - lane, 0.0, 1.0)
    img[:10] = 0.01  # sky band (top)
    return img


def _table(r):
    t = Table()
    t["r_med_geo"] = np.asarray(r, float)
    return t


def test_budget_scales_with_distance_spread():
    near = _table(np.full(100, 300.0))            # no spread
    spread = _table(np.geomspace(100.0, 5000.0, 100))
    assert gaia_depth_budget(near["r_med_geo"]) < 0.05
    assert gaia_depth_budget(spread["r_med_geo"]) > 0.5


def test_budget_zero_when_too_few_matches():
    assert gaia_depth_budget(_table(np.full(5, 300.0))["r_med_geo"]) == 0.0


def test_structural_field_dust_is_near_sky_is_neutral():
    img = _nebula_with_dust_lane()
    s = structural_depth(img)
    lane = s[60, 58:62].mean()      # over the dark dust stripe
    glow = s[60, 100:110].mean()    # over diffuse glow, away from lane
    sky = np.abs(s[:8]).mean()      # sky band
    assert lane > glow              # dust comes forward (more +near) than glow
    assert sky < 0.05               # empty sky not graded


def test_zero_strength_is_identity():
    img = _nebula_with_dust_lane()[..., None].repeat(3, axis=2)
    out = render_nebula(img, _table(np.geomspace(100, 5000, 100)), strength=0.0)
    assert np.allclose(out, img)


def test_dust_lane_brightened_glow_recessed():
    img = _nebula_with_dust_lane()[..., None].repeat(3, axis=2)
    out = render_nebula(img, _table(np.geomspace(100, 5000, 100)),
                        strength=1.0, atmos=1.0, structure=1.0)
    lane_before = img[60, 58:62].mean()
    lane_after = out[60, 58:62].mean()
    glow_before = img[60, 100:110].mean()
    glow_after = out[60, 100:110].mean()
    assert lane_after > lane_before        # near dust forward (brighter)
    assert glow_after <= glow_before + 1e-6  # diffuse glow recessed (not brighter)
