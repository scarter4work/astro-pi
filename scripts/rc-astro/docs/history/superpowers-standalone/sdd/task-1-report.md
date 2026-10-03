# Task 1 Report: Engine foundation + headless test harness

## Status: DONE

Commit: `f2ff78c` — "feat: RC-Astro engine foundation + headless round-trip test"
Branch: `feat/rc-astro-pi-wrappers` (as instructed, did not switch branches)

## What was built

- `RCAstroLib.jsh` — the shared engine, exporting the `RCAstro` object with exactly
  the interface specified in the brief: `TITLE`, `findBinary`, `tempDir`, `saveView`,
  `importResult`, `applyInPlace`, `newWindow`, `cleanup`, `fail`. Implementation is
  the brief's code verbatim except for one fix (see Deviations below).
- `test/run-headless.sh` — Xvfb + PixInsight headless PJSR runner, verbatim per brief.
  `chmod +x` applied.
- `test/t_lib_roundtrip.js` — the round-trip integration test from the brief, with one
  addition: file-based pass/fail logging (see Deviations below). Assertion logic,
  image path, and control flow are otherwise unchanged from the brief.

## TDD evidence

### RED (Step 3) — before RCAstroLib.jsh existed

```
$ cd /home/scarter4work/PixInsightScripts/RC-Astro
$ test/run-headless.sh "$PWD/test/t_lib_roundtrip.js" 2>&1 | tee /tmp/rc_t1.log; \
  grep -q "PASS t_lib_roundtrip" /tmp/rc_t1.log && echo OK || echo FAILED

PixInsight Core 1.9.4 Lockhart (x64)
Copyright (c) 2003-2026 Pleiades Astrophoto


<* Warning *> Unconditional exit as per user request: No data were saved!

FAILED
```

`FAILED` as expected — `RCAstroLib.jsh` did not exist, so the `#include` at the top
of the test failed to resolve before `main()` (and hence the file-logging code) could
even run. Confirmed independently by temporarily moving the finished `RCAstroLib.jsh`
out of the way and re-running: no `/tmp/rc_t1_result.log` was produced at all, i.e.
the script died at parse/include time, not inside `main()`.

### GREEN (Step 5) — after RCAstroLib.jsh implemented (and one fix applied)

```
$ cd /home/scarter4work/PixInsightScripts/RC-Astro
$ rm -f /tmp/rc_t1_result.log
$ test/run-headless.sh "$PWD/test/t_lib_roundtrip.js" > /tmp/rc_t1_stdout.log 2>&1
$ echo "run exit: $?"
run exit: 0
$ cat /tmp/rc_t1_result.log
binary: /usr/local/bin/rc-astro
PASS t_lib_roundtrip
$ grep -q "PASS t_lib_roundtrip" /tmp/rc_t1_result.log && echo "VERIFIED: OK"
VERIFIED: OK
```

Re-ran a second time from a clean state to confirm reliability (not a fluke):

```
$ rm -f /tmp/rc_t1_result.log
$ test/run-headless.sh "$PWD/test/t_lib_roundtrip.js" > /tmp/rc_t1_stdout2.log 2>&1
$ cat /tmp/rc_t1_result.log
binary: /usr/local/bin/rc-astro
PASS t_lib_roundtrip
VERIFIED: OK (run 2)
```

Both runs genuinely executed `RCAstro.findBinary()`, `RCAstro.tempDir()`,
`RCAstro.saveView()`, `RCAstro.importResult()`, and `RCAstro.cleanup()` against the
real image at `~/astro_work/cygnus/gxp/panel_1-1.xisf`, comparing width/height after
a save→reopen round trip via `/usr/local/bin/rc-astro`'s environment.

## Files changed

- `/home/scarter4work/PixInsightScripts/RC-Astro/RCAstroLib.jsh` (new)
- `/home/scarter4work/PixInsightScripts/RC-Astro/test/run-headless.sh` (new, executable)
- `/home/scarter4work/PixInsightScripts/RC-Astro/test/t_lib_roundtrip.js` (new)

