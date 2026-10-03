### Task 8: Render the modulated star layer

**Files:**
- Create: `src/gaia_depth_grade/render.py`
- Test: `tests/test_render.py`

**Interfaces:**
- Consumes: stars-layer image, detection `Table` (`x, y, flux, fwhm`), `Modulation` (Task 7), `base_sigma_px` from config.
- Produces:
  - `render_stars(stars_layer: np.ndarray, detected, modulation: Modulation, base_sigma_px: float) -> np.ndarray`. Image may be 2-D (mono) or 3-D `(ny, nx, 3)` (color). For each star, within a window of radius `ceil(4*base_sigma_px)`:
    - **brightness:** multiply the window pixels by `modulation.brightness[i]`.
    - **size/glow:** add a Gaussian halo of integrated weight `(size[i] - 1)` × local flux, sigma `base_sigma_px * size[i]`.
    - **local contrast:** unsharp the window by `modulation.contrast[i]` (`win += amount*(win - blur(win))`).
    - **saturation (color only):** push window chroma around its luminance by `saturation[i]`.
  - Output is clipped to `[0, 1]`. The function never mutates the input array.

- [ ] **Step 1: Write the failing test**

`tests/test_render.py`:
```python
import numpy as np
import pytest
from astropy.table import Table
from gaia_depth_grade.modulate import Modulation
from gaia_depth_grade.render import render_stars


def _one_star_layer(flux=0.5, x=32, y=32, sigma=2.0):
    ny = nx = 64
    yy, xx = np.mgrid[0:ny, 0:nx]
    img = flux * np.exp(-((xx - x) ** 2 + (yy - y) ** 2) / (2 * sigma**2))
    det = Table()
    det["x"] = [float(x)]; det["y"] = [float(y)]
    det["flux"] = [flux]; det["fwhm"] = [2.355 * sigma]
    return img, det


def test_brightness_up_increases_peak():
    img, det = _one_star_layer()
    base_peak = img.max()
    m = Modulation(brightness=np.array([1.5]), size=np.array([1.0]),
                   contrast=np.array([0.0]), saturation=np.array([1.0]))
    out = render_stars(img, det, m, base_sigma_px=2.0)
    assert out.max() > base_peak
    assert np.shares_memory(out, img) is False  # input not mutated


def test_brightness_down_decreases_peak():
    img, det = _one_star_layer()
    base_peak = img.max()
    m = Modulation(brightness=np.array([0.5]), size=np.array([1.0]),
                   contrast=np.array([0.0]), saturation=np.array([1.0]))
    out = render_stars(img, det, m, base_sigma_px=2.0)
    assert out.max() < base_peak


def test_size_up_widens_footprint():
    img, det = _one_star_layer()
    # Measure spatial extent with a FIXED absolute threshold. A relative
    # max*0.5 threshold rises as the glow raises the peak, masking the
    # widening; a fixed threshold counts the genuinely-expanded wings.
    def above_thresh(a, t=0.1):
        return int((a > t).sum())
    base_area = above_thresh(img)
    m = Modulation(brightness=np.array([1.0]), size=np.array([1.6]),
                   contrast=np.array([0.0]), saturation=np.array([1.0]))
    out = render_stars(img, det, m, base_sigma_px=2.0)
    assert above_thresh(out) > base_area


def test_output_clipped_and_color_shape_preserved():
    img, det = _one_star_layer()
    color = np.stack([img, img * 0.5, img * 0.2], axis=-1)
    m = Modulation(brightness=np.array([2.0]), size=np.array([1.0]),
                   contrast=np.array([0.0]), saturation=np.array([1.3]))
    out = render_stars(color, det, m, base_sigma_px=2.0)
    assert out.shape == color.shape
    assert out.max() <= 1.0 and out.min() >= 0.0
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/test_render.py -v`
Expected: FAIL — `ModuleNotFoundError: gaia_depth_grade.render`.

- [ ] **Step 3: Implement `render.py`**

```python
from __future__ import annotations

import math

import numpy as np
from scipy.ndimage import gaussian_filter


def _window_slice(x, y, rad, shape):
    ny, nx = shape[0], shape[1]
    x0, x1 = max(0, int(x) - rad), min(nx, int(x) + rad + 1)
    y0, y1 = max(0, int(y) - rad), min(ny, int(y) + rad + 1)
    return slice(y0, y1), slice(x0, x1)


def _gaussian_stamp(h, w, cx, cy, sigma):
    yy, xx = np.mgrid[0:h, 0:w]
    return np.exp(-((xx - cx) ** 2 + (yy - cy) ** 2) / (2 * sigma**2))


def render_stars(stars_layer, detected, modulation, base_sigma_px):
    out = np.array(stars_layer, dtype=float, copy=True)
    is_color = out.ndim == 3
    rad = int(math.ceil(4 * base_sigma_px))

    for i in range(len(detected)):
        x, y = float(detected["x"][i]), float(detected["y"][i])
        flux = float(detected["flux"][i])
        b = float(modulation.brightness[i])
        zsize = float(modulation.size[i])
        camount = float(modulation.contrast[i])
        sat = float(modulation.saturation[i])

        ys, xs = _window_slice(x, y, rad, out.shape)
        win = out[ys, xs]
        if win.size == 0:
            continue

        # brightness
        win *= b

        # size/glow: add a peak-normalized Gaussian halo whose peak amplitude
        # is (zsize-1)*flux and whose width scales with zsize. Peak-normalizing
        # (not area-normalizing) is what makes a size increase visibly widen the
        # star's footprint rather than just adding a negligible flux spread.
        if abs(zsize - 1.0) > 1e-9:
            h, w = win.shape[0], win.shape[1]
            cx, cy = x - xs.start, y - ys.start
            stamp = _gaussian_stamp(h, w, cx, cy, base_sigma_px * zsize)
            peak = stamp.max()
            if peak > 0:
                stamp = stamp / peak
            extra = (zsize - 1.0) * flux
            if is_color:
                for c in range(win.shape[2]):
                    win[..., c] += extra * stamp
            else:
                win += extra * stamp

        # local contrast (unsharp)
        if abs(camount) > 1e-9:
            blur = gaussian_filter(win, sigma=base_sigma_px, axes=(0, 1) if is_color else None)
            win += camount * (win - blur)

        # saturation (color only)
        if is_color and abs(sat - 1.0) > 1e-9:
            lum = win.mean(axis=2, keepdims=True)
            win[:] = lum + sat * (win - lum)

        out[ys, xs] = win

    np.clip(out, 0.0, 1.0, out=out)
    return out
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `pytest tests/test_render.py -v`
Expected: PASS (4 passed).

- [ ] **Step 5: Commit**

```bash
git add src/gaia_depth_grade/render.py tests/test_render.py
git commit -m "feat: render depth-modulated star layer (brightness/size/contrast/saturation)"
```

---

