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

from dataclasses import dataclass
from pathlib import Path

import numpy as np

from autocontrast.fingerprint.distance import fingerprint_distance
from autocontrast.fingerprint.extract import FingerprintData, extract

from .actions import Action
from .beam import BeamConfig, Branch, prune
from .executor import Executor
from .guardrails import GuardrailLimits, evaluate_guardrails
from .propose import propose_actions
from .recipe import Recipe, action_from_dict, action_to_dict, pixinsight_step
from .session import Session


def _measure(rgb, session: Session) -> FingerprintData:
    return extract(
        rgb, pixel_scale_arcsec=session.pixel_scale_arcsec,
        n_scales=session.n_scales, psf_fwhm_arcsec=session.psf_fwhm_arcsec,
        palette_class=session.palette_class,
    )


def _branch_fingerprint(branch: Branch, session: Session, *, load) -> FingerprintData:
    """A branch's own fingerprint, measured only if it isn't already known.

    Both entry points need this and neither should pay for it twice. Every
    branch in the beam got there by being scored as a candidate, and scoring
    means fingerprinting -- so `ingest_candidate` already holds the answer and
    now hands it to the `Branch` it builds. The root branch likewise carries the
    baseline measurement `begin` took.

    A `None` fingerprint is a cache miss, not a degraded measurement: it happens
    only on a session file written before `Branch` carried the field, and
    measuring produces the identical value because `extract` is a deterministic
    function of the pixels at `image_path`. Nothing about the result differs, so
    there is nothing for §12 to surface -- only the cost differs, and the loop
    goes on to pay it.

    ``load`` is called ONLY on that miss. That is the point: on the hot path the
    branch's image is never read.
    """
    if branch.fingerprint is not None:
        return branch.fingerprint
    return _measure(load(branch.image_path), session)


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

    root = Branch(recipe=Recipe.empty(), image_path=proxy_path, distance=baseline,
                  fingerprint=baseline_fp)
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

    ``top_k=None`` is ``propose_actions``'s own contract for "the whole
    ranking, no truncation" -- this is not a second implementation of that
    idea, it delegates outright. ``propose.py`` alone knows how the menu is
    built and grouped; a second copy of that construction here could drift
    from it (a new keyword argument, a changed ``psf_fwhm_arcsec`` expression)
    and would then silently under-count the ranking, truncating exactly the
    tail the bounded retry (SS3.3) exists to reach.
    """
    return propose_actions(
        reference_fp, parent_fp, applied_kinds=applied_kinds,
        n_scales=n_scales, top_k=None,
    )


@dataclass
class _Walk:
    """One branch's progress through the ranked menu.

    Held as locals inside ``advance``'s inner loop and as a persisted dict in
    the §2.5 batched protocol -- the same two counters either way, so the
    "may this branch issue another action?" rule below has exactly one
    definition rather than one per entry point.
    """

    kept: int = 0
    attempts: int = 0

    def may_issue(self, config: BeamConfig) -> bool:
        return self.kept < config.top_k and self.attempts < config.max_attempts


def _log_executor_failure(session: Session, iteration: int, action_key: str,
                          reason: str) -> None:
    """A dead candidate, not a dead run (§12)."""
    session.guardrail_log.append({
        "iteration": iteration, "action": action_key,
        "failed": ["executor"], "reason": reason,
    })


def _log_attempt_cap(session: Session, iteration: int, branch: Branch,
                     walk: _Walk) -> None:
    """The attempt cap bound before the branch filled its slots.

    Surfaced, not silent: this says the guardrails rejected nearly everything
    tried on this image, which is a different statement from "the menu was
    explored and this is all there was" (§12). ``noted``, not ``failed`` --
    nothing was discarded here, the branch just stopped looking. Keyed by
    ``branch`` rather than ``action`` because it is a property of the branch's
    whole expansion, not of one candidate.
    """
    if walk.kept >= session.config.top_k or walk.attempts < session.config.max_attempts:
        return
    session.guardrail_log.append({
        "iteration": iteration,
        "branch": branch.recipe.key or "(root)",
        "noted": ["attempt_cap"],
        "reason": (
            f"branch stopped after {walk.attempts} attempts (cap "
            f"{session.config.max_attempts}) holding only {walk.kept} of "
            f"{session.config.top_k} live candidates; guardrails or the "
            "executor rejected nearly everything tried on this image"
        ),
    })


def ingest_candidate(
    session: Session, *, iteration: int, parent_recipe: Recipe, parent, produced,
    source, action: Action, reference_fp: FingerprintData,
    limits: GuardrailLimits, candidate_path: str, save=None,
) -> Branch | None:
    """No-op check -> guardrails -> score. ``None`` means the candidate is out.

    This is the whole §7 filter and the scoring step, and it has exactly one
    implementation. ``advance`` reaches it having just produced ``produced``
    itself through an ``Executor``; the §2.5 batched path reaches it having
    loaded ``produced`` from a file PixInsight wrote. Nothing below can tell
    the difference, which is the point -- a second copy of this block would be
    free to drift from the first, and the drift would be invisible until a
    guardrail quietly stopped applying on one of the two paths.

    ``save`` is passed only by the caller that holds pixels nobody has written
    yet; PixInsight has already saved its own.
    """
    if np.array_equal(produced, parent):
        # A candidate identical to its parent has, by definition, learned
        # nothing: same pixels, same fingerprint, same distance -- but a
        # distinct recipe key, so `prune`'s deduplication cannot see it for what
        # it is. Left in, it ties with its parent's score, which on an
        # already-good image is the BEST score available, and takes a beam slot
        # having explored none of the space.
        #
        # The test is the PROPERTY (pixels unchanged), never the action's name.
        # `star_split` is the no-op that exists today, but only because the
        # numpy executor has no starless layer to route; the PixInsight
        # executor's star_split does change the image and must survive this
        # check. Matching on the string would hard-code one executor's
        # limitation into the loop.
        session.guardrail_log.append({
            "iteration": iteration, "action": action.key,
            "failed": ["no_op"],
            "reason": (
                "candidate was pixel-identical to its parent; it explored "
                "nothing and must not hold a beam slot"
            ),
        })
        return None

    verdicts = evaluate_guardrails(produced, parent, source, limits)

    # A guardrail may PASS and still report that it could not assess anything --
    # star_integrity does exactly that on a starless baseline. That reason has to
    # reach the caller, or the run cannot report "this guardrail did not actually
    # protect anything here" (§12: degraded paths surface). Recorded under
    # `noted`, never `failed`: it is an observation, not a discard, and the two
    # must stay distinguishable in the log. Recorded whatever the candidate's
    # fate, because "star integrity was never evaluated" is equally true of a
    # candidate some OTHER guardrail went on to reject.
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
        return None

    if save is not None:
        save(candidate_path, produced)
    # Kept on the Branch, not discarded. Scoring already had to fingerprint
    # these exact pixels; if this candidate survives into the beam, the next
    # iteration needs that same fingerprint to build its ranked menu, and used
    # to re-extract it from a re-read file.
    fingerprint = _measure(produced, session)
    distance = fingerprint_distance(reference_fp, fingerprint)
    return Branch(recipe=parent_recipe.extend(action), image_path=candidate_path,
                  distance=distance, fingerprint=fingerprint)


def _close_iteration(session: Session, candidates: list[Branch]) -> Session:
    """Prune -> update best -> check convergence. ``session.iteration`` is
    already the number of the iteration being closed."""
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
        session.best = best_candidate  # in-memory only; durability is Session.save (§2.1),
        # called by the sidecar between loop calls, not by the loop itself

    session.distance_history.append(session.best.distance)
    _check_convergence(session)
    return session


def advance(session: Session, executor: Executor, *, load, save,
            limits: GuardrailLimits = GuardrailLimits()) -> Session:
    """One iteration: expand every live branch, guardrail, score, prune.

    A branch reaches for ``config.top_k`` LIVE candidates, walking the ranked
    menu until it has them or until ``config.max_attempts`` actions have been
    tried -- whichever comes first (§3.3). It is deliberately not "attempt the
    first top_k actions and keep whatever survives": a discarded candidate would
    then cost search breadth as well as itself, and a guardrail that narrows the
    search is a score term wearing a different hat (§7 forbids exactly that).
    Measured on the flattened fixture: the first three ranked actions are two
    guardrail trips and one move that climbs, while the action that actually
    closes the gap -- ``local_contrast@2"`` -- ranks fifth. Stopping at three
    proposals means never reaching it, and the loop declines on an image it could
    plainly have fixed.

    Nor is it "retry until top_k survive, however long that takes": at the 1600px
    proxy that is up to 66 attempts and ~86s per iteration against ~38s for the
    bounded retry (§3.3, re-measured 2026-07-28), and a cost that varies with how
    often guardrails happen to trip is a cost nobody can budget for. When the cap
    binds the branch simply contributes fewer than ``top_k``
    candidates -- legitimate, since the beam already tolerates shrinking and an
    empty beam is itself a convergence condition -- and the event is LOGGED. A
    branch that spent every attempt without filling its slots means the
    guardrails are rejecting nearly everything on that image, which is worth
    reporting; truncating silently would read as "explored fully" when it was
    not (§12).

    This is the OFFLINE entry point, driving a synchronous ``Executor`` one
    candidate at a time. §2.5's batched entry point (:func:`step_batch`) issues
    several instructions at once and ingests them a round trip later; both run
    the same proposer, the same :func:`ingest_candidate`, the same ``_Walk``
    bookkeeping and the same :func:`_close_iteration`.
    """
    if session.converged:
        return session

    reference_fp = FingerprintData.from_dict(session.reference_fp)
    source = load(session.proxy_path)
    candidates: list[Branch] = []
    iteration = session.iteration + 1

    for branch in session.branches:
        parent = load(branch.image_path)
        actions = _ranked_menu(
            reference_fp, _branch_fingerprint(branch, session, load=load),
            applied_kinds=branch.recipe.applied_kinds, n_scales=session.n_scales,
        )

        walk = _Walk()
        for action in actions:
            if not walk.may_issue(session.config):
                break
            # Counted here, before the executor runs: an attempt is an action
            # this branch spent, whatever became of it. Every path below costs
            # at least an executor call, and the bound exists to cap work.
            walk.attempts += 1

            try:
                produced = executor.apply(
                    parent, action, pixel_scale_arcsec=session.pixel_scale_arcsec
                )
            except Exception as exc:
                _log_executor_failure(session, iteration, action.key,
                                      f"{type(exc).__name__}: {exc}")
                continue

            candidate = ingest_candidate(
                session, iteration=iteration, parent_recipe=branch.recipe,
                parent=parent, produced=produced, source=source, action=action,
                reference_fp=reference_fp, limits=limits,
                candidate_path=f"{session.work_dir}/cand-{iteration}-{len(candidates)}.png",
                save=save,
            )
            if candidate is None:
                continue
            candidates.append(candidate)
            walk.kept += 1

        _log_attempt_cap(session, iteration, branch, walk)

    session.iteration = iteration
    return _close_iteration(session, candidates)


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


# ---------------------------------------------------------------------------
# §2.5: the batched protocol. PixInsight applies the pixels; the sidecar
# measures, guardrails, scores and prunes candidates it never produced.
#
#   PJSR                          sidecar (spawned per call)
#   ----                          -------------------------
#   optimize_begin  ---------->   load image, resolve reference, measure baseline
#                   <----------   session_id + instruction BATCH 1
#   apply N processes in PI
#   save N candidates
#   optimize_step   ---------->   measure all N, guardrail, score, prune beam
#                   <----------   BATCH 2  |  or  converged + recipe
#
# A batch cannot know up front how many of its candidates will survive --
# liveness is knowable only AFTER guardrails run on produced pixels -- so an
# iteration whose guardrails trip needs a SUPPLEMENTARY batch. "One batch per
# iteration" is the nominal case, not an invariant. `max_attempts` bounds the
# attempts across all batches of one iteration, never per batch.
# ---------------------------------------------------------------------------


def _open_iteration(session: Session) -> None:
    """Start a fresh iteration: no candidates, every branch at menu position 0."""
    session.candidates = []
    session.pending = []
    session.cursors = [
        {"branch_key": b.recipe.key, "cursor": 0, "kept": 0, "attempts": 0}
        for b in session.branches
    ]


def _cursor_for(session: Session, index: int, branch: Branch) -> dict:
    """The persisted walk state for ``branch``, checked against it.

    ``cursors`` is parallel to ``branches`` by construction. Verified rather
    than assumed: a desync would silently hand one branch another's cursor,
    which reads as a normal run but searches the wrong menu positions.
    """
    cursor = session.cursors[index]
    if cursor["branch_key"] != branch.recipe.key:
        raise ValueError(
            f"session {session.session_id!r} is inconsistent: branch {index} has "
            f"recipe key {branch.recipe.key!r} but its cursor records "
            f"{cursor['branch_key']!r}. The in-flight iteration cannot be resumed."
        )
    return cursor


def _instruction(session: Session, *, iteration: int, branch_index: int,
                 branch: Branch, position: int, action: Action) -> dict:
    """One thing for PixInsight to do, and everything needed to reassociate it.

    ``process``/``params`` come from :func:`pixinsight_step` -- the same
    rendering the §12 audit log reports -- so the instruction cannot describe a
    different process than the recipe will claim was run. ``action`` carries the
    canonical Action serialization, which is what lets the next sidecar rebuild
    the exact Action rather than parse it back out of the PI parameters.

    ``instruction_id`` is derived from (iteration, branch, menu position), all
    of which are already unique within an open iteration, so ids stay stable and
    collision-free across supplementary batches without a counter to persist.
    """
    step = pixinsight_step(action)
    return {
        "instruction_id": f"it{iteration}-br{branch_index}-ac{position}",
        "branch_key": branch.recipe.key,
        "action_key": action.key,
        "action": action_to_dict(action),
        "process": step["process"],
        "params": step["params"],
        "parent_path": branch.image_path,
        "candidate_path": (
            f"{session.work_dir}/cand-{iteration}-{branch_index}-{position}"
            f"{session.candidate_suffix}"
        ),
    }


def _plan_batch(session: Session, *, load) -> list[dict]:
    """The next instruction batch for the OPEN iteration, or ``[]`` if none.

    Each branch issues ``min(top_k - kept, max_attempts - attempts)`` actions --
    exactly what ``advance`` would issue before it next needed to know whether
    anything survived. The ranked menu is recomputed rather than persisted:
    ``propose_actions`` is deterministic given (parent fingerprint, reference
    fingerprint, ``applied_kinds``), so what has to survive the round trip is
    those inputs and the branch's POSITION in the result -- never the menu
    itself, which would put a second copy of ``propose.py``'s output on disk,
    free to go stale the moment the proposer changed.

    The parent fingerprint is one of those inputs, and it is now carried on the
    ``Branch`` (see ``beam.Branch.fingerprint``) rather than re-extracted from
    the parent image on every batch. That matters most HERE: an iteration whose
    guardrails trip needs supplementary batches, each planned by a fresh sidecar
    process, and each one used to re-read and re-fingerprint the same unchanged
    parent.
    """
    reference_fp = FingerprintData.from_dict(session.reference_fp)
    iteration = session.iteration + 1
    instructions: list[dict] = []

    for index, branch in enumerate(session.branches):
        cursor = _cursor_for(session, index, branch)
        walk = _Walk(kept=cursor["kept"], attempts=cursor["attempts"])
        if not walk.may_issue(session.config):
            continue

        actions = _ranked_menu(
            reference_fp, _branch_fingerprint(branch, session, load=load),
            applied_kinds=branch.recipe.applied_kinds, n_scales=session.n_scales,
        )
        # `kept` cannot advance during planning -- nothing has been produced yet
        # -- so the batch is bounded explicitly rather than by re-testing
        # `may_issue` as `advance` does after each candidate. Issuing more than
        # `top_k - kept` would over-attempt: if every one of them survived, the
        # branch would hold more live candidates than a beam slot allows and the
        # surplus attempts would have been spent for nothing.
        budget = min(session.config.top_k - walk.kept,
                     session.config.max_attempts - walk.attempts)
        for _ in range(budget):
            position = cursor["cursor"]
            if position >= len(actions):
                break  # the menu is exhausted; the cap is not what stopped us
            instructions.append(_instruction(
                session, iteration=iteration, branch_index=index, branch=branch,
                position=position, action=actions[position],
            ))
            cursor["cursor"] = position + 1
            # Counted when the instruction is ISSUED, not when it succeeds --
            # the same semantic `advance` uses, for the same reason: an attempt
            # is an action this branch spent, whatever became of it.
            walk.attempts += 1

        cursor["attempts"] = walk.attempts

    session.pending = instructions
    return instructions


def _produced_index(produced) -> dict[str, str]:
    """``[{instruction_id, path}, ...]`` -> ``{instruction_id: path}``, loudly.

    A duplicate id means PixInsight reported two files for one instruction; the
    sidecar has no basis for choosing between them, and quietly keeping the last
    would score a candidate the caller may not have meant (§12).
    """
    index: dict[str, str] = {}
    for entry in produced:
        instruction_id = entry["instruction_id"]
        if instruction_id in index:
            raise ValueError(
                f"instruction {instruction_id!r} was reported twice, as "
                f"{index[instruction_id]!r} and {entry['path']!r}"
            )
        index[instruction_id] = entry["path"]
    return index


def _ingest_batch(session: Session, produced, *, load,
                  limits: GuardrailLimits) -> None:
    """Score everything PixInsight came back with, into the open iteration."""
    by_id = _produced_index(produced)
    issued = {inst["instruction_id"] for inst in session.pending}
    unknown = sorted(set(by_id) - issued)
    if unknown:
        # Not a dead candidate but a desync between PixInsight and the session:
        # the two disagree about what run they are on. Surfaced, because
        # ingesting a candidate off an instruction this session never issued
        # would attribute it to the wrong branch and the wrong parent.
        raise ValueError(
            f"session {session.session_id!r} was handed results for instructions "
            f"it never issued: {unknown}. Issued this batch: {sorted(issued)}."
        )

    reference_fp = FingerprintData.from_dict(session.reference_fp)
    source = load(session.proxy_path)
    iteration = session.iteration + 1
    by_key = {b.recipe.key: (i, b) for i, b in enumerate(session.branches)}
    parents: dict[str, object] = {}

    for inst in session.pending:
        if inst["branch_key"] not in by_key:
            raise ValueError(
                f"session {session.session_id!r} has an in-flight instruction "
                f"{inst['instruction_id']!r} for branch {inst['branch_key']!r}, "
                f"which is not in the beam ({sorted(by_key)}). The iteration "
                "cannot be resumed against this session file."
            )
        index, branch = by_key[inst["branch_key"]]
        cursor = _cursor_for(session, index, branch)

        path = by_id.get(inst["instruction_id"])
        if path is None:
            # PixInsight was told to make this candidate and did not. That is an
            # executor failure for this candidate ONLY (§12) -- the attempt is
            # already spent, and the branch goes on to ask for a replacement.
            _log_executor_failure(
                session, iteration, inst["action_key"],
                f"PixInsight reported no candidate for instruction "
                f"{inst['instruction_id']!r} ({inst['process']})",
            )
            continue

        try:
            candidate_rgb = load(path)
            if inst["parent_path"] not in parents:
                parents[inst["parent_path"]] = load(inst["parent_path"])
        except Exception as exc:
            _log_executor_failure(
                session, iteration, inst["action_key"],
                f"could not read {path!r} for instruction "
                f"{inst['instruction_id']!r}: {type(exc).__name__}: {exc}",
            )
            continue

        candidate = ingest_candidate(
            session, iteration=iteration, parent_recipe=branch.recipe,
            parent=parents[inst["parent_path"]], produced=candidate_rgb,
            source=source, action=action_from_dict(inst["action"]),
            reference_fp=reference_fp, limits=limits, candidate_path=path,
        )
        if candidate is None:
            continue
        session.candidates.append(candidate)
        cursor["kept"] += 1

    session.pending = []


def _close_iteration_batched(session: Session) -> None:
    """End the open iteration: report any bound branches, then prune."""
    iteration = session.iteration + 1
    for index, branch in enumerate(session.branches):
        cursor = _cursor_for(session, index, branch)
        _log_attempt_cap(session, iteration, branch,
                         _Walk(kept=cursor["kept"], attempts=cursor["attempts"]))

    candidates = session.candidates
    session.candidates = []
    session.cursors = []
    session.iteration = iteration
    _close_iteration(session, candidates)


def begin_batch(session: Session, *, load) -> list[dict]:
    """Open the first iteration and return its instruction batch."""
    _open_iteration(session)
    return _plan_batch(session, load=load)


def step_batch(session: Session, produced, *, load,
               limits: GuardrailLimits = GuardrailLimits()) -> list[dict]:
    """Ingest what PixInsight produced; return the next batch, or ``[]``.

    ``[]`` means converged -- there is nothing further to apply and
    :func:`outcome` is the answer. A non-empty return may be a SUPPLEMENTARY
    batch for the iteration still open (some candidates were discarded and the
    branch has slots and attempts left) or the first batch of the next one.
    """
    if session.converged:
        return []

    _ingest_batch(session, produced, load=load, limits=limits)

    instructions = _plan_batch(session, load=load)
    while not instructions and not session.converged:
        _close_iteration_batched(session)
        if session.converged:
            break
        _open_iteration(session)
        instructions = _plan_batch(session, load=load)
    return instructions


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