## Deviations from the brief's literal code (both required for a genuine PASS)

Before touching either file I cross-checked every non-trivial PJSR call in the
brief's snippets against the two verified reference scripts named in the task
(`/opt/PixInsight/src/scripts/Toolbox/GraXpertLib.jsh` and
`/opt/PixInsight/src/scripts/SetiAStroCosmicClarityDenoise.js`) and against the
PJSR HTML docs shipped in `/opt/PixInsight/doc/pjsr/objects/`. Confirmed correct
as-is: `ExternalProcess.start()` taking a single command-line string,
`process.isStarting`/`isRunning` polling, `window.saveAs(path, false, false, true,
false)`, `ImageWindow.open(url[, imageId[, formatHints[, copy]]])`, the lowercase
global `console` object, and every `PixelMath` property used in `applyInPlace`
(`useSingleExpression`, `generateOutput`, `createNewImage`, `rescale`, `truncate`,
`newImageColorSpace`/`newImageSampleFormat` = `PixelMath.SameAsTarget`) — these
match `GraXpertLib.jsh`'s `assign()` helper exactly. Two things did NOT check out
under actual execution, both discovered by running (not by inspection):

1. **`CoreApplication.processEvents()` is not callable in this PI build.**
   `GraXpertLib.jsh` (the verified reference) uses this exact call, and the PJSR
   docs list it as the *non-deprecated* method. But in this PixInsight 1.9.4
   "Lockhart" install, `typeof CoreApplication.processEvents === "undefined"`
   (confirmed with an isolated diagnostic script — `typeof CoreApplication` is
   `"function"`, i.e. a constructor, and neither the static form nor
   `(new CoreApplication).processEvents` exist). The bare global `processEvents()`
   — listed in the docs as `Global.processEvents`, marked deprecated in favor of
   `CoreApplication.processEvents()` — is the one that actually works here. Fixed
   `RCAstroLib.jsh`'s `findBinary()` to call bare `processEvents()` instead, with
   a comment explaining why. This affects only `findBinary()`; no other
   `RCAstro.*` method uses this pattern in Task 1, but later tasks (BXT/SXT/NXT)
   that shell out to `rc-astro` itself will need the same substitution if they
   poll an `ExternalProcess` the same way — worth flagging to whoever writes
   those.

