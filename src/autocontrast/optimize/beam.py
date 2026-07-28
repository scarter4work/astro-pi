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

    # How many actions a branch may ATTEMPT while reaching for ``top_k`` live
    # candidates (§3.3, added 2026-07-27 on measurement). `top_k` counts
    # candidates that SURVIVED, not candidates tried: a guardrail trip discards
    # the candidate and must not also cost the branch a slot, or the loop
    # searches least thoroughly exactly where the image is most fragile, and the
    # §7 filter becomes a score effect wearing a different hat. Measured on a
    # real degraded render, 4 of 10 proposed actions tripped a guardrail, so
    # trips are the common case.
    #
    # But the retry has to be bounded. At the 1600px search proxy the menu holds
    # 50 actions at ~2.5s each (fingerprint extraction 1.5s dominates; the image
    # operation itself is 0.2s), so retrying until top_k survive costs ~150
    # candidates/iteration and ~127 min over 20 iterations against a 15-minute
    # budget -- and an unpredictable cost makes a budget meaningless. At 3x it is
    # <=27 candidates/iteration, ~23 min worst case, and the cap binds only when
    # trips are frequent.
    #
    # ``None`` means "derive 3 x top_k", so the relationship survives a change to
    # top_k instead of silently decoupling from it. A tunable under §3.7's
    # calibration discipline. Always an int after construction.
    max_attempts: int | None = None

    def __post_init__(self) -> None:
        if self.max_attempts is None:
            object.__setattr__(self, "max_attempts", 3 * self.top_k)


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
