#!/usr/bin/env bash
# Prove that module .so files load on stock, OLDER Linux distributions -- not
# just that their symbol versions look right. Each .so is dlopen'ed with
# RTLD_NOW inside BARE distro images (nothing extra installed), so a missing
# shared library or a too-new GLIBC/GLIBCXX/GOMP version fails loudly.
#
# Also enforces the static floor: nothing may require more than the
# PixInsight core itself does (GLIBC_2.34, GLIBCXX_3.4.30), and no module may
# carry an RPATH/RUNPATH (it would point into the build machine).
#
# Usage: tools/build-env/verify-portable.sh <module.so>...
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$ROOT/tools/build-env/image-tag.sh"
DISTROS=(docker.io/library/ubuntu:22.04 docker.io/library/debian:12 docker.io/rockylinux/rockylinux:9)
MAX_GLIBC=2.34
MAX_GLIBCXX=3.4.30
[ $# -ge 1 ] || { echo "usage: $0 <module.so>..." >&2; exit 2; }

fail=0
vmax() { printf '%s\n' "$@" | sort -V | tail -1; }

# 1. static floor
for so in "$@"; do
   so="$(realpath -e "$so")"
   g="$(objdump -T "$so" | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -Vu | tail -1)"
   # A module may legitimately use no libstdc++ versioned symbol at all.
   x="$(objdump -T "$so" | { grep -oE 'GLIBCXX_[0-9.]+' || true; } | sed 's/GLIBCXX_//' | sort -Vu | tail -1)"
   needed="$(readelf -d "$so" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p' | tr '\n' ' ')"
   echo "$(basename "$so"): GLIBC_$g GLIBCXX_${x:-none}  NEEDED: $needed"
   [ "$(vmax "$g" "$MAX_GLIBC")" = "$MAX_GLIBC" ] || { echo "  FAIL: GLIBC_$g > $MAX_GLIBC"; fail=1; }
   [ -z "$x" ] || [ "$(vmax "$x" "$MAX_GLIBCXX")" = "$MAX_GLIBCXX" ] || { echo "  FAIL: GLIBCXX_$x > $MAX_GLIBCXX"; fail=1; }
   if readelf -d "$so" | grep -qE 'RPATH|RUNPATH'; then echo "  FAIL: carries an RPATH/RUNPATH"; fail=1; fi
   # PixInsight loads a module through exactly these three symbols. dlopen()
   # succeeds without them, so check them explicitly (two live in the PCL
   # static library, so a symbol-hiding linker flag can silently drop them).
   for ep in IdentifyPixInsightModule InitializePixInsightModule InstallPixInsightModule; do
      nm -D --defined-only "$so" | grep -qE " T $ep\$" || { echo "  FAIL: PixInsight entry point $ep is not exported"; fail=1; }
   done
done

# 2. dlopen probe, built once in the Rocky 9 image (glibc 2.34) so it runs on
#    every distro under test.
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
cat > "$work/probe.c" <<'EOF'
#include <dlfcn.h>
#include <stdio.h>
int main(int argc, char **argv) {
   int bad = 0;
   for (int i = 1; i < argc; ++i) {
      void *h = dlopen(argv[i], RTLD_NOW | RTLD_LOCAL);
      if (!h) { printf("  FAIL %s\n", dlerror()); bad = 1; }
      else    { printf("  ok   %s\n", argv[i]); }
   }
   return bad;
}
EOF
ensure_image
podman run --rm --security-opt label=disable -v "$work:/w" "$IMAGE" gcc -O2 -o /w/probe /w/probe.c
args=(); mounts=()
for so in "$@"; do
   so="$(realpath -e "$so")"; b="$(basename "$so")"
   mounts+=(-v "$so:/m/$b:ro"); args+=("/m/$b")
done
for d in "${DISTROS[@]}"; do
   echo "== $d"
   podman run --rm --security-opt label=disable -v "$work/probe:/probe:ro" "${mounts[@]}" "$d" /probe "${args[@]}" || fail=1
done

[ "$fail" = 0 ] && echo "PASS: all modules load on ${DISTROS[*]}" || { echo "FAIL: see above"; exit 1; }
