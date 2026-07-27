import subprocess
import sys

import numpy as np

from autocontrast.fingerprint.extract import extract
from autocontrast.optimize.propose import component_gaps, propose_actions


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
    # local_contrast, local_equalize, and core_hdr are all structural remedies for
    # the spectrum gap (see _REMEDIES); which one sorts first among ties is a
    # tie-break detail, not the thing under test here.
    assert any(a.kind in ("local_contrast", "local_equalize", "core_hdr") for a in proposed)
