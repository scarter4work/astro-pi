#!/usr/bin/env bash
# Build the PixInsight modules (NukeX, PICopilot, Planetary, GaiaDepthGrade, AutoContrast)
# inside the portable Rocky 9 image (see Containerfile for why), run the NukeX,
# Planetary, GaiaDepthGrade and AutoContrast unit tests there, and leave the .so files at:
#   modules/nukex/build-portable/.../NukeX-pxm.so
#   modules/pi-copilot/build-portable/.../PICopilot-pxm.so
#   modules/planetary/build-portable/.../Planetary-pxm.so
#   modules/gaia-depth-grade/build-portable/.../GaiaDepthGrade-pxm.so
#   modules/autocontrast/build-portable/.../AutoContrast-pxm.so
#   modules/autocontrast/build-portable-probes/.../AutoContrast-pxm.so   (test-only: carries AutoContrastProbe)
# PCL comes from the image (PCL 2.13.0, built there from the pinned tarball of
# PixInsight 1.9.6's bundled source), never from the host ~/PCL, whose
# archives carry the dev box's glibc.
# Signing stays on the host (it needs PixInsight); release.sh does that.
#
# Usage: tools/build-env/build-modules.sh [--no-tests] [--only <module>]
#   --only builds (and tests) just one module: nukex | pi-copilot | planetary | gaia-depth-grade | autocontrast.
#   BUILD_JOBS=<n> sets the parallel build/test job count (default: nproc).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$ROOT/tools/build-env/image-tag.sh"
RUN_TESTS=1
ONLY=""
while [ $# -gt 0 ]; do
   case "$1" in
      --no-tests) RUN_TESTS=0 ;;
      --only) [ $# -ge 2 ] || { echo "FAIL: --only needs a module name" >&2; exit 2; }; ONLY="$2"; shift ;;
      --only=*) ONLY="${1#--only=}" ;;
      *) echo "usage: $0 [--no-tests] [--only <module>]" >&2; exit 2 ;;
   esac
   shift
done
case "$ONLY" in
   ""|nukex|pi-copilot|planetary|gaia-depth-grade|autocontrast) ;;
   *) echo "FAIL: --only: unknown module '$ONLY' (nukex|pi-copilot|planetary|gaia-depth-grade|autocontrast)" >&2; exit 2 ;;
esac
JOBS="${BUILD_JOBS:-$(nproc)}"
case "$JOBS" in ''|*[!0-9]*|0) echo "FAIL: BUILD_JOBS must be a positive integer, got '$JOBS'" >&2; exit 2 ;; esac
want() { [ -z "$ONLY" ] || [ "$ONLY" = "$1" ]; }

die() { echo "FAIL: $*" >&2; exit 1; }
command -v podman >/dev/null || die "podman not found"
ensure_image

