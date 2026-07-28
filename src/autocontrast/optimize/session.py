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

    # ---- SS2.5 batched-protocol state -------------------------------------
    # An iteration now spans MULTIPLE sidecar processes: one plans a batch and
    # exits, PixInsight executes it, a fresh one ingests the results. The
    # per-branch locals `advance()` holds across its inner loop have to live on
    # disk instead, or every sidecar would restart each branch at menu position
    # zero and the SS3.3 attempt cap could never bind.

    # The batch currently in flight in PixInsight: the instruction dicts exactly
    # as they were handed over, which is what lets a reported `instruction_id`
    # be mapped back to the branch and Action it came from.
    pending: list[dict] = field(default_factory=list)

    # Parallel to `branches`: {branch_key, cursor, kept, attempts}. The ranked
    # menu itself is NOT stored -- it is deterministic given (parent pixels,
    # reference fingerprint, applied_kinds) and is recomputed per batch, so only
    # the branch's POSITION in it has to survive.
    cursors: list[dict] = field(default_factory=list)

    # Candidates scored so far in the OPEN iteration. They accumulate across
    # supplementary batches and are pruned only when the iteration closes.
    candidates: list[Branch] = field(default_factory=list)

    # The proxy is re-read by every spawned sidecar and `optimize_step` carries
    # no max_dim of its own; reading it at a different size than the baseline
    # was measured at would silently change every subsequent measurement.
    max_dim: int | None = None

    # The extension PixInsight is told to save candidates under. The sidecar has
    # to read those files back, so the format is not PixInsight's to choose
    # alone -- it is recorded here rather than assumed at both ends.
    candidate_suffix: str = ".png"

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
            "pending": self.pending, "cursors": self.cursors,
            "candidates": [_branch_to_dict(b) for b in self.candidates],
            "max_dim": self.max_dim, "candidate_suffix": self.candidate_suffix,
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
            pending=d.get("pending", []), cursors=d.get("cursors", []),
            candidates=[_branch_from_dict(b) for b in d.get("candidates", [])],
            max_dim=d.get("max_dim"),
            candidate_suffix=d.get("candidate_suffix", ".png"),
        )
