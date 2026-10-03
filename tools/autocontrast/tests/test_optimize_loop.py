"""The loop (§6) and the fail-safe.

The fail-safe is structural: best-so-far is seeded with the INPUT, so an image
already at the valley floor sees every candidate score worse and the loop
declines by itself. These tests assert BOTH sides of that -- a flattened image is
measurably improved, and an already-good one is returned byte-identical.
"""

from __future__ import annotations

import numpy as np
import pytest

import autocontrast.optimize.loop as loop_mod
from autocontrast.eval.degrade import flatten
from autocontrast.fingerprint.extract import extract
from autocontrast.optimize.beam import BeamConfig
from autocontrast.optimize.guardrails import GuardrailLimits, detect_stars
from autocontrast.optimize.executors.numpy_exec import NumpyExecutor
from autocontrast.optimize.loop import (
    _ranked_menu, advance, begin, outcome, resume, run_to_convergence,
)
from autocontrast.optimize.propose import propose_actions
from autocontrast.optimize.session import Session, session_path

SCALE, PSF, NSCALES = 1.0, 2.0, 7

# Every convergence reason the loop is allowed to report (§6.3). A run that
# terminates for a reason outside this set has invented one.
_RECOGNIZED_REASONS = (
    "iteration cap",
    "deltaD below",
    "every branch was discarded",
)


def _reference_image(h=160, w=160, seed=11):
    """A deep-looking render: structure across several scales plus color."""
    rng = np.random.default_rng(seed)
    field = rng.normal(0, 1, (h, w))
    layered = sum(
        np.roll(field, k, axis=0) / (k + 1) for k in (1, 2, 4, 8, 16)
    )
    # np.ptp(), not layered.ptp(): the ndarray method was removed in NumPy 2.0.
    layered = (layered - layered.min()) / (np.ptp(layered) + 1e-12)
    return np.clip(np.stack([layered, layered * 0.85, layered * 1.1], axis=-1), 0.02, 0.98)


def _starry_image(h=160, w=160, seed=11, n_stars=40):
    """``_reference_image`` with point sources injected.

    The roll-sum field on its own is starless -- nothing in it survives the
    5-sigma threshold as a multi-pixel blob -- which is exactly why it serves as
    the "star integrity could not be assessed" fixture. This is its counterpart:
    the same field with real stars in it, so the absence of a `noted` entry can
    be shown to mean something. Both fixtures assert their star counts rather
    than trusting these docstrings.
    """
    base = _reference_image(h, w, seed)
    rng = np.random.default_rng(seed + 1)
    ys = rng.integers(4, h - 4, n_stars)
    xs = rng.integers(4, w - 4, n_stars)
    yy, xx = np.mgrid[0:h, 0:w]
    out = np.asarray(base, dtype=np.float64)
    for y, x in zip(ys, xs):
        out = out + 0.6 * np.exp(-((yy - y) ** 2 + (xx - x) ** 2) / (2 * 1.3**2))[..., None]
    return np.clip(out, 0.02, 0.98)


def _memory_io():
    """In-memory stand-ins for the disk round-trip the PI executor performs."""
    store: dict[str, np.ndarray] = {}

    def save(path, rgb):
        store[path] = np.array(rgb, copy=True)

    def load(path):
        return np.array(store[path], copy=True)

    return store, load, save


def _session_for(image, reference, store, load, save, **overrides):
    ref_fp = extract(reference, pixel_scale_arcsec=SCALE, n_scales=NSCALES,
                     psf_fwhm_arcsec=PSF, palette_class="HOO")
    save("/mem/proxy.png", image)
    kwargs = dict(
        source_path="/mem/src.png", proxy_path="/mem/proxy.png",
        reference_id="synthetic", reference_fp=ref_fp, work_dir="/mem",
        pixel_scale_arcsec=SCALE, psf_fwhm_arcsec=PSF, palette_class="HOO",
        n_scales=NSCALES, session_id="t1", load=load,
    )
    kwargs.update(overrides)
    return begin(**kwargs)


# --------------------------------------------------------------------------
# The two sides of the fail-safe.
# --------------------------------------------------------------------------

