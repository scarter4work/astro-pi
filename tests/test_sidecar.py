"""Tests for the sidecar file-protocol service (§3.1).

The sidecar is a headless request->response worker that PixInsight (PJSR) drives:
PJSR writes a request JSON, runs `python -m autocontrast.sidecar req.json resp.json`,
and reads the response JSON. handle_request() is the pure core; main() is the file I/O.
"""

import json

import numpy as np
import pytest
from PIL import Image

from autocontrast.fingerprint.extract import extract
from autocontrast.sidecar import handle_request, main


def _write_png(path, size=64):
    rng = np.random.default_rng(0)
    img = rng.uniform(0, 0.05, size=(size, size, 3))
    yy, xx = np.mgrid[0:size, 0:size]
    blob = np.exp(-(((xx - size / 2) ** 2 + (yy - size / 2) ** 2) / (2 * (size / 8) ** 2)))
    img[..., 0] += 0.6 * blob
    Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8)).save(path)


_FP_ARGS = dict(pixel_scale_arcsec=1.0, psf_fwhm_arcsec=2.0, palette_class="RGB", n_scales=6)


def test_ping():
    resp = handle_request({"op": "ping"})
    assert resp["ok"] is True
    assert resp["result"]["pong"] is True


def test_fingerprint_returns_serializable_record(tmp_path):
    png = tmp_path / "img.png"; _write_png(png)
    resp = handle_request({"op": "fingerprint", "image": str(png), **_FP_ARGS})
    assert resp["ok"] is True
    # Result must be pure JSON (the bridge serializes it).
    json.dumps(resp["result"])
    assert resp["result"]["palette_class"] == "RGB"
    assert len(resp["result"]["tonal_quantiles"]) == 10


def test_distance_between_two_fingerprints(tmp_path):
    png = tmp_path / "img.png"; _write_png(png)
    fp = extract(np.asarray(Image.open(png), dtype=np.float64) / 255.0, **_FP_ARGS).to_dict()
    resp = handle_request({"op": "distance", "reference": fp, "target": fp})
    assert resp["ok"] is True
    assert resp["result"]["distance"] == pytest.approx(0.0, abs=1e-9)


def test_unknown_op_is_a_loud_error():
    resp = handle_request({"op": "frobnicate"})
    assert resp["ok"] is False
    assert "frobnicate" in resp["error"]


def test_missing_image_surfaces_error_not_silent(tmp_path):
    resp = handle_request({"op": "fingerprint", "image": str(tmp_path / "nope.png"), **_FP_ARGS})
    assert resp["ok"] is False
    assert resp["error"]  # loud, per §12


def test_main_does_file_round_trip(tmp_path):
    req_path = tmp_path / "req.json"
    resp_path = tmp_path / "resp.json"
    req_path.write_text(json.dumps({"op": "ping"}))

    rc = main([str(req_path), str(resp_path)])

    assert rc == 0
    resp = json.loads(resp_path.read_text())
    assert resp["ok"] is True and resp["result"]["pong"] is True
