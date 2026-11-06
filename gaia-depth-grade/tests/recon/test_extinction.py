import numpy as np

from gaia_depth_grade.recon.extinction import dust_from_balmer, dust_from_structure


def _lane_field():
    ny = nx = 96
    lum = np.full((ny, nx), 0.01, np.float32)      # sky floor
    lum[:, 20:] = 1.0                              # bright emission field
    lum[:, 20:] += 0.02 * np.sin(np.arange(nx - 20))[None, :]
    lum[30:66, 50:56] = 0.1                        # dark lane carved into the bright field
    return lum


def test_dark_lane_reads_high_opacity():
    lum = _lane_field()
    dust = dust_from_structure(lum)
    assert dust.shape == lum.shape and dust.dtype == np.float32
    assert dust.min() >= 0.0 and dust.max() <= 1.0
    assert dust[40:56, 51:55].mean() > 0.6          # lane embedded in bright surroundings
    assert dust[:20, :10].mean() < 0.02             # plain sky corner, equally dark, is not dust
    assert dust[:, 0:12].max() < 0.05               # nor is the sky/field boundary region
    assert dust[10:20, 70:90].mean() < 0.05         # bright field itself


def test_dust_from_structure_flat_sky_is_zero():
    assert dust_from_structure(np.full((32, 32), 0.01, np.float32)).max() < 1e-3


def test_balmer_decrement_tracks_extinction():
    hb = np.full((32, 32), 1.0, np.float32)
    ha = np.full((32, 32), 2.86, np.float32)        # Case B: no extinction
    ha[:, 16:] = 2.86 * 3.0                         # reddened half
    d = dust_from_balmer(ha, hb)
    assert d.dtype == np.float32 and d.min() >= 0.0 and d.max() <= 1.0
    assert d[:, :16].max() < 0.01
    assert d[:, 16:].mean() > 0.3
    ha2 = ha.copy(); ha2[:, 16:] = 2.0              # below Case B -> ~0
    assert dust_from_balmer(ha2, hb).max() < 0.01
