from autocontrast.optimize.actions import Action
from autocontrast.optimize.beam import Branch, prune
from autocontrast.optimize.recipe import Recipe


def _branch(distance, *kinds, alive=True):
    r = Recipe.empty()
    for k in kinds:
        r = r.extend(Action(k, "moderate", None, {}))
    return Branch(recipe=r, image_path=f"/tmp/{'-'.join(kinds) or 'root'}.xisf",
                  distance=distance, alive=alive)


def test_prune_keeps_the_lowest_distances():
    kept = prune([_branch(0.5, "a"), _branch(0.1, "b"), _branch(0.3, "c")], width=2)
    assert [b.distance for b in kept] == [0.1, 0.3]


def test_prune_respects_width():
    assert len(prune([_branch(i / 10, f"k{i}") for i in range(9)], width=3)) == 3


def test_prune_drops_dead_branches():
    kept = prune([_branch(0.1, "a", alive=False), _branch(0.4, "b")], width=3)
    assert [b.recipe.key for b in kept] == [_branch(0.4, "b").recipe.key]


def test_prune_deduplicates_identical_recipes():
    # Two branches that reached the same action sequence must not both hold slots.
    kept = prune([_branch(0.2, "a", "b"), _branch(0.25, "a", "b"), _branch(0.3, "c")],
                 width=3)
    assert len(kept) == 2


def test_prune_returns_empty_when_everything_is_dead():
    assert prune([_branch(0.1, "a", alive=False)], width=3) == []
