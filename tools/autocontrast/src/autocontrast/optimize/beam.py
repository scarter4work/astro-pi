"""Beam search state (SS6.1).

A beam, not a hill-climb: improvement is non-monotonic, and a greedy loop walks
straight into the over-cooked attractor because every individual step "increased
contrast" (SS6.1).
"""

from __future__ import annotations

from dataclasses import dataclass, field

from autocontrast.fingerprint.extract import FingerprintData

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
    # But the retry has to be bounded, and an unpredictable cost makes a budget
    # meaningless. Re-measured 2026-07-28 (Task 16) at the real 1600px search
    # proxy, because the first version of this comment -- and §3.3, which it
    # mirrors -- rejected the unbounded retry for exceeding a 15-minute budget
    # and then adopted a cap that also exceeded it:
    #
    #   band-limited menu        14-22 actions (NOT 50: actions are constructed
    #                            band-limited, so the menu is bounded by the
    #                            resolvable wavelet planes, §2.2/§4.4)
    #   candidate SCORED         2.136s  (executor 0.360 + guardrails 0.857
    #                                     + fingerprint 0.919)
    #   candidate DISCARDED      1.217s  -- `ingest_candidate` guardrails BEFORE
    #                            it fingerprints, so a trip never pays for an
    #                            extraction. The cap binds exactly when trips are
    #                            frequent, i.e. when candidates are cheapest.
    #
    # A branch can only burn all `max_attempts` by keeping fewer than `top_k`, so
    # the most expensive branch is (top_k - 1) scored plus the rest discarded.
    # At width=3, top_k=3, cap=9, iteration_cap=20:
    #
    #   top_k attempts (original)   ~6.4 min over 20 iterations
    #   bounded retry (adopted)     ~12.8 min worst case  <- fits the budget
    #   unbounded until top_k live  ~28.6 min             <- still rejected
    #
    # The budget is met by attacking the cost driver, not by narrowing this cap:
    # before Task 16 removed a duplicated starlet transform inside `extract` and
    # the redundant parent re-measure in `loop`, the adopted policy cost ~15.5
    # min and did not fit. Narrowing the cap instead would stop a branch before
    # `local_contrast@2"`, which ranks FIFTH on the measured flattened fixture
    # and is the action that actually closes the gap. See §3.3 for the full
    # table and two qualifications: the figures cover the sidecar's work only
    # (the PixInsight round trip of §2.5 is additive and unmeasured), and the
    # model over-predicts real measured iterations by 9-12%.
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

    # The fingerprint of `image_path`, carried forward rather than re-measured.
    #
    # Every branch was a CANDIDATE one iteration ago, and scoring it required
    # fingerprinting it (`loop.ingest_candidate`). The loop then threw that
    # result away and, on the next iteration, extracted it again from scratch to
    # build the branch's ranked menu -- a full extraction per branch per
    # iteration in `advance`, and per branch per BATCH in the §2.5 path, where
    # supplementary batches can pay it three times over in three separate
    # sidecar processes.
    #
    # It is a cache of a deterministic function of `image_path`'s pixels, so it
    # cannot disagree with a fresh measurement; `None` simply means nobody has
    # measured this branch yet (a session file written before this field
    # existed), and the loop falls back to measuring. That fallback is not a
    # degraded path in the §12 sense -- it yields the identical value, only
    # slower -- so it is not logged as one.
    #
    # `compare=False`: a branch's identity is its recipe and the image it points
    # at. The fingerprint is derived from that image, never independent of it,
    # and including it would put numpy arrays in the generated `__eq__` and
    # `__hash__`, where they raise rather than compare.
    fingerprint: FingerprintData | None = field(default=None, compare=False)


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
