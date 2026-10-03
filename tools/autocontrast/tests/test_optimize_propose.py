import subprocess
import sys

import numpy as np
from scipy.ndimage import gaussian_filter

from autocontrast.fingerprint.extract import extract
from autocontrast.optimize.actions import ONCE_ONLY
from autocontrast.optimize.propose import (
    GAP_GENTLE_MAX, GAP_MODERATE_MAX, _band_deficits, _level_for_gap,
    component_gaps, propose_actions,
)


def _fp(rgb, palette="HOO", psf=2.0, scale=1.0):
    return extract(rgb, pixel_scale_arcsec=scale, n_scales=7,
                   psf_fwhm_arcsec=psf, palette_class=palette)


def _textured(h=128, w=128, amp=0.2, seed=3):
    rng = np.random.default_rng(seed)
    base = 0.4 + amp * rng.normal(0, 1, (h, w)).cumsum(axis=0) / h
    base = np.clip(base, 0.05, 0.95)
    return np.stack([base, base * 0.95, base * 1.05], axis=-1)


def _flat(h=128, w=128):
    return np.full((h, w, 3), 0.45)


def _blurred_at_2arcsec(h=128, w=128, amp=0.2, seed=3, sigma=2.0):
    # Same texture as _textured(), but with fine detail smeared by a Gaussian
    # blur -- a stand-in for an under-sampled/soft capture. The blur pulls
    # energy out of the finest comparable band and redistributes it onto
    # coarser ones, so the deficit should localize to that one fine band, not
    # spread evenly or land on a coarse one.
    rng = np.random.default_rng(seed)
    base = 0.4 + amp * rng.normal(0, 1, (h, w)).cumsum(axis=0) / h
    base = np.clip(base, 0.05, 0.95)
    blurred = gaussian_filter(base, sigma=sigma)
    return np.stack([blurred, blurred * 0.95, blurred * 1.05], axis=-1)


def test_component_gaps_names_every_distance_component():
    ref, target = _fp(_textured()), _fp(_flat())
    gaps = component_gaps(ref, target)
    print(f"component gaps (textured ref vs. flat target): {gaps}")
    assert set(gaps) == {"spectrum", "tonal", "chroma", "background"}
    assert all(v >= 0 for v in gaps.values())
    # The textured/flat fixture pair exists to exercise the "structural help
    # first" case below; confirm it actually produces a spectrum gap that
    # dominates tonal and chroma, rather than assuming the fixture works.
    assert gaps["spectrum"] > 0.3
    assert gaps["spectrum"] > gaps["tonal"]
    assert gaps["spectrum"] > gaps["chroma"]


def test_proposals_are_deterministic():
    ref, target = _fp(_textured()), _fp(_flat())
    kwargs = dict(applied_kinds=frozenset(), n_scales=7, top_k=3)
    first = [a.key for a in propose_actions(ref, target, **kwargs)]
    second = [a.key for a in propose_actions(ref, target, **kwargs)]
    assert first == second


def test_proposals_are_deterministic_across_hash_seeds():
    # Calling twice in the same process only catches nondeterminism from
    # sources like random.shuffle -- it would NOT catch an implementation that
    # secretly relies on set/dict iteration order, since str hashing (and thus
    # unordered-container iteration order) is fixed for the lifetime of one
    # process but varies *across* processes unless PYTHONHASHSEED is pinned.
    # Run under two different hash seeds and require identical output to rule
    # that class of bug out.
    script = (
        "import numpy as np\n"
        "from autocontrast.fingerprint.extract import extract\n"
        "from autocontrast.optimize.propose import propose_actions\n"
        "rng = np.random.default_rng(3)\n"
        "base = 0.4 + 0.2 * rng.normal(0, 1, (128, 128)).cumsum(axis=0) / 128\n"
        "base = np.clip(base, 0.05, 0.95)\n"
        "textured = np.stack([base, base * 0.95, base * 1.05], axis=-1)\n"
        "flat = np.full((128, 128, 3), 0.45)\n"
        "ref = extract(textured, pixel_scale_arcsec=1.0, n_scales=7, psf_fwhm_arcsec=2.0, palette_class='HOO')\n"
        "target = extract(flat, pixel_scale_arcsec=1.0, n_scales=7, psf_fwhm_arcsec=2.0, palette_class='HOO')\n"
        "proposed = propose_actions(ref, target, applied_kinds=frozenset(), n_scales=7, top_k=8)\n"
        "print(','.join(a.key for a in proposed))\n"
    )
    import os

    outputs = []
    for seed in ("0", "1", "982451653"):
        env = dict(os.environ, PYTHONHASHSEED=seed)
        result = subprocess.run(
            [sys.executable, "-c", script],
            env=env, capture_output=True, text=True, check=True,
        )
        outputs.append(result.stdout.strip())
    assert outputs[0] == outputs[1] == outputs[2]
    assert outputs[0] != ""


