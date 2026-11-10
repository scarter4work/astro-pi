# Sourced by build-modules.sh / verify-portable.sh. The image tag is a hash of
# everything that goes into the image, so bumping a pin (PCL tarball, Ceres,
# OpenCL, the patch) builds a NEW image instead of silently reusing the old one.
# The PCL source tarball itself is not committed: pcl-2.13/SHA256SUMS pins it
# (and is hashed here); ensure_image makes it from the installed PixInsight with
# pcl-2.13/make-tarball.sh, which fails unless the bytes match the pin.
BUILD_ENV_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="astro-pi-build:$(cat "$BUILD_ENV_DIR/Containerfile" "$BUILD_ENV_DIR"/*.patch "$BUILD_ENV_DIR/pcl-2.13/SHA256SUMS" | sha256sum | cut -c1-12)"
ensure_image() {
   if ! podman image exists "$IMAGE"; then
      "$BUILD_ENV_DIR/pcl-2.13/make-tarball.sh" || return 1
      echo "== building $IMAGE (inputs changed or first run; ~10 min) =="
      podman build -t "$IMAGE" "$BUILD_ENV_DIR" >/dev/null
   fi
}
