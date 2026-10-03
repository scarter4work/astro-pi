"""Task 16: `extract` got cheaper. Proof that it did not get DIFFERENT.

Every threshold in this project is calibrated against fingerprint values -- the
0.075/0.15 magnitude buckets (tertiles of 227 measured gaps), ``epsilon_improve``,
``epsilon``, the §7 guardrail limits, and the §10 exit criterion. Nothing in the
suite compares a fingerprint against a stored constant, so a fingerprint that
drifted would silently invalidate all of them and fail nothing. That is what these
tests are for.

The proof is deliberately not a golden file. A recorded number proves only that
today's code agrees with the day the number was recorded, and it goes stale the
first time anyone regenerates it. Instead the PRE-OPTIMIZATION composition is
recomputed here, from the single-image entry points that still exist and still do
their own starlet transform, and compared against what ``extract`` now returns from
one shared transform. Both paths are live code; the test compares them on real
cached renders at the real search-proxy size.

Exact equality is the assertion. Not ``allclose`` -- picking a tolerance here would
be a calibration decision, and §3.7 makes those the user's.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest

from autocontrast.fingerprint.energy import energy_spectrum
from autocontrast.fingerprint.extract import (
    CHROMA_BINS,
    CHROMA_EXTENT,
    FingerprintData,
    extract,
)
from autocontrast.fingerprint.color import rgb_to_lab
from autocontrast.fingerprint.metrics import (
    background_channel_balance,
    chroma_histogram,
    floor_to_peak_ratio,
    saturation_quantiles,
    structure_to_gradient_ratio,
    tonal_quantiles,
)
from autocontrast.io.loaders import load_image

# The real search proxy size (§2.5) and the ingest layer's real fingerprint depth.
PROXY_MAX_DIM = 1600
N_SCALES = 7

CACHE = Path(__file__).resolve().parents[1] / "data" / "discovery_cache"


def _cached_renders(limit: int = 4) -> list[Path]:
    """Real professional renders, largest first so the test exercises images that
    actually reach the 1600px proxy bound rather than small ones that don't."""
    if not CACHE.is_dir():
        return []
    found = sorted(CACHE.glob("*.jpg"), key=lambda p: p.stat().st_size, reverse=True)
    return found[:limit]


RENDERS = _cached_renders()

requires_cache = pytest.mark.skipif(
    not RENDERS,
    reason=(
        f"no cached renders under {CACHE}; this equivalence proof is only "
        "meaningful on real data and must not be faked with synthetic input"
    ),
)


def _extract_pre_optimization(
    rgb: np.ndarray, *, pixel_scale_arcsec: float, n_scales: int,
    psf_fwhm_arcsec: float, palette_class: str,
) -> FingerprintData:
    """`extract` exactly as it was composed before Task 16.

    Note the two independent calls -- ``energy_spectrum`` and
    ``structure_to_gradient_ratio`` each run their own ``starlet_transform`` of
    the same ``L`` at the same depth. That duplication is the 0.574s this task
    removed; reproducing it here is what makes the comparison a real before/after
    rather than the new code checked against itself.
    """
    lab = rgb_to_lab(rgb)
    L, a, b = lab[..., 0], lab[..., 1], lab[..., 2]
    chroma = np.hypot(a, b)

    return FingerprintData(
        energy=energy_spectrum(L, pixel_scale_arcsec=pixel_scale_arcsec, n_scales=n_scales),
        tonal_quantiles=tonal_quantiles(L),
        saturation_quantiles=saturation_quantiles(chroma),
        chroma_hist=chroma_histogram(a, b, bins=CHROMA_BINS, extent=CHROMA_EXTENT),
        background={
            "floor_to_peak_ratio": floor_to_peak_ratio(L),
            "structure_to_gradient_ratio": structure_to_gradient_ratio(L, n_scales=n_scales),
            "channel_balance": background_channel_balance(rgb),
        },
        pixel_scale_arcsec=pixel_scale_arcsec,
        psf_fwhm_arcsec=psf_fwhm_arcsec,
        palette_class=palette_class,
    )


def _assert_identical(old: FingerprintData, new: FingerprintData, what: str) -> None:
    """Bit-exact equality, component by component, so a failure names the drift."""
    np.testing.assert_array_equal(
        old.energy.centers_arcsec, new.energy.centers_arcsec,
        err_msg=f"{what}: energy spectrum angular centers moved (§2.2 scale convention)",
    )
    np.testing.assert_array_equal(
        old.energy.energy, new.energy.energy,
        err_msg=f"{what}: per-scale structural energy moved (§4.2 depth signature)",
    )
    np.testing.assert_array_equal(
        old.tonal_quantiles, new.tonal_quantiles, err_msg=f"{what}: tonal quantiles moved",
    )
    np.testing.assert_array_equal(
        old.saturation_quantiles, new.saturation_quantiles,
        err_msg=f"{what}: saturation quantiles moved",
    )
    np.testing.assert_array_equal(
        old.chroma_hist, new.chroma_hist, err_msg=f"{what}: chroma histogram moved",
    )
    assert old.background["floor_to_peak_ratio"] == new.background["floor_to_peak_ratio"], (
        f"{what}: floor-to-peak ratio moved"
    )
    assert (
        old.background["structure_to_gradient_ratio"]
        == new.background["structure_to_gradient_ratio"]
    ), f"{what}: structure-to-gradient ratio moved -- the shared starlet is not equivalent"
    np.testing.assert_array_equal(
        old.background["channel_balance"], new.background["channel_balance"],
        err_msg=f"{what}: background channel balance moved",
    )
    assert old.pixel_scale_arcsec == new.pixel_scale_arcsec
    assert old.psf_fwhm_arcsec == new.psf_fwhm_arcsec
    assert old.palette_class == new.palette_class


