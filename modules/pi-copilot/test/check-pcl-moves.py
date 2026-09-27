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


MUTATIONS_R6 = [r"(?:std::)?ranges::(sort|stable_sort|remove_if|remove|unique|rotate|reverse|shuffle|partition)\s*\(\s*{v}\b",
                r"(?:std::)?(?:ranges::)?(sort|stable_sort|remove_if|unique|rotate|reverse)\s*\(\s*{v}\s*[,)]"]
MOVED = re.compile(r"std::move\s*\(\s*((?:this\s*->\s*)?[A-Za-z_]\w*(?:\s*(?:\.|->)\s*[A-Za-z_]\w*)*)\s*\)")


def container_decl_regex(code):
    if re.search(r"using\s+namespace\s+std\s*;", code) or re.search(r"using\s+std::(vector|deque|list|array)\s*;", code):
        return re.compile(r"(?<![\w:])(?:std::)?(vector|deque|list|array)\s*<")
    return re.compile(r"std::(vector|deque|list|array)\s*<")


def bearing_container(inner, bearing):
    if re.match(r"\s*(std::)?(unique_ptr|shared_ptr)\b", inner) or inner.strip().endswith("*"):
        return set()
    words = set(re.findall(r"[A-Za-z_]\w*", inner)) - {"std", "pcl", "const", "unique_ptr", "shared_ptr"}
    return words & (PCL_VALUE_TYPES | bearing)


def aliases_of(code, bearing):
    """using A = std::vector<Bearing>;  typedef std::vector<Bearing> A;  -> {A: hit}"""
    out = {}
    decl = container_decl_regex(code)
    for m in re.finditer(r"\busing\s+([A-Za-z_]\w*)\s*=\s*([^;]+);", code):
        d = decl.search(m.group(2))
        if d:
            targs, _ = template_text(m.group(2), d.end() - 1)
            hit = bearing_container(targs[1:-1], bearing)
            if hit:
                out[m.group(1)] = hit
    for m in re.finditer(r"\btypedef\s+([^;]+?)\s+([A-Za-z_]\w*)\s*;", code):
        d = decl.search(m.group(1))
        if d:
            targs, _ = template_text(m.group(1), d.end() - 1)
            hit = bearing_container(targs[1:-1], bearing)
            if hit:
                out[m.group(2)] = hit
    return out


def mutated(v, all_code):
    for pat in MUTATIONS + MUTATIONS_R6:
        rx = pat.format(v=re.escape(v))
        if re.search(rx, all_code):
            return rx
    return ""


def function_ends(code):
    """For every offset, where the enclosing FUNCTION body closes: {offset of '{': offset of its '}'}, only for
    the outermost code brace (a function body, not a namespace/class/struct/enum/extern scope). A moved-from
    local cannot outlive that body, so R3 never looks past it into another function."""
    spans = []
    stack = []   # (kind, open offset)
    last = 0     # start of the text preceding the next '{' (after the last ';', '{' or '}')
    for i, c in enumerate(code):
        if c == "{":
            head = code[last:i]
            scope = re.search(r"\b(namespace|class|struct|union|enum)\b|\bextern\s*\"", head) and "(" not in head
            in_code = any(k == "code" for k, _ in stack)
            kind = "code" if (in_code or not scope) else "scope"
            stack.append((kind, i))
            last = i + 1
        elif c == "}":
            if stack:
                kind, start = stack.pop()
                if kind == "code" and not any(k == "code" for k, _ in stack):
                    spans.append((start, i))
            last = i + 1
        elif c == ";":
            last = i + 1
    return spans


