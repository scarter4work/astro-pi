#!/usr/bin/env bash
# Pack the PCL source tree that ships with the installed PixInsight into the
# tarball the build image compiles libPCL from (see ../Containerfile).
#
# WHY: upstream (gitlab pixinsight/PCL) has not published PCL 2.13; the only
# PCL 2.13 source is the tree bundled with PixInsight 1.9.6
# (/opt/PixInsight/include/pcl + /opt/PixInsight/src/{pcl,3rdparty}), and 2.13
# is the first PCL with BlindAstrometricSolver. The tarball is NOT committed
# (it is PixInsight's own distribution); SHA256SUMS is, so the image is built
# from exactly the bytes it pins, and a different PixInsight build fails loudly.
#
# The archive is reproducible: sorted names, fixed mtime/owner/mode, gzip -n.
#
#   make-tarball.sh               build $TARBALL and check it against SHA256SUMS
#   make-tarball.sh --write-sums  build it and (re)write SHA256SUMS (a deliberate re-pin)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PI_DIR="${ASTROPI_PI_DIR:-/opt/PixInsight}"
PCL_VERSION=2.13.0
PCL_BUILD=1072
TARBALL="$HERE/PCL-$PCL_VERSION-$PCL_BUILD.tar.gz"
die(){ echo "make-tarball: $*" >&2; exit 1; }

for d in include/pcl src/pcl src/3rdparty; do [ -d "$PI_DIR/$d" ] || die "$PI_DIR/$d missing"; done
# The bundle must be the release the sums pin, before anything is packed.
v="$(sed -n '/int Version::\(Major\|Minor\|Release\|Build\)()/,/return/p' "$PI_DIR/src/pcl/Version.cpp" \
     | sed -nE 's/^ *return ([0-9]+);/\1/p' | paste -sd. -)"
[ "$v" = "$PCL_VERSION.$PCL_BUILD" ] || die "$PI_DIR/src/pcl/Version.cpp is PCL $v, expected $PCL_VERSION.$PCL_BUILD"
[ -f "$PI_DIR/include/pcl/BlindAstrometricSolver.h" ] || die "$PI_DIR/include/pcl has no BlindAstrometricSolver.h"
# No build products may ride along (the bundle ships empty x64/Release dirs).
if find "$PI_DIR/src/pcl" "$PI_DIR/src/3rdparty" -type f \( -name '*.o' -o -name '*.a' -o -name '*.d' \) | grep -q .; then
   die "$PI_DIR/src carries build products (*.o/*.a/*.d)"
fi

tmp="$(mktemp "$HERE/.PCL-tarball.XXXXXX")"
trap 'rm -f "$tmp"' EXIT
tar -C "$PI_DIR" --sort=name --format=gnu --mtime='2026-10-28 20:47:34Z' \
    --owner=0 --group=0 --numeric-owner --mode='u+rwX,go+rX,go-w' \
    -cf - include/pcl src/pcl src/3rdparty | gzip -n -9 > "$tmp"
chmod 0644 "$tmp"
mv -f "$tmp" "$TARBALL"
trap - EXIT

if [ "${1:-}" = "--write-sums" ]; then
   (cd "$HERE" && sha256sum "$(basename "$TARBALL")" > SHA256SUMS)
   echo "wrote $HERE/SHA256SUMS: $(cat "$HERE/SHA256SUMS")"
else
   [ -f "$HERE/SHA256SUMS" ] || die "no $HERE/SHA256SUMS (pin with --write-sums)"
   (cd "$HERE" && sha256sum -c --status SHA256SUMS) \
      || die "$(basename "$TARBALL") does not match SHA256SUMS: the installed PixInsight's PCL differs from the pin (re-pin deliberately with --write-sums)"
   echo "ok: $(basename "$TARBALL") matches SHA256SUMS"
fi
