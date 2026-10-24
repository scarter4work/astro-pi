#!/usr/bin/env bash
# Build the shipped PixInsight modules (NukeX, PICopilot, Planetary) inside the
# portable Rocky 9 image (see Containerfile for why), run the NukeX and
# Planetary unit tests there, and leave the .so files at:
#   modules/nukex/build-portable/.../NukeX-pxm.so
#   modules/pi-copilot/build-portable/.../PICopilot-pxm.so
#   modules/planetary/build-portable/.../Planetary-pxm.so
# PCL comes from the image (built there from the pinned commit), never from
# the host ~/PCL, whose archives carry the dev box's glibc.
# Signing stays on the host (it needs PixInsight); release.sh does that.
#
# Usage: tools/build-env/build-modules.sh [--no-tests]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$ROOT/tools/build-env/image-tag.sh"
RUN_TESTS=1
[ "${1:-}" = "--no-tests" ] && RUN_TESTS=0

die() { echo "FAIL: $*" >&2; exit 1; }
command -v podman >/dev/null || die "podman not found"
ensure_image

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
         -DPICOPILOT_BUILD_MODULE=ON -DPICOPILOT_SELFTEST_BUILD=OFF >/dev/null
   cmake --build /src/modules/pi-copilot/build-portable -j"$(nproc)" >/dev/null
   if [ "$RUN_TESTS" = 1 ]; then
      # The PI Copilot SELF-TEST build (test hooks compiled in): what test/run-selftest.sh and the GUI
      # tests run against (PICOPILOT_SO=.../build-portable-selftest/...). Never shipped: build-portable/
      # above has no self-test code, and release.sh checks that (test/check-no-selftest-hooks.sh).
      cmake -S /src/modules/pi-copilot -B /src/modules/pi-copilot/build-portable-selftest "${COMMON[@]}" \
            -DPICOPILOT_BUILD_MODULE=ON -DPICOPILOT_SELFTEST_BUILD=ON >/dev/null
      cmake --build /src/modules/pi-copilot/build-portable-selftest -j"$(nproc)" >/dev/null
   fi
   cmake -S /src/modules/planetary -B /src/modules/planetary/build-portable "${COMMON[@]}" \
         -DPLANETARY_BUILD_MODULE=ON >/dev/null
   cmake --build /src/modules/planetary/build-portable -j"$(nproc)" >/dev/null
   if [ "$RUN_TESTS" = 1 ]; then
      # test_gpu_context needs a GPU; the container has none. It runs on the
      # host below, against this same container-built binary.
      (cd /src/modules/nukex/build-portable && ctest --output-on-failure --no-tests=error -j"$(nproc)" -E "^test_gpu_context$")
      (cd /src/modules/planetary/build-portable && ctest --output-on-failure --no-tests=error -j"$(nproc)")
   fi'

if [ "$RUN_TESTS" = 1 ]; then
   # Host run of the portable build's GPU test: proves the statically linked
   # OpenCL ICD loader still finds the real driver ICD (/etc/OpenCL/vendors).
   # Run the binary directly: the CTest file holds the container's /src paths.
   "$ROOT/modules/nukex/build-portable/test/test_gpu_context" \
      || die "test_gpu_context failed on the host GPU"
fi

for so in "$ROOT/modules/nukex/build-portable" "$ROOT/modules/pi-copilot/build-portable" "$ROOT/modules/planetary/build-portable"; do
   f="$(find "$so" -name '*-pxm.so' -print -quit)"
   [ -n "$f" ] || die "no *-pxm.so under $so"
   echo "built: $f"
done
