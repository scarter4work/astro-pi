# Task 2 Report: `runCli` — rc-astro CLI invocation layer with NDJSON parsing

## Summary

Added `RCAstro.runCli(tool, argsArray, onEvent)` to `RCAstroLib.jsh`, exactly per the
brief's Step 3 code, with one adaptation to the already-established environment fact
from Task 1: `CoreApplication.processEvents()` is not callable in this PI 1.9.4
"Lockhart" build, so the poll loops use the bare global `processEvents()` (same as
`findBinary()` already does). Everything else in the implementation matches the brief
verbatim.

## Files changed

- `/home/scarter4work/PixInsightScripts/RC-Astro/RCAstroLib.jsh` — added `runCli`
  (appended after `fail`, comma-joined into the `RCAstro` object literal; no existing
  methods renamed or altered).
- `/home/scarter4work/PixInsightScripts/RC-Astro/test/t_lib_runcli.js` — new test,
  copied verbatim from the brief's Step 1 code block.
- `/home/scarter4work/PixInsightScripts/RC-Astro/.superpowers/sdd/progress.md` —
  appended a Task 2 ledger entry (matching the existing Task 1 entry's format).

## TDD evidence

### RED (before implementing `runCli`)

```
$ rm -f /tmp/rc_t2_result.log && test/run-headless.sh "$PWD/test/t_lib_runcli.js" >/tmp/rc_t2_console_red.log 2>&1
$ grep -q "PASS t_lib_runcli" /tmp/rc_t2_result.log 2>/dev/null && echo OK || { echo FAILED; cat /tmp/rc_t2_result.log; }
FAILED
FAIL: RCAstro.runCli is not a function
```

Confirms the test genuinely exercises the new method and fails for the right reason
before implementation exists.

### GREEN (after implementing `runCli`)

```
$ rm -f /tmp/rc_t2_result.log && test/run-headless.sh "$PWD/test/t_lib_runcli.js" > /tmp/rc_t2_console_final.log 2>&1
exit=0
$ cat /tmp/rc_t2_result.log
progress events seen: true
PASS t_lib_runcli
```

Re-ran a second time after committing to confirm it's not a fluke — same result both
times.

## GPU verification (and a brief discrepancy found)

The brief's Step 4 says to additionally confirm GPU usage via
`grep -i "Using gpu" /tmp/rc_t2_console.log`, on the theory that rc-astro's own stdout
is echoed through the PJSR console handler. **This does not work as written.** Task 1
already established that `console.*` calls do not reach process stdout under
`PixInsight --automation-mode` in this build — and that applies equally to `runCli`'s
own `console.writeln("running: " + cmdLine)` and the `dispatch()` function's
`console.writeln`/`criticalln`/`warningln` calls, not just to the code that discovered
the fact originally (`findBinary`). Checking `/tmp/rc_t2_console.log` after the GREEN
run confirms it: only the PixInsight banner + an "Unconditional exit" warning appear,
nothing from rc-astro.

Rather than accept this on faith or skip GPU verification, I wrote a one-off
(uncommitted, scratchpad-only) script that calls `RCAstro.runCli` with the same args
and captures every NDJSON event via `onEvent` directly into a file-based log (same
RESULT_LOG pattern), instead of relying on console/stdout. Result:

```
elapsed ms: 2778
ok: true exit: 0 errorMsg:
EVENT: {"event":"info","topic":"version","cliVersion":"0.9.10","schemaVersion":4}
EVENT: {"event":"status","phase":"initializing","message":"Initializing"}
EVENT: {"event":"info","topic":"device","device":"gpu","id":"gpu","name":"NVIDIA GeForce RTX 5070 Ti","provider":"CUDA","runtime":"onnxruntime 1.23.2"}
EVENT: {"event":"progress","done":1.7,...}
...
EVENT: {"event":"status","phase":"complete","message":"Done", ...}
```

This confirms: (a) the CLI actually selected the GPU device (`"device":"gpu"`,
`"name":"NVIDIA GeForce RTX 5070 Ti"`, `"provider":"CUDA"`) — not a silent CPU
fallback — and (b) the ~2.8s wall-clock matches the task's expectation of "~3s on
GPU" for this test image (a CPU run of the same 512-tile benchmark measured ~0.9s/tile
single-threaded during CLI exploration, so a CPU run of the full image would be far
slower than 2.8s). The verification script was not committed — it's throwaway
diagnostic tooling, not part of the deliverable.

**Recommendation for future tasks/brief authors:** any verification instruction that
says "grep console output" should instead say "grep the RESULT_LOG" or "capture via
onEvent into a file," consistent with the environment fact already documented in the
Global Constraints. I did not modify the brief; I'm flagging this so Task 3–5 authors
don't repeat the same now-known-wrong verification recipe.

