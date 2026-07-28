"""The batched sidecar protocol (spec §2.5).

In production the sidecar NEVER applies pixels. PixInsight does. The sidecar
measures, guardrails, scores and prunes candidates it has only been handed the
paths of. These tests play PixInsight's role -- they apply ``NumpyExecutor``
themselves and write the files -- which is the structural proof that the seam is
real rather than a server-side loop wearing a batched costume.
"""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

import autocontrast.optimize.loop as loop
from autocontrast.eval.degrade import flatten
from autocontrast.fingerprint.extract import extract
from autocontrast.io.loaders import load_image
from autocontrast.optimize.beam import BeamConfig
from autocontrast.optimize.executors.numpy_exec import NumpyExecutor
from autocontrast.optimize.recipe import action_from_dict
from autocontrast.optimize.session import Session, session_path
from autocontrast.sidecar import handle_request

SCALE, PSF, NSCALES, MAX_DIM = 1.0, 2.0, 7, 1600

# Everything an instruction must carry: enough for PixInsight to execute one
# action and save one candidate, enough for the sidecar to reassociate it.
_INSTRUCTION_FIELDS = {
    "instruction_id", "branch_key", "action_key", "action",
    "process", "params", "parent_path", "candidate_path",
}


def _reference_image(h=160, w=160, seed=11):
    """A deep-looking render: structure across several scales plus color.

    Deliberately noisy at the finest scale -- the candidates in these tests make
    an 8-bit round trip through disk, and a fixture whose own finest-plane sigma
    was near zero would see quantization dominate the noise guardrail.
    """
    rng = np.random.default_rng(seed)
    field = rng.normal(0, 1, (h, w))
    layered = sum(np.roll(field, k, axis=0) / (k + 1) for k in (1, 2, 4, 8, 16))
    layered = (layered - layered.min()) / (np.ptp(layered) + 1e-12)
    return np.clip(np.stack([layered, layered * 0.85, layered * 1.1], axis=-1), 0.02, 0.98)


def _write_png(path, rgb):
    Image.fromarray((np.clip(rgb, 0, 1) * 255).astype(np.uint8)).save(str(path))


def _fixture(tmp_path, name="proxy.png", strength=0.6):
    """A flattened proxy on disk plus the reference fingerprint to chase."""
    ref = _reference_image()
    png = tmp_path / name
    _write_png(png, flatten(ref, strength=strength))
    ref_fp = extract(ref, pixel_scale_arcsec=SCALE, n_scales=NSCALES,
                     psf_fwhm_arcsec=PSF, palette_class="HOO")
    return png, ref_fp


def _begin(tmp_path, png, ref_fp, **extra):
    req = {
        "op": "optimize_begin", "image": str(png), "work_dir": str(tmp_path),
        "reference_id": "synthetic", "reference_fingerprint": ref_fp.to_dict(),
        "pixel_scale_arcsec": SCALE, "psf_fwhm_arcsec": PSF,
        "palette_class": "HOO", "n_scales": NSCALES, "max_dim": MAX_DIM,
    }
    req.update(extra)
    return handle_request(req)


def _step(tmp_path, session_id, png, produced):
    return handle_request({
        "op": "optimize_step", "work_dir": str(tmp_path), "session_id": session_id,
        "image": str(png), "produced": produced,
    })


def _play_pixinsight(instructions, executor=None, skip=()):
    """Do what PixInsight does: apply the named process, save the candidate.

    ``skip`` names instruction ids to leave unproduced, standing in for a PI-side
    process failure.
    """
    executor = executor or NumpyExecutor()
    produced = []
    for inst in instructions:
        if inst["instruction_id"] in skip:
            continue
        parent = load_image(inst["parent_path"], max_dim=MAX_DIM)
        out = executor.apply(parent, action_from_dict(inst["action"]),
                             pixel_scale_arcsec=SCALE)
        _write_png(inst["candidate_path"], out)
        produced.append({"instruction_id": inst["instruction_id"],
                         "path": inst["candidate_path"]})
    return produced