def test_top_k_is_respected():
    ref, target = _fp(_textured()), _fp(_flat())
    assert len(propose_actions(ref, target, applied_kinds=frozenset(),
                               n_scales=7, top_k=3)) == 3


def test_top_k_none_returns_the_whole_ranking_untruncated():
    """``top_k=None`` is the explicit "no truncation" contract this function
    exposes for a caller that needs the ENTIRE ranking -- `loop.py`'s
    `_ranked_menu`, in service of SS3.3's bounded retry -- rather than a fixed
    slice of it. It must be the SAME ranking a bounded call sees a prefix of,
    not a second computation that only happens to agree with it.
    """
    ref, target = _fp(_textured()), _fp(_flat())
    full = propose_actions(ref, target, applied_kinds=frozenset(), n_scales=7, top_k=None)
    generous = propose_actions(ref, target, applied_kinds=frozenset(), n_scales=7, top_k=1000)
    small = propose_actions(ref, target, applied_kinds=frozenset(), n_scales=7, top_k=3)

    # top_k=1000 already exceeds the group count, so it too is "everything" --
    # confirming top_k=None isn't a distinct code path that merely matches by luck.
    assert full == generous
    assert full[:3] == small  # a bounded call is a strict PREFIX of the full ranking
    assert len(full) > 3  # otherwise the prefix check above is vacuous


def test_chroma_is_never_proposed_on_a_palette_mismatch():
    # L-only matches nothing, not even itself -- the hardest gate case.
    ref = _fp(_textured(), palette="L-only")
    target = _fp(_flat(), palette="HOO")
    proposed = propose_actions(ref, target, applied_kinds=frozenset(),
                               n_scales=7, top_k=8)
    assert not any(a.kind == "chroma" for a in proposed)


def test_chroma_is_proposed_when_palette_is_compatible():
    # The absence check above would also pass against an implementation that
    # never proposes chroma at all, gate or no gate. Confirm the positive side:
    # a compatible palette pair must still be able to surface a chroma action
    # when top_k is generous enough to reach it in the ranking.
    ref, target = _fp(_textured(), palette="HOO"), _fp(_flat(), palette="HOO")
    proposed = propose_actions(ref, target, applied_kinds=frozenset(),
                               n_scales=7, top_k=1000)
    assert any(a.kind == "chroma" for a in proposed)


def test_a_flat_target_is_offered_structural_help_first():
    ref, target = _fp(_textured()), _fp(_flat())
    proposed = propose_actions(ref, target, applied_kinds=frozenset(),
                               n_scales=7, top_k=3)
    print(f"top-3 for textured ref vs. flat target: {[a.key for a in proposed]}")
    # Per-band scoring (SS3.5) puts local_contrast and local_equalize at the
    # dominant band (2") ahead of core_hdr's aggregate score for this fixture
    # (measured: max per-band deficit 0.918 at 2" vs. aggregate spectrum 0.707,
    # see test_component_gaps_... above) -- both are now reachable within
    # top_k=3, not just "one of three structural kinds" as a looser check would
    # allow.
    kinds = [a.kind for a in proposed]
    assert "local_contrast" in kinds
    assert "local_equalize" in kinds


