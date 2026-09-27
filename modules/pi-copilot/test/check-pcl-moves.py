#!/usr/bin/env python3
# PI Copilot -- static guard for the moved-from PCL value bug class (Task 10 fix round 5).
#
# A moved-from pcl::String / IsoString (and the PCL containers built the same
# way) holds a null data pointer, and PCL's copy AND move assignment dereference
# it. So anything that ASSIGNS INTO a moved-from object crashes PixInsight:
#   - std::sort / stable_sort / remove_if / unique / rotate / reverse / swap,
#     vector::erase or insert anywhere but the end, deque erase -- on a std
#     sequence container whose elements are, or contain, such a PCL value;
#   - reusing a data member after `x = std::move( m_member )`.
# (Proven standalone: re-review round 3; the round-4 scrubber SIGSEGV; the
# PendingFreeze erase; PICopilotInterface's held reply.) push_back and growth
# only move-CONSTRUCT and destroy, which is safe.
#
# The rule this script enforces over modules/pi-copilot/src/module:
#   R1  A std::vector / deque / list / array whose element type is, or holds
#       (transitively, by value), a PCL value type is not declared at all,
#       unless the declaring line carries "pcl-move-ok: <why>" AND the file
#       never erases / inserts / sorts / removes / swaps / rotates / reverses /
#       uniques that variable. Hold such elements by std::unique_ptr, use a PCL
#       container (StringList, Array), or store std::string (UTF-8) instead.
#       References and pointers to such containers (function parameters) are fine.
#   R2  No `std::move( m_<member> )` (moving out of a data member that is later
#       reused) unless the line carries "pcl-move-ok: <why>".
# The script first checks itself against known-bad snippets, so a broken guard
# fails loudly instead of passing everything.
import os
import re
import sys

PCL_VALUE_TYPES = {"String", "IsoString", "StringList", "IsoStringList", "FITSKeywordArray", "FITSHeaderKeyword",
                   "ByteArray", "Variant", "Array", "ReferenceArray"}
MUTATIONS = [r"{v}\s*\.\s*(erase|insert|emplace)\s*\(", r"(sort|stable_sort|remove_if|remove|unique|rotate|reverse|partition|"
             r"nth_element|shuffle)\s*\(\s*{v}\s*\.", r"std::swap\s*\(\s*{v}\b", r"iter_swap\s*\(\s*{v}\b"]
DECL = re.compile(r"std::(vector|deque|list|array)\s*<")


