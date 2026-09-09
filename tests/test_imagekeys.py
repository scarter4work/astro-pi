import numpy as np
from astrometa import imagekeys


def test_content_hash_is_stable_and_distinct(tmp_path):
    a = tmp_path / "a"; a.write_bytes(b"hello")
    b = tmp_path / "b"; b.write_bytes(b"hello")
    c = tmp_path / "c"; c.write_bytes(b"world")
    assert imagekeys.content_hash(a) == imagekeys.content_hash(b)
    assert imagekeys.content_hash(a) != imagekeys.content_hash(c)


def _field(seed: int, scale: float = 1.0, offset: float = 0.0) -> np.ndarray:
    rng = np.random.default_rng(seed)
    img = rng.normal(100, 5, (256, 256))
    for _ in range(40):                      # deterministic "stars"
        y, x = rng.integers(0, 250, 2)
        img[y:y+3, x:x+3] += 3000
    return img * scale + offset


def test_same_field_at_different_exposure_matches():
    base = _field(1)
    brighter = _field(1, scale=4.0, offset=500.0)
    d = imagekeys.hamming(imagekeys.fingerprint(base),
                          imagekeys.fingerprint(brighter))
    assert d <= 8, f"same field should survive exposure change, got {d}"


def test_different_fields_do_not_match():
    d = imagekeys.hamming(imagekeys.fingerprint(_field(1)),
                          imagekeys.fingerprint(_field(2)))
    assert d > 40, f"different fields should differ, got {d}"


def test_fingerprint_is_deterministic():
    img = _field(7)
    assert imagekeys.fingerprint(img) == imagekeys.fingerprint(img)


def test_flat_image_does_not_crash():
    assert isinstance(imagekeys.fingerprint(np.full((64, 64), 5.0)), str)


def test_flat_fingerprint_matches_length_of_normal_fingerprint():
    # The early-return path for a degenerate (flat) image must emit the
    # same bit width as the normal code path, or fingerprints become
    # incomparable widths for hamming() depending on which branch ran.
    flat_fp = imagekeys.fingerprint(np.full((64, 64), 5.0))
    normal_fp = imagekeys.fingerprint(_field(3))
    assert len(flat_fp) == len(normal_fp)


def test_pixel_stats_reports_background_and_saturation():
    img = np.full((100, 100), 200.0)
    img[:10, :] = 65535.0
    bg, sat = imagekeys.pixel_stats(img)
    assert bg == 200.0
    assert abs(sat - 0.10) < 0.001