## Implementation notes

- `runCli` builds `<bin> --no-banner <tool> <quoted args...> --json --overwrite`.
  Verified empirically (direct CLI invocation, outside PI) that `rc-astro` accepts
  `--json`/`--overwrite` as trailing global options after the subcommand and its
  arguments — CLI11-style parsing handles the intermixing fine:
  `rc-astro --no-banner bxt --benchmark 1 --json --device cpu` produced valid NDJSON
  on the first line, confirming placement is correct before ever running it inside PI.
- NDJSON parsing is line-buffered across `onStandardOutputDataAvailable` calls (PJSR
  delivers stdout in chunks, not guaranteed to be line-aligned) — buffer text and
  split on `\n`, keeping any partial trailing line in `buffer` for the next callback.
  A final `if (buffer.length) dispatch(buffer)` after the process exits, handles any
  unterminated last line.
- Lines that don't start with `{` are treated as non-JSON stray output and passed to
  `console.writeln` rather than JSON-parsed (defensive — matches the brief).
- `errorMsg` is only set from `{"event":"error",...}` NDJSON objects, or synthesized
  from a nonzero exit code if the CLI dies without ever emitting a JSON error object.
  `ok` is `exit==0 && errorMsg==""`.
- Reused the environment-fact fix from Task 1 (bare `processEvents()`) rather than the
  brief's `CoreApplication.processEvents()` — per the task's explicit instruction to
  use the bare global and to treat the brief's snippet as possibly using the wrong
  form in comments.
- `runCli` does not call `RCAstro.fail()` on rc-astro exiting nonzero or emitting an
  error event — only on binary-not-found or launch failure (`ExternalProcess.start()`
  throwing). This matches the brief's note: "runCli returns a result object; callers
  decide whether to fail."

## Self-review

- Confirmed `RCAstroLib.jsh`'s existing methods (`findBinary`, `tempDir`, `saveView`,
  `importResult`, `applyInPlace`, `newWindow`, `cleanup`, `fail`) are untouched except
  for the trailing comma added after `fail`'s closing brace to admit `runCli` into the
  object literal — no renames, no behavior changes to Task 1's engine API.
- Confirmed the test never calls `resultWindow.forceClose()` on anything passed to
  `applyInPlace` — this test doesn't use `applyInPlace` at all (it uses
  `importResult` + inspects the standalone window + `forceClose()`s it directly, which
  is the correct/expected pattern since ownership transfer only applies to
  `applyInPlace`).
- `RCAstro.cleanup([inP, outP])` is called before the "PASS" line is written, and the
  test's temp dir and files are gone afterward (verified via the Task-1-established
  cleanup path, which this test relies on unchanged).
- Ran the GREEN test twice for reproducibility (once pre-commit, once post-commit as
  a final confirmation) — both produced identical `PASS t_lib_runcli` /
  `progress events seen: true` output.
- No workarounds were taken for the console/stdout discrepancy — it was surfaced
  loudly (this report + the progress ledger) rather than silently declaring victory on
  a `grep` that could never have matched.

## Concerns

1. **Brief Step 4's stdout-grep instruction is unusable as written** (see GPU
   Verification section above) — flagging for whoever picks up Tasks 3–5 so the same
   mistake isn't repeated in their briefs' verification steps.
2. `runCli`'s quoting (`quote()` regex-based token quoting) is unit-untested in
   isolation — it's only exercised end-to-end via this one CLI invocation, which
   happens not to contain shell metacharacters worth stress-testing (paths under
   `/home/...`, plain numeric args). A future task that passes arguments with spaces
   or shell-special characters should add a focused test for `quote()`'s edge cases,
   but that's out of scope for this task's brief.
3. Verified only the `bxt` tool's NDJSON schema (`info`/`status`/`progress`/`error`
   event shapes). `sxt`/`nxt` are assumed to share the same schema (their `--help`
   output showed the same `--json` flag) but were not directly exercised here.

## Commits

- `704a3fc` — feat: runCli with NDJSON parsing + GPU CLI integration test

---

## Review fixes (Findings 1-3)

Three Important review findings on `runCli` were fixed in a follow-up commit.

### Finding 1 — stderr lost from errorMsg

`RCAstro.runCli` echoed stderr via `console.warningln` but never folded it into the
returned `errorMsg`, so a failure that happens before rc-astro ever emits a JSON
`{"event":"error"}` (crash, bad argument caught pre-JSON-init, license failure) only
produced the generic `"rc-astro exited with code N"` — violating the project's
"no silent fallbacks that hide real errors" rule.