def test_a_flattened_image_is_measurably_improved():
    ref = _reference_image()
    flat = flatten(ref, strength=0.6)
    store, load, save = _memory_io()
    session = _session_for(flat, ref, store, load, save)

    done = run_to_convergence(session, NumpyExecutor(), load=load, save=save)
    result = outcome(done)

    assert result["improved"] is True
    assert result["distance"] < result["baseline_distance"]
    assert len(result["recipe"]["actions"]) >= 1
    # The mirror of the decline test: having improved, the loop must hand back a
    # DIFFERENT file, whose pixels actually differ from the input.
    assert result["result_path"] != done.proxy_path
    assert not np.array_equal(load(result["result_path"]), load(done.proxy_path))
    # The §12 audit artifact must cover every action, not a subset.
    assert len(result["pixinsight_steps"]) == len(result["recipe"]["actions"])


def test_the_reference_itself_is_declined():
    ref = _reference_image()
    store, load, save = _memory_io()
    session = _session_for(ref, ref, store, load, save)

    # The fixture must genuinely sit at the valley floor, or this test proves
    # nothing about declining -- it would just be a test of a bad reference.
    assert session.baseline_distance == pytest.approx(0.0, abs=1e-12), (
        f"fixture is not at the valley floor: D={session.baseline_distance}"
    )

    done = run_to_convergence(session, NumpyExecutor(), load=load, save=save)
    result = outcome(done)

    # D is a valley: the image is already at the floor, every move climbs.
    assert result["improved"] is False
    assert result["recipe"]["actions"] == []
    assert result["pixinsight_steps"] == []
    assert result["result_path"] == "/mem/proxy.png"
    assert result["distance"] == result["baseline_distance"]
    # The loop really did search -- it declined on the evidence, it did not
    # simply fail to run.
    assert done.iteration >= 1


def test_declining_returns_the_input_untouched():
    ref = _reference_image()
    store, load, save = _memory_io()
    session = _session_for(ref, ref, store, load, save)
    before = np.array(store["/mem/proxy.png"], copy=True)

    done = run_to_convergence(session, NumpyExecutor(), load=load, save=save)
    result = outcome(done)
    assert result["improved"] is False

    returned = load(result["result_path"])
    # Byte-identical, not merely "equal": a split-and-recombined approximation
    # of the input would pass array_equal on a lucky fixture but not this.
    assert returned.dtype == before.dtype
    assert returned.shape == before.shape
    assert returned.tobytes() == before.tobytes()


def test_a_climbing_candidate_never_becomes_best():
    """Every candidate off an already-good image scores WORSE than baseline.

    This is the mechanism behind the decline, asserted directly rather than
    inferred from the outcome: if some candidate did beat baseline, the decline
    above would be an accident of convergence timing rather than the valley.
    """
    ref = _reference_image()
    store, load, save = _memory_io()
    session = _session_for(ref, ref, store, load, save)

    session = advance(session, NumpyExecutor(), load=load, save=save)

    assert session.branches, "nothing was explored, so nothing was proven"
    assert all(b.distance > session.baseline_distance for b in session.branches)
    assert session.best.image_path == session.proxy_path
    assert session.best.recipe.actions == ()


# --------------------------------------------------------------------------
# Termination, honestly reported (§6.3).
# --------------------------------------------------------------------------

def test_iteration_cap_terminates_the_loop():
    ref = _reference_image()
    flat = flatten(ref, strength=0.6)
    store, load, save = _memory_io()
    session = _session_for(flat, ref, store, load, save)
    session.config = type(session.config)(iteration_cap=2)

    done = run_to_convergence(session, NumpyExecutor(), load=load, save=save)
    assert done.iteration <= 2
    assert done.converged
    assert "iteration cap (2) reached" == done.convergence_reason


def test_convergence_reason_is_always_reported():
    ref = _reference_image()
    store, load, save = _memory_io()
    session = _session_for(ref, ref, store, load, save)
    done = run_to_convergence(session, NumpyExecutor(), load=load, save=save)
    assert done.convergence_reason
    assert any(r in done.convergence_reason for r in _RECOGNIZED_REASONS)


