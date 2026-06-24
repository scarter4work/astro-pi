from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from astropy.coordinates import SkyCoord
from astropy.wcs import WCS


@dataclass(frozen=True)
class FieldFootprint:
    center_ra: float
    center_dec: float
    radius_deg: float


def load_wcs(header) -> WCS:
    w = WCS(header)
    if not w.has_celestial:
        raise ValueError("FITS header has no usable WCS")
    return w.celestial


def field_footprint(wcs: WCS, shape: tuple[int, int]) -> FieldFootprint:
    ny, nx = shape
    cx, cy = (nx - 1) / 2.0, (ny - 1) / 2.0
    center = wcs.pixel_to_world(cx, cy)
    corners_x = [0, nx - 1, 0, nx - 1]
    corners_y = [0, 0, ny - 1, ny - 1]
    corners = wcs.pixel_to_world(corners_x, corners_y)
    sep = center.separation(corners).deg
    return FieldFootprint(
        center_ra=float(center.ra.deg),
        center_dec=float(center.dec.deg),
        radius_deg=float(np.max(sep)),
    )