def _drive_to_iteration(tmp_path, session_id, png, instructions, n, executor=None):
    """Step until ``n`` iterations have closed, playing PixInsight each time.

    An iteration takes MORE than one batch whenever guardrails discard
    candidates, and on this fixture two of the first three ranked actions do
    (hue_invention and noise_floor). A test written as "one step closes one
    iteration" would only ever assert against the nominal case.

    Returns the last result, every candidate path the test wrote, and the batch
    still outstanding.
    """
    written: list[str] = []
    result = None
    while instructions:
        produced = _play_pixinsight(instructions, executor)
        written.extend(p["path"] for p in produced)
        resp = _step(tmp_path, session_id, png, produced)
        assert resp["ok"] is True, resp.get("error")
        result = resp["result"]
        instructions = result["instructions"]
        if result["iterations"] >= n:
            break
    return result, written, instructions


# --------------------------------------------------------------------------
# 1. The ops exist and are discoverable.
# --------------------------------------------------------------------------

def test_unknown_op_lists_the_optimizer_ops():
    resp = handle_request({"op": "frobnicate"})
    assert resp["ok"] is False
    assert "optimize_begin" in resp["error"]
    assert "optimize_step" in resp["error"]


# --------------------------------------------------------------------------
# 2. begin: a resumable session on disk and a first batch to execute.
# --------------------------------------------------------------------------

def test_optimize_begin_writes_a_resumable_session_and_a_first_batch(tmp_path):
    png, ref_fp = _fixture(tmp_path)
    resp = _begin(tmp_path, png, ref_fp)
    assert resp["ok"] is True, resp.get("error")
    result = resp["result"]

    json.dumps(result)  # the bridge serializes it; it must be pure JSON
    assert result["reference_id"] == "synthetic"
    assert result["baseline_distance"] > 0
    assert result["converged"] is False
    assert result["iteration"] == 1
    assert result["instructions"], "nothing for PixInsight to do"

    path = session_path(tmp_path, result["session_id"])
    assert path.exists(), "a crashed run must leave an inspectable session file"
    session = Session.load(path)
    assert session.pending, "the in-flight batch was not persisted"
    assert len(session.pending) == len(result["instructions"])

    for inst in result["instructions"]:
        assert set(inst) >= _INSTRUCTION_FIELDS
        assert inst["parent_path"] == str(png)
        assert inst["process"], "PixInsight was told no process to run"
        assert inst["candidate_path"].startswith(str(tmp_path))
    ids = [i["instruction_id"] for i in result["instructions"]]
    assert len(set(ids)) == len(ids), "instruction ids collide, so results cannot be reassociated"


def test_the_first_batch_never_exceeds_the_beam_width_of_the_root(tmp_path):
    """One branch reaching for top_k live candidates -- not the whole menu."""
    png, ref_fp = _fixture(tmp_path)
    resp = _begin(tmp_path, png, ref_fp)
    session = Session.load(session_path(tmp_path, resp["result"]["session_id"]))
    assert len(resp["result"]["instructions"]) == session.config.top_k


def test_an_unreadable_candidate_suffix_is_refused_at_begin_not_misdiagnosed_later(tmp_path):
    """`.xisf` is PixInsight's native format and the obvious choice for a PJSR
    caller to pick -- but the sidecar reads every candidate back through
    `load_image`, which cannot open it. Left unvalidated, every candidate would
    raise inside `_ingest_batch`, every branch would be discarded, and the run
    would report "guardrails or executor failure" for what is really one bad
    config field. This must fail immediately, at `optimize_begin`, naming the
    offending suffix."""
    png, ref_fp = _fixture(tmp_path)
    resp = _begin(tmp_path, png, ref_fp, candidate_suffix=".xisf")
    assert resp["ok"] is False
    assert ".xisf" in resp["error"]
    assert "candidate_suffix" in resp["error"]
    # No session should be left behind implying a run is in flight.
    assert not any(tmp_path.glob("*.json"))


def test_the_default_candidate_suffix_still_works(tmp_path):
    """The `.png` default must not regress: PixInsight can save it and the
    sidecar can read it back."""
    png, ref_fp = _fixture(tmp_path)
    resp = _begin(tmp_path, png, ref_fp)
    assert resp["ok"] is True, resp.get("error")
    assert resp["result"]["instructions"][0]["candidate_path"].endswith(".png")


# --------------------------------------------------------------------------
# 3. The key structural proof: the sidecar scores what it did not produce.
# --------------------------------------------------------------------------