@requires_cache
@pytest.mark.parametrize("render", RENDERS, ids=lambda p: p.stem)
def test_extract_is_bit_identical_to_the_pre_optimization_composition(render):
    """The shared starlet transform changes the cost and nothing else."""
    rgb = load_image(render, max_dim=PROXY_MAX_DIM)

    args = dict(pixel_scale_arcsec=0.5, n_scales=N_SCALES,
                psf_fwhm_arcsec=2.0, palette_class="RGB")
    _assert_identical(
        _extract_pre_optimization(rgb, **args), extract(rgb, **args), render.name,
    )


@requires_cache
def test_equivalence_holds_across_the_scale_depths_the_loop_uses():
    """`n_scales` is what the shared transform is parameterized by, so the two
    paths must agree at every depth the loop can ask for -- not just the ingest
    default. A sharing bug that mismatched depth would show up here first."""
    rgb = load_image(RENDERS[0], max_dim=PROXY_MAX_DIM)

    for n_scales in (1, 3, 5, 7, 8):
        args = dict(pixel_scale_arcsec=1.22, n_scales=n_scales,
                    psf_fwhm_arcsec=2.5, palette_class="SHO")
        _assert_identical(
            _extract_pre_optimization(rgb, **args), extract(rgb, **args),
            f"{RENDERS[0].name} @ n_scales={n_scales}",
        )


@requires_cache
def test_fingerprint_survives_the_session_json_round_trip_exactly():
    """A branch's fingerprint is now carried on the `Branch` and persisted by
    `session.py`, so the §2.5 batched path rebuilds it from JSON rather than
    remeasuring. That is only behavior-preserving if the round trip is EXACT:
    the value feeds `propose_actions`, whose magnitude buckets are hard
    thresholds. A last-bit difference could flip an action's bucket and change
    which action the branch tries next."""
    import json

    rgb = load_image(RENDERS[0], max_dim=PROXY_MAX_DIM)
    fp = extract(rgb, pixel_scale_arcsec=0.5, n_scales=N_SCALES,
                 psf_fwhm_arcsec=2.0, palette_class="RGB")

    # Through real JSON text, not just to_dict/from_dict -- the session file is
    # what actually sits between two sidecar processes.
    revived = FingerprintData.from_dict(json.loads(json.dumps(fp.to_dict())))
    _assert_identical(fp, revived, "session JSON round trip")


# ---------------------------------------------------------------------------
# The carried-forward parent fingerprint (hypothesis (a)) is likewise an
# optimization, so it needs its own equivalence proof. The question here is not
# whether one array equals another but whether the LOOP still does the same
# thing: same actions proposed, same candidates kept, same recipe, same
# distances. Proved by running the loop twice over the same fixture -- once
# normally, once with the cache defeated so every branch is re-measured the way
# it was before Task 16 -- and comparing the outcomes.
# ---------------------------------------------------------------------------

import autocontrast.optimize.loop as loop_mod  # noqa: E402
from autocontrast.eval.degrade import flatten  # noqa: E402
from autocontrast.optimize.beam import Branch  # noqa: E402
from autocontrast.optimize.executors.numpy_exec import NumpyExecutor  # noqa: E402
from autocontrast.optimize.loop import (  # noqa: E402
    begin, begin_batch, outcome, run_to_convergence, step_batch,
)

_SCALE, _PSF, _NSCALES = 1.0, 2.0, 7


def _fixture_pair(h=160, w=160, seed=11):
    """A deep reference and a flattened version of it -- the same fixture shape
    `test_optimize_loop` runs the loop against, so this exercises a run that
    actually improves rather than one that declines on the first iteration."""
    rng = np.random.default_rng(seed)
    field = rng.normal(0, 1, (h, w))
    layered = sum(np.roll(field, k, axis=0) / (k + 1) for k in (1, 2, 4, 8, 16))
    layered = (layered - layered.min()) / (np.ptp(layered) + 1e-12)
    ref = np.clip(np.stack([layered, layered * 0.85, layered * 1.1], axis=-1), 0.02, 0.98)
    return ref, flatten(ref, strength=0.6)


def _memory_io():
    store: dict[str, np.ndarray] = {}

    def save(path, rgb):
        store[path] = np.array(rgb, copy=True)

    def load(path):
        return np.array(store[path], copy=True)

    return store, load, save


