#!/usr/bin/env bash
# Build the frozen gaia-depth-grade sidecar (linux-x64) and package it as a
# PixInsight update package.
#
#   tools/build-sidecar.sh <out.tar.gz>
#
# The package deploys bin/gaia-depth-grade-sidecar (mode 0755), next to the
# PixInsight core executable -- where the PJSR scripts look for it -- and the
# licences of the third-party software it bundles, as
# doc/scripts/GaiaDepthGrade/gaia-depth-grade-sidecar-THIRD_PARTY_NOTICES.txt. PixInsight's updater keeps the tar member's mode bits (an installed
# package's files carry exactly the modes of its tar members), so the program is
# executable once installed; nothing on the user's machine ever chmods it.
#
# Portability: the binary is frozen inside the Rocky 9 build image (glibc 2.34,
# the PixInsight core's own floor), never on the dev box. A host build bundles
# the host's libstdc++/libz, and sidecar 1.0.7 -- built that way on Fedora --
# does not start on Ubuntu 22.04, Debian 12 or Rocky 9 (GLIBC_ABI_DT_RELR /
# GLIBC_ABI_GNU2_TLS not found). The smoke test below runs the result in those
# three bare images.
#
# Inputs are pinned: Python 3.14.3 (python-build-standalone 20260303, uv-managed),
# tools/sidecar-requirements.txt (hash-locked), and PBS's full archive of the same
# build (sha256-pinned), read only for the licence texts of the libraries linked
# into the interpreter. The source package is copied into the build, never edited.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GDG="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$GDG/.." && pwd)"
. "$ROOT/tools/build-env/image-tag.sh"
PY_VER=3.14.3
PBS_RELEASE=20260303
PBS_FULL="cpython-3.14.3+20260303-x86_64-unknown-linux-gnu-pgo+lto-full.tar.zst"
PBS_FULL_SHA256=5ea947f98514d84b467e05028bc62f281dac4aa9d4215863b4240a0fdde5f603
PBS_URL="https://github.com/astral-sh/python-build-standalone/releases/download/$PBS_RELEASE/${PBS_FULL//+/%2B}"
NAME=gaia-depth-grade-sidecar
DISTROS=(docker.io/library/ubuntu:22.04 docker.io/library/debian:12 docker.io/rockylinux/rockylinux:9)

