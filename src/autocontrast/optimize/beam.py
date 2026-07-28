"""Beam search state (SS6.1).

A beam, not a hill-climb: improvement is non-monotonic, and a greedy loop walks
straight into the over-cooked attractor because every individual step "increased
contrast" (SS6.1).
"""

from __future__ import annotations

from dataclasses import dataclass

from .recipe import Recipe


@dataclass(frozen=True)
class BeamConfig:
    width: int = 3
    top_k: int = 3
    iteration_cap: int = 20
    convergence_window: int = 3
    epsilon: float = 1e-3
    epsilon_improve: float = 1e-3


@dataclass(frozen=True)
class Branch:
    recipe: Recipe
    image_path: str
    distance: float
    alive: bool = True


def prune(candidates: list[Branch], width: int) -> list[Branch]:
    """Keep the ``width`` best live, distinct branches.

    Distinctness is by recipe: two branches that arrived at the same action
    sequence must not both hold a beam slot, or the beam silently narrows to one
    line of search while appearing to be three.
    """
    seen: set[str] = set()
    kept: list[Branch] = []
    for branch in sorted((c for c in candidates if c.alive), key=lambda b: b.distance):
        if branch.recipe.key in seen:
            continue
        seen.add(branch.recipe.key)
        kept.append(branch)
        if len(kept) == width:
            break
    return kept
