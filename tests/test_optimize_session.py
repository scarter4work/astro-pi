from autocontrast.optimize.actions import Action
from autocontrast.optimize.beam import BeamConfig, Branch
from autocontrast.optimize.recipe import Recipe
from autocontrast.optimize.session import Session, session_path


def _session(tmp_path):
    root = Recipe.empty()
    base = Branch(recipe=root, image_path=str(tmp_path / "proxy.xisf"), distance=0.5)
    return Session(
        session_id="s1", work_dir=str(tmp_path), source_path=str(tmp_path / "src.fit"),
        proxy_path=str(tmp_path / "proxy.xisf"), reference_id="eso1103a",
        reference_fp={"stub": True}, pixel_scale_arcsec=1.01, psf_fwhm_arcsec=2.0,
        palette_class="HOO", n_scales=7, config=BeamConfig(), iteration=0,
        branches=[base], best=base, baseline_distance=0.5, distance_history=[0.5],
        converged=False, convergence_reason="", guardrail_log=[],
    )


def test_session_round_trips_through_disk(tmp_path):
    s = _session(tmp_path)
    # Non-empty, distinctive params: Action.params is compare=False and
    # Action.key omits params entirely, so two actions differing only in
    # params compare equal and produce identical recipe keys. Checking
    # recipe.key alone would pass even if from_dict silently dropped params.
    # to_pixinsight_steps() renders params into the actual PI process
    # invocation, so it actually exercises the round trip.
    s.branches = [s.branches[0], Branch(
        recipe=Recipe.empty().extend(Action("chroma", "gentle", None, {"layer": 3})),
        image_path=str(tmp_path / "c1.xisf"), distance=0.42)]
    p = session_path(tmp_path, "s1")
    s.save(p)

    loaded = Session.load(p)
    assert loaded.session_id == "s1"
    assert loaded.reference_id == "eso1103a"
    assert loaded.baseline_distance == 0.5
    assert [b.distance for b in loaded.branches] == [0.5, 0.42]
    assert loaded.branches[1].recipe.key == s.branches[1].recipe.key
    assert (loaded.branches[1].recipe.to_pixinsight_steps()
            == s.branches[1].recipe.to_pixinsight_steps())
    assert loaded.branches[1].recipe.actions[0].params == {"layer": 3}
    assert loaded.config.width == BeamConfig().width
    assert loaded.config == s.config
    assert loaded.best.distance == s.best.distance
    assert loaded.pixel_scale_arcsec == s.pixel_scale_arcsec
    assert loaded.psf_fwhm_arcsec == s.psf_fwhm_arcsec
    assert loaded.palette_class == s.palette_class
    assert loaded.n_scales == s.n_scales
    assert loaded.converged == s.converged
    assert loaded.reference_fp == s.reference_fp


def test_resume_preserves_iteration_and_history(tmp_path):
    s = _session(tmp_path)
    s.iteration = 4
    s.distance_history = [0.5, 0.47, 0.45, 0.44, 0.44]
    p = session_path(tmp_path, "s1")
    s.save(p)
    loaded = Session.load(p)
    assert loaded.iteration == 4
    assert loaded.distance_history == [0.5, 0.47, 0.45, 0.44, 0.44]


def test_guardrail_log_survives_the_round_trip(tmp_path):
    s = _session(tmp_path)
    s.guardrail_log = [
        {"iteration": 1, "action": "chroma@-/strong",
         "failed": ["hue_invention"], "reason": "0.02 chroma mass"},
        {"iteration": 2, "action": "core_hdr@30.0/moderate",
         "failed": [], "reason": ""},
    ]
    p = session_path(tmp_path, "s1")
    s.save(p)
    loaded = Session.load(p)
    assert loaded.guardrail_log == s.guardrail_log


def test_session_with_nontrivial_beam_round_trips_fully(tmp_path):
    """A beam of >1 branch with distinct recipes, distances, and aliveness,
    so a from_dict that dropped the whole `branches` list -- not just one
    field of one branch -- would be caught."""
    s = _session(tmp_path)
    b0 = s.branches[0]
    b1 = Branch(
        recipe=Recipe.empty().extend(Action("core_hdr", "moderate", 30.0, {"layer": 2})),
        image_path=str(tmp_path / "b1.xisf"), distance=0.40)
    b2 = Branch(
        recipe=Recipe.empty().extend(Action("chroma", "strong", None, {"layer": 5})),
        image_path=str(tmp_path / "b2.xisf"), distance=0.55, alive=False)
    s.branches = [b0, b1, b2]
    s.best = b1
    p = session_path(tmp_path, "s1")
    s.save(p)

    loaded = Session.load(p)
    assert len(loaded.branches) == 3
    assert [b.distance for b in loaded.branches] == [0.5, 0.40, 0.55]
    assert [b.alive for b in loaded.branches] == [True, True, False]
    assert loaded.branches[1].recipe.to_pixinsight_steps() == b1.recipe.to_pixinsight_steps()
    assert loaded.branches[2].recipe.to_pixinsight_steps() == b2.recipe.to_pixinsight_steps()
    assert loaded.best.distance == b1.distance
    assert loaded.best.recipe.to_pixinsight_steps() == b1.recipe.to_pixinsight_steps()
