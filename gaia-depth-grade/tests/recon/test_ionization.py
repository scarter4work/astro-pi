import numpy as np

from gaia_depth_grade.recon.ionization import ionization_ratio
from gaia_depth_grade.recon.model import ChannelSet


def _disc_channels(sii=None, conf=0.8):
    ny = nx = 64
    yy, xx = np.mgrid[0:ny, 0:nx]
    r = np.hypot(yy - 32, xx - 32)
    core = r < 10
    ring = (r >= 18) & (r < 26)
    ha = np.full((ny, nx), 0.01, np.float32)
    oiii = np.full((ny, nx), 0.01, np.float32)
    oiii[core] = 1.0
    ha[core] = 0.1
    ha[ring] = 1.0
    oiii[ring] = 0.05
    return ChannelSet(ha=ha, oiii=oiii, sii=sii, ratio_confidence=conf, is_linear=True), core, ring


def test_oiii_bright_core_reads_high_fraction():
    ch, core, _ = _disc_channels()
    m = ionization_ratio(ch)
    assert m.oiii_fraction[32, 32] > 0.7
    assert m.oiii_fraction[core].mean() > 0.7


def test_ha_dominant_edge_reads_low_fraction():
    ch, _, ring = _disc_channels()
    m = ionization_ratio(ch)
    assert m.oiii_fraction[ring].mean() < 0.3


def test_handles_sii_none():
    ch, _, _ = _disc_channels(sii=None)
    m = ionization_ratio(ch)
    f = m.oiii_fraction
    assert f.shape == (64, 64) and f.dtype == np.float32
    assert np.isfinite(f).all() and f.min() >= 0.0 and f.max() <= 1.0


def test_handles_sii_present():
    sii = np.full((64, 64), 0.3, np.float32)
    ch, _, _ = _disc_channels(sii=sii)
    f = ionization_ratio(ch).oiii_fraction
    assert np.isfinite(f).all() and f.min() >= 0.0 and f.max() <= 1.0


def test_confidence_propagates():
    ch, _, _ = _disc_channels(conf=0.37)
    assert ionization_ratio(ch).confidence == 0.37


def test_zero_signal_is_finite():
    z = np.zeros((16, 16), np.float32)
    f = ionization_ratio(ChannelSet(z, z, None, 0.5, True)).oiii_fraction
    assert np.isfinite(f).all() and (f == 0).all()
