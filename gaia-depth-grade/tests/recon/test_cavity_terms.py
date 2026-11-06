import numpy as np

from gaia_depth_grade.recon.cavity import (
    emission_measure_relief,
    geometry_prior,
    ionization_depth,
)
from gaia_depth_grade.recon.model import IonizationMap


def test_geometry_prior_is_concave_about_apex():
    ny = nx = 101
    apex = (50.0, 50.0)  # (x=col, y=row)
    yy, xx = np.mgrid[0:ny, 0:nx]
    r = np.hypot(xx - apex[0], yy - apex[1])
    footprint = r <= 40.0

    z = geometry_prior((ny, nx), apex, footprint)

    assert z.shape == (ny, nx) and z.dtype == np.float32
    assert np.isfinite(z).all()
    assert 0.0 <= z.min() and z.max() <= 1.0

    # Depth convention: 0 = nearest observer, 1 = farthest. We look INTO the bowl,
    # so the apex/center sightline is the DEEPEST (largest z), and z decreases to
    # the rim. The apex is the global maximum within the footprint.
    assert z[50, 50] == z[footprint].max()
    assert z[50, 50] > z[footprint].mean()

    # Along a radius from the apex outward to the rim, depth is non-increasing...
    prof = z[50, 50:91]  # apex -> rim along +x
    assert np.all(np.diff(prof) <= 1e-6)

    # ...and the surface is concave (second difference negative).
    d2 = np.diff(prof.astype(np.float64), 2)
    assert np.median(d2) < 0.0

    # The rim is near the observer (small z); sky outside the footprint is 0.
    assert z[50, 90] < 0.1
    assert z[0, 0] == 0.0


def test_ionization_depth_places_oiii_forward():
    ny = nx = 16
    f = np.empty((ny, nx), np.float32)
    f[:, :8] = 0.9  # high ionization: interior, near the hot star
    f[:, 8:] = 0.1  # low ionization: outer skin / ionization front
    ion = IonizationMap(oiii_fraction=f, confidence=0.8)

    z = ionization_depth(ion)

    assert z.shape == (ny, nx) and z.dtype == np.float32
    assert np.isfinite(z).all()
    assert 0.0 <= z.min() and z.max() <= 1.0

    # High oiii_fraction -> SMALLER z (nearer/forward) than low oiii_fraction.
    assert z[:, :8].mean() < z[:, 8:].mean()


def test_emission_measure_relief_bulges_bright_edges():
    ny = nx = 64
    yy, xx = np.mgrid[0:ny, 0:nx]
    r = np.hypot(xx - 32, yy - 32)
    lum = np.zeros((ny, nx), np.float32)
    ring = (r >= 18) & (r < 20)  # a thin bright filament / limb-brightened rim
    lum[ring] = 1.0

    z = emission_measure_relief(lum)

    assert z.shape == (ny, nx) and z.dtype == np.float32
    assert np.isfinite(z).all()

    # Flat sky far from the rim carries no relief.
    assert abs(z[0, 0]) < 1e-3

    # The bright rim bulges along the LOS -> nonzero signed relief.
    assert np.abs(z[ring]).max() > 0.01

    # Signed relief centered on 0: both nearer and farther contributions appear.
    assert np.any(z > 0.0) and np.any(z < 0.0)


def test_emission_measure_relief_flat_sky_is_zero():
    # A flat, nonzero sky has no structure -> relief is ~0 everywhere (DC removed).
    lum = np.full((32, 32), 0.3, np.float32)
    z = emission_measure_relief(lum)
    assert np.isfinite(z).all()
    assert np.abs(z).max() < 1e-3
