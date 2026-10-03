### Task 9: Session — serialize and resume

**Files:**
- Create: `src/autocontrast/optimize/session.py`
- Test: `tests/test_optimize_session.py`

**Interfaces:**
- Consumes: `Branch`, `BeamConfig`, `Recipe`.
- Produces: `Session` (dataclass: `session_id: str`, `work_dir: str`, `source_path: str`, `proxy_path: str`, `reference_id: str`, `reference_fp: dict`, `pixel_scale_arcsec: float`, `psf_fwhm_arcsec: float`, `palette_class: str`, `n_scales: int`, `config: BeamConfig`, `iteration: int`, `branches: list[Branch]`, `best: Branch`, `baseline_distance: float`, `distance_history: list[float]`, `converged: bool`, `convergence_reason: str`, `guardrail_log: list[dict]`); `Session.save(path)`, `Session.load(path)`, `session_path(work_dir, session_id) -> Path`.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_optimize_session.py
from autocontrast.optimize.actions import Action
from autocontrast.optimize.beam import BeamConfig, Branch
from autocontrast.optimize.recipe import Recipe
from autocontrast.optimize.session import Session, session_path


def _session(tmp_path):
    root = Recipe.empty()
    base = Branch(recipe=root, image_path=str(tmp_path / "proxy.xisf"), distance=0.5)
    return Session(
        session_id="s1", work_dir=str(tmp_path), source_path=str(tmp_path / "src.fit"),
        proxy_path=str(tmp_path / "proxy.xisf"), reference_id="eso1103a",
        reference_fp={"stub": True}, pixel_scale_arcsec=1.01, psf_fwhm_arcsec=2.0,
        palette_class="HOO", n_scales=7, config=BeamConfig(), iteration=0,
        branches=[base], best=base, baseline_distance=0.5, distance_history=[0.5],
        converged=False, convergence_reason="", guardrail_log=[],
    )


def test_session_round_trips_through_disk(tmp_path):
    s = _session(tmp_path)
    s.branches = [s.branches[0], Branch(
        recipe=Recipe.empty().extend(Action("chroma", "gentle", None, {})),
        image_path=str(tmp_path / "c1.xisf"), distance=0.42)]
    p = session_path(tmp_path, "s1")
    s.save(p)

    loaded = Session.load(p)
    assert loaded.session_id == "s1"
    assert loaded.reference_id == "eso1103a"
    assert loaded.baseline_distance == 0.5
    assert [b.distance for b in loaded.branches] == [0.5, 0.42]
    assert loaded.branches[1].recipe.key == s.branches[1].recipe.key
    assert loaded.config.width == BeamConfig().width


def test_resume_preserves_iteration_and_history(tmp_path):
    s = _session(tmp_path)
    s.iteration = 4
    s.distance_history = [0.5, 0.47, 0.45, 0.44, 0.44]
    p = session_path(tmp_path, "s1")
    s.save(p)
    assert Session.load(p).iteration == 4
    assert Session.load(p).distance_history[-1] == 0.44


def test_guardrail_log_survives_the_round_trip(tmp_path):
    s = _session(tmp_path)
    s.guardrail_log = [{"iteration": 1, "action": "chroma@-/strong",
                        "failed": ["hue_invention"], "reason": "0.02 chroma mass"}]
    p = session_path(tmp_path, "s1")
    s.save(p)
    assert Session.load(p).guardrail_log[0]["failed"] == ["hue_invention"]
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_optimize_session.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'autocontrast.optimize.session'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/autocontrast/optimize/session.py
"""Resumable loop state (spec SS2.1).

The sidecar is spawned fresh per request -- `python -m autocontrast.sidecar
req.json resp.json` -- so "the sidecar owns the loop" cannot mean holding it in
memory. Everything the loop knows serializes here between calls. A crashed run
leaves an inspectable session file rather than nothing.
"""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass, field
from pathlib import Path

from .beam import BeamConfig, Branch
from .recipe import Recipe


def session_path(work_dir: str | Path, session_id: str) -> Path:
    return Path(work_dir) / f"session-{session_id}.json"


def _branch_to_dict(b: Branch) -> dict:
    return {"recipe": b.recipe.to_dict(), "image_path": b.image_path,
            "distance": b.distance, "alive": b.alive}


def _branch_from_dict(d: dict) -> Branch:
    return Branch(recipe=Recipe.from_dict(d["recipe"]), image_path=d["image_path"],
                  distance=d["distance"], alive=d["alive"])


@dataclass
class Session:
    session_id: str
    work_dir: str
    source_path: str
    proxy_path: str
    reference_id: str
    reference_fp: dict
    pixel_scale_arcsec: float
    psf_fwhm_arcsec: float
    palette_class: str
    n_scales: int
    config: BeamConfig
    iteration: int
    branches: list[Branch]
    best: Branch
    baseline_distance: float
    distance_history: list[float]
    converged: bool
    convergence_reason: str
    guardrail_log: list[dict] = field(default_factory=list)

    def save(self, path: str | Path) -> None:
        payload = {
            "session_id": self.session_id, "work_dir": self.work_dir,
            "source_path": self.source_path, "proxy_path": self.proxy_path,
            "reference_id": self.reference_id, "reference_fp": self.reference_fp,
            "pixel_scale_arcsec": self.pixel_scale_arcsec,
            "psf_fwhm_arcsec": self.psf_fwhm_arcsec,
            "palette_class": self.palette_class, "n_scales": self.n_scales,
            "config": asdict(self.config), "iteration": self.iteration,
            "branches": [_branch_to_dict(b) for b in self.branches],
            "best": _branch_to_dict(self.best),
            "baseline_distance": self.baseline_distance,
            "distance_history": self.distance_history,
            "converged": self.converged, "convergence_reason": self.convergence_reason,
            "guardrail_log": self.guardrail_log,
        }
        Path(path).write_text(json.dumps(payload, indent=2))

    @classmethod
    def load(cls, path: str | Path) -> "Session":
        d = json.loads(Path(path).read_text())
        return cls(
            session_id=d["session_id"], work_dir=d["work_dir"],
            source_path=d["source_path"], proxy_path=d["proxy_path"],
            reference_id=d["reference_id"], reference_fp=d["reference_fp"],
            pixel_scale_arcsec=d["pixel_scale_arcsec"],
            psf_fwhm_arcsec=d["psf_fwhm_arcsec"],
            palette_class=d["palette_class"], n_scales=d["n_scales"],
            config=BeamConfig(**d["config"]), iteration=d["iteration"],
            branches=[_branch_from_dict(b) for b in d["branches"]],
            best=_branch_from_dict(d["best"]),
            baseline_distance=d["baseline_distance"],
            distance_history=d["distance_history"], converged=d["converged"],
            convergence_reason=d["convergence_reason"],
            guardrail_log=d.get("guardrail_log", []),
        )
```

- [ ] **Step 4: Run test to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_optimize_session.py -v`
Expected: PASS (3 tests)

- [ ] **Step 5: Commit**

```bash
git add src/autocontrast/optimize/session.py tests/test_optimize_session.py
git commit -m "optimize: resumable session state

The sidecar is spawned per request, never long-running, so the loop cannot
live in memory. A crashed run now leaves an inspectable session file
rather than nothing."
```

---