**Fix**: accumulate stderr text into `stderrText` inside `onStandardErrorDataAvailable`
(keeping the existing `console.warningln` echo). When the run fails (`ok == false`)
and no JSON error event set `errorMsg`, build `errorMsg` from `stderrText.trim()`,
falling back to the generic exit-code message only when that's also empty.

**Empirical surprise while verifying**: manually invoking `rc-astro` outside PI with a
corrupt (non-XISF) input file showed the crash message
(`terminate called after throwing an instance of 'rcastro::Error' ... is not a valid
XISF file (bad signature)`) on real fd 2 (stderr), confirmed via shell redirection.
But when the identical command is run through PJSR's `ExternalProcess` inside the
xvfb/PixInsight automation harness, that same text arrives via
`onStandardOutputDataAvailable`, NOT `onStandardErrorDataAvailable` — `errCallbacks`
stayed at 0 and `p.stderr`/`p.standardError` were empty after the process exited, even
after pumping `processEvents()` an extra 50 times. This was confirmed with a disposable
probe script (not committed) that logged every callback and post-exit property read.
Root cause is presumably how PCL's ExternalProcess/Qt-based channel plumbing handles a
child that dies by signal (SIGABRT, `exitStatus == 1`) with output written right before
the abort — it is not something `runCli` can rely on being routed to the "correct" fd.

**Fix (extended)**: `dispatch()`'s existing non-JSON branch (which already
`console.writeln()`s any stray non-`{`-prefixed line) now also accumulates that text
into `strayText`. The final fallback logic prefers `stderrText`, then `strayText`, then
the generic exit-code message. This makes `errorMsg` robust regardless of which channel
the harness happens to deliver the text on.

### Finding 2 — no negative-path test

Added a second scenario to `test/t_lib_runcli.js`: after the positive BXT run and
cleanup, it writes a bogus (non-XISF) file via `RCAstro.tempDir()` and calls
`RCAstro.runCli("bxt", [badInP, "--device", "gpu", "--output", badOutP], null)`. This is
a genuine rc-astro failure (the CLI process aborts with SIGABRT on the bad XISF
signature — not simulated/mocked). Asserts `r2.ok === false` and `r2.errorMsg` is a
non-empty string, and logs the observed `errorMsg` to `RESULT_LOG`.

### Finding 3 — GPU selection unasserted

Extended the existing positive-path `onEvent` callback in `test/t_lib_runcli.js` to
also capture the device-info event (`{"event":"info","topic":"device","device":"gpu",
"id":"gpu","name":"NVIDIA GeForce RTX 5070 Ti","provider":"CUDA","runtime":"onnxruntime
1.23.2"}` — real observed shape, not invented). Asserts `deviceInfo != null` and
`deviceInfo.device == "gpu"`, and logs the device name to `RESULT_LOG`.

### Verify (genuine, ran twice for reproducibility)

```
cd /home/scarter4work/PixInsightScripts/RC-Astro && rm -f /tmp/rc_t2_result.log && test/run-headless.sh "$PWD/test/t_lib_runcli.js" >/dev/null 2>&1; cat /tmp/rc_t2_result.log
```

Output (both runs identical in structure, temp dir names differ per run):

```
progress events seen: true
device used: gpu (NVIDIA GeForce RTX 5070 Ti)
negative-path errorMsg: terminate called after throwing an instance of 'rcastro::Error'
what():  '/home/scarter4work/pixinsight-swap/rc-astro-<run-id>/not_really_xisf.xisf' is not a valid XISF file (bad signature)
PASS t_lib_runcli
```

### Files changed
- `RCAstroLib.jsh` — `runCli`: added `stderrText`/`strayText` accumulation and the
  stderr/stray-text fallback for `errorMsg`. No signature or public-API change;
  `{exit, ok, errorMsg}` shape and `ok` semantics (`exit==0 && errorMsg==""`) unchanged.
- `test/t_lib_runcli.js` — added device-info capture/assertion to the existing positive
  run, and a new negative-path block exercising a genuine rc-astro crash.

### Concerns for future tasks
- The stdout/stderr channel-routing quirk for signal-killed child processes (Finding 1
  investigation) is worth a standing note for Tasks 3-5: any code that inspects
  `ExternalProcess.stderr`/`standardError` directly (rather than going through
  `runCli`'s combined fallback) may miss output for processes that die by signal.
- Only the `bxt` tool's crash/device-event schema was exercised here (reusing Task 1's
  note that `sxt`/`nxt` are assumed, not verified, to share the same schema).

## Commits (fix)

- (see commit created immediately after this report entry)