def test_exhausting_every_branch_is_not_dressed_up_as_success():
    """Guardrails that discard everything must say so, and must decline."""
    ref = _reference_image()
    flat = flatten(ref, strength=0.6)
    store, load, save = _memory_io()
    session = _session_for(flat, ref, store, load, save)

    # No candidate may raise the noise floor at all -- every real action does.
    impossible = GuardrailLimits(noise_growth=-1.0)
    done = run_to_convergence(session, NumpyExecutor(), load=load, save=save,
                              limits=impossible)

    assert done.converged
    assert done.branches == []
    assert "every branch was discarded" in done.convergence_reason
    result = outcome(done)
    assert result["improved"] is False
    assert result["result_path"] == done.proxy_path
    assert result["guardrail_log"], "candidates were discarded with no record of why"
    # Every guardrail discard names the guardrail that did it. `no_op` and
    # `executor` entries are discards too, but not guardrail ones, so they are
    # excluded rather than folded in -- lumping them together would let a run
    # that discarded everything for the WRONG reason pass this assertion.
    guardrail_discards = [
        e for e in result["guardrail_log"]
        if "failed" in e and e["failed"] not in (["no_op"], ["executor"])
    ]
    assert guardrail_discards
    assert all("noise_floor" in e["failed"] for e in guardrail_discards)


def test_a_guardrail_violation_discards_rather_than_scores():
    """A violation is a filter, never a score term (§7).

    A candidate that trips a guardrail must not appear in the beam even when its
    distance would have made it the best of the iteration.
    """
    ref = _reference_image()
    flat = flatten(ref, strength=0.6)
    store, load, save = _memory_io()
    session = _session_for(flat, ref, store, load, save)

    strict = GuardrailLimits(noise_growth=-1.0)
    session = advance(session, NumpyExecutor(), load=load, save=save, limits=strict)
    assert session.branches == []

    # Same candidates, ordinary limits: they DO survive, so the emptiness above
    # is the guardrail's doing and not an empty proposal list.
    store2, load2, save2 = _memory_io()
    other = _session_for(flat, ref, store2, load2, save2)
    other = advance(other, NumpyExecutor(), load=load2, save=save2)
    assert other.branches


def test_an_executor_failure_kills_the_candidate_not_the_run():
    ref = _reference_image()
    flat = flatten(ref, strength=0.6)
    store, load, save = _memory_io()
    session = _session_for(flat, ref, store, load, save)
    session.config = BeamConfig(iteration_cap=2)

    class _Flaky:
        inner = NumpyExecutor()

        def apply(self, rgb, action, *, pixel_scale_arcsec):
            if action.kind == "local_contrast":
                raise RuntimeError("simulated PixInsight process failure")
            return self.inner.apply(rgb, action, pixel_scale_arcsec=pixel_scale_arcsec)

    done = run_to_convergence(session, _Flaky(), load=load, save=save)

    executor_entries = [e for e in done.guardrail_log if "executor" in e.get("failed", [])]
    assert executor_entries, "an executor failure went unrecorded"
    assert "RuntimeError: simulated PixInsight process failure" in executor_entries[0]["reason"]
    assert done.converged
    assert done.branches, "one dead candidate killed the whole run"


# --------------------------------------------------------------------------
# (A) A guardrail that PASSED without assessing anything must still be heard.
# --------------------------------------------------------------------------

def test_a_passing_guardrail_that_assessed_nothing_is_logged():
    starless = _reference_image()
    ref = _reference_image()
    store, load, save = _memory_io()
    session = _session_for(starless, ref, store, load, save)

    # Verify the fixture actually is what it claims -- a starless baseline is
    # the whole premise of this test.
    assert detect_stars(starless.mean(axis=-1)).count == 0, (
        "fixture has detectable stars, so star_integrity would assess normally"
    )

    session = advance(session, NumpyExecutor(), load=load, save=save)

    # Filtered by name, not merely "has a noted field": `noted` also carries the
    # attempt-cap event, and a test that accepted any noted entry would pass on
    # the wrong one.
    noted = [e for e in session.guardrail_log if "star_integrity" in e.get("noted", [])]
    assert noted, (
        "star_integrity passed while reporting it could not assess anything, "
        "and that reason never reached the log"
    )
    assert all("not assessed" in e["reason"] for e in noted)
    # Distinguishable from a discard: a noted entry is not a failure.
    assert all("failed" not in e for e in noted)


