"""Shared pytest fixtures for the PlanetaryStack benchmark harness (test/bench).

Synthetic scenes are deterministic (fixed seeds). Real-data tests read the NAS read-only and are
skipped, with the reason printed, when /mnt/qnap/astro_data is not mounted.
"""
import os
import sys

import numpy as np
import pytest
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

NAS = os.environ.get("PLANETARY_BENCH_NAS", "/mnt/qnap/astro_data")


def nas_path(*parts):
    """Absolute path under the NAS root; the test is skipped if the file is not there."""
    p = os.path.join(NAS, *parts)
    if not os.path.exists(p):
        pytest.skip(f"NAS file not available: {p}")
    return p


def fractal_field(shape, seed, slope=1.6):
    """Zero-mean, unit-RMS random texture with an amplitude spectrum ~ 1/f**slope."""
    rng = np.random.default_rng(seed)
    h, w = shape
    fy = np.fft.fftfreq(h)[:, None]
    fx = np.fft.rfftfreq(w)[None, :]
    f = np.hypot(fy, fx)
    f[0, 0] = 1.0
    amp = f ** (-slope)
    amp[0, 0] = 0.0
    phase = rng.uniform(0, 2 * np.pi, amp.shape)
    field = np.fft.irfft2(amp * np.exp(1j * phase), s=shape)
    field -= field.mean()
    return field / field.std()


def planet_scene(size=(240, 320), radius=70, seed=1, seeing=1.0, texture=0.25):
    """A textured, limb-darkened disc on dark sky, blurred by `seeing` (Gaussian sigma, px).

    A faint 'moon' sits far from the disc so target-box tests can check it is left out.
    Values are in [0, ~1] before any noise.
    """
    h, w = size
    yy, xx = np.mgrid[0:h, 0:w].astype(float)
    cy, cx = h / 2 + 3.3, w / 2 - 7.6
    r = np.hypot(yy - cy, xx - cx) / radius
    mu = np.sqrt(np.clip(1 - r ** 2, 0, 1))
    disc = (r < 1) * (0.35 + 0.65 * mu)
    tex = 1 + texture * fractal_field(size, seed)
    img = 0.02 + 0.8 * disc * tex
    img[(np.hypot(yy - 25, xx - 25) < 2.5)] += 0.3  # small moon, far from the disc
    return ndimage.gaussian_filter(img, seeing) if seeing > 0 else img


def lunar_scene(size=(256, 384), seed=2, seeing=1.0):
    """Frame-filling lunar-like surface: large-scale albedo plus crater-scale texture."""
    base = 0.45 + 0.12 * ndimage.gaussian_filter(fractal_field(size, seed + 100, 1.2), 20) * 8
    tex = 0.10 * fractal_field(size, seed, 1.5)
    img = np.clip(base + tex, 0.02, None)
    return ndimage.gaussian_filter(img, seeing) if seeing > 0 else img


def add_noise(img, sigma, seed):
    return img + np.random.default_rng(seed).normal(0.0, sigma, img.shape)


@pytest.fixture
def planet():
    return add_noise(planet_scene(), 0.002, 11)


@pytest.fixture
def lunar():
    return add_noise(lunar_scene(), 0.002, 12)
