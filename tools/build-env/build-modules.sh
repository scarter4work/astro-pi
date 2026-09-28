#!/usr/bin/env bash
# Build the shipped PixInsight modules (NukeX, PICopilot) inside the portable
# Rocky 9 image (see Containerfile for why), run NukeX's unit tests there, and
# leave the .so files at:
#   modules/nukex/build-portable/.../NukeX-pxm.so
#   modules/pi-copilot/build-portable/.../PICopilot-pxm.so
# PCL comes from the image (built there from the pinned commit), never from
# the host ~/PCL, whose archives carry the dev box's glibc.
# Signing stays on the host (it needs PixInsight); release.sh does that.
#
# Usage: tools/build-env/build-modules.sh [--no-tests]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMAGE="astro-pi-build:rocky9"
RUN_TESTS=1
[ "${1:-}" = "--no-tests" ] && RUN_TESTS=0

die() { echo "FAIL: $*" >&2; exit 1; }
command -v podman >/dev/null || die "podman not found"
if ! podman image exists "$IMAGE"; then
   echo "== building $IMAGE (one-time, ~10 min) =="
   podman build -t "$IMAGE" "$ROOT/tools/build-env" >/dev/null
fi

# label=disable: bind-mounting the repo without relabelling it (no :Z) on
# SELinux hosts. keep-id runs the build AS the invoking user (not container
# root): outputs are owned by the user, and permission-based tests are real
# (root bypasses directory permissions, so test_atomic_write's read-only-dir
# case can never fail as root).
podman run --rm --security-opt label=disable --userns=keep-id \
   -v "$ROOT:/src" -e RUN_TESTS="$RUN_TESTS" \
   "$IMAGE" bash -euo pipefail -c '
   COMMON=(-DCMAKE_BUILD_TYPE=Release -DPCLDIR=/opt/pcl -DCMAKE_PREFIX_PATH=/opt/deps -DCMAKE_SKIP_RPATH=ON)
   cmake -S /src/modules/nukex -B /src/modules/nukex/build-portable "${COMMON[@]}" \
         -DNUKEX_BUILD_MODULE=ON -DNUKEX_BUILD_TESTS=ON -DNUKEX_RELEASE_BUILD=ON >/dev/null
   cmake --build /src/modules/nukex/build-portable -j"$(nproc)" >/dev/null
   cmake -S /src/modules/pi-copilot -B /src/modules/pi-copilot/build-portable "${COMMON[@]}" \
         -DPICOPILOT_BUILD_MODULE=ON >/dev/null
   cmake --build /src/modules/pi-copilot/build-portable -j"$(nproc)" >/dev/null
   if [ "$RUN_TESTS" = 1 ]; then
      # test_gpu_context needs a GPU; the container has none. It runs on the
      # host below, against this same container-built binary.
      (cd /src/modules/nukex/build-portable && ctest --output-on-failure -j"$(nproc)" -E "^test_gpu_context$")
   fi'

if [ "$RUN_TESTS" = 1 ]; then
   # Host run of the portable build's GPU test: proves the statically linked
   # OpenCL ICD loader still finds the real driver ICD (/etc/OpenCL/vendors).
   # Run the binary directly: the CTest file holds the container's /src paths.
   "$ROOT/modules/nukex/build-portable/test/test_gpu_context" \
      || die "test_gpu_context failed on the host GPU"
fi

for so in "$ROOT/modules/nukex/build-portable" "$ROOT/modules/pi-copilot/build-portable"; do
   f="$(find "$so" -name '*-pxm.so' -print -quit)"
   [ -n "$f" ] || die "no *-pxm.so under $so"
   echo "built: $f"
done
