# Task 5 Report: Recipe (Audit Artifact)

## Summary
Implemented the Recipe dataclass (`src/autocontrast/optimize/recipe.py`) and comprehensive test suite (`tests/test_optimize_recipe.py`). The recipe is the SS12 audit artifact — an ordered, immutable, replayable log of image operations sufficient to reproduce results with AutoContrast absent from the machine.

## Implementation

Created `src/autocontrast/optimize/recipe.py` with:
- **Recipe** frozen dataclass with `actions: tuple[Action, ...]`
- **Key methods**:
  - `empty()` — creates empty recipe
  - `extend(action)` — returns new Recipe with action appended (immutable)
  - `key` — order-sensitive identity string (beam distinctness)
  - `applied_kinds` — frozenset of action kinds (occurrence tracking)
  - `to_dict()` / `from_dict()` — serialization round-trip
  - `to_pixinsight_steps()` — renders stock PixInsight process invocations
- **PROCESS_FOR_KIND** mapping: action kind → stock PI process name

## Test Results

### Step 2: Failing test run
```
ERROR tests/test_optimize_recipe.py
ModuleNotFoundError: No module named 'autocontrast.optimize.recipe'
```
✓ Expected failure confirmed

### Step 4: Passing tests
```bash
$ .venv/bin/python -m pytest tests/test_optimize_recipe.py -v

tests/test_optimize_recipe.py::test_recipe_is_immutable_and_extends_to_a_new_object PASSED [ 20%]
tests/test_optimize_recipe.py::test_recipe_key_distinguishes_order PASSED [ 40%]
tests/test_optimize_recipe.py::test_applied_kinds_tracks_once_only_actions PASSED [ 60%]
tests/test_optimize_recipe.py::test_round_trips_through_dict PASSED [ 80%]
tests/test_optimize_recipe.py::test_renders_to_stock_pixinsight_steps PASSED [100%]

============================== 5 passed in 0.01s ===============================
```
✓ All 5 tests pass

## Commit
```
SHA: 9fdb920
Message: "optimize: the recipe is the audit artifact

SS12 requires the result be reconstructible as stock PixInsight processes.
The recipe must suffice to reproduce it with AutoContrast absent from the
machine -- that is what auditability means here."
```

## Analysis: The `.strength` Question

**Question**: `to_pixinsight_steps()` calls `a.strength` for every action. Since `.strength` raises `ValueError` for unrecognized levels, what happens if a Recipe somehow holds an action with a bad level? Is raising the correct behavior for an audit artifact?

**Answer**: **YES, raising is correct.** 

**Reasoning**:
1. **Auditability is the entire point** — the recipe must be sufficient to reproduce the result with AutoContrast absent. A recipe with an unrecognized level cannot be rendered honestly.
2. **Silent fallbacks are forbidden by design** (SS12 constraint). A plausible-looking but incorrect step is worse than a clear error.
3. **The recipe cannot self-corrupt** — `from_dict()` only creates Actions from existing serialized data; it cannot introduce invalid levels. Only direct code bugs (or externally corrupted persisted data) could create such an action.
4. **Loud failure catches real problems** — if somehow a bad level gets into a recipe, failing to render it forces the operator to notice and fix the root cause, rather than silently proceeding with wrong data.
5. **A Recipe is immutable and scoped to the optimizer** — there's no reason an invalid action should ever exist in a well-formed recipe. Raising is a reasonable defense against programmer error or external corruption.

## Self-Review

- ✓ Immutability verified: both original and new Recipe tested
- ✓ Order-sensitivity verified: two orderings compared and confirmed distinct
- ✓ Occurrence tracking verified: `applied_kinds` contains only the kinds that appear
- ✓ Round-trip verified: `to_dict()` → `from_dict()` preserves key
- ✓ Rendering verified: `to_pixinsight_steps()` includes process name, original params, and computed strength
- ✓ No new dependencies introduced
- ✓ Commit message matches specification exactly
- ✓ Test hygiene: assertions check both immutability directions, order effects, and field preservation

---

## Fix Round 1 — Test Hygiene (Review Feedback)

**Finding 1 (Important) — order-sensitivity test lacked positive half**
- **Issue**: Only asserted different orderings produce different keys; a broken impl like `id(self)` would pass
- **Fix**: Added assertion that same ordering produces same key:
  ```python
  assert Recipe.empty().extend(a).extend(b).key == Recipe.empty().extend(a).extend(b).key
  ```
- Kept existing inequality assertion for both directions

**Finding 2 (Important) — round-trip test gave false confidence**
- **Issue**: Only checked `.key == .key`, but `.key` excludes `params`; a broken `from_dict` dropping params would still pass
- **Fix**: Added assertion comparing full `to_pixinsight_steps()` output before and after round-trip:
  ```python
  assert r.to_pixinsight_steps() == Recipe.from_dict(r.to_dict()).to_pixinsight_steps()
  ```
- This catches if params are silently dropped during deserialization

**Minor — error message for unmapped kinds**
- **Issue**: `PROCESS_FOR_KIND[a.kind]` raised bare `KeyError` without context
- **Fix**: Explicit check with clear error message listing known kinds:
  ```python
  if a.kind not in PROCESS_FOR_KIND:
      raise ValueError(
          f"unmapped action kind '{a.kind}'; known kinds: "
          f"{', '.join(sorted(PROCESS_FOR_KIND.keys()))}"
      )
  ```

### Fix Round 1: Test Run
```bash
$ .venv/bin/python -m pytest tests/test_optimize_recipe.py -v

tests/test_optimize_recipe.py::test_recipe_is_immutable_and_extends_to_a_new_object PASSED [ 20%]
tests/test_optimize_recipe.py::test_recipe_key_distinguishes_order PASSED [ 40%]
tests/test_optimize_recipe.py::test_applied_kinds_tracks_once_only_actions PASSED [ 60%]
tests/test_optimize_recipe.py::test_round_trips_through_dict PASSED [ 80%]
tests/test_optimize_recipe.py::test_renders_to_stock_pixinsight_steps PASSED [100%]

============================== 5 passed in 0.01s ===============================
```
✓ All 5 tests pass

### Fix Round 1: Commit
```
SHA: f0a3a3b
Message: "optimize: strengthen recipe tests + error handling (round 1)

- test_recipe_key_distinguishes_order: add positive half (same ordering = same key)
- test_round_trips_through_dict: verify params preserved through serialization
- to_pixinsight_steps: replace bare KeyError with clear unmapped-kind message"
```