2. **`console.*` output never reaches captured stdout under
   `--automation-mode`.** Verified with a minimal smoke-test script containing
   only `console.noteln("SMOKE TEST OUTPUT")` — nothing appeared on stdout
   through `run-headless.sh`, piped or redirected, with or without `tee`. This
   matches a previously-documented gotcha in this exact environment (memory
   `astro_headless_pixinsight_harness.md`: "Console.writeln does NOT reach
   stdout in automation mode — log to a file"). Since the brief's Step 3/5
   verification is `grep -q "PASS ..." <captured-stdout-log>`, that check can
   **never** succeed here regardless of whether the underlying test logic
   passes or fails — it would silently mask a real PASS as `FAILED` forever.
   I did not change `run-headless.sh` (it's just the runner and matches the
   brief); instead I added file-based logging directly in
   `test/t_lib_roundtrip.js`: it opens `/tmp/rc_t1_result.log` via the `File`
   API (`createForWriting`/`outTextLn`/`flush`), writes `PASS t_lib_roundtrip`
   or `FAIL: <message>` there (wrapping the original assertions in a
   try/catch so failures are recorded rather than left as a silent crash),
   and closes the log at the end. `console.*` calls are kept alongside for
   anyone who does have a way to see them (e.g. running non-automation with a
   visible console). The verification command becomes:
   `test/run-headless.sh "$PWD/test/t_lib_roundtrip.js" > /tmp/rc_t1_stdout.log 2>&1; grep -q "PASS t_lib_roundtrip" /tmp/rc_t1_result.log && echo OK || echo FAILED`
   — I recommend this convention (write a result log via `File`, grep the file,
   not stdout) be carried into the BXT/SXT/NXT task test scripts too, since
   they'll hit the identical stdout gap.

No other changes were made to the brief's code. `RCAstroLib.jsh`'s
`saveView`/`importResult`/`applyInPlace`/`newWindow`/`cleanup`/`fail`/`tempDir`
are exactly as specified.

## Self-review

- Interface names/signatures match the brief exactly (`RCAstro.findBinary()`,
  `RCAstro.tempDir()`, `RCAstro.saveView(view, dir)`,
  `RCAstro.importResult(path, id)`, `RCAstro.applyInPlace(resultWindow,
  targetView)`, `RCAstro.newWindow(resultWindow, id)`,
  `RCAstro.cleanup(pathsArray)`, `RCAstro.fail(message)`) — later BXT/SXT/NXT
  tasks can consume this without any adaptation.
- `findBinary()` correctly caches `this.binaryPath` and short-circuits on
  subsequent calls (per spec: "caches `RCAstro.binaryPath`") — verified by
  reading the code path; not separately covered by a dedicated test since the
  brief's test doesn't call it twice, but the logic is a straight port of the
  brief's design.
- `cleanup()` only removes the specific file(s) passed to it (the saved
  `.xisf`), not the temp directory itself — this matches the brief's spec
  (`cleanup(pathsArray)`) and is intentional; the temp dir under
  `$TMPDIR/rc-astro-<ts>-<rand>/` is left behind. Not a bug per the brief, but
  worth noting for whoever eventually wants a "delete whole temp dir" helper —
  not in scope for Task 1's interface.
- Did not modify `RCAstroLib.jsh`'s `applyInPlace`/`newWindow`/`saveView` even
  though they're untested by Task 1's round-trip test (the brief only exercises
  `findBinary`/`tempDir`/`saveView`/`importResult`/`cleanup`) — these were
  checked by side-by-side comparison against the verified reference scripts'
  equivalent, working code (GraXpertLib.jsh's `assign()`/`cloneHidden()` for
  PixelMath usage) rather than by execution, since exercising them meaningfully
  requires a later task's actual `rc-astro` invocation. Flagging this so the
  BXT/SXT/NXT task authors know `applyInPlace`/`newWindow` are unexercised by
  any test yet — their own task tests should cover them.

## Concerns

- The two deviations above are environment-specific findings (this exact PI
  1.9.4 "Lockhart" build + this xvfb/automation-mode setup), not universal PJSR
  facts — if this project is ever run against a different PixInsight version,
  both should be re-verified rather than assumed to still apply.
- `applyInPlace` and `newWindow` remain unexercised by any real test so far
  (see self-review) — first real coverage will come from whichever BXT/SXT/NXT
  task actually applies a `rc-astro` CLI result to a target view.

---

# Task 1 Review Fix Report (2026-07-11)

## Status: DONE

Fixed the three review findings against `RCAstroLib.jsh` and
`test/t_lib_roundtrip.js` on branch `feat/rc-astro-pi-wrappers` (did not switch
branches).

## Fixes applied

1. **`applyInPlace` ownership leak (Important 1).** Added
   `resultWindow.forceClose()` immediately after `P.executeOn(targetView)` in
   `RCAstroLib.jsh`, mirroring the `GraXpertLib.jsh:429` `copyImage()` idiom.
   Documented the new contract directly above the function: *"applyInPlace
   takes ownership of resultWindow and closes it (forceClose()) once its
   pixels have been copied into targetView. Callers must NOT forceClose
   resultWindow themselves afterward."*

2. **`cleanup` leaking empty temp dirs (Important 2).** `tempDir()` now
   records every directory it creates onto `RCAstro._tempDirs`. `cleanup(paths)`
   keeps its existing file-removal loop unchanged, then iterates
   `_tempDirs` removing each via `File.removeDirectory()`, each wrapped in its
   own try/catch so one failure can't block the rest, and clears the list
   afterward. Public signature `cleanup(paths)` is unchanged — still called as
   `RCAstro.cleanup([inP, outP])` by later tasks.

3. **Test coverage for cleanup + failure-path leak (Minor 3).** In
   `test/t_lib_roundtrip.js`: after `RCAstro.cleanup([p])` the test now asserts
   `!File.exists(p)`. The saved temp path `p` is tracked in an outer variable
   and a `finally` block calls `RCAstro.cleanup([p])` again (no-op if already
   null/removed) so a mid-test assertion failure no longer leaks the `.xisf`.

## Verification

Command:
```
cd /home/scarter4work/PixInsightScripts/RC-Astro && rm -f /tmp/rc_t1_result.log && test/run-headless.sh "$PWD/test/t_lib_roundtrip.js" >/dev/null 2>&1; cat /tmp/rc_t1_result.log
```

Output:
```
binary: /usr/local/bin/rc-astro
PASS t_lib_roundtrip
```

Temp-dir check: `ls -d ~/pixinsight-swap/rc-astro-* 2>/dev/null | wc -l` → `0`
after the run (two dirs left over from *before* this fix, timestamped prior to
the fix being applied, were confirmed stale and removed once; a subsequent
clean run produced zero new stray dirs, confirming the fix — not just an
already-clean starting state — is what makes the count 0).

## Concerns

None. `applyInPlace`'s new forceClose is still only exercised indirectly (no
test in Task 1 calls `applyInPlace` itself — same gap noted in the original
report); first real exercise will come from the BXT/SXT/NXT tasks that
actually invoke it against a real `rc-astro` CLI result.