def test_nothing_is_noted_when_the_guardrail_really_did_its_job():
    """The other direction: with stars to protect, star_integrity assesses them
    and reports no reason, so the log stays quiet. Without this, the test above
    would pass against an implementation that noted every candidate."""
    starry = _starry_image()
    ref = _reference_image()
    store, load, save = _memory_io()
    session = _session_for(starry, ref, store, load, save)

    assert detect_stars(starry.mean(axis=-1)).count > 0, (
        "fixture is starless, so this proves nothing about a working guardrail"
    )

    session = advance(session, NumpyExecutor(), load=load, save=save)

    # Candidates really were guardrailed -- otherwise "no noted entries" is
    # vacuous, since nothing was evaluated.
    assert session.branches, "no candidate reached the guardrails"
    assert not [e for e in session.guardrail_log if "star_integrity" in e.get("noted", [])]


# --------------------------------------------------------------------------
# The bounded retry (§3.3): top_k live candidates OR max_attempts, whichever
# comes first.
# --------------------------------------------------------------------------

def _attempt_cap_events(session):
    return [e for e in session.guardrail_log if "attempt_cap" in e.get("noted", [])]


def test_max_attempts_defaults_to_three_times_top_k():
    """The bound is a named, visible tunable that tracks top_k rather than an
    inline literal that silently decouples from it."""
    assert BeamConfig().max_attempts == 9
    assert BeamConfig(top_k=5).max_attempts == 15
    # ...and it stays overridable.
    assert BeamConfig(top_k=3, max_attempts=4).max_attempts == 4


def test_max_attempts_survives_a_session_round_trip(tmp_path):
    ref = _reference_image(h=64, w=64)
    store, load, save = _memory_io()
    session = _session_for(ref, ref, store, load, save)
    session.config = BeamConfig(top_k=4, max_attempts=7)
    path = session_path(tmp_path, session.session_id)
    session.save(path)

    restored = resume(path, proxy_path="/mem/proxy.png")
    assert restored.config.max_attempts == 7
    assert restored.config.top_k == 4


def test_the_attempt_cap_binds_and_says_so():
    """When nearly everything is rejected the branch contributes fewer than
    top_k candidates -- without error -- and the truncation is surfaced."""
    ref = _reference_image()
    flat = flatten(ref, strength=0.6)
    store, load, save = _memory_io()
    session = _session_for(flat, ref, store, load, save)

    class _OnlyChromaWorks:
        """Everything but chroma is unavailable, so the branch spends its whole
        attempt budget to find a single live candidate."""

        inner = NumpyExecutor()

        def apply(self, rgb, action, *, pixel_scale_arcsec):
            if action.kind != "chroma":
                raise RuntimeError(f"{action.kind} unavailable")
            return self.inner.apply(rgb, action, pixel_scale_arcsec=pixel_scale_arcsec)

    session = advance(session, _OnlyChromaWorks(), load=load, save=save)

    # Fewer than top_k, and no exception: a bound branch is legitimate.
    assert 0 < len(session.branches) < session.config.top_k

    events = _attempt_cap_events(session)
    assert len(events) == 1, "the branch truncated silently"
    event = events[0]
    assert event["branch"] == "(root)"
    assert "failed" not in event, "stopping early is not a discard"
    assert str(session.config.max_attempts) in event["reason"]
    # The count of attempts actually spent must be the cap, not a guess.
    assert f"stopped after {session.config.max_attempts} attempts" in event["reason"]

    # The cap bounded the work: no more actions were tried than the cap allows.
    attempted = {e["action"] for e in session.guardrail_log if "action" in e}
    assert len(attempted) <= session.config.max_attempts


def test_the_attempt_cap_does_not_bind_on_an_ordinary_branch():
    """The other direction: a branch whose actions survive still gets its full
    top_k. A cap that always bound would satisfy the test above on its own."""
    ref = _reference_image()
    store, load, save = _memory_io()
    session = _session_for(ref, ref, store, load, save)

    session = advance(session, NumpyExecutor(), load=load, save=save)

    assert len(session.branches) == session.config.top_k
    assert not _attempt_cap_events(session)


def test_a_tight_cap_binds_where_a_loose_one_does_not():
    """Same image, same executor, same guardrails -- only the bound differs.

    This isolates the cap as the cause: if the flattened branch still fills its
    slots under max_attempts=2, the test above proves nothing about the bound.
    """
    ref = _reference_image()
    flat = flatten(ref, strength=0.6)

    store, load, save = _memory_io()
    loose = _session_for(flat, ref, store, load, save)
    loose = advance(loose, NumpyExecutor(), load=load, save=save)
    assert len(loose.branches) == loose.config.top_k
    assert not _attempt_cap_events(loose)

    store2, load2, save2 = _memory_io()
    tight = _session_for(flat, ref, store2, load2, save2)
    tight.config = BeamConfig(max_attempts=2)
    tight = advance(tight, NumpyExecutor(), load=load2, save=save2)

    # The first two ranked actions on this image are one climb and one guardrail
    # trip, so two attempts cannot yield three live candidates.
    assert len(tight.branches) < tight.config.top_k
    assert _attempt_cap_events(tight)