def test_the_sidecar_scores_candidates_it_did_not_produce(tmp_path):
    png, ref_fp = _fixture(tmp_path)
    first = _begin(tmp_path, png, ref_fp)["result"]
    sid = first["session_id"]

    result, written, next_batch = _drive_to_iteration(
        tmp_path, sid, png, first["instructions"], 1)
    assert written, "the test failed to play PixInsight, so nothing is proven"
    json.dumps(result)

    assert result["iterations"] == 1, "the iteration did not close"
    assert next_batch, "no batch for the next iteration"
    assert result["iteration"] == 2

    session = Session.load(session_path(tmp_path, sid))
    assert session.branches, "nothing survived, so nothing was scored"
    scored = {b.image_path for b in session.branches}
    # Every surviving branch points at a file the TEST wrote. The sidecar never
    # applied a pixel.
    assert scored <= set(written)
    assert all(Path(p).exists() for p in scored)
    assert all(len(b.recipe.actions) == 1 for b in session.branches)
    # Distances were really measured off those files, not copied from the parent.
    assert all(b.distance != session.baseline_distance for b in session.branches)
    assert len(session.distance_history) == 2
    # The next batch descends from the candidates, not from the proxy.
    assert {i["parent_path"] for i in next_batch} <= set(written)


def test_a_passing_guardrail_that_assessed_nothing_is_still_logged(tmp_path):
    """Requirement (A) survives the batched path: PASS-with-a-reason is `noted`,
    distinguished from a discard by FIELD, never by matching on the string."""
    png, ref_fp = _fixture(tmp_path)
    first = _begin(tmp_path, png, ref_fp)["result"]
    _step(tmp_path, first["session_id"], png, _play_pixinsight(first["instructions"]))

    session = Session.load(session_path(tmp_path, first["session_id"]))
    noted = [e for e in session.guardrail_log if "star_integrity" in e.get("noted", [])]
    assert noted, (
        "star_integrity passed while reporting it could not assess anything, "
        "and that reason never reached the log"
    )
    assert all("not assessed" in e["reason"] for e in noted)
    assert all("failed" not in e for e in noted)


class _NoOpFor:
    """PixInsight saves a candidate identical to its parent for one kind."""

    def __init__(self, kind):
        self.kind = kind
        self.inner = NumpyExecutor()

    def apply(self, rgb, action, *, pixel_scale_arcsec):
        if action.kind == self.kind:
            return np.array(rgb, copy=True)
        return self.inner.apply(rgb, action, pixel_scale_arcsec=pixel_scale_arcsec)


def test_a_no_op_candidate_from_pixinsight_does_not_take_a_beam_slot(tmp_path):
    """Requirement (B): the test is the PROPERTY (pixels unchanged), not a name.

    The kind is taken from the top of the ranking rather than hard-coded, and
    asserted not to be ``star_split`` -- so an implementation that discarded
    no-ops by matching that one string would let this candidate through.
    """
    png, ref_fp = _fixture(tmp_path)
    first = _begin(tmp_path, png, ref_fp)["result"]
    kind = action_from_dict(first["instructions"][0]["action"]).kind
    assert kind != "star_split", "pick a fixture where the name rule would differ"

    produced = _play_pixinsight(first["instructions"], _NoOpFor(kind))
    resp = _step(tmp_path, first["session_id"], png, produced)
    assert resp["ok"] is True, resp.get("error")

    session = Session.load(session_path(tmp_path, first["session_id"]))
    no_ops = [e for e in session.guardrail_log if "no_op" in e.get("failed", [])]
    assert no_ops, "the no-op candidate vanished with no record"
    assert any(kind in e["action"] for e in no_ops)
    assert all("identical" in e["reason"] for e in no_ops)
    assert not any(
        a.kind == kind
        for b in session.branches + session.candidates
        for a in b.recipe.actions
    )


# --------------------------------------------------------------------------
# 4 & 5. Refusals that protect a run's progress.
# --------------------------------------------------------------------------

def test_stepping_a_missing_session_is_a_loud_error(tmp_path):
    resp = _step(tmp_path, "no-such-session", tmp_path / "proxy.png", [])
    assert resp["ok"] is False
    assert "no-such-session" in resp["error"]
    assert "FileNotFoundError" in resp["error"]