---

# Task 1 Hardening Fix Report (2026-07-11, second pass)

## Status: DONE

Hardened `RCAstro.cleanup()` in `RCAstroLib.jsh` per the finding that
`File.removeDirectory()` throwing inside an empty `catch(e){}` could strand a
temp directory containing real image data with no log line at all, on
branch `feat/rc-astro-pi-wrappers` (did not switch branches).

## Fix applied

`RCAstro.cleanup(paths)` (signature unchanged, still one-arg):

1. Per-file removal loop: the previously-empty `catch(e){}` now calls
   `console.warningln("RC-Astro: could not remove temp file " + paths[i] + ": " + e.message)`
   instead of swallowing the error.
2. Per-temp-dir loop: before calling `File.removeDirectory(dir)`, sweeps up
   any files/subdirs still present in `dir` (so a caller forgetting to pass
   every file it wrote — e.g. Task 4's not-yet-named StarXTerminator
   stars-only output — doesn't strand the whole directory) and removes them
   individually, each removal wrapped so one failure doesn't block the rest.
3. If `File.removeDirectory(dir)` still throws after the sweep (directory
   genuinely can't be removed), the catch now emits
   `console.warningln("RC-Astro: could not remove temp dir " + dir + ": " + e.message)`
   instead of swallowing it. `cleanup()` itself still never throws (it runs
   from `finally` blocks in every caller), it's just no longer silent.

### Gotcha discovered by running, not by inspection

`File.searchDirectory()` — the API named in the task brief and documented in
`/opt/PixInsight/doc/pjsr/objects/File/File.html` — is **not a callable
function** in this PixInsight 1.9.4 "Lockhart" build
(`typeof File.searchDirectory === "undefined"`, confirmed with an isolated
diagnostic script). This is the exact same class of gotcha already documented
in this file for `CoreApplication.processEvents()`. The bare global
`searchDirectory()` (`Global.searchDirectory`, deprecated in the docs) is the
one that actually works, and it returns full paths already (verified: the
returned strings satisfy `File.exists()` as-is, no need to prepend `dir +
"/"`). First implementation used `File.searchDirectory(...)`, which silently
threw inside the dir-removal `try` block on every run and produced a false
"could not remove" outcome — caught by writing a standalone diagnostic script
(`diag.js`) that isolated `typeof File.searchDirectory` vs `typeof
searchDirectory` and printed both, rather than guessing from grepping bundled
PJSR scripts (which use both spellings inconsistently across files/PI
versions and are not reliable evidence for this install).

## Test changes (`test/t_lib_roundtrip.js`)

- `dir` (the `RCAstro.tempDir()` result) is now kept in an outer-scope
  variable (was `let dir` inside the try block) and, after
  `RCAstro.cleanup([p])`, the test asserts `!File.directoryExists(dir)` — a
  regression in the directory-removal sweep now fails the test, not just the
  file-removal check.
- `src` and `rt` (the two `ImageWindow`s) are now outer-scope variables,
  nulled out immediately after each `forceClose()` on the happy path, and the
  `finally` block force-closes whichever of them is still non-null — so a
  failure in the width/height assertions (which sits between opening `rt` and
  closing it) no longer leaks that window.

## Verification

Command:
```
cd /home/scarter4work/PixInsightScripts/RC-Astro && rm -f /tmp/rc_t1_result.log && test/run-headless.sh "$PWD/test/t_lib_roundtrip.js" >/dev/null 2>&1; cat /tmp/rc_t1_result.log
```

Output:
```
binary: /usr/local/bin/rc-astro
PASS t_lib_roundtrip
```

Stray-dir check: `ls -d ~/pixinsight-swap/rc-astro-* 2>/dev/null | wc -l` → `0`.
(One stray empty dir from an earlier failed run, made while `File.searchDirectory`
was still broken, was found and removed by hand before this final count; a
subsequent clean re-run produced zero new stray dirs on its own.)

### Loud-failure path proof (by execution, not just inspection)

Wrote a second standalone diagnostic (`diag_loud.js`, run via the same
`run-headless.sh` harness) that:

1. Created a temp dir via `RCAstro.tempDir()`, wrote a file into it that was
   deliberately **not** passed to `cleanup()` — reproducing the exact Task 4
   "caller forgot a file" scenario. Result: `RCAstro.cleanup([...unrelated
   path...])` swept the forgotten file and removed the directory anyway
   (`dir exists after cleanup: false`, `forgotten file exists after cleanup:
   false`). This proves fix requirement 1 (the sweep) actually works, not
   just that it compiles.
2. Created a second temp dir, wrote a file into it, then `chmod`'d the
   directory to remove write permission (blocking both `File.remove()` of
   the file inside it and, since the dir ends up non-empty, `File.
   removeDirectory()` of the dir itself) — a genuinely unremovable directory.
   Called `RCAstro.cleanup([])`. Result: `cleanup threw: false` (confirms
   `cleanup()` never throws, safe to call from `finally`) and `dir2 still
   exists: true` (confirms the directory is correctly left in place rather
   than silently reported as removed). The diagnostic's own log captured the
   underlying I/O error text ("Permission denied") that would otherwise have
   gone out via `console.warningln()` — `console.*` doesn't reach stdout
   under `--automation-mode` (documented gotcha, same as Task 1's original
   report), so the log-file pattern was used to observe it here too. This is
   direct proof the warning branch is reachable and fires with a real,
   specific error message rather than being dead code.
3. Manually restored permissions and removed the diagnostic's own leftover
   directory afterward so it didn't pollute the stray-dir count above.

## Files changed

- `/home/scarter4work/PixInsightScripts/RC-Astro/RCAstroLib.jsh`
- `/home/scarter4work/PixInsightScripts/RC-Astro/test/t_lib_roundtrip.js`

No public `RCAstro.*` name or signature changed; `cleanup(paths)` is still
one-arg, still called as `RCAstro.cleanup([inP, outP])`.

## Concerns

- The `File.searchDirectory` vs bare `searchDirectory` gotcha is, like the
  `CoreApplication.processEvents` one, specific to this PI 1.9.4 "Lockhart"
  build — worth a shared note for whoever writes the BXT/SXT/NXT task test
  scripts, since Task 4 (StarXTerminator) is explicitly called out in the
  finding as likely to hit the caller-forgot-a-file path for real, not just
  in a diagnostic.
- Diagnostic scripts (`diag.js`, `diag_loud.js`) were run from the scratchpad
  directory, not committed — they were exploratory/verification tooling, not
  part of the deliverable.
