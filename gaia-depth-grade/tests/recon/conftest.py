"""Shared fixtures for the nebula 3D depth reconstruction tests."""

from __future__ import annotations

from types import SimpleNamespace

import numpy as np
import pytest

from gaia_depth_grade.recon.channels import from_mono_sho
from gaia_depth_grade.recon.model import Ionizer


@pytest.fixture
def synthetic_blister():
    """A planted champagne-flow blister HII region with known ground truth.

    Physical layout (depth convention: 0 = nearest observer, 1 = farthest):
    - A concave ionization front (bowl) whose deepest point (the apex, max z)
      sits OFF-CENTER at ``apex_xy`` -- this is the ionizer (hot star) axis.
    - Halpha / [S II] trace that deep concave front: a bright blob AROUND the
      apex (large z, low ionization).
    - [O III] escapes forward through the open throat: a bright blob OFFSET from
      the apex toward the opening (small z, high ionization). Placing it disjoint
      from the apex is what lets both "apex deepest" and "O III forward of front"
      hold at once -- co-locating them is physically and mathematically
      contradictory under the Task-7 depth terms.
    - One dark near-side dust lane between the two blobs, over otherwise-deep gas.

    Returns a namespace with ``channels`` (SHO, linear, confidence 1.0),
    ``ionizer``, ``lum``, ``dust``, ``pc_per_px`` and a ``truth`` dict of masks.
    """
    ny = nx = 140
    ax, ay = 50.0, 86.0  # apex / ionizer, (x=col, y=row), off-center
    R = 50.0             # planted bowl / footprint radius about the apex
    ox, oy = 94.0, 54.0  # [O III] forward-throat lobe centre
    sigma = 13.0         # blob scale (tight enough to keep regions disjoint)

    yy, xx = np.mgrid[0:ny, 0:nx].astype(np.float64)
    r_apex = np.hypot(xx - ax, yy - ay)
    d_oiii = np.hypot(xx - ox, yy - oy)
    foot = r_apex <= R

    ha_blob = np.exp(-(r_apex**2) / (2.0 * sigma**2))   # bright at the deep apex
    oiii_blob = np.exp(-(d_oiii**2) / (2.0 * sigma**2))  # bright at forward throat

    # Faint diffuse nebular floor so the whole bowl is a solid footprint; added
    # equally to Ha and O III so it does not bias the ionization ratio.
    diffuse = 0.05
    ha = (ha_blob + diffuse) * foot
    oiii = (oiii_blob + diffuse) * foot
    sii = 0.5 * ha

    # Dark near-side dust lane: a stripe between the two blobs, over deep gas.
    lane = foot & (xx >= 70) & (xx <= 76) & (yy >= 60) & (yy <= 96)
    atten = np.where(lane, 0.55, 1.0)  # the lane absorbs -> dimmer in the image
    ha = (ha * atten).astype(np.float32)
    oiii = (oiii * atten).astype(np.float32)
    sii = (sii * atten).astype(np.float32)

    dust = np.where(lane, 0.9, 0.0).astype(np.float32)
    lum = (ha + oiii + sii).astype(np.float32)

    channels = from_mono_sho(ha, oiii, sii)  # confidence 1.0, is_linear True
    ionizer = Ionizer(x=ax, y=ay, distance_pc=950.0, source="gaia-blue")

    oiii_norm = oiii_blob / float(oiii_blob.max())
    ha_norm = ha_blob / float(ha_blob.max())
    oiii_region = foot & (oiii_norm > 0.4) & (ha_norm < 0.2)
    ha_region = foot & (ha_norm > 0.4) & (oiii_norm < 0.2)

    truth = {
        "apex_xy": (ax, ay),
        "R": R,
        "footprint": foot,
        "oiii_region": oiii_region,  # high ionization, forward (small z)
        "ha_region": ha_region,      # low ionization, deep front (large z)
        "dust_mask": lane,           # near-side dust (small z)
        "concave_apex_is_max_z": True,
        "oiii_forward_of_front": True,
    }
    return SimpleNamespace(
        channels=channels,
        ionizer=ionizer,
        lum=lum,
        dust=dust,
        pc_per_px=0.015,
        truth=truth,
    )