def check_file(name, src, bearing, aliases, all_code, problems):
    code = strip_comments(src)
    fn_spans = function_ends(code)
    line_start = [0]
    for part in code.split("\n")[:-1]:
        line_start.append(line_start[-1] + len(part) + 1)
    lines = code.split("\n")
    raw = src.split("\n")
    decl = container_decl_regex(code)
    std_ns = re.search(r"using\s+namespace\s+std\s*;", code) is not None
    for ln, line in enumerate(lines, 1):
        annotated = "pcl-move-ok:" in raw[ln - 1] if ln - 1 < len(raw) else False
        # R2: moving out of a data member (reused later).
        if re.search(r"std::move\s*\(\s*m_\w+\s*\)", line) and not annotated:
            problems.append(f"{name}:{ln}: R2 moving out of a data member (reused later?): {line.strip()}")
        # R4 (round 6): std::swap / std::exchange move-assign into a moved-from value.
        if (re.search(r"std::(swap|exchange)\s*\(", line)
                or (std_ns and re.search(r"(?<![\w.:>])(swap|exchange)\s*\(", line))) and not annotated:
            problems.append(f"{name}:{ln}: R4 std::swap / std::exchange assigns into a moved-from value: {line.strip()}")
        # R3 (round 6): a moved-from object (local, obj.member, this->member) assigned to again later.
        for m in MOVED.finditer(line):
            target = re.sub(r"\s+", "", m.group(1))
            if target.startswith("m_") or annotated:
                continue   # data members: R2
            pos = line_start[ln - 1] + m.end()
            end = next((e for s, e in fn_spans if s < pos <= e), None)
            if end is not None:
                after = code[pos:end]   # the rest of the enclosing function body, never the next function
            else:
                tail = "\n".join(lines[ln - 1:ln + 400])
                after = tail[tail.find(m.group(0)) + len(m.group(0)):]
            pieces = [re.escape(x) for x in re.split(r"(\.|->)", target)]
            name_rx = r"\s*".join(pieces)
            if re.search(r"(^|[;{}])\s*" + name_rx + r"\s*(=(?!=)|\+=)", after, re.M) \
               or re.search(r"(?<![\w.>])" + name_rx + r"\s*\.\s*(Assign|Append|Add|Clear|Insert|Remove)\s*\(", after):
                problems.append(f"{name}:{ln}: R3 '{target}' is moved from and assigned again later (a moved-from "
                                f"PCL value crashes on assignment): {line.strip()}")
        # R1: container declarations (std::, bare under using namespace std, aliases, auto v = std::vector<...>{}).
        found = []
        for m in decl.finditer(line):
            targs, end = template_text(line, m.end() - 1)
            hit = bearing_container(targs[1:-1], bearing)
            if not hit:
                continue
            if re.search(r"\b(using|typedef)\b", line[:m.start()]):
                continue   # an alias definition: its uses are checked below
            rest = line[end:].lstrip()
            if rest.startswith(("&", "*", ">")) or rest.startswith("const"):
                continue
            var = re.match(r"([A-Za-z_]\w*)", rest)
            if var is None:
                am = re.search(r"\bauto\s+([A-Za-z_]\w*)\s*=\s*$", line[:m.start()])
                if am is None:
                    continue   # a return type or a cast
                var = am
            found.append((var.group(1), hit, "std::" + m.group(1)))
        for alias, hit in aliases.items():
            for am in re.finditer(r"(?<![\w:])" + re.escape(alias) + r"\s+([A-Za-z_]\w*)\s*[;={(]", line):
                found.append((am.group(1), hit, "alias " + alias))
        for v, hit, kind in found:
            if not annotated:
                problems.append(f"{name}:{ln}: R1 {kind} of {sorted(hit)} (moved-from PCL values crash on "
                                f"erase/insert/sort): {line.strip()}")
                continue
            rx = mutated(v, all_code)   # every source: a member declared in a .h is mutated in its .cpp (C1)
            if rx:
                problems.append(f"{name}:{ln}: R1 '{v}' is annotated pcl-move-ok but some source mutates it ({rx})")


def run(files):
    sources = {f: open(f, encoding="utf-8").read() for f in files}
    bearing = bearing_types(sources.values())
    stripped = {f: strip_comments(s) for f, s in sources.items()}
    all_code = "\n".join(stripped.values())
    aliases = {}
    for c in stripped.values():
        aliases.update(aliases_of(c, bearing))
    problems = []
    for f, src in sources.items():
        check_file(os.path.basename(f), src, bearing, aliases, all_code, problems)
    return problems, bearing