# --------------------------------------------------------------------------
# (B) A candidate identical to its parent has learned nothing.
# --------------------------------------------------------------------------

def _wide_session(image, reference, store, load, save):
    """top_k wide enough that the levelless actions -- including star_split --
    are actually proposed. With all component gaps at zero they sort last."""
    session = _session_for(image, reference, store, load, save)
    session.config = BeamConfig(top_k=6, width=3)
    return session


class _NoOpFor:
    """Delegates to NumpyExecutor except for ``kind``, which returns the input.

    Used to prove the no-op rule keys on the PROPERTY (pixels unchanged) and not
    on the string "star_split".
    """

    def __init__(self, kind: str):
        self.kind = kind
        self.inner = NumpyExecutor()

    def apply(self, rgb, action, *, pixel_scale_arcsec):
        if action.kind == self.kind:
            return np.array(rgb, copy=True)
        return self.inner.apply(rgb, action, pixel_scale_arcsec=pixel_scale_arcsec)


class _EffectiveStarSplit:
    """An executor where star_split genuinely changes pixels -- it must survive."""

    inner = NumpyExecutor()

    def apply(self, rgb, action, *, pixel_scale_arcsec):
        if action.kind == "star_split":
            return np.clip(np.asarray(rgb, dtype=np.float64) * 0.999, 0.0, 1.0)
        return self.inner.apply(rgb, action, pixel_scale_arcsec=pixel_scale_arcsec)


def test_a_no_op_candidate_does_not_take_a_beam_slot():
    ref = _reference_image()
    store, load, save = _memory_io()
    session = _wide_session(ref, ref, store, load, save)

    session = advance(session, NumpyExecutor(), load=load, save=save)

    # star_split is a no-op in the numpy executor: pixel-identical to its
    # parent, identical distance, distinct recipe key. Under a plain
    # append-and-prune it would be the BEST candidate of the iteration (its
    # distance equals the floor) and would hold a beam slot having explored
    # nothing.
    assert session.branches, "nothing survived, so the slot claim is untested"
    kinds = {a.kind for b in session.branches for a in b.recipe.actions}
    assert "star_split" not in kinds
    assert len(session.branches) == session.config.width

    discarded = [e for e in session.guardrail_log if "no_op" in e.get("failed", [])]
    assert discarded, "the no-op candidate vanished with no record"
    assert any("star_split" in e["action"] for e in discarded)
    assert all("identical" in e["reason"] for e in discarded)


def test_the_no_op_rule_is_a_property_not_a_name():
    """Both directions: a non-star_split no-op is discarded, and a star_split
    that genuinely changes pixels is not."""
    ref = _reference_image()

    store, load, save = _memory_io()
    session = _wide_session(ref, ref, store, load, save)
    session = advance(session, _NoOpFor("chroma"), load=load, save=save)
    no_ops = [e for e in session.guardrail_log if "no_op" in e.get("failed", [])]
    assert any("chroma" in e["action"] for e in no_ops), (
        "a chroma action that changed nothing was allowed to hold a slot"
    )

    store2, load2, save2 = _memory_io()
    other = _wide_session(ref, ref, store2, load2, save2)
    other = advance(other, _EffectiveStarSplit(), load=load2, save=save2)
    other_no_ops = [e for e in other.guardrail_log if "no_op" in e.get("failed", [])]
    assert not any("star_split" in e["action"] for e in other_no_ops), (
        "a star_split that really changed the image was discarded as a no-op"
    )


# --------------------------------------------------------------------------
# (C) Resuming against the wrong image.
# --------------------------------------------------------------------------

def test_resuming_against_a_different_proxy_is_refused(tmp_path):
    ref = _reference_image(h=64, w=64)
    store, load, save = _memory_io()
    session = _session_for(ref, ref, store, load, save)
    path = session_path(tmp_path, session.session_id)
    session.save(path)

    with pytest.raises(ValueError) as exc:
        resume(path, proxy_path="/mem/some-other-proxy.png")

    message = str(exc.value)
    assert "/mem/some-other-proxy.png" in message
    assert "/mem/proxy.png" in message


