"""Task 8: confidence-weighted blister CavityModel assembly."""

import numpy as np

from gaia_depth_grade.recon.cavity import reconstruct_cavity
from gaia_depth_grade.recon.ionization import ionization_ratio
from gaia_depth_grade.recon.model import (
    CavityModel,
    ChannelSet,
    IonizationMap,
    ReconConfig,
)


def _reconstruct(sb, confidence=None, with_sii=True):
    ch = sb.channels
    if not with_sii:
        ch = ChannelSet(ch.ha, ch.oiii, None, ch.ratio_confidence, ch.is_linear)
    ion = ionization_ratio(ch)
    if confidence is not None:
        ion = IonizationMap(ion.oiii_fraction, confidence)
    return reconstruct_cavity(
        ch, ion, sb.ionizer, sb.lum, sb.dust, sb.pc_per_px, ReconConfig()
    )


def _rim(truth):
    return truth["footprint"] & ~truth["ha_region"] & ~truth["oiii_region"]


def test_reconstructs_concave_cavity_with_apex(synthetic_blister):
    sb = synthetic_blister
    m = _reconstruct(sb)

    assert isinstance(m, CavityModel)
    assert m.z.shape == sb.lum.shape and m.z.dtype == np.float32
    assert m.emissivity.shape == (*sb.lum.shape, 3) and m.emissivity.dtype == np.float32
    assert np.isfinite(m.z).all()
    assert 0.0 <= m.z.min() and m.z.max() <= 1.0
    assert m.apex_xy == sb.truth["apex_xy"]
    assert m.pc_per_px == sb.pc_per_px

    # Deepest (max z) sightline is near the planted apex.
    foot = sb.truth["footprint"]
    zf = np.where(foot, m.z, -np.inf)
    iy, ix = np.unravel_index(int(np.argmax(zf)), zf.shape)
    ax, ay = sb.truth["apex_xy"]
    assert np.hypot(ix - ax, iy - ay) < 12.0

    # Concave, not flat: the deep front is clearly deeper than the rim.
    assert m.z[sb.truth["ha_region"]].mean() > m.z[_rim(sb.truth)].mean()


def test_oiii_interior_placed_forward_of_front(synthetic_blister):
    sb = synthetic_blister
    m = _reconstruct(sb)
    assert m.z[sb.truth["oiii_region"]].mean() < m.z[sb.truth["ha_region"]].mean()


def test_dust_lane_is_near_side(synthetic_blister):
    sb = synthetic_blister
    m = _reconstruct(sb)
    lane = sb.truth["dust_mask"]
    non_lane = sb.truth["footprint"] & ~lane
    assert m.z[lane].mean() < m.z[non_lane].mean()
    assert np.allclose(m.dust, sb.dust)


def test_low_ratio_confidence_still_concave(synthetic_blister):
    sb = synthetic_blister
    m = _reconstruct(sb, confidence=0.05)  # OSC-like low confidence

    foot = sb.truth["footprint"]
    zf = np.where(foot, m.z, -np.inf)
    iy, ix = np.unravel_index(int(np.argmax(zf)), zf.shape)
    ax, ay = sb.truth["apex_xy"]
    assert np.hypot(ix - ax, iy - ay) < 12.0

    # Geometry alone must still carry a concave bowl (deep front >> rim).
    margin = m.z[sb.truth["ha_region"]].mean() - m.z[_rim(sb.truth)].mean()
    assert margin > 0.05


def test_sii_none_supported(synthetic_blister):
    sb = synthetic_blister
    m = _reconstruct(sb, with_sii=False)
    assert isinstance(m, CavityModel)
    assert np.isfinite(m.z).all()
    assert m.z[sb.truth["ha_region"]].mean() > m.z[_rim(sb.truth)].mean()
    assert m.meta["sii_present"] is False