# label=disable: bind-mounting the repo without relabelling it (no :Z) on
# SELinux hosts. keep-id runs the build AS the invoking user (not container
# root): outputs are owned by the user, and permission-based tests are real
# (root bypasses directory permissions, so test_atomic_write's read-only-dir
# case can never fail as root).
podman run --rm --security-opt label=disable --userns=keep-id \
   -v "$ROOT:/src" -e RUN_TESTS="$RUN_TESTS" -e ONLY="$ONLY" -e JOBS="$JOBS" \
   "$IMAGE" bash -euo pipefail -c '
   want() { [ -z "$ONLY" ] || [ "$ONLY" = "$1" ]; }
   # test_pcl213_solver_links: the image libPCL is 2.13 with the blind solver
   # GaiaDepthGrade links (absent from every older PCL). grep -c, not -q: under
   # pipefail a grep that exits early kills nm with SIGPIPE and fails the check.
   nm -C /opt/pcl/lib/x64/libPCL-pxi.a 2>/dev/null | grep -c "T pcl::BlindAstrometricSolver::Solve(" >/dev/null \
      || { echo "FAIL: /opt/pcl/lib/x64/libPCL-pxi.a has no pcl::BlindAstrometricSolver::Solve (not PCL 2.13)" >&2; exit 1; }
   COMMON=(-DCMAKE_BUILD_TYPE=Release -DPCLDIR=/opt/pcl -DCMAKE_PREFIX_PATH=/opt/deps -DCMAKE_SKIP_RPATH=ON)
   if want nukex; then
      cmake -S /src/modules/nukex -B /src/modules/nukex/build-portable "${COMMON[@]}" \
            -DNUKEX_BUILD_MODULE=ON -DNUKEX_BUILD_TESTS=ON -DNUKEX_RELEASE_BUILD=ON >/dev/null
      cmake --build /src/modules/nukex/build-portable -j"$JOBS" >/dev/null
   fi
   if want pi-copilot; then
      cmake -S /src/modules/pi-copilot -B /src/modules/pi-copilot/build-portable "${COMMON[@]}" \
            -DPICOPILOT_BUILD_MODULE=ON -DPICOPILOT_SELFTEST_BUILD=OFF >/dev/null
      cmake --build /src/modules/pi-copilot/build-portable -j"$JOBS" >/dev/null
      if [ "$RUN_TESTS" = 1 ]; then
         # The PI Copilot SELF-TEST build (test hooks compiled in): what test/run-selftest.sh and the GUI
         # tests run against (PICOPILOT_SO=.../build-portable-selftest/...). Never shipped: build-portable/
         # above has no self-test code, and release.sh checks that (test/check-no-selftest-hooks.sh).
         cmake -S /src/modules/pi-copilot -B /src/modules/pi-copilot/build-portable-selftest "${COMMON[@]}" \
               -DPICOPILOT_BUILD_MODULE=ON -DPICOPILOT_SELFTEST_BUILD=ON >/dev/null
         cmake --build /src/modules/pi-copilot/build-portable-selftest -j"$JOBS" >/dev/null
      fi
   fi
   if want planetary; then
      cmake -S /src/modules/planetary -B /src/modules/planetary/build-portable "${COMMON[@]}" \
            -DPLANETARY_BUILD_MODULE=ON >/dev/null
      cmake --build /src/modules/planetary/build-portable -j"$JOBS" >/dev/null
   fi
   if want gaia-depth-grade; then
      cmake -S /src/modules/gaia-depth-grade -B /src/modules/gaia-depth-grade/build-portable "${COMMON[@]}" \
            -DGDG_BUILD_MODULE=ON >/dev/null
      cmake --build /src/modules/gaia-depth-grade/build-portable -j"$JOBS" >/dev/null
      # The link probe pulled the solver into the module (PclProbe.cpp, -z defs).
      nm -C "$(find /src/modules/gaia-depth-grade/build-portable -name GaiaDepthGrade-pxm.so -print -quit)" \
         | grep -c " pcl::BlindAstrometricSolver::Solve(" >/dev/null \
         || { echo "FAIL: GaiaDepthGrade-pxm.so does not contain pcl::BlindAstrometricSolver::Solve" >&2; exit 1; }
   fi
   if want autocontrast; then
      # The shipped module, then the probe module (AutoContrastProbe compiled in; test-only, run by
      # modules/autocontrast/test/run-probe.sh). The shipped .so must not carry the probe.
      cmake -S /src/modules/autocontrast -B /src/modules/autocontrast/build-portable "${COMMON[@]}" \
            -DAC_BUILD_MODULE=ON -DAC_BUILD_TESTS=ON -DAC_BUILD_PROBES=OFF >/dev/null
      cmake --build /src/modules/autocontrast/build-portable -j"$JOBS" >/dev/null
      cmake -S /src/modules/autocontrast -B /src/modules/autocontrast/build-portable-probes "${COMMON[@]}" \
            -DAC_BUILD_MODULE=ON -DAC_BUILD_TESTS=OFF -DAC_BUILD_PROBES=ON >/dev/null
      cmake --build /src/modules/autocontrast/build-portable-probes -j"$JOBS" >/dev/null
      shipped="$(find /src/modules/autocontrast/build-portable -name AutoContrast-pxm.so -print -quit)"
      probed="$(find /src/modules/autocontrast/build-portable-probes -name AutoContrast-pxm.so -print -quit)"
      [ -n "$shipped" ] && [ -n "$probed" ] || { echo "FAIL: AutoContrast-pxm.so missing from build-portable or build-portable-probes" >&2; exit 1; }
      if strings -a "$shipped" | grep -c AutoContrastProbe >/dev/null; then
         echo "FAIL: the shipped AutoContrast-pxm.so contains AutoContrastProbe ($shipped)" >&2; exit 1
      fi
      strings -a "$probed" | grep -c AutoContrastProbe >/dev/null \
         || { echo "FAIL: the probe AutoContrast-pxm.so has no AutoContrastProbe ($probed)" >&2; exit 1; }
   fi
   if [ "$RUN_TESTS" = 1 ]; then
      # test_gpu_context needs a GPU; the container has none. It runs on the
      # host below, against this same container-built binary.
      want nukex && (cd /src/modules/nukex/build-portable && ctest --output-on-failure --no-tests=error -j"$JOBS" -E "^test_gpu_context$")
      want planetary && (cd /src/modules/planetary/build-portable && ctest --output-on-failure --no-tests=error -j"$JOBS")
      want gaia-depth-grade && (cd /src/modules/gaia-depth-grade/build-portable && ctest --output-on-failure --no-tests=error -j"$JOBS")
      want autocontrast && (cd /src/modules/autocontrast/build-portable && ctest --output-on-failure --no-tests=error -j"$JOBS")
      true
   fi'

if [ "$RUN_TESTS" = 1 ] && want nukex; then
   # Host run of the portable build's GPU test: proves the statically linked
   # OpenCL ICD loader still finds the real driver ICD (/etc/OpenCL/vendors).
   # Run the binary directly: the CTest file holds the container's /src paths.
   "$ROOT/modules/nukex/build-portable/test/test_gpu_context" \
      || die "test_gpu_context failed on the host GPU"
fi

built=()
want nukex && built+=("$ROOT/modules/nukex/build-portable")
want pi-copilot && built+=("$ROOT/modules/pi-copilot/build-portable")
want planetary && built+=("$ROOT/modules/planetary/build-portable")
want gaia-depth-grade && built+=("$ROOT/modules/gaia-depth-grade/build-portable")
want autocontrast && built+=("$ROOT/modules/autocontrast/build-portable" "$ROOT/modules/autocontrast/build-portable-probes")
for so in "${built[@]}"; do
   f="$(find "$so" -name '*-pxm.so' -print -quit)"
   [ -n "$f" ] || die "no *-pxm.so under $so"
   echo "built: $f"
done
