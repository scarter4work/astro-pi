### Task 11: Sidecar ops

**Files:**
- Modify: `src/autocontrast/sidecar.py` (add two handlers; register in `_OPS` at lines 182-188)
- Test: `tests/test_optimize_sidecar.py`

**Interfaces:**
- Consumes: `loop.begin`, `loop.advance`, `loop.outcome`, `Session`, `session_path`; existing `_op_analyze` helpers `_derive_palette`, `acquire_wcs`, `load_image`, `downsample_factor`.
- Produces: sidecar ops `optimize_begin` and `optimize_step`.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_optimize_sidecar.py
import numpy as np
from PIL import Image

from autocontrast.sidecar import handle_request


def _write(path, rgb):
    Image.fromarray((np.clip(rgb, 0, 1) * 255).astype(np.uint8)).save(path)


def _textured(h=128, w=128, seed=5):
    rng = np.random.default_rng(seed)
    f = rng.normal(0, 1, (h, w)).cumsum(axis=1)
    f = (f - f.min()) / (f.ptp() + 1e-12)
    return np.stack([f, f * 0.9, f * 1.05], axis=-1)


def test_unknown_op_still_lists_the_new_ops():
    resp = handle_request({"op": "nope"})
    assert resp["ok"] is False
    assert "optimize_begin" in resp["error"]
    assert "optimize_step" in resp["error"]


def test_optimize_begin_opens_a_resumable_session(tmp_path):
    img = tmp_path / "target.png"
    _write(img, _textured())
    resp = handle_request({
        "op": "optimize_begin", "image": str(img), "work_dir": str(tmp_path),
        "reference_fingerprint": _ref_fp_dict(), "reference_id": "synthetic",
        "pixel_scale_arcsec": 1.0, "psf_fwhm_arcsec": 2.0, "palette_class": "HOO",
    })
    assert resp["ok"] is True, resp.get("error")
    assert resp["result"]["session_id"]
    assert resp["result"]["baseline_distance"] >= 0
    assert (tmp_path / f"session-{resp['result']['session_id']}.json").exists()


def test_optimize_step_advances_and_persists(tmp_path):
    img = tmp_path / "target.png"
    _write(img, _textured())
    started = handle_request({
        "op": "optimize_begin", "image": str(img), "work_dir": str(tmp_path),
        "reference_fingerprint": _ref_fp_dict(), "reference_id": "synthetic",
        "pixel_scale_arcsec": 1.0, "psf_fwhm_arcsec": 2.0, "palette_class": "HOO",
    })["result"]

    stepped = handle_request({
        "op": "optimize_step", "work_dir": str(tmp_path),
        "session_id": started["session_id"],
    })
    assert stepped["ok"] is True, stepped.get("error")
    assert stepped["result"]["iteration"] == 1
    assert "converged" in stepped["result"]


def test_optimize_step_on_a_missing_session_is_a_loud_error(tmp_path):
    resp = handle_request({"op": "optimize_step", "work_dir": str(tmp_path),
                           "session_id": "does-not-exist"})
    assert resp["ok"] is False
    assert "does-not-exist" in resp["error"]


def _ref_fp_dict():
    from autocontrast.fingerprint.extract import extract
    return extract(_textured(seed=9), pixel_scale_arcsec=1.0, n_scales=7,
                   psf_fwhm_arcsec=2.0, palette_class="HOO").to_dict()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_optimize_sidecar.py -v`
Expected: FAIL — `assert 'optimize_begin' in resp['error']`

- [ ] **Step 3: Write minimal implementation**

```python
# add to src/autocontrast/sidecar.py, above _OPS
import uuid

from autocontrast.fingerprint.extract import FingerprintData
from autocontrast.optimize.executors.numpy_exec import NumpyExecutor
from autocontrast.optimize.loop import advance, begin, outcome
from autocontrast.optimize.session import Session, session_path


def _rgb_io(max_dim: int):
    """Load/save helpers the loop uses. The NumpyExecutor path round-trips through
    disk exactly as the PixInsight path will, so the two behave alike."""
    from PIL import Image

    def load(path):
        return load_raster(path, max_dim=max_dim)

    def save(path, rgb):
        import numpy as np
        Image.fromarray((np.clip(rgb, 0, 1) * 255).astype("uint8")).save(path)

    return load, save


def _op_optimize_begin(req: dict) -> dict:
    """Open an optimizer session and measure the baseline (spec SS2.1, SS3.2)."""
    max_dim = req.get("max_dim", 1600)
    load, _save = _rgb_io(max_dim)
    work_dir = req["work_dir"]
    session_id = req.get("session_id") or uuid.uuid4().hex[:12]

    reference_fp = FingerprintData.from_dict(req["reference_fingerprint"])
    session = begin(
        source_path=req["image"], proxy_path=req["image"],
        reference_id=req["reference_id"], reference_fp=reference_fp,
        work_dir=work_dir, pixel_scale_arcsec=req["pixel_scale_arcsec"],
        psf_fwhm_arcsec=req["psf_fwhm_arcsec"],
        palette_class=req["palette_class"], n_scales=req.get("n_scales", 7),
        session_id=session_id, load=load,
    )
    session.save(session_path(work_dir, session_id))
    return {"session_id": session_id, "baseline_distance": session.baseline_distance,
            "reference_id": session.reference_id}


def _op_optimize_step(req: dict) -> dict:
    """Advance one iteration and persist. Resumable across sidecar invocations."""
    work_dir, session_id = req["work_dir"], req["session_id"]
    path = session_path(work_dir, session_id)
    if not path.exists():
        raise FileNotFoundError(
            f"no optimizer session {session_id!r} under {work_dir}; "
            "call optimize_begin first"
        )

    session = Session.load(path)
    load, save = _rgb_io(req.get("max_dim", 1600))
    session = advance(session, NumpyExecutor(), load=load, save=save)
    session.save(path)

    result = outcome(session)
    result["iteration"] = session.iteration
    result["converged"] = session.converged
    return result
```

```python
# replace _OPS in src/autocontrast/sidecar.py
_OPS = {
    "ping": _op_ping,
    "fingerprint": _op_fingerprint,
    "distance": _op_distance,
    "solve": _op_solve,
    "analyze": _op_analyze,
    "optimize_begin": _op_optimize_begin,
    "optimize_step": _op_optimize_step,
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_optimize_sidecar.py -v && .venv/bin/python -m pytest tests/test_sidecar.py -v`
Expected: PASS both — the existing sidecar tests must not regress.

- [ ] **Step 5: Commit**

```bash
git add src/autocontrast/sidecar.py tests/test_optimize_sidecar.py
git commit -m "sidecar: optimize_begin and optimize_step

Resumable across invocations, because the sidecar is spawned per request.
A missing session is a loud FileNotFoundError naming the id, not a silent
fresh start that would quietly discard a run's progress (SS12)."
```

---

