#!/usr/bin/env python3
"""Static guard (0.2.2.0, CPD rules 5.1/5.4): PI Copilot never evaluates code
it did not ship.

Every call that hands text to a JavaScript engine (MetaModule::EvaluateScript,
the module's EvaluateAsciiJson wrapper) or asks the core to run a script file
(Console::ExecuteScript*) is listed below, per production source file, with
the reason its input is the module's own fixed text (data only as
ScriptLiteral() literals, integers, or identifiers from PixInsight's own
process catalog). A new call site, a removed one, or any trace of the removed
run_pjsr tool fails the run until it is reviewed and listed here.

Self-test sources (*SelfTest*.cpp, JourneySpikeProbe.cpp) are test-only (they
run only when the harness sets PICOPILOT_SELFTEST_OUT) and are not limited.

usage: check-eval-sites.py            -> exit 0 when clean, 1 with the findings
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src", "module")

CALL = re.compile(r"\b(EvaluateScript|EvaluateAsciiJson|ExecuteScriptOn|ExecuteScriptGlobal|ExecuteScript)\s*\(")
FORBIDDEN = re.compile(r"run_pjsr|RunPjsr(?!Enabled)|CheckPjsrSyntax|PjsrRunner|ScriptConfirmDialog|confirmScript\b")
# The retired setting's key may appear only where it is removed at load.
RETIRED_KEY = re.compile(r"RunPjsrEnabled")
RETIRED_KEY_FILES = {"CopilotSettings.cpp"}

# file -> {call: count}; every entry was reviewed (see the docstring).
ALLOWED = {
    # EvaluateAsciiJson's own definition and its single EvaluateScript (fixed helpers + the caller's body).
    "ModuleScript.cpp": {"EvaluateAsciiJson": 1, "EvaluateScript": 1},
    "ModuleScript.h": {"EvaluateAsciiJson": 1},
    # View history read: fixed text + ScriptLiteral(view id) + integers.
    "HistoryReader.cpp": {"EvaluateAsciiJson": 1},
    # history_step: fixed text + ScriptLiteral(view id) + an integer index.
    "HistoryStepTool.cpp": {"EvaluateAsciiJson": 1},
    # Workspace process icons: fixed text + ScriptLiteral(JSON list of process ids).
    "WorkspaceIcons.cpp": {"EvaluateAsciiJson": 1},
    # Journey batch counts: fixed text + ScriptLiteral(view ids).
    "JourneyTracker.cpp": {"EvaluateScript": 1},
    # Enumeration element ids: fixed text + the core's own process/parameter ids (IsValidIdentifier) + integers.
    "ProcessCatalog.cpp": {"EvaluateScript": 1},
    # Script signatures: fixed text + ScriptLiteral(JSON list of real paths) -> Security.getScriptSignature().
    "ScriptCatalog.cpp": {"EvaluateAsciiJson": 1},
    # run_script: console capture (fixed text, twice), and the core runs the eligible, re-verified file
    # (ExecuteScriptOn / ExecuteScriptGlobal).
    "ScriptTools.cpp": {"EvaluateAsciiJson": 2, "ExecuteScriptOn": 1, "ExecuteScriptGlobal": 1},
}
# Comments that mention the API by name, not calls.
COMMENT_ONLY = re.compile(r"^\s*(//|\*|/\*)")


def is_test_only(name):
    return "SelfTest" in name or name.startswith("JourneySpikeProbe")


def scan(src):
    found, bad = {}, []
    for name in sorted(os.listdir(src)):
        if not name.endswith((".cpp", ".h", ".h.in")):
            continue
        text = open(os.path.join(src, name), encoding="utf-8", errors="replace").read()
        for n, line in enumerate(text.splitlines(), 1):
            if is_test_only(name):
                continue
            if FORBIDDEN.search(line) and not COMMENT_ONLY.match(line):
                bad.append("%s:%d: trace of the removed run_pjsr tool: %s" % (name, n, line.strip()))
            if RETIRED_KEY.search(line) and not COMMENT_ONLY.match(line) and name not in RETIRED_KEY_FILES:
                bad.append("%s:%d: the retired setting is used outside its cleanup: %s" % (name, n, line.strip()))
            if COMMENT_ONLY.match(line):
                continue
            for m in CALL.finditer(line):
                found.setdefault(name, {}).setdefault(m.group(1), 0)
                found[name][m.group(1)] += 1
    return found, bad


def main():
    # Self-check: the scanner must see a planted call (a guard that sees nothing proves nothing).
    if not CALL.search('   r = ThePICopilotModule->EvaluateScript( code, "JavaScript" );'):
        print("FAIL: check-eval-sites.py self-check: the call pattern no longer matches")
        return 1
    found, bad = scan(SRC)
    for name, calls in found.items():
        if ALLOWED.get(name) != calls:
            bad.append("%s: evaluation/script calls %r, reviewed %r" % (name, calls, ALLOWED.get(name)))
    for name, calls in ALLOWED.items():
        if name not in found:
            bad.append("%s: reviewed calls %r no longer present (update the list)" % (name, calls))
    if bad:
        print("FAIL: evaluation call sites changed or run_pjsr traces found:")
        for b in bad:
            print("  " + b)
        return 1
    total = sum(sum(c.values()) for c in found.values())
    print("eval-site guard: OK (%d reviewed call sites in %d production files)" % (total, len(found)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
