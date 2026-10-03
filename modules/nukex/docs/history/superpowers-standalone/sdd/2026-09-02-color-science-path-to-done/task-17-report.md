# Task 17 Report: Document the override file + correct the remediation messages

## What Was Implemented

1. **Created `docs/qe_overrides_format.md`** — User-facing documentation of the QE override JSON format, including:
   - Schema with examples (cameras and filters sections)
   - How INSTRUME and FILTER keywords are matched to database keys
   - Generic camera fallback behavior
   - Override merge semantics
   - Error handling guidance

2. **Updated `src/lib/stacker/src/stacking_engine.cpp` (two sites)**:
   - Lines 199–201 (batch-stop error): Changed from pointing at `~/.nukex4/qe_overrides.json` to directing users to the QE override picker in the interface, with a reference to the new doc.
   - Lines 332–333 (per-frame skip message): Updated parenthetical to clarify that the override file is selected in the NukeX interface.

3. **Updated `src/lib/io/src/filter_classifier.cpp` (line 140)**:
   - Mono warning: Changed from "add it to qe_overrides.json" to "rename FILTER to Ha/OIII/SII or add it to a qe_overrides.json selected in the NukeX interface."

## Testing Results

- **Build**: Clean, no errors or warnings
- **Tests**: 67/67 passed (60.41 seconds)
  - `test_filter_classifier_io` ✓
  - `test_stacking_engine` ✓
  - All other integration and unit tests ✓

## Files Changed

- `docs/qe_overrides_format.md` — Created (80 lines)
- `src/lib/stacker/src/stacking_engine.cpp` — Lines 199–201 and 332–333 modified
- `src/lib/io/src/filter_classifier.cpp` — Line 140 modified

## Self-Review Findings

### Completeness ✓
- [x] Doc present at correct location with exact text from brief
- [x] Three message sites changed (batch-stop, per-frame skip, mono warning)
- [x] No remaining `~/.nukex4/qe_overrides.json` references in `src/`
- [x] Required substrings preserved:
  - `qe_overrides.json` in batch-stop error (for integration test assertion)
  - `Unknown filter '` in mono warning (for module test assertion)

### Discipline ✓
- [x] Changes match brief exactly (no scope creep)
- [x] Commit message from brief used verbatim
- [x] Two-trailer attribution block included

### Testing ✓
- [x] Build completes without errors
- [x] All 67 tests pass
- [x] No test assertions needed updating (substrings preserved)

## Issues or Concerns

None. Task completed as specified.

## Commit

- SHA: `2c6d1ec`
- Message: "docs: QE override file format + accurate remediation messages"
- Branch: `v5-color-science`
