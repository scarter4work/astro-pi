"""identity.py -- what a calibration or a bench record was made with (Ruling F2; final review, bench I1).

Freeze-before-use is a guard, not a convention: calibration.json records the identity it was frozen
against, every run directory the identity it was stacked with, every compare.json the identity it was
judged with, and the bench refuses to judge (status unmeasured) when they differ from what is in use.

An identity has four parts, each a SHA-256:
  engine            the engine's sources: every file under src/stack and src/ser (RunStack and the reader);
  probe_sources     the probes' own sources: test/stack-probe.cpp, test/stack-region-probe.cpp and the
                    headers they include from test/ (ProbeFits.h, ProbeSource.h);
  probe_binaries    the built stack-probe and stack-region-probe in use, byte for byte (a rebuild that
                    changes a byte is a different build: re-freeze);
  procedure         the calibration procedure and the scene generators: bench/halves.py, noise.py,
                    metric.py, refs.py, benchset.py, bayer.py and conftest.py (CAL_* constants, the
                    synthetic scenes). bench.py (the verdict rules), the README and the tests are not
                    part of it.
A source hash is over the files' relative paths and contents (sorted), so it describes the working tree
as it is -- committed or not -- and is the same on any machine. A probe binary is STALE when it is older
than a source it is built from (the engine's or its own); a stale probe is refused too.

    python3 -I identity.py [--stack-probe P] [--region-probe P]   # print the identity in use (JSON)
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TEST = os.path.dirname(HERE)
MODULE = os.path.dirname(TEST)                     # modules/planetary
ENGINE_DIRS = ("src/stack", "src/ser")
PROBE_SOURCES = ("test/stack-probe.cpp", "test/stack-region-probe.cpp", "test/ProbeFits.h", "test/ProbeSource.h")
PROCEDURE = tuple(f"test/bench/{n}" for n in ("halves.py", "noise.py", "metric.py", "refs.py", "benchset.py",
                                               "bayer.py", "conftest.py"))
PARTS = ("engine", "probe_sources", "probe_binaries", "procedure")
BUILD = os.path.join(MODULE, "build", "test")
DEFAULT_PROBES = (os.path.join(BUILD, "stack-probe"), os.path.join(BUILD, "stack-region-probe"))


def _sha_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def engine_files(root=MODULE):
    out = []
    for d in ENGINE_DIRS:
        for dirpath, _, names in os.walk(os.path.join(root, d)):
            out += [os.path.relpath(os.path.join(dirpath, n), root) for n in names]
    return sorted(out)


def content_hash(rel_paths, read):
    """SHA-256 over 'path NUL sha256(content) LF' for each relative path, sorted; read(rel) -> bytes."""
    h = hashlib.sha256()
    for rel in sorted(rel_paths):
        h.update(rel.encode() + b"\0" + hashlib.sha256(read(rel)).hexdigest().encode() + b"\n")
    return h.hexdigest()


def _read_tree(root):
    def read(rel):
        with open(os.path.join(root, rel), "rb") as fh:
            return fh.read()
    return read


def source_parts(root=MODULE):
    """{engine, probe_sources, procedure} of the tree at `root` (a modules/planetary directory)."""
    read = _read_tree(root)
    return {"engine": content_hash(engine_files(root), read),
            "probe_sources": content_hash(PROBE_SOURCES, read),
            "procedure": content_hash(PROCEDURE, read)}


def source_parts_at(commit, repo=None):
    """The same three parts for the tree of a git commit (git show; nothing checked out)."""
    repo = repo or os.path.dirname(os.path.dirname(MODULE))
    prefix = "modules/planetary/"
    listed = subprocess.run(["git", "-C", repo, "ls-tree", "-r", "--name-only", commit, "--",
                             *(prefix + d for d in ENGINE_DIRS)], check=True, capture_output=True, text=True).stdout
    eng = [p[len(prefix):] for p in listed.split()]

    def read(rel):
        return subprocess.run(["git", "-C", repo, "show", f"{commit}:{prefix}{rel}"], check=True,
                              capture_output=True).stdout
    return {"engine": content_hash(eng, read), "probe_sources": content_hash(PROBE_SOURCES, read),
            "procedure": content_hash(PROCEDURE, read)}


def stale(binary, root=MODULE):
    """The sources newer than `binary` (empty: up to date): the engine's, the shared probe headers and the binary's
    own .cpp (test/<name>.cpp) -- not the other probe's .cpp, which it is not built from (an edit there left this
    binary up to date but marked it stale; Task A3 concern 5). A binary named after neither .cpp is held to both.
    A missing binary is reported as such."""
    if not os.path.exists(binary):
        return [f"{binary} does not exist"]
    t = os.path.getmtime(binary)
    own = [s for s in PROBE_SOURCES if not s.endswith(".cpp") or os.path.basename(s)[:-4] == os.path.basename(binary)]
    own = own if any(s.endswith(".cpp") for s in own) else list(PROBE_SOURCES)
    newer = [rel for rel in engine_files(root) + own if os.path.getmtime(os.path.join(root, rel)) > t]
    return [f"{os.path.basename(binary)} is older than {rel}" for rel in newer]


def current(stack_probe=None, region_probe=None, root=MODULE):
    """The identity in use: the tree's three source parts and the probes' binary hashes, with any
    staleness of the probes against the tree."""
    probes = {"stack-probe": stack_probe or DEFAULT_PROBES[0], "stack-region-probe": region_probe or DEFAULT_PROBES[1]}
    ident = source_parts(root)
    ident["probe_binaries"] = {n: (_sha_file(p) if os.path.exists(p) else None) for n, p in probes.items()}
    ident["stale"] = [s for p in probes.values() for s in stale(p, root)]
    return ident


def mismatches(frozen, used):
    """What differs between a recorded identity and the one in use, worded (empty: the same). A part
    the record does not hold is a mismatch: an unknown identity cannot be trusted."""
    if not frozen:
        return ["no identity is recorded"]
    out = []
    for part in PARTS:
        a, b = frozen.get(part), used.get(part)
        if a is None or b is None or a != b:
            out.append(f"{part}: recorded {_short(a)}, in use {_short(b)}")
    return out


def _short(v):
    if isinstance(v, dict):
        return "{" + ", ".join(f"{k} {_short(x)}" for k, x in sorted(v.items())) + "}"
    return "none" if v is None else str(v)[:12]


def git_dirty(path):
    """True when `path` differs from git's HEAD (or git cannot say): a calibration that is not committed
    as it stands is not a freeze."""
    repo = os.path.dirname(os.path.dirname(MODULE))
    r = subprocess.run(["git", "-C", repo, "status", "--porcelain", "--", os.path.relpath(path, repo)],
                       capture_output=True, text=True)
    return r.returncode != 0 or bool(r.stdout.strip())


def _main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--stack-probe", default=DEFAULT_PROBES[0])
    ap.add_argument("--region-probe", default=DEFAULT_PROBES[1])
    ap.add_argument("--at", help="the three source parts at a git commit instead (no probe binaries)")
    a = ap.parse_args(argv)
    print(json.dumps(source_parts_at(a.at) if a.at else current(a.stack_probe, a.region_probe), indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(_main())
