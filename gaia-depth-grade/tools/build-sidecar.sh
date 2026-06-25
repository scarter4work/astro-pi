#!/usr/bin/env bash
# Build the frozen gaia-depth-grade sidecar (linux-x64), smoke-test it, package
# it as a tarball, and print the sha256 the PJSR bootstrap must pin.
#
#   tools/build-sidecar.sh
#
# Output: dist/gaia-depth-grade-sidecar-<ver>-linux-x64.tar.gz  (+ printed sha256)
# The tarball is uploaded to GitHub Releases (manual in Phase 1); it is NOT in the
# git repo (a PyInstaller astro bundle is hundreds of MB, over GitHub's 100 MB limit).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
PY="$ROOT/.venv/bin/python"
PYINSTALLER="$ROOT/.venv/bin/pyinstaller"
DIST="$ROOT/dist"
BUILD="$ROOT/build"

die(){ echo "ERROR: $*" >&2; exit 1; }
[ -x "$PY" ]          || die "venv python not found at $PY (run: uv sync)"
[ -x "$PYINSTALLER" ] || die "pyinstaller not in venv ($PYINSTALLER); run: uv pip install 'pyinstaller>=6'"

VER="$("$PY" -c 'import gaia_depth_grade; print(gaia_depth_grade.__version__)')"
[ -n "$VER" ] || die "could not read gaia_depth_grade.__version__"
BIN="$DIST/gaia-depth-grade-sidecar"
TGZ="gaia-depth-grade-sidecar-${VER}-linux-x64.tar.gz"

echo "== 1/4 freeze sidecar (PyInstaller, v$VER) =="
"$PYINSTALLER" --clean --noconfirm \
  --distpath "$DIST" --workpath "$BUILD" \
  "$HERE/gaia-depth-grade-sidecar.spec"
[ -x "$BIN" ] || die "frozen binary not produced at $BIN"

echo "== 2/4 smoke test =="
# (a) --version proves the whole import chain (numpy/scipy/astropy/photutils/
#     astroquery) is bundled — a missing data file or hidden import crashes here.
got="$("$BIN" --version)"
[ "$got" = "$VER" ] || die "frozen --version printed '$got', expected '$VER'"
echo "   --version OK ($got)"
# (b) a hermetic render round-trip exercises the real image pipeline, no network.
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
"$PY" "$HERE/_smoke_fixture.py" "$TMP"
"$BIN" render "$TMP/cache" "$TMP/stars.fits" "$TMP/out.fits" \
  --gains 0.5,0.4,0.3,0.3 --p-low 5 --p-high 95 --base-sigma 2
[ -s "$TMP/out.fits" ] || die "frozen render produced no output FITS"
echo "   render round-trip OK ($(stat -c%s "$TMP/out.fits") bytes)"

echo "== 3/4 package tarball =="
tar -C "$DIST" -czf "$DIST/$TGZ" "gaia-depth-grade-sidecar"
SHA="$(sha256sum "$DIST/$TGZ" | cut -d' ' -f1)"

echo "== 4/4 done =="
echo
echo "  artifact : dist/$TGZ"
echo "  size     : $(stat -c%s "$DIST/$TGZ") bytes"
echo "  sha256   : $SHA"
echo
echo "Next: upload as a GitHub Release asset, then pin into pi/gaia_depth_grade_lib.jsh:"
echo "  SIDECAR_VERSION = \"$VER\""
echo "  SIDECAR_URL     = \"https://github.com/scarter4work/astro-pi/releases/download/gaia-depth-grade-v$VER/$TGZ\""
echo "  SIDECAR_SHA256  = \"$SHA\""