def strip_comments(src):
    """Blanks string / char literals and comments (line numbers kept), so a "/*" inside a string never hides code."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == "R" and src.startswith('R"', i):
            m = re.match(r'R"([^(\s]*)\(', src[i:])
            if m:
                end = src.find(")" + m.group(1) + '"', i + m.end())
                end = n if end < 0 else end + len(m.group(1)) + 2
                out.append(re.sub(r"[^\n]", " ", src[i:end]))
                i = end
                continue
        if c in "\"'":
            j = i + 1
            while j < n and src[j] != c and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            out.append(c + " " * (min(j, n) - i - 1) + (c if j < n and src[j] == c else ""))
            i = j + 1 if j < n and src[j] == c else j
            continue
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
            continue
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", src[i:j]))
            i = j
            continue
        out.append(c)
        i += 1
    return "".join(out)


def bodies(src):
    """(name, body) of every struct/class definition."""
    for m in re.finditer(r"\b(struct|class)\s+([A-Za-z_]\w*)\s*(?:final\s*)?(?::[^{;()]*)?\{", src):
        i = m.end()
        depth = 1
        j = i
        while j < len(src) and depth:
            if src[j] == "{":
                depth += 1
            elif src[j] == "}":
                depth -= 1
            j += 1
        yield m.group(2), src[i:j - 1]


def member_types(body):
    # top-level statements of the body (nested braces -- methods, nested types -- removed)
    flat = body
    for _ in range(6):
        flat = re.sub(r"\{[^{}]*\}", ";", flat)
    types = set()
    for stmt in flat.split(";"):
        stmt = re.sub(r"//[^\n]*", "", stmt).strip()
        stmt = re.sub(r"^(public|private|protected)\s*:\s*", "", stmt)
        if not stmt or "(" in stmt or stmt.startswith(("using ", "typedef ", "friend ", "enum ", "static ", "template")):
            continue
        for t in re.findall(r"[A-Za-z_]\w*", stmt.split("=")[0]):
            types.add(t)
    return types


def bearing_types(sources):
    defs = {}
    for src in sources:
        for name, body in bodies(src):
            defs.setdefault(name, set()).update(member_types(body))
    bearing = set()
    changed = True
    while changed:
        changed = False
        for name, used in defs.items():
            if name not in bearing and (used & PCL_VALUE_TYPES or used & bearing):
                # a unique_ptr / shared_ptr / pointer member is indirection, but member_types cannot tell the
                # template args apart; be conservative only for the direct PCL value types and known bearing types.
                bearing.add(name)
                changed = True
    return bearing


def template_text(line, start):
    depth = 0
    for k in range(start, len(line)):
        if line[k] == "<":
            depth += 1
        elif line[k] == ">":
            depth -= 1
            if depth == 0:
                return line[start:k + 1], k + 1
    return line[start:], len(line)


def check_file(name, src, bearing, problems):
    code = strip_comments(src)
    lines = code.split("\n")
    raw = src.split("\n")
    for ln, line in enumerate(lines, 1):
        comment_free = re.sub(r"//.*", "", line)
        annotated = "pcl-move-ok:" in raw[ln - 1] if ln - 1 < len(raw) else False
        # R2
        if re.search(r"std::move\s*\(\s*m_\w+\s*\)", comment_free) and not annotated:
            problems.append(f"{name}:{ln}: R2 moving out of a data member (reused later?): {line.strip()}")
        # R1
        for m in DECL.finditer(comment_free):
            targs, end = template_text(comment_free, m.end() - 1)
            inner = targs[1:-1]
            if re.match(r"\s*(std::)?(unique_ptr|shared_ptr)\b", inner) or inner.strip().endswith("*"):
                continue
            words = set(re.findall(r"[A-Za-z_]\w*", inner))
            words -= {"std", "pcl", "const", "unique_ptr", "shared_ptr"}
            # pointers / unique_ptr nested inside a pair etc. are still flagged: be explicit then.
            hit = words & (PCL_VALUE_TYPES | bearing)
            if not hit:
                continue
            rest = comment_free[end:].lstrip()
            if rest.startswith(("&", "*", ">")) or rest.startswith("const") :
                continue   # a reference / pointer (parameter) or a nested template argument
            var = re.match(r"([A-Za-z_]\w*)", rest)
            if var is None:
                continue   # a return type or a cast
            v = var.group(1)
            if not annotated:
                problems.append(f"{name}:{ln}: R1 std::{m.group(1)} of {sorted(hit)} (moved-from PCL values crash on "
                                f"erase/insert/sort): {line.strip()}")
                continue
            for pat in MUTATIONS:
                if re.search(pat.format(v=re.escape(v)), code):
                    problems.append(f"{name}:{ln}: R1 '{v}' is annotated pcl-move-ok but the file mutates it "
                                    f"({pat.format(v=v)})")


def run(files):
    sources = {f: open(f, encoding="utf-8").read() for f in files}
    bearing = bearing_types(sources.values())
    problems = []
    for f, src in sources.items():
        check_file(os.path.basename(f), src, bearing, problems)
    return problems, bearing


BAD_FIXTURES = {
    "bad1.cpp": "struct PendingFreeze { const void* handle = nullptr; std::string id; String reason; };\n"
                "std::vector<PendingFreeze> m_pendingFreeze;\n"
                "void f() { m_pendingFreeze.erase( m_pendingFreeze.begin() + 1 ); }\n",
    "bad2.cpp": "struct R { String text; };\nstruct X { R m_held; void g() { R r = std::move( m_held ); m_held = R(); } };\n",
    "bad3.cpp": "struct N { IsoString id; };\nstruct W { N n; };\nvoid h() { std::vector<W> v; std::sort( v.begin(), v.end() ); }\n",
    "bad4.cpp": "std::vector<String> dirs; // pcl-move-ok: read only\nvoid k() { std::sort( dirs.begin(), dirs.end() ); }\n",
}
GOOD_FIXTURE = ("struct P { std::string a; };\nstd::vector<P> v;\nvoid f( const std::vector<String>& in );\n"
                "std::vector<std::unique_ptr<W>> w;\nstd::vector<String> ok; // pcl-move-ok: push_back + read only\n")


def self_check(tmp):
    os.makedirs(tmp, exist_ok=True)
    for name, text in BAD_FIXTURES.items():
        p = os.path.join(tmp, name)
        open(p, "w").write(text)
        problems, _ = run([p])
        if not problems:
            return f"self-check: the guard did not flag {name}"
    p = os.path.join(tmp, "good.cpp")
    open(p, "w").write(GOOD_FIXTURE)
    problems, _ = run([p])
    if problems:
        return "self-check: the guard flagged a safe snippet: " + "; ".join(problems)
    return ""


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    srcdir = os.path.join(here, "..", "src", "module")
    tmp = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.environ.get("TMPDIR", "/tmp"), "pcl-move-guard-selfcheck")
    err = self_check(tmp)
    if err:
        print("FAIL: " + err)
        return 2
    files = sorted(os.path.join(srcdir, f) for f in os.listdir(srcdir) if f.endswith((".h", ".cpp")))
    problems, bearing = run(files)
    if problems:
        print("FAIL: moved-from PCL value guard (test/check-pcl-moves.py):")
        for p in problems:
            print("  " + p)
        return 1
    print("pcl-move guard: OK (%d files, %d PCL-value-bearing types)" % (len(files), len(bearing)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
