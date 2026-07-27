from autocontrast.optimize.actions import Action
from autocontrast.optimize.recipe import Recipe


def _a(kind="local_contrast", level="moderate", scale=8.0):
    return Action(kind=kind, level=level, scale_arcsec=scale, params={"layer": 3})


def test_recipe_is_immutable_and_extends_to_a_new_object():
    empty = Recipe.empty()
    one = empty.extend(_a())
    assert len(empty.actions) == 0
    assert len(one.actions) == 1


def test_recipe_key_distinguishes_order():
    a, b = _a(kind="black_point", scale=None), _a(kind="chroma", scale=None)
    # Same ordering must produce same key
    assert Recipe.empty().extend(a).extend(b).key == Recipe.empty().extend(a).extend(b).key
    # Different orderings must produce different keys
    assert Recipe.empty().extend(a).extend(b).key != Recipe.empty().extend(b).extend(a).key


def test_applied_kinds_tracks_once_only_actions():
    r = Recipe.empty().extend(_a(kind="star_split", level="", scale=None))
    assert "star_split" in r.applied_kinds


def test_round_trips_through_dict():
    r = Recipe.empty().extend(_a()).extend(_a(kind="chroma", scale=None))
    # Key must match
    assert Recipe.from_dict(r.to_dict()).key == r.key
    # Params must be preserved through round-trip (Action.params not in key, so explicit check)
    assert r.to_pixinsight_steps() == Recipe.from_dict(r.to_dict()).to_pixinsight_steps()


def test_renders_to_stock_pixinsight_steps():
    r = Recipe.empty().extend(_a())
    steps = r.to_pixinsight_steps()
    assert steps[0]["process"] == "MultiscaleLinearTransform"
    assert steps[0]["params"]["layer"] == 3
    assert "strength" in steps[0]["params"]
