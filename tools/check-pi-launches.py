#!/usr/bin/env python3
"""Fail if any tracked shell script launches PixInsight outside pi_headless.

Every PixInsight launch must either go through `pi_headless` (tools/pi-headless.sh)
or, for a harness that runs its own Xvfb, come straight after a
`pi_require_private_display` check. Anything
else can inherit WAYLAND_DISPLAY and open on the user's desktop. The one
exception is a launch whose whole point is the desktop, which must say so on
the same line: `# interactive-gui: <why>` (only after an explicit user prompt;
never in anything an automated run executes).

Usage: tools/check-pi-launches.py [repo-root]   (exit 1 and a list on violation)
"""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parent.parent)

# A command word that runs PixInsight: the wrapper script, the binary, or one
# of the variables this repo keeps them in.
LAUNCH = re.compile(
    r'(?:^|[\s;&|(]|timeout\s+\S+\s+)'
    r'(?:"?\$\{?(?:PI|PI_EXE|PICOPILOT_PI|PI_BIN)\}?(?:/PixInsight(?:\.sh)?)?"?'
    r'|"?[^\s"]*/PixInsight(?:\.sh)?"?)'
    r'(?=\s+-)')
GUARDS = ("pi_require_private_display",)

def logical_lines(text):
    """Join backslash continuations; yield (first_line_no, joined)."""
    buf, start = [], None
    for no, line in enumerate(text.splitlines(), 1):
        if start is None:
            start = no
        if line.rstrip().endswith("\\"):
            buf.append(line.rstrip()[:-1])
            continue
        buf.append(line)
        yield start, " ".join(buf)
        buf, start = [], None
    if buf:
        yield start, " ".join(buf)

def main():
    files = subprocess.run(["git", "-C", str(ROOT), "ls-files", "*.sh"],
                           capture_output=True, text=True, check=True).stdout.split()
    bad = []
    for rel in files:
        if rel.startswith(("archive/", "docs/")) or "/docs/" in rel or rel == "tools/pi-headless.sh":
            continue
        prev_code = ""
        for no, line in logical_lines((ROOT / rel).read_text(errors="replace")):
            if "# interactive-gui:" in line:
                continue
            code = line.split("#", 1)[0] if not line.lstrip().startswith("#") else ""
            if not code.strip():
                continue
            if LAUNCH.search(code) and "pi_headless" not in code \
                    and not any(g in prev_code for g in GUARDS) \
                    and not re.match(r'\s*(?:\[|\[\[|echo|die|printf|command -v|test )', code) \
                    and not re.search(r'\b(?:pgrep|pkill)\b', code):
                bad.append(f"{rel}:{no}: {line.strip()[:140]}")
            prev_code = code
    if bad:
        print("PixInsight launched outside pi_headless (tools/pi-headless.sh):", file=sys.stderr)
        print("\n".join("  " + b for b in bad), file=sys.stderr)
        return 1
    print(f"OK  every PixInsight launch in {len(files)} tracked shell scripts is headless")
    return 0

if __name__ == "__main__":
    sys.exit(main())
