"""bayer.py: the CFA pattern of a capture, measured on its raw frames in the reader's layout."""
import numpy as np
import pytest

import bayer
from conftest import nas_path, planet_scene

GAINS = {"R": 1.0, "G": 0.8, "B": 0.45}      # a warm target (Jupiter, Mars, the Moon) on a CMOS CFA


def mosaic(scene, pattern):
    out = np.empty_like(scene)
    for k, (dy, dx) in enumerate(((0, 0), (0, 1), (1, 0), (1, 1))):
        out[dy::2, dx::2] = scene[dy::2, dx::2] * GAINS[pattern[k]]
    return out


@pytest.mark.parametrize("pattern", ["RGGB", "GRBG", "GBRG", "BGGR"])
def test_pattern_of_a_synthetic_warm_disc(pattern):
    rng = np.random.default_rng(1)
    frames = np.stack([np.clip(mosaic(planet_scene(seed=1) * 230, pattern) + rng.normal(0, 2, (240, 320)), 0, 255)
                       for _ in range(5)]).astype(np.uint8)
    m = bayer.measure_frames(frames)
    assert m.pattern == pattern
    assert m.green_split < m.red_blue_split


def test_a_saturated_disc_is_unmeasurable():
    frames = np.full((3, 64, 64), 3, np.uint8)
    frames[:, 20:44, 20:44] = 255
    with pytest.raises(bayer.Unmeasurable):
        bayer.measure_frames(frames)


@pytest.mark.nas
@pytest.mark.ffmpeg
def test_real_capture_agrees_with_the_readers_sidecar_resolution():
    """3_6_2023 Moon: ASIAIR sidecar Bayer = RG with Flip=Vert, which the reader resolves to BGGR
    (Ruling 10); the frames measure BGGR, red/blue far apart."""
    p = nas_path("3_6_2023", "moon", "2023-03-05-224808-Moon-Bin1 18.3F.avi")
    m = bayer.measure_video(p, (1920, 1080))
    print(f"\n{m}")
    assert m.pattern == "BGGR" and m.red_blue_split > 0.3 and m.green_split < 0.01
