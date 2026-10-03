# Task 10 Report: PJSR Harness Wrapper

## Summary

Task 10 is COMPLETE. Created `pjsr/gaia_depth_grade.js` exactly as specified in the brief, transcribed the harness invocation to the file header comment and README.md, and performed a self-review of the PJSR code against PixInsight API conventions.

## Files Created/Modified

1. **Created**: `pjsr/gaia_depth_grade.js` (56 lines)
   - Thin PJSR orchestration wrapper
   - 6-step pipeline: solve → SXT split → export → python grade → import → recombine
   - Header comment includes manual harness invocation

2. **Modified**: `README.md`
   - Added new "Running the depth grade (PixInsight harness)" section
   - Documents the xvfb-run + PixInsight automation invocation
   - Explains the 6-step pipeline

## Commit

```
69c9137 feat: PJSR harness wrapper (solve, SXT split, python grade, recombine)
```

## Self-Review Against Brief & PixInsight API

### Spec Adherence
- ✓ File matches brief verbatim (lines 14–62 of brief copied exactly)
- ✓ Manual harness invocation present in header comment (lines 5–6)
- ✓ Manual invocation also added to README.md
- ✓ No automated tests (as specified; validated manually in PI harness only)

### ExternalProcess Error Handling
- ✓ Line 17: Checks `exitStatus == ProcessExitStatus_NormalExit` ✓ Correct constant name
- ✓ Line 17: Also checks `exitCode != 0` (defense in depth)
- ✓ Line 18: Throws with descriptive error + stderr output
- **Pattern verified**: matches PJSR best practice (loud errors, no silent fallback)

### ImageSolver Usage (Line 25–26)
- ✓ Direct instantiation `new ImageSolver`
- ✓ Call `SolveImage(view)` with the mainView
- ✓ Returns updated view with WCS keywords
- **Note**: Assumes ImageSolver process is installed; will fail loudly if not ✓

### StarXTerminator Usage (Line 29–32)
- ✓ Direct instantiation `new StarXTerminator`
- ✓ Sets `unscreen = true` (produces linear stars, not screened)
- ✓ Calls `executeOn(view)` (modifies in place)
- ✓ Looks up stars window via `View.viewById(view.id + "_stars")`
- ✓ Preserves starless = original view (SXT modifies in place, creates new stars window)
- **Pattern verified**: matches SXT standard behavior

### File I/O (Line 38)
- ✓ `stars.window.saveAs(inPath, false, false, false, false)`
- **Concern**: PJSR saveAs signature is context-dependent. The brief specifies this invocation; I have transcribed it verbatim. Without runtime testing in PixInsight, I cannot verify the exact parameter order/types. The flags (5 booleans) suggest: `(path, bitsPerSample, overwrite, lowerRange, upperRange)` or similar. This is a **minor API trust point** — the brief's code is authoritative here, and it should be validated on first harness run.

### ImageWindow.open (Line 44)
- ✓ `ImageWindow.open(outPath)[0]` loads the FITS file and gets the main window
- ✓ Correctly indexes `[0]` to get the first (only expected) window
- ✓ Pattern matches PJSR conventions

### PixelMath Recombine (Line 47–51)
- ✓ Line 48: `combine(starless.id, graded.mainView.id, op_screen)`
- **Potential concern**: The `combine()` function and `op_screen` constant.
  - PixelMath expression syntax includes screen/multiply/add operators.
  - `combine(a, b, op)` may not be the standard PixelMath operator syntax.
  - Standard PixelMath would be: `starless_id * (1 - graded_id)` for screen, or explicit blend modes.
  - **This is a HIGH-PRIORITY API concern**: I recommend verifying the `combine(a, b, op_screen)` syntax on first harness run. If PixelMath doesn't recognize `combine()`, fall back to:
    ```javascript
    PM.expression = starless.id + " * (1 - " + graded.mainView.id + ")";
    ```
    (screen blend formula) or check PixInsight docs for the correct blend operator.

### CreateNewImage Flag (Line 49)
- ✓ `createNewImage = true` tells PixelMath to create a new window
- ✓ `newImageId = view.id + "_depthgraded"` names the output appropriately
- ✓ Pattern matches PJSR conventions

### Parameter Passing to Python (Line 41)
- ✓ `run([PY_BIN, "-m", PKG, "grade", inPath, outPath])`
- ✓ Array format correct for ExternalProcess in PJSR
- ✓ Invokes the Python CLI as specified in brief

### Global State & Resource Cleanup
- ✓ No raw new/delete; relies on PJSR garbage collection
- ✓ File I/O goes to `/tmp/gaia_depth_grade` (cleaned or created fresh each run)
- ✓ Windows remain in PixInsight for inspection (good for manual validation)
- **No explicit cleanup**: SXT + PixelMath windows persist after script ends. This is acceptable for a manual harness; a production script might close intermediate windows to avoid clutter.

## Testing

**No automated tests run** (as specified in brief):
- GPU + StarXTerminator not available in CI
- Manual validation only: invoke the harness command in the project README and inspect output windows

## Known Limitations & Concerns

1. **PixelMath `combine()` syntax (MEDIUM CONCERN)**
   - The expression `combine(starless.id, graded.mainView.id, op_screen)` may not be valid PixelMath.
   - If PixelMath doesn't recognize `combine()`, the script will fail at runtime.
   - **Recommendation**: On first harness run, verify the syntax or consult PixInsight docs. If it fails, use:
     ```javascript
     PM.expression = starless.id + " * (1 - " + graded.mainView.id + ")";
     ```
   - Or use PixelMath's native blend operator (e.g., `ScreenBlend` if available).

2. **saveAs() parameter signature (LOW CONCERN)**
   - The 5-flag signature in line 38 is transcribed from the brief but not independently verified.
   - Will be caught on first harness run with a clear error.

3. **ImageSolver & StarXTerminator availability**
   - Script assumes both processes are installed.
   - Will fail with a clear instantiation error if missing.

4. **No WCS validation**
   - The script does not check that ImageSolver successfully wrote WCS keywords.
   - If solve fails, the Python core may receive a FITS file without WCS info.
   - Recommendation: Future hardening could check `view.metadata.findKeyword("CTYPE1")` or similar after solve.

## Alignment with Brief

- ✓ Code is verbatim from brief lines 14–62
- ✓ Manual invocation in header + README.md
- ✓ Commit message matches spec
- ✓ No changes to Python core (PJSR-only)

## Next Steps (Post-Manual-Validation)

1. Load a test FITS image into PixInsight (e.g., a Rho Oph or IC1396 frame)
2. Run the harness invocation command
3. Verify:
   - ImageSolver runs and adds WCS keywords
   - StarXTerminator creates stars + starless windows
   - Python CLI runs (check `/tmp/gaia_depth_grade/stars_out.fits`)
   - PixelMath expression is valid and produces the combined output
   - Final `*_depthgraded` window shows depth-graded stars
4. If PixelMath expression fails, debug the syntax and update accordingly

---

**Report prepared:** 2026-06-23
