import numpy as np
from astropy.io import fits
from PIL import Image

from gaia_depth_grade.cli import main


def _write_fits(path, img, header):
    fits.PrimaryHDU(data=np.asarray(img, dtype="float32"),
                    header=header).writeto(str(path), overwrite=True)


# Render args matching GradeConfig() defaults, so grade == prepare+render.
_RENDER = ["--gains", "0.5,0.4,0.3,0.3", "--p-low", "5", "--p-high", "95", "--base-sigma", "2"]


def test_grade_equals_prepare_then_render(tmp_path, two_star_scene, monkeypatch):
    header, img, src, near, far = two_star_scene
    stars = tmp_path / "stars.fits"
    _write_fits(stars, img, header)
    monkeypatch.setattr("gaia_depth_grade.cli.GaiaStarSource", lambda cache_dir, mag_limit=None: src)

    g = tmp_path / "g.fits"
    main(["grade", str(stars), str(g)])

    cache = tmp_path / "cache"
    main(["prepare", str(stars), str(cache)])
    r = tmp_path / "r.fits"
    main(["render", str(cache), str(stars), str(r), *_RENDER])

    assert np.allclose(fits.getdata(str(g)), fits.getdata(str(r)))


def test_preview_writes_full_and_inset(tmp_path, two_star_scene, monkeypatch):
    header, img, src, near, far = two_star_scene
    stars = tmp_path / "stars.fits"
    _write_fits(stars, img, header)
    starless = tmp_path / "starless.fits"
    _write_fits(starless, np.zeros_like(img), header)
    monkeypatch.setattr("gaia_depth_grade.cli.GaiaStarSource", lambda cache_dir, mag_limit=None: src)

    cache = tmp_path / "cache"
    main(["prepare", str(stars), str(cache)])

    full = tmp_path / "full.png"
    inset = tmp_path / "inset.png"
    main(["preview", str(cache), str(stars), str(starless), str(full),
          "--inset", str(inset), "--region", "50,40,80,60",
          "--max-width", "150", *_RENDER])

    assert Image.open(str(full)).size[0] <= 150
    assert Image.open(str(inset)).size == (80, 60)
