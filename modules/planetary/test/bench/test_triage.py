"""triage.py's functions that the README's triage tables rest on (final review, bench M4): the split-half
decomposition (gain / local shift / local blur), the sky-row banding covariance, and the belt / sensor-row
split of a row-structure profile. Synthetic pairs whose answer is known; no NAS, no probes."""
import numpy as np
import pytest
from scipy import ndimage

import triage
from conftest import lunar_scene


def _surface(seed=3):
    # A frame-filling lunar texture (the target box is then the frame less its inset).
    return lunar_scene(size=(256, 256), seed=seed, seeing=1.0)


def _smooth_field(shape, amplitude, period=128):
    """A field varying smoothly across the frame (period `period` px along y and x), as the engine's
    region blends make the halves' differences vary: sin^2 weights over windows 64 px apart. Smooth, so its
    own spectrum stays below the END band and only its product with A's detail reaches D there."""
    yy, xx = np.mgrid[0:shape[0], 0:shape[1]].astype(float)
    return amplitude * np.sin(2 * np.pi * yy / period) * np.cos(2 * np.pi * xx / period)


def test_decompose_reads_a_local_gain_difference_as_gain():
    """B = A with a gain that changes across the frame (kept subsets at different levels): the gain term
    explains most of D's END-band power, the shift term little."""
    A = _surface()
    B = A * (1 + _smooth_field(A.shape, 0.03))
    sh = triage.decompose(A, B)
    print(f"\nlocal gain: {sh}")
    assert sh["gain"] > 0.5 and sh["gain"] > 3 * sh["shift"]
    assert sh["all"] >= sh["gain"] - 1e-9
    # Why not after refined_difference's old global fit: before bench6 it read this field as a pseudo-shift,
    # (+0.10, -0.18) px (triage.py module doc), which the tiles' shift terms then took for a local shift. Since
    # Ruling F5 its shift is fitted jointly with a level field and reads the field as no shift.
    import halves
    _, _, _, coef, _ = halves.refined_difference(A, B, tile=None)
    print(f"refined_difference's global shift fit for the gain field: ({coef[1]:+.4f}, {coef[2]:+.4f}) px")
    assert abs(coef[1]) <= 0.005 and abs(coef[2]) <= 0.005


def test_decompose_reads_a_local_shift_as_shift():
    """B = A moved by a shift that changes across the frame (regions registered differently): the shift term
    explains most of D's END-band power, the gain term little."""
    A = _surface()
    dy = _smooth_field(A.shape, 0.15)
    yy, xx = np.mgrid[0:A.shape[0], 0:A.shape[1]].astype(float)
    B = ndimage.map_coordinates(A, [yy + dy, xx], order=3, mode="reflect")
    sh = triage.decompose(A, B)
    print(f"\nlocal shift: {sh}")
    assert sh["shift"] > 0.5 and sh["shift"] > 3 * sh["gain"]


def test_decompose_refuses_a_box_without_a_whole_tile():
    A = np.full((40, 40), 0.02)
    A[8:30, 8:30] = 0.5 + 0.01 * np.random.default_rng(1).standard_normal((22, 22))
    with pytest.raises(ValueError, match="holds no"):
        triage.decompose(A, A.copy(), tile=64)


def test_sky_banding_measures_a_shared_row_offset_and_cancels_white_noise():
    """Additive banding is constant along a sensor row, so the sky left and right of the disc share it:
    the covariance of their row means is the banding's variance; independent white noise gives ~0."""
    rng = np.random.default_rng(5)
    h, w = 120, 200
    yy, xx = np.mgrid[0:h, 0:w]
    mask = np.hypot(yy - 60, xx - 100) < 40
    noise = [rng.normal(0, 1.0, (h, w)) for _ in range(40)]
    cov0, n = triage.sky_banding(noise, mask)
    bands = [im + rng.normal(0, 0.5, (h, 1)) for im in noise]            # +0.5 sd row offsets, shared along each row
    cov1, _ = triage.sky_banding(bands, mask)
    print(f"\nwhite only: cov {cov0:.4f} over {n} rows; with 0.5 sd row offsets: {cov1:.4f}")
    assert n > 40
    assert abs(cov0) < 0.01
    assert cov1 == pytest.approx(0.25, rel=0.25)


def test_split_by_angle_separates_belt_aligned_from_sensor_rows():
    """A profile built as 2 x the target's own (belt-aligned, peaking at -9 deg) plus 1 x the same shape
    at 0 deg (sensor rows) is split back into those parts, with no residual."""
    a = triage.ANGLES
    belt = 0.2 + np.exp(-((a + 9) / 3.0) ** 2)
    row = np.interp(a + (-9), a, belt - 0.2)                    # the belt shape moved to 0 deg
    coef, res, peak, _ = triage.split_by_angle(belt, 2 * (belt - 0.2) + 1 * row + 0.2)
    print(f"\ncoef {coef}, residual {res:.3g}, target peak {peak}")
    assert peak == -9
    assert coef == pytest.approx([2.0, 1.0], abs=0.05)
    assert res < 0.02