def test_stepping_against_a_different_image_is_refused(tmp_path):
    png, ref_fp = _fixture(tmp_path)
    other = tmp_path / "other.png"
    _write_png(other, _reference_image())
    first = _begin(tmp_path, png, ref_fp)["result"]

    resp = _step(tmp_path, first["session_id"], other, [])
    assert resp["ok"] is False
    assert str(other) in resp["error"]
    assert str(png) in resp["error"]
    assert "refusing to resume" in resp["error"]


def test_an_equivalent_looking_path_is_still_refused(tmp_path):
    """Requirement (C): `resume()` compares EXACTLY, and `optimize_step` goes
    through `resume()` rather than calling `Session.load` itself -- otherwise the
    guard is dead code at the one seam it exists for."""
    png, ref_fp = _fixture(tmp_path)
    first = _begin(tmp_path, png, ref_fp)["result"]
    equivalent = f"{tmp_path}/./proxy.png"

    resp = _step(tmp_path, first["session_id"], equivalent, [])
    assert resp["ok"] is False
    assert "refusing to resume" in resp["error"]


# --------------------------------------------------------------------------
# 6. A candidate PixInsight failed to produce.
# --------------------------------------------------------------------------

def test_an_unproduced_instruction_kills_the_candidate_not_the_run(tmp_path):
    png, ref_fp = _fixture(tmp_path)
    first = _begin(tmp_path, png, ref_fp)["result"]
    missing = first["instructions"][0]
    produced = _play_pixinsight(first["instructions"], skip={missing["instruction_id"]})

    resp = _step(tmp_path, first["session_id"], png, produced)
    assert resp["ok"] is True, resp.get("error")

    session = Session.load(session_path(tmp_path, first["session_id"]))
    failures = [e for e in session.guardrail_log if "executor" in e.get("failed", [])]
    assert failures, "PixInsight silently produced nothing and nobody said so"
    assert any(e["action"] == missing["action_key"] for e in failures)
    assert any(missing["instruction_id"] in e["reason"] for e in failures)
    # ...and the run carried on.
    assert session.branches
    assert session.converged is False
    assert resp["result"]["instructions"]


def test_a_candidate_file_that_cannot_be_read_is_an_executor_failure(tmp_path):
    """PixInsight reported a path that is not a readable image. One dead
    candidate, not a dead run (§12)."""
    png, ref_fp = _fixture(tmp_path)
    first = _begin(tmp_path, png, ref_fp)["result"]
    produced = _play_pixinsight(first["instructions"])
    corrupt = tmp_path / "corrupt.png"
    corrupt.write_text("not an image")
    produced[0]["path"] = str(corrupt)

    resp = _step(tmp_path, first["session_id"], png, produced)
    assert resp["ok"] is True, resp.get("error")
    session = Session.load(session_path(tmp_path, first["session_id"]))
    failures = [e for e in session.guardrail_log if "executor" in e.get("failed", [])]
    assert failures
    assert any(str(corrupt) in e["reason"] for e in failures)
    assert session.branches


def test_an_instruction_id_the_sidecar_never_issued_is_refused(tmp_path):
    """A desync between PixInsight and the session is not a dead candidate, it
    is a protocol violation -- surfaced rather than quietly ignored (§12)."""
    png, ref_fp = _fixture(tmp_path)
    first = _begin(tmp_path, png, ref_fp)["result"]
    produced = _play_pixinsight(first["instructions"])
    produced.append({"instruction_id": "ghost", "path": produced[0]["path"]})

    resp = _step(tmp_path, first["session_id"], png, produced)
    assert resp["ok"] is False
    assert "ghost" in resp["error"]


# --------------------------------------------------------------------------
# 7. The attempt cap bounds attempts across ALL batches of one iteration.
# --------------------------------------------------------------------------

