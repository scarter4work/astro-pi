# ASCII console-string fix report

## Scope

Two live, user-visible non-ASCII-in-console-string bugs, per PCL's `const char*` ->
ISO-8859-1 reading of console output (a UTF-8 byte sequence in a string literal that
reaches the Process Console renders as mojibake).

## Fix 1: `src/lib/calibration/src/channel_decomposer.cpp`

Line 35, inside the `SingularQError` message thrown when the QE matrix is singular.
Raw UTF-8 em-dash (U+2014, bytes `e2 80 94`), surrounded by spaces on both sides, was
in the string literal itself (not a comment). Replaced with ASCII `" -- "`, matching
the convention used elsewhere on this branch. No other wording changed.

Before:
```cpp
throw SingularQError("Q matrix for (" + camera + ", " + filter_name +
                     ") is singular — QE values must form a non-degenerate basis. " +
                     "Filter QE in DB is suspect. Report bug + check override.");
```

After:
```cpp
throw SingularQError("Q matrix for (" + camera + ", " + filter_name +
                     ") is singular -- QE values must form a non-degenerate basis. " +
                     "Filter QE in DB is suspect. Report bug + check override.");
```

## Fix 2: `src/module/NukeXProgress.cpp`

Line 117, the outermost-phase banner written by `begin_phase()` at `depth_ == 1` --
printed once per phase, for every phase of every run. Six hex-escaped UTF-8 bytes
(`\xe2\x95\x90` x6, each one U+2550, a double horizontal line) inside the format
string literal. Being escaped (not a literal Unicode character) is why this survived
a plain visible-character grep. Replaced each triple-escape with ASCII `=`, giving
`"=== %s (%d steps) ==="`. Format specifiers and arguments unchanged.

Before:
```cpp
console_.WriteLn( String().Format( "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90 %s (%d steps) \xe2\x95\x90\xe2\x95\x90\xe2\x95\x90",
                                   name.c_str(), total_steps ) );
```

After:
```cpp
console_.WriteLn( String().Format( "=== %s (%d steps) ===",
                                   name.c_str(), total_steps ) );
```

## Build

`cmake --build build -j$(nproc)` -- clean build, no warnings/errors introduced,
`NukeX-pxm.so` linked successfully.

## Full test suite

`ctest --test-dir build --output-on-failure` -- **73/73 passed**, 61.73s total.
No test was affected by either change (expected -- neither touches logic, only
string content).

## Non-ASCII-in-literal scan (both raw characters and escaped byte sequences)

Wrote a small tokenizing scanner (Python) that walks each `.cpp`/`.h`/`.hpp`/`.cc`/
`.cxx` file under `src/` character-by-character, tracking whether the current
position is inside a `//` line comment, a `/* */` block comment, a string literal,
or a char literal, and flags:
- any raw byte with codepoint > 0x7F occurring *inside* a string or char literal
- any `\xHH...`, octal `\NNN`, `\uXXXX`, or `\UXXXXXXXX` escape *inside* a string or
  char literal whose decoded value is > 0x7F

Comments are excluded by construction (the state machine skips their contents),
so it does not flag the legitimate non-ASCII in this codebase's comments (em-dashes,
box-drawing banners, ×/µ/σ math symbols, etc.).

Script: `/tmp/claude-1000/-home-scarter4work-projects-nukex5/b4fa40e8-a54f-4e20-8924-28702af5e54f/scratchpad/scan_nonascii.py`

Command:
```
python3 <scratchpad>/scan_nonascii.py
```

Output:
```
Total findings: 0
```

Cross-checks performed to sanity-check the scanner:
1. `grep -rlP '[^\x00-\x7f]' src/ --include=*.cpp --include=*.h --include=*.hpp --include=*.cc --include=*.cxx`
   lists ~65 files containing *some* raw non-ASCII byte somewhere (mostly comments:
   em-dashes, box-drawing section banners, math symbols). Spot-checked several
   (`qe_database.cpp`, `gpu_executor.cpp`) -- confirmed the flagged lines are `//`
   comments, consistent with the tokenizer finding 0 in-literal occurrences.
2. `grep -rnoP '\\x[0-9a-fA-F]{2,}|\\u[0-9a-fA-F]{4}|\\U[0-9a-fA-F]{8}' src/ ...`
   (searching for escape sequences anywhere at all, comments included) returned no
   matches anywhere in `src/` after the fix -- the two occurrences fixed here were
   the only escaped non-ASCII byte sequences in the tree.
3. `grep -c 'R"('` across `src/**/*.cpp` and `src/**/*.hpp` -- no raw string literals
   present, so the tokenizer's lack of raw-string handling is not a gap here.

**Result: no further in-literal non-ASCII occurrences remain.**

## Commit

Single commit, both files, on branch `v5-channel-registration`. See git log for
exact SHA/message.