def test_resuming_against_the_same_proxy_succeeds(tmp_path):
    ref = _reference_image(h=64, w=64)
    store, load, save = _memory_io()
    session = _session_for(ref, ref, store, load, save)
    path = session_path(tmp_path, session.session_id)
    session.save(path)

    restored = resume(path, proxy_path="/mem/proxy.png")
    assert isinstance(restored, Session)
    assert restored.proxy_path == session.proxy_path
    assert restored.baseline_distance == pytest.approx(session.baseline_distance)


def test_resume_compares_exactly_not_by_resolution(tmp_path):
    """An equivalent-looking path is still a different path.

    The session records no image dimensions, so path identity is the only
    evidence available that the resumed image is the one the ``layer`` values
    were computed for. Normalizing the comparison would trade that evidence for
    a guess.
    """
    ref = _reference_image(h=64, w=64)
    store, load, save = _memory_io()
    session = _session_for(ref, ref, store, load, save)
    path = session_path(tmp_path, session.session_id)
    session.save(path)

    with pytest.raises(ValueError):
        resume(path, proxy_path="/mem/./proxy.png")


def test_ranked_menu_has_no_menu_construction_of_its_own(monkeypatch):
    """`_ranked_menu` must be nothing more than "the whole ranking" -- it must
    not know how the menu is built. It used to: a verbatim copy of
    `propose.py`'s ``palette_chroma_compatible`` + ``available_actions(...)``
    call, kept only to learn ``len(menu)``. That copy is dangerous because
    nothing enforces that it stays byte-identical to the real one -- if
    `propose.py` ever changes what it considers the menu, this module's own
    (unchanged) copy would compute a bound too small, and ``propose_actions``
    would silently truncate the tail of the ranking. On the measured fixture
    that tail is exactly where ``local_contrast@2"`` sits, ranked fifth, and it
    is the action that actually closes the gap.

    Simulated here by patching THIS module's own ``available_actions`` binding
    -- exactly the shape a reintroduced duplicate would take -- while leaving
    ``propose.py``'s binding (the real menu construction) untouched. If
    `_ranked_menu` delegates entirely to ``propose_actions(..., top_k=None)``,
    the patch is inert. If it reconstructs the menu itself, the patch starves
    it and the ranking it returns diverges from the real one.
    """
    ref = _reference_image()
    flat = flatten(ref, strength=0.6)
    ref_fp = extract(ref, pixel_scale_arcsec=SCALE, n_scales=NSCALES,
                      psf_fwhm_arcsec=PSF, palette_class="HOO")
    target_fp = extract(flat, pixel_scale_arcsec=SCALE, n_scales=NSCALES,
                         psf_fwhm_arcsec=PSF, palette_class="HOO")

    # `raising=False`: after the fix, loop.py has no `available_actions` name
    # of its own to patch at all -- that absence is part of what this proves.
    monkeypatch.setattr(loop_mod, "available_actions", lambda *a, **k: [], raising=False)

    via_wrapper = _ranked_menu(ref_fp, target_fp, applied_kinds=frozenset(), n_scales=NSCALES)
    via_direct = propose_actions(ref_fp, target_fp, applied_kinds=frozenset(),
                                  n_scales=NSCALES, top_k=None)

    assert len(via_wrapper) > 0
    assert via_wrapper == via_direct


def test_ranked_menu_matches_an_explicit_top_k_none_call():
    """`_ranked_menu` and ``propose_actions(..., top_k=None)`` are the same
    call under two names -- not two implementations that happen to agree."""
    ref = _reference_image()
    flat = flatten(ref, strength=0.6)
    ref_fp = extract(ref, pixel_scale_arcsec=SCALE, n_scales=NSCALES,
                      psf_fwhm_arcsec=PSF, palette_class="HOO")
    target_fp = extract(flat, pixel_scale_arcsec=SCALE, n_scales=NSCALES,
                         psf_fwhm_arcsec=PSF, palette_class="HOO")

    assert _ranked_menu(
        ref_fp, target_fp, applied_kinds=frozenset(), n_scales=NSCALES,
    ) == propose_actions(
        ref_fp, target_fp, applied_kinds=frozenset(), n_scales=NSCALES, top_k=None,
    )