def test_the_attempt_cap_bounds_attempts_across_supplementary_batches(tmp_path):
    """PixInsight produces nothing at all, so no batch can ever fill a slot.

    The branch must keep asking for supplementary batches until `max_attempts`
    is spent across ALL of them -- not per batch -- and must then say it stopped
    short (requirement D). Silent truncation would read as "explored fully".
    """
    png, ref_fp = _fixture(tmp_path)
    first = _begin(tmp_path, png, ref_fp)["result"]
    sid = first["session_id"]
    config = BeamConfig()

    issued = list(first["instructions"])
    batches = 1
    result = first
    while result["instructions"]:
        assert len(result["instructions"]) <= config.top_k
        result = _step(tmp_path, sid, png, [])["result"]
        if result["instructions"]:
            batches += 1
            issued.extend(result["instructions"])

    assert batches > 1, "no supplementary batch was ever issued"
    assert len(issued) == config.max_attempts, (
        f"{len(issued)} actions were issued against a cap of {config.max_attempts}"
    )
    assert len({i["instruction_id"] for i in issued}) == len(issued)

    session = Session.load(session_path(tmp_path, sid))
    caps = [e for e in session.guardrail_log if "attempt_cap" in e.get("noted", [])]
    assert len(caps) == 1, "the branch truncated silently"
    assert caps[0]["branch"] == "(root)"
    assert "failed" not in caps[0], "stopping early is not a discard"
    assert f"stopped after {config.max_attempts} attempts" in caps[0]["reason"]


def test_a_guardrail_trip_costs_no_search_breadth(tmp_path):
    """§3.3: a discarded candidate must not also cost the branch a beam slot.

    On this fixture two of the first three ranked actions trip a guardrail, so
    the first batch yields one live candidate out of three. A SUPPLEMENTARY
    batch must follow -- still inside iteration 1, still off the proxy -- and
    the iteration must ultimately close holding a full beam rather than one
    branch. "Attempt the first top_k actions and keep whatever survives" would
    let the §7 filter narrow the search, which §7 forbids.
    """
    png, ref_fp = _fixture(tmp_path)
    first = _begin(tmp_path, png, ref_fp)["result"]
    sid = first["session_id"]

    result = _step(tmp_path, sid, png,
                   _play_pixinsight(first["instructions"]))["result"]

    # Still inside iteration 1: a supplementary batch, not the next iteration.
    assert result["iterations"] == 0, "the iteration closed with discarded slots unfilled"
    assert result["iteration"] == 1
    assert result["instructions"], "the discarded candidates silently cost slots"
    assert {i["parent_path"] for i in result["instructions"]} == {str(png)}

    result, _written, _next = _drive_to_iteration(
        tmp_path, sid, png, result["instructions"], 1)
    assert result["iterations"] == 1
    session = Session.load(session_path(tmp_path, sid))
    assert len(session.branches) == session.config.width
    # ...and the guardrails really were the reason a supplement was needed.
    discards = [e for e in session.guardrail_log
                if e.get("failed") not in (None, ["no_op"], ["executor"])]
    assert discards


# --------------------------------------------------------------------------
# 8. Everything added to the session round-trips.
# --------------------------------------------------------------------------

def test_the_in_flight_batch_state_survives_a_session_round_trip(tmp_path):
    png, ref_fp = _fixture(tmp_path)
    first = _begin(tmp_path, png, ref_fp)["result"]
    path = session_path(tmp_path, first["session_id"])

    # Half-executed iteration: some candidates scored, a batch still in flight.
    _step(tmp_path, first["session_id"], png,
          _play_pixinsight(first["instructions"][:1]))

    session = Session.load(path)
    assert session.pending and session.cursors and session.candidates
    session.save(path)
    again = Session.load(path)

    assert again.pending == session.pending
    assert again.cursors == session.cursors
    assert again.max_dim == session.max_dim == MAX_DIM
    assert again.candidate_suffix == session.candidate_suffix
    assert [b.image_path for b in again.candidates] == \
           [b.image_path for b in session.candidates]
    assert [b.distance for b in again.candidates] == \
           [b.distance for b in session.candidates]
    assert [b.recipe.to_pixinsight_steps() for b in again.candidates] == \
           [b.recipe.to_pixinsight_steps() for b in session.candidates]


# --------------------------------------------------------------------------
# One search, two entry points.
# --------------------------------------------------------------------------