def _begin(image, reference, load, save):
    ref_fp = extract(reference, pixel_scale_arcsec=_SCALE, n_scales=_NSCALES,
                     psf_fwhm_arcsec=_PSF, palette_class="HOO")
    save("/mem/proxy.png", image)
    return begin(
        source_path="/mem/src.png", proxy_path="/mem/proxy.png",
        reference_id="synthetic", reference_fp=ref_fp, work_dir="/mem",
        pixel_scale_arcsec=_SCALE, psf_fwhm_arcsec=_PSF, palette_class="HOO",
        n_scales=_NSCALES, session_id="t1", load=load,
    )


def _forget_fingerprints(session):
    """Strip every carried fingerprint, forcing the pre-Task-16 re-measure.

    This is the `None` branch of `_branch_fingerprint` -- the same path a session
    file written before the field existed takes -- so defeating the cache also
    exercises that backward-compatible fallback.
    """
    session.branches = [
        Branch(recipe=b.recipe, image_path=b.image_path, distance=b.distance,
               alive=b.alive) for b in session.branches
    ]
    session.best = Branch(recipe=session.best.recipe, image_path=session.best.image_path,
                          distance=session.best.distance, alive=session.best.alive)
    return session


def test_carrying_the_parent_fingerprint_does_not_change_the_offline_run():
    ref, flat = _fixture_pair()

    _, load_a, save_a = _memory_io()
    cached = run_to_convergence(_begin(flat, ref, load_a, save_a), NumpyExecutor(),
                                load=load_a, save=save_a)

    _, load_b, save_b = _memory_io()
    session_b = _begin(flat, ref, load_b, save_b)
    # Defeat the cache at every iteration, not just the first: `ingest_candidate`
    # attaches a fingerprint to every candidate it builds, so stripping once
    # would only reproduce the old behavior for one iteration.
    original_advance = loop_mod.advance

    def measuring_advance(session, executor, **kw):
        return original_advance(_forget_fingerprints(session), executor, **kw)

    remeasured = session_b
    while not remeasured.converged:
        remeasured = measuring_advance(remeasured, NumpyExecutor(),
                                       load=load_b, save=save_b)

    # The whole outcome: recipe, distances, result path, convergence reason and
    # the §12 guardrail log. Not a spot check on the distance.
    assert outcome(remeasured) == outcome(cached)


def test_carrying_the_parent_fingerprint_does_not_change_the_batched_run():
    """The §2.5 path is where the saving is largest -- a supplementary batch used
    to re-measure the same parent in a third separate sidecar process -- so it
    gets its own proof rather than inheriting the offline one's."""
    ref, flat = _fixture_pair()
    executor = NumpyExecutor()

    def drive(defeat_cache: bool):
        store, load, save = _memory_io()
        session = _begin(flat, ref, load, save)
        instructions = begin_batch(session, load=load)
        while instructions:
            if defeat_cache:
                _forget_fingerprints(session)
            produced = []
            for inst in instructions:
                parent = load(inst["parent_path"])
                action = loop_mod.action_from_dict(inst["action"])
                try:
                    out = executor.apply(parent, action,
                                         pixel_scale_arcsec=session.pixel_scale_arcsec)
                except Exception:
                    continue
                save(inst["candidate_path"], out)
                produced.append({"instruction_id": inst["instruction_id"],
                                 "path": inst["candidate_path"]})
            instructions = step_batch(session, produced, load=load)
        return outcome(session)

    assert drive(defeat_cache=True) == drive(defeat_cache=False)


def _count_measures_in_one_advance(session, load, save) -> int:
    """`_measure` calls made by a single `advance`, with the module patched."""
    calls: list[int] = []
    original = loop_mod._measure

    def counting_measure(rgb, s):
        calls.append(1)
        return original(rgb, s)

    loop_mod._measure = counting_measure
    try:
        loop_mod.advance(session, NumpyExecutor(), load=load, save=save)
    finally:
        loop_mod._measure = original
    return len(calls)


def test_the_beam_actually_carries_a_fingerprint_forward():
    """Guards the SAVING, which the equivalence tests above cannot.

    If `ingest_candidate` ever stopped attaching the fingerprint, both
    equivalence tests would still pass -- identically, and identically slowly.
    So the cache being populated is asserted separately from it being correct,
    by counting extractions with and without it and pinning the difference to
    exactly one per live branch.
    """
    ref, flat = _fixture_pair()

    _, load_a, save_a = _memory_io()
    cached = _begin(flat, ref, load_a, save_a)
    assert cached.branches[0].fingerprint is not None, (
        "the root branch must carry the baseline measurement `begin` already took"
    )
    n_branches = len(cached.branches)
    with_cache = _count_measures_in_one_advance(cached, load_a, save_a)

    _, load_b, save_b = _memory_io()
    without_cache = _count_measures_in_one_advance(
        _forget_fingerprints(_begin(flat, ref, load_b, save_b)), load_b, save_b,
    )

    assert without_cache - with_cache == n_branches, (
        f"expected exactly one saved extraction per live branch ({n_branches}); "
        f"got {without_cache} measurements without the cache and {with_cache} with it"
    )
    assert all(b.fingerprint is not None for b in cached.branches), (
        "every branch surviving into the next iteration must carry its fingerprint, "
        "or the saving lasts one iteration"
    )