die(){ echo "ERROR: $*" >&2; exit 1; }
[ $# -eq 1 ] || die "usage: $0 <out.tar.gz>"
OUT="$(realpath -m "$1")"
command -v podman >/dev/null || die "podman not found"
UV="$(command -v uv)" || die "uv not found (it is mounted into the build image)"
VER="$(sed -nE 's/^__version__ = "([0-9.]+)"$/\1/p' "$GDG/src/gaia_depth_grade/__init__.py")"
[ -n "$VER" ] || die "could not read gaia_depth_grade.__version__"

# The uv cache and the managed interpreter persist between builds (gitignored).
CACHE="$GDG/.cache/sidecar-build"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$CACHE/uv" "$CACHE/python" "$CACHE/pbs" "$WORK/pkg"
cp -R "$GDG/pyproject.toml" "$GDG/src" "$WORK/pkg/"
if [ ! -f "$CACHE/pbs/$PBS_FULL" ]; then
   echo "== fetching $PBS_FULL (licence texts of the bundled interpreter; once) =="
   curl -fsSL -o "$CACHE/pbs/$PBS_FULL.part" "$PBS_URL" && mv "$CACHE/pbs/$PBS_FULL.part" "$CACHE/pbs/$PBS_FULL"
fi
echo "$PBS_FULL_SHA256  $CACHE/pbs/$PBS_FULL" | sha256sum -c --quiet - || die "$PBS_FULL does not match its pinned sha256"
ensure_image

echo "== 1/4 freeze sidecar $VER in $IMAGE =="
podman run --rm --security-opt label=disable --userns=keep-id \
   -v "$GDG/tools:/tools:ro" -v "$WORK:/work" -v "$CACHE:/cache" -v "$UV:/usr/local/bin/uv:ro" \
   -e UV_CACHE_DIR=/cache/uv -e UV_PYTHON_INSTALL_DIR=/cache/python -e UV_PYTHON_BIN_DIR=/work/pybin \
   -e PYINSTALLER_CONFIG_DIR=/work/pyinstaller -e PY_VER="$PY_VER" -e PBS_RELEASE="$PBS_RELEASE" \
   -e PBS_FULL="$PBS_FULL" -e GDG_CPYTHON_LICENSES=/work/cpython-licenses \
   "$IMAGE" bash -euo pipefail -c '
   uv python install "$PY_VER" >/dev/null
   # The licence texts must be those of the interpreter actually bundled.
   build="$(cat /cache/python/cpython-$PY_VER-linux-x86_64-gnu/BUILD)"
   [ "$build" = "$PBS_RELEASE" ] || { echo "uv installed PBS build $build, expected $PBS_RELEASE" >&2; exit 1; }
   uv venv -q --python "$PY_VER" /work/venv
   /work/venv/bin/python -I /tools/cpython-licenses.py "/cache/pbs/$PBS_FULL" /work/cpython-licenses
   uv pip install -q --python /work/venv/bin/python --require-hashes -r /tools/sidecar-requirements.txt
   uv pip install -q --python /work/venv/bin/python --no-deps /work/pkg
   cd /work
   /work/venv/bin/pyinstaller --clean --noconfirm --log-level WARN \
      --distpath /work/dist --workpath /work/build /tools/gaia-depth-grade-sidecar.spec
   # The hermetic fixture needs the source package; the frozen binary does not.
   /work/venv/bin/python -I /tools/_smoke_fixture.py /work/fixture >/dev/null'
BIN="$WORK/dist/$NAME"
[ -x "$BIN" ] || die "frozen binary not produced at $BIN"

# Smoke output (astropy chatters on stderr) is shown only when a run fails.
smoke(){ # <label> <command...>
   local log="$WORK/smoke.log"
   "${@:2}" > "$log" 2>&1 || { cat "$log" >&2; die "smoke test failed: $1"; }
   tail -1 "$log"
}

echo "== 2/4 smoke test on this host =="
smoke host "$HERE/sidecar-smoke.sh" "$BIN" "$VER" "$WORK/fixture" "$WORK/smoke-host"

echo "== 3/4 smoke test on stock distributions (no network) =="
for d in "${DISTROS[@]}"; do
   echo "-- $d"
   smoke "$d" podman run --rm --network=none --security-opt label=disable \
      -v "$BIN:/s/$NAME:ro" -v "$HERE/sidecar-smoke.sh:/s/smoke.sh:ro" -v "$WORK/fixture:/s/fixture:ro" \
      "$d" bash /s/smoke.sh "/s/$NAME" "$VER" /s/fixture /tmp/smoke
done

echo "== 4/4 package =="
# bin/gaia-depth-grade-sidecar (0755) + the licences of what it bundles, in the
# script's documentation folder (Repository Reference 4.2: bin, doc/scripts).
NOTICES="$WORK/dist/THIRD_PARTY_NOTICES.txt"
[ -s "$NOTICES" ] || die "the spec wrote no THIRD_PARTY_NOTICES.txt"
STAGE="$WORK/stage"
mkdir -p "$STAGE/bin" "$STAGE/doc/scripts/GaiaDepthGrade"
install -m 0755 "$BIN" "$STAGE/bin/$NAME"
install -m 0644 "$NOTICES" "$STAGE/doc/scripts/GaiaDepthGrade/$NAME-THIRD_PARTY_NOTICES.txt"
mkdir -p "$(dirname "$OUT")"
tar -C "$STAGE" --sort=name --owner=0 --group=0 --numeric-owner --mode=go-w -czf "$OUT" bin doc
# Exactly those two files: the program executable by everyone, the notices readable.
listing="$(tar -tvzf "$OUT")"
files="$(printf '%s\n' "$listing" | grep -v '/$' | awk '{print $1" "$6}')"
[ "$files" = "-rwxr-xr-x bin/$NAME
-rw-r--r-- doc/scripts/GaiaDepthGrade/$NAME-THIRD_PARTY_NOTICES.txt" ] \
   || die "unexpected package contents: $listing"
echo
printf '%s\n' "$listing"
echo "  package : $OUT"
echo "  size    : $(stat -c%s "$OUT") bytes"
echo "  sha1    : $(sha1sum "$OUT" | cut -d' ' -f1)"