def test_both_entry_points_route_through_one_ingestion(tmp_path, monkeypatch):
    """`advance()` and the batched step must not hold two copies of the
    no-op/guardrail/score block.

    Patching the single implementation must starve BOTH. If either grew its own
    copy the patch would be inert for that one, and its branches would survive.
    """
    png, ref_fp = _fixture(tmp_path)
    first = _begin(tmp_path, png, ref_fp)["result"]
    produced = _play_pixinsight(first["instructions"])

    calls: list[str] = []

    def _refuse_everything(*args, **kwargs):
        calls.append(kwargs["action"].key)
        return None

    monkeypatch.setattr(loop, "ingest_candidate", _refuse_everything)

    # Batched entry point.
    resp = _step(tmp_path, first["session_id"], png, produced)
    assert resp["ok"] is True, resp.get("error")
    batched_calls = len(calls)
    assert batched_calls == len(produced), "the batched path has its own ingestion"

    # Offline entry point, same patch still in force.
    store = {str(png): load_image(png, max_dim=MAX_DIM)}
    session = loop.begin(
        source_path=str(png), proxy_path=str(png), reference_id="synthetic",
        reference_fp=ref_fp, work_dir=str(tmp_path), pixel_scale_arcsec=SCALE,
        psf_fwhm_arcsec=PSF, palette_class="HOO", n_scales=NSCALES,
        session_id="offline", load=lambda p: np.array(store[p], copy=True),
    )
    session = loop.advance(
        session, NumpyExecutor(),
        load=lambda p: np.array(store[p], copy=True),
        save=lambda p, a: store.__setitem__(p, np.array(a, copy=True)),
    )
    assert len(calls) > batched_calls, "advance() has its own ingestion"
    assert session.branches == [], "the patch did not actually govern advance()"


def test_the_batch_planner_uses_the_shared_ranked_menu(tmp_path, monkeypatch):
    """The planner must not rebuild `propose.py`'s menu -- starving the one
    ranking must starve the batch."""
    png, ref_fp = _fixture(tmp_path)
    store = {str(png): load_image(png, max_dim=MAX_DIM)}
    load = lambda p: np.array(store[p], copy=True)  # noqa: E731
    session = loop.begin(
        source_path=str(png), proxy_path=str(png), reference_id="synthetic",
        reference_fp=ref_fp, work_dir=str(tmp_path), pixel_scale_arcsec=SCALE,
        psf_fwhm_arcsec=PSF, palette_class="HOO", n_scales=NSCALES,
        session_id="menu", load=load,
    )
    assert loop.begin_batch(session, load=load), "baseline: a batch is produced"

    monkeypatch.setattr(loop, "_ranked_menu", lambda *a, **k: [])
    assert loop.begin_batch(session, load=load) == []


# --------------------------------------------------------------------------
# End to end: PixInsight drives the whole run through the sidecar.
# --------------------------------------------------------------------------

def test_a_full_run_driven_batch_by_batch_improves_a_flattened_image(tmp_path):
    png, ref_fp = _fixture(tmp_path)
    first = _begin(tmp_path, png, ref_fp, session_id="e2e")["result"]
    instructions = first["instructions"]

    result = first
    calls = 0
    while instructions:
        calls += 1
        assert calls < 200, "the batched protocol never terminated"
        result = _step(tmp_path, "e2e", png, _play_pixinsight(instructions))["result"]
        instructions = result["instructions"]

    assert result["converged"] is True
    assert result["convergence_reason"]
    assert result["improved"] is True
    assert result["distance"] < result["baseline_distance"]
    assert result["result_path"] != str(png)
    assert Path(result["result_path"]).exists()
    # The §12 audit artifact covers every action in the recipe.
    assert len(result["pixinsight_steps"]) == len(result["recipe"]["actions"]) >= 1
    assert all(s["process"] for s in result["pixinsight_steps"])


def test_stepping_a_converged_session_reports_the_outcome_without_new_work(tmp_path):
    png, ref_fp = _fixture(tmp_path)
    first = _begin(tmp_path, png, ref_fp, session_id="done")["result"]
    instructions = first["instructions"]
    result = first
    while instructions:
        result = _step(tmp_path, "done", png, _play_pixinsight(instructions))["result"]
        instructions = result["instructions"]

    again = _step(tmp_path, "done", png, [])
    assert again["ok"] is True
    assert again["result"]["instructions"] == []
    assert again["result"]["converged"] is True
    assert again["result"]["result_path"] == result["result_path"]
    assert again["result"]["distance"] == pytest.approx(result["distance"])
