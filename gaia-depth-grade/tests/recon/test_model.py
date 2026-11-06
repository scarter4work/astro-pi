import numpy as np
import pytest

from gaia_depth_grade.recon.model import CavityModel, ChannelSet


def test_channelset_rejects_shape_mismatch():
    a = np.zeros((4, 4), np.float32)
    b = np.zeros((4, 5), np.float32)
    with pytest.raises(ValueError):
        ChannelSet(ha=a, oiii=b, sii=None, ratio_confidence=1.0, is_linear=True)


def test_cavitymodel_fields_roundtrip():
    ny, nx = 6, 8
    z = np.zeros((ny, nx), np.float32)
    emissivity = np.zeros((ny, nx, 3), np.float32)
    dust = np.zeros((ny, nx), np.float32)
    model = CavityModel(
        z=z,
        emissivity=emissivity,
        dust=dust,
        pc_per_px=2.5,
        apex_xy=(3.0, 4.0),
        meta={"source": "test"},
    )
    assert model.z.shape == (ny, nx)
    assert model.emissivity.shape == (ny, nx, 3)
    assert model.dust.shape == (ny, nx)
    assert model.pc_per_px == 2.5
    assert model.apex_xy == (3.0, 4.0)
    assert model.meta == {"source": "test"}


def test_cavitymodel_rejects_emissivity_shape_mismatch():
    ny, nx = 6, 8
    with pytest.raises(ValueError):
        CavityModel(
            z=np.zeros((ny, nx), np.float32),
            emissivity=np.zeros((ny, nx + 1, 3), np.float32),
            dust=np.zeros((ny, nx), np.float32),
            pc_per_px=None,
            apex_xy=(0.0, 0.0),
            meta={},
        )
