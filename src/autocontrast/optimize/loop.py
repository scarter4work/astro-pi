"""The optimization loop (§6).

The fail-safe is structural, not a special case. Best-so-far is seeded with the
INPUT and its own baseline distance; a candidate replaces it only on a strict
improvement beyond ``epsilon_improve``. Because the fingerprint distance is a
VALLEY -- D=0 at the reference and rising in BOTH directions, including the
over-processed one -- an already-good image starts near the floor, every move
climbs, and the loop declines on its own. A monotone "more contrast is better"
objective could not do that at any amount of guardrailing.

"Untouched" is literal. On a decline the loop hands back ``proxy_path`` itself,
not a recombination of whatever the search happened to produce along the way.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np

from autocontrast.fingerprint.distance import fingerprint_distance
from autocontrast.fingerprint.extract import FingerprintData, extract
from autocontrast.fingerprint.palette import palette_chroma_compatible

from .actions import Action, available_actions
from .beam import BeamConfig, Branch, prune
from .executor import Executor
from .guardrails import GuardrailLimits, evaluate_guardrails
from .propose import propose_actions
from .recipe import Recipe
from .session import Session


def _measure(rgb, session: Session) -> FingerprintData:
    return extract(
        rgb, pixel_scale_arcsec=session.pixel_scale_arcsec,
        n_scales=session.n_scales, psf_fwhm_arcsec=session.psf_fwhm_arcsec,
        palette_class=session.palette_class,
    )


class _Stub:
    """Carries just the measurement metadata `_measure` needs before a Session exists."""

    def __init__(self, pixel_scale_arcsec, psf_fwhm_arcsec, palette_class, n_scales):
        self.pixel_scale_arcsec = pixel_scale_arcsec
        self.psf_fwhm_arcsec = psf_fwhm_arcsec
        self.palette_class = palette_class
        self.n_scales = n_scales


def begin(
    *, source_path: str, proxy_path: str, reference_id: str,
    reference_fp: FingerprintData, work_dir: str, pixel_scale_arcsec: float,
    psf_fwhm_arcsec: float, palette_class: str, n_scales: int, session_id: str,
    load, config: BeamConfig | None = None,
) -> Session:
    """Open a session, measuring the input as the baseline AND as best-so-far."""
    config = config or BeamConfig()
    proxy = load(proxy_path)
    baseline_fp = _measure(proxy, _Stub(pixel_scale_arcsec, psf_fwhm_arcsec,
                                        palette_class, n_scales))
    baseline = fingerprint_distance(reference_fp, baseline_fp)

    root = Branch(recipe=Recipe.empty(), image_path=proxy_path, distance=baseline)
    return Session(
        session_id=session_id, work_dir=work_dir, source_path=source_path,
        proxy_path=proxy_path, reference_id=reference_id,
        reference_fp=reference_fp.to_dict(), pixel_scale_arcsec=pixel_scale_arcsec,
        psf_fwhm_arcsec=psf_fwhm_arcsec, palette_class=palette_class,
        n_scales=n_scales, config=config, iteration=0, branches=[root], best=root,
        baseline_distance=baseline, distance_history=[baseline], converged=False,
        convergence_reason="", guardrail_log=[],
    )


def resume(session_file: str | Path, *, proxy_path: str) -> Session:
    """Reopen a persisted session, refusing to resume against a different image.

    ``Session.load`` deliberately does not check this: the session record carries
    no image dimensions, so it has nothing to verify a proxy against, and the
    check belongs where the information lives. This is that place -- the caller
    here is the one holding the path of the image it means to keep optimizing.

    The comparison is EXACT string equality, not a resolved or normalized one.
    Two reasons. First, path identity is the only evidence available that the
    resumed image is the one the session's ``layer`` values were computed for;
    normalizing would trade that evidence for a guess about which distinct
    strings ought to denote the same file. Second, ``proxy_path`` need not be a
    filesystem path at all -- the executor seam is free to address images through
    an in-memory or PixInsight-side store -- so there is not always anything to
    resolve.

    A session silently resumed against a different image would carry ``layer``
    values, band assignments and a baseline distance computed for the wrong
    geometry, and would go on to report an improvement it never measured.
    """
    session = Session.load(session_file)
    if proxy_path != session.proxy_path:
        raise ValueError(
            f"refusing to resume session {session.session_id!r} against a "
            f"different image: the session was built for proxy_path "
            f"{session.proxy_path!r} but resume was called with {proxy_path!r}. "
            "The session records no image dimensions, so its layer assignments "
            "and baseline distance cannot be re-verified against another image."
        )
    return session


def _ranked_menu(
    reference_fp: FingerprintData, parent_fp: FingerprintData, *,
    applied_kinds: frozenset[str], n_scales: int,
) -> list[Action]:
    """Every legal action for this state, in the proposer's own priority order.

    ``propose_actions`` returns at most ``top_k`` entries, so obtaining the WHOLE
    ranking means handing it a bound that cannot truncate the list. The menu is
    that bound: ``available_actions`` is precisely what the proposer groups and
    ranks, and a set of groups can never outnumber its members. Rebuilding the
    menu here duplicates no ranking logic -- it only counts what is on offer --
    and it tracks ``actions.py`` automatically if a kind is ever added.
    """
    compatible = palette_chroma_compatible(
        reference_fp.palette_class, parent_fp.palette_class
    )
    menu = available_actions(
        pixel_scale_arcsec=parent_fp.pixel_scale_arcsec,
        psf_fwhm_arcsec=max(reference_fp.psf_fwhm_arcsec, parent_fp.psf_fwhm_arcsec),
        n_scales=n_scales,
        palette_compatible=compatible,
        applied_kinds=applied_kinds,
    )
    return propose_actions(
        reference_fp, parent_fp, applied_kinds=applied_kinds,
        n_scales=n_scales, top_k=len(menu),
    )


def advance(session: Session, executor: Executor, *, load, save,
            limits: GuardrailLimits = GuardrailLimits()) -> Session:
    """One iteration: expand every live branch, guardrail, score, prune.

    A branch contributes ``config.top_k`` LIVE candidates, walking the ranked
    menu until it has them. It is deliberately not "attempt the first top_k
    actions and keep whatever survives": a discarded candidate would then cost
    search breadth as well as itself, and a guardrail that narrows the search is
    a score term wearing a different hat (§7 forbids exactly that). Measured on
    the flattened fixture: the first three ranked actions are two guardrail trips
    and one move that climbs, while the action that actually closes the gap --
    ``local_contrast@2"`` -- ranks fifth. Stopping at three proposals means never
    reaching it, and the loop declines on an image it could plainly have fixed.
    """
    if session.converged:
        return session

    reference_fp = FingerprintData.from_dict(session.reference_fp)
    source = load(session.proxy_path)
    candidates: list[Branch] = []
    iteration = session.iteration + 1

    for branch in session.branches:
        parent = load(branch.image_path)
        parent_fp = _measure(parent, session)
        actions = _ranked_menu(
            reference_fp, parent_fp,
            applied_kinds=branch.recipe.applied_kinds, n_scales=session.n_scales,
        )

        kept = 0
        for action in actions:
            if kept >= session.config.top_k:
                break

            try:
                produced = executor.apply(
                    parent, action, pixel_scale_arcsec=session.pixel_scale_arcsec
                )
            except Exception as exc:  # a dead candidate, not a dead run (§12)
                session.guardrail_log.append({
                    "iteration": iteration, "action": action.key,
                    "failed": ["executor"], "reason": f"{type(exc).__name__}: {exc}",
                })
                continue

            if np.array_equal(produced, parent):
                # A candidate identical to its parent has, by definition, learned
                # nothing: same pixels, same fingerprint, same distance -- but a
                # distinct recipe key, so `prune`'s deduplication cannot see it
                # for what it is. Left in, it ties with its parent's score, which
                # on an already-good image is the BEST score available, and takes
                # a beam slot having explored none of the space.
                #
                # The test is the PROPERTY (pixels unchanged), never the action's
                # name. `star_split` is the no-op that exists today, but only
                # because the numpy executor has no starless layer to route; the
                # PixInsight executor's star_split does change the image and must
                # survive this check. Matching on the string would hard-code one
                # executor's limitation into the loop.
                session.guardrail_log.append({
                    "iteration": iteration, "action": action.key,
                    "failed": ["no_op"],
                    "reason": (
                        "candidate was pixel-identical to its parent; it explored "
                        "nothing and must not hold a beam slot"
                    ),
                })
                continue

            verdicts = evaluate_guardrails(produced, parent, source, limits)

            # A guardrail may PASS and still report that it could not assess
            # anything -- star_integrity does exactly that on a starless
            # baseline. That reason has to reach the caller, or the run cannot
            # report "this guardrail did not actually protect anything here"
            # (§12: degraded paths surface). Recorded under `noted`, never
            # `failed`: it is an observation, not a discard, and the two must
            # stay distinguishable in the log. Recorded whatever the candidate's
            # fate, because "star integrity was never evaluated" is equally true
            # of a candidate some OTHER guardrail went on to reject.
            noted = [v for v in verdicts if v.ok and v.reason]
            if noted:
                session.guardrail_log.append({
                    "iteration": iteration, "action": action.key,
                    "noted": [v.name for v in noted],
                    "reason": "; ".join(v.reason for v in noted),
                })

            failed = [v for v in verdicts if not v.ok]
            if failed:
                # A violation DISCARDS the candidate; it is never a score term (§7).
                session.guardrail_log.append({
                    "iteration": iteration, "action": action.key,
                    "failed": [v.name for v in failed],
                    "reason": "; ".join(v.reason for v in failed),
                })
                continue

            recipe = branch.recipe.extend(action)
            path = f"{session.work_dir}/cand-{iteration}-{len(candidates)}.png"
            save(path, produced)
            distance = fingerprint_distance(reference_fp, _measure(produced, session))
            candidates.append(Branch(recipe=recipe, image_path=path, distance=distance))
            kept += 1

    session.iteration = iteration
    survivors = prune(candidates, session.config.width)

    if not survivors:
        # §6.3 condition 3: guardrails exhausted the branch set. Reported
        # honestly -- and `best` is untouched, so this terminates in a decline
        # rather than in a result nobody vetted.
        session.branches = []
        session.converged = True
        session.convergence_reason = (
            "every branch was discarded by guardrails or executor failure"
        )
        return session

    session.branches = survivors
    best_candidate = survivors[0]
    if best_candidate.distance < session.best.distance - session.config.epsilon_improve:
        session.best = best_candidate  # checkpoint (§6.1)

    session.distance_history.append(session.best.distance)
    _check_convergence(session)
    return session


def _check_convergence(session: Session) -> None:
    """§6.3, OR of the conditions. Condition 2 (VLM) is absent in Phase 2."""
    cfg = session.config
    if session.iteration >= cfg.iteration_cap:
        session.converged = True
        session.convergence_reason = f"iteration cap ({cfg.iteration_cap}) reached"
        return

    window = session.distance_history[-(cfg.convergence_window + 1):]
    if len(window) > cfg.convergence_window:
        if abs(window[0] - window[-1]) < cfg.epsilon:
            session.converged = True
            session.convergence_reason = (
                f"deltaD below {cfg.epsilon} across the last "
                f"{cfg.convergence_window} iterations"
            )


def run_to_convergence(session: Session, executor: Executor, *, load, save,
                       limits: GuardrailLimits = GuardrailLimits()) -> Session:
    while not session.converged:
        session = advance(session, executor, load=load, save=save, limits=limits)
    return session


def outcome(session: Session) -> dict:
    """The reportable result. ``improved`` False means we DECLINED and the user
    gets their original file back untouched (spec §3.2)."""
    improved = session.best.distance < (
        session.baseline_distance - session.config.epsilon_improve
    )
    return {
        "improved": improved,
        "baseline_distance": session.baseline_distance,
        "distance": session.best.distance if improved else session.baseline_distance,
        "result_path": session.best.image_path if improved else session.proxy_path,
        "recipe": (session.best.recipe if improved else Recipe.empty()).to_dict(),
        "pixinsight_steps": (
            session.best.recipe if improved else Recipe.empty()
        ).to_pixinsight_steps(),
        "iterations": session.iteration,
        "convergence_reason": session.convergence_reason,
        "reference_id": session.reference_id,
        "guardrail_log": session.guardrail_log,
    }