def test_scale_denominated_actions_target_the_band_with_the_real_deficit():
    # A target blurred at fine scales should pull local_contrast/local_equalize
    # toward the band that actually lost structure, not toward the aggregate
    # spectrum gap's favorite or some arbitrary coarse band.
    ref, target = _fp(_textured()), _fp(_blurred_at_2arcsec())
    deficits = _band_deficits(ref, target)
    centers = target.energy.centers_arcsec
    print(f"centers (arcsec): {centers}")
    print(f"per-band deficits: {deficits}")

    # Confirm the fixture actually exhibits the condition under test: one
    # comparable band clearly short of the reference (positive), and the
    # coarser comparable bands showing the blur's energy spilling into them
    # (negative), rather than assuming the blur produced the expected shape.
    band_2_idx = list(centers).index(2.0)
    assert deficits[band_2_idx] > 0.3
    for coarser in (4.0, 8.0, 16.0):
        idx = list(centers).index(coarser)
        assert deficits[idx] < 0
    assert deficits[band_2_idx] == max(deficits)

    proposed = propose_actions(ref, target, applied_kinds=frozenset(),
                               n_scales=7, top_k=8)
    scale_actions = [a for a in proposed if a.kind in ("local_contrast", "local_equalize")]
    print(f"proposed scale actions: {[a.key for a in scale_actions]}")
    assert scale_actions, "expected at least one local_contrast/local_equalize proposal"
    assert all(a.scale_arcsec == 2.0 for a in scale_actions)


def test_no_single_action_kind_monopolises_top_k():
    # Direct regression test for the first bug the lead found: the initial
    # implementation scored every scale-denominated action off one aggregate
    # spectrum scalar, so local_contrast/local_equalize/core_hdr tied on every
    # band and an alphabetical tiebreak handed core_hdr all of top_k, forever.
    # This must fail against that implementation (it did: measured top-3 was
    # ['core_hdr', 'core_hdr', 'core_hdr']) and pass against the current
    # per-band, one-action-per-(kind,band)-group ranking.
    ref, target = _fp(_textured()), _fp(_flat())
    proposed = propose_actions(ref, target, applied_kinds=frozenset(),
                               n_scales=7, top_k=3)
    kinds = [a.kind for a in proposed]
    print(f"top-3 kinds: {kinds}")
    assert len(set(kinds)) > 1


def test_magnitude_follows_gap_size():
    # Direct regression test for the second bug the lead found: fixing the
    # kind-monopoly by round-robin (gentlest level first) exposed a same-shaped
    # monopoly on MAGNITUDE -- LEVELS has exactly 3 entries and top_k defaults
    # to 3, so round-robin never reaches a second pass and every proposal comes
    # back "gentle" (measured against the round-1 implementation: distinct
    # levels seen across top_k=12 was {'', 'gentle'} -- confirmed this test
    # fails against it). Each (kind, band) group's OWN gap size must now pick
    # its magnitude.
    ref, target = _fp(_textured()), _fp(_flat())
    proposed = propose_actions(ref, target, applied_kinds=frozenset(),
                               n_scales=7, top_k=12)
    print(f"top-12: {[a.key for a in proposed]}")

    # band 2" carries the largest measured gap in this fixture (deficit 0.918,
    # see test_scale_denominated_..." pattern / test_component_gaps_...); it
    # must come back strong. tonal_reshape carries a small gap (0.064, well
    # under GAP_GENTLE_MAX); it must come back gentle. Checking only one
    # direction would pass against an implementation that always returns a
    # fixed level (e.g. always "strong").
    band2_action = next(a for a in proposed if a.kind == "local_contrast" and a.scale_arcsec == 2.0)
    tonal_action = next(a for a in proposed if a.kind == "tonal_reshape")
    print(f"large-gap action: {band2_action.key}; small-gap action: {tonal_action.key}")
    assert band2_action.level == "strong"
    assert tonal_action.level == "gentle"

    # General property mirroring the kind-monopoly test: more than one
    # magnitude level must appear among the leveled proposals (once-only
    # actions have no magnitude axis and are excluded).
    leveled = [a for a in proposed if a.kind not in ONCE_ONLY]
    levels_seen = {a.level for a in leveled}
    print(f"distinct levels among leveled actions: {levels_seen}")
    assert len(levels_seen) > 1


