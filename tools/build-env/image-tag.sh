# Sourced by build-modules.sh / verify-portable.sh. The image tag is a hash of
# everything that goes into the image, so bumping a pin (PCL commit, Ceres,
# OpenCL, the patch) builds a NEW image instead of silently reusing the old one.
BUILD_ENV_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="astro-pi-build:$(cat "$BUILD_ENV_DIR/Containerfile" "$BUILD_ENV_DIR"/*.patch | sha256sum | cut -c1-12)"
ensure_image() {
   if ! podman image exists "$IMAGE"; then
      echo "== building $IMAGE (inputs changed or first run; ~10 min) =="
      podman build -t "$IMAGE" "$BUILD_ENV_DIR" >/dev/null
   fi
}