BAD_FIXTURES = {
    "bad1.cpp": "struct PendingFreeze { const void* handle = nullptr; std::string id; String reason; };\n"
                "std::vector<PendingFreeze> m_pendingFreeze;\n"
                "void f() { m_pendingFreeze.erase( m_pendingFreeze.begin() + 1 ); }\n",
    "bad2.cpp": "struct R { String text; };\nstruct X { R m_held; void g() { R r = std::move( m_held ); m_held = R(); } };\n",
    "bad3.cpp": "struct N { IsoString id; };\nstruct W { N n; };\nvoid h() { std::vector<W> v; std::sort( v.begin(), v.end() ); }\n",
    "bad4.cpp": "std::vector<String> dirs; // pcl-move-ok: read only\nvoid k() { std::sort( dirs.begin(), dirs.end() ); }\n",
}
# Round 6 (re-review 4, M1): each gap gets a known-bad snippet. Multi-file entries are checked together.
BAD_FIXTURES_R6 = {
    "cross-file member (C1's layout: annotated in .h, erased in .cpp)": {
        "x.h": "struct PF { String reason; };\nclass T { std::vector<PF> m_pf; // pcl-move-ok: claimed read only\n};\n",
        "x.cpp": "void T::f() { m_pf.erase( m_pf.begin() ); }\n"},
    "std::swap of PCL values": {"s.cpp": "void f( String& a, String& b ) { std::swap( a, b ); }\n"},
    "std::exchange out of a PCL value": {"e.cpp": "struct R { String t; };\nvoid f( R& r ) { R old = std::exchange( r, R() ); }\n"},
    "moved-from local reused": {"l.cpp": "void f() { String a = \"x\"; String b = std::move( a ); a = b; }\n"},
    "moved-from obj.member reused": {"o.cpp": "struct R { String t; };\nvoid f( R& r ) { String b = std::move( r.t ); r.t = b; }\n"},
    "moved-from this->member reused": {"t.cpp": "struct R { String t; void f() { String b = std::move( this->t ); this->t = b; } };\n"},
    "type alias of a bearing container": {"a.cpp": "using Names = std::vector<String>;\nvoid f() { Names v; std::sort( v.begin(), v.end() ); }\n"},
    "auto v = std::vector<...>{}": {"u.cpp": "void f() { auto v = std::vector<IsoString>{}; v.erase( v.begin() ); }\n"},
    "using namespace std": {"n.cpp": "using namespace std;\nvoid f() { vector<String> v; sort( v.begin(), v.end() ); }\n"},
    "ranges::sort": {"r.cpp": "std::vector<String> v; // pcl-move-ok: claimed read only\nvoid f() { std::ranges::sort( v ); }\n"},
    "moved-from StringList reused": {"sl.cpp": "void f() { StringList a; StringList b = std::move( a ); a = b; }\n"},
    "moved-from pcl::Array member": {"am.cpp": "struct Q { Array<int> m_a; void f() { Array<int> b = std::move( m_a ); m_a = b; } };\n"},
    "moved in an inner block, reused in the outer block": {
        "ib.cpp": "namespace pcl {\nvoid f( bool c ) {\n  String a;\n  if ( c ) {\n    String b = std::move( a );\n  }\n  a = \"y\";\n}\n}\n"},
}
GOOD_FIXTURE = ("struct P { std::string a; };\nstd::vector<P> v;\nvoid f( const std::vector<String>& in );\n"
                "std::vector<std::unique_ptr<W>> w;\nstd::vector<String> ok; // pcl-move-ok: push_back + read only\n"
                # Same local name in two functions: the second function's `r = ...` is not a reuse of the first's r.
                "namespace pcl {\nstruct H { String t; };\nclass I {\n  H m_h;\n  void f();\n  void g();\n};\n"
                "void I::f() {\n  { H r; m_h = std::move( r ); }\n}\n"
                "void I::g() {\n  H r;\n  r = H();\n}\n}\n")


def self_check(tmp):
    os.makedirs(tmp, exist_ok=True)
    for name, text in BAD_FIXTURES.items():
        p = os.path.join(tmp, name)
        open(p, "w").write(text)
        problems, _ = run([p])
        if not problems:
            return f"self-check: the guard did not flag {name}"
    for label, files in BAD_FIXTURES_R6.items():
        d = os.path.join(tmp, re.sub(r"\W+", "_", label))
        os.makedirs(d, exist_ok=True)
        paths = []
        for name, text in files.items():
            paths.append(os.path.join(d, name))
            open(paths[-1], "w").write(text)
        problems, _ = run(paths)
        if not problems:
            return f"self-check: the guard did not flag: {label}"
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