def test_level_for_gap_covers_all_three_buckets_at_their_boundaries():
    # The mapping is the whole point of the magnitude axis, so pin its buckets
    # directly rather than only through propose_actions. Boundaries are
    # inclusive-below ("gap >= X" promotes), so test exactly at each one: an
    # off-by-one in the comparison would silently shift a third of real gaps
    # into the neighbouring level.
    assert _level_for_gap(GAP_MODERATE_MAX) == "strong"
    assert _level_for_gap(GAP_MODERATE_MAX - 1e-9) == "moderate"
    assert _level_for_gap(GAP_GENTLE_MAX) == "moderate"
    assert _level_for_gap(GAP_GENTLE_MAX - 1e-9) == "gentle"
    assert _level_for_gap(0.0) == "gentle"
    # A band the target already matches or exceeds yields a non-positive
    # deficit; it must fall through to the gentlest step, never crash or
    # promote.
    assert _level_for_gap(-0.5) == "gentle"


def test_all_three_magnitudes_are_reachable_in_one_slate():
    # test_magnitude_follows_gap_size above pins the two ENDS (strong/gentle).
    # That pair would still pass against an implementation whose middle bucket
    # is unreachable -- which is precisely the class of bug this whole fix
    # exists to close (moderate/strong were structurally dead before it). So
    # assert the MIDDLE bucket lands too, from a group whose measured gap sits
    # inside it.
    ref, target = _fp(_textured()), _fp(_flat())
    deficits = _band_deficits(ref, target)
    centers = list(target.energy.centers_arcsec)
    gaps = component_gaps(ref, target)
    print(f"per-band deficits: {deficits}")

    # Confirm the fixture really straddles all three buckets before relying on
    # it, rather than assuming: 2" is large, 8" is mid-range, tonal is small.
    d2 = deficits[centers.index(2.0)]
    d8 = deficits[centers.index(8.0)]
    assert d2 >= GAP_MODERATE_MAX, f"2\" deficit {d2} should be in the strong bucket"
    assert GAP_GENTLE_MAX <= d8 < GAP_MODERATE_MAX, \
        f"8\" deficit {d8} should be in the moderate bucket"
    assert gaps["tonal"] < GAP_GENTLE_MAX, \
        f"tonal gap {gaps['tonal']} should be in the gentle bucket"

    proposed = propose_actions(ref, target, applied_kinds=frozenset(),
                               n_scales=7, top_k=12)
    by_key = {a.key: a for a in proposed}
    print(f"top-12: {list(by_key)}")

    assert by_key["local_contrast@2.000/strong"].level == "strong"
    assert by_key["local_contrast@8.000/moderate"].level == "moderate"
    assert by_key["tonal_reshape@-/gentle"].level == "gentle"

    leveled = {a.level for a in proposed if a.level != ""}
    print(f"distinct levels among leveled actions: {leveled}")
    assert leveled == {"gentle", "moderate", "strong"}


def test_mode_change_actions_are_never_given_a_magnitude():
    # background_neutralize/star_split change mode, not degree: they carry
    # level "" and must pass through the gap-to-level mapping untouched.
    # Action.strength returns 1.0 for "" but RAISES ValueError for any other
    # unrecognized level, so a mode-change action that got handed a magnitude
    # -- or an empty level that stopped being legitimate -- surfaces here.
    ref, target = _fp(_textured()), _fp(_flat())
    proposed = propose_actions(ref, target, applied_kinds=frozenset(),
                               n_scales=7, top_k=1000)
    mode_changes = [a for a in proposed if a.kind in ONCE_ONLY]
    print(f"mode-change proposals: {[a.key for a in mode_changes]}")
    assert mode_changes, "expected background_neutralize/star_split to be reachable"
    for a in mode_changes:
        assert a.level == "", f"{a.kind} was given magnitude {a.level!r}"
        assert a.strength == 1.0
