#!/usr/bin/env bash
#
# NukeX v4 — release packaging driver.
#
# Produces a PI-compatible release tarball from the current build and
# updates updates.xri to reference it.  Does NOT bump the version string
# in NukeXModule.cpp, edit CHANGELOG.md, commit, tag, or push — those
# steps require human decisions (release notes text, tag name) and stay
# manual.  See CLAUDE.md "PixInsight Release Workflow" for the full
# end-to-end checklist.
#
# Usage:
#   tools/release.sh                 # sign .so, tarball, update xri
#   tools/release.sh sign            # just sign the module, nothing else
#
# Requires environment (with sensible defaults):
#   NUKEX_PI_DIR           /opt/PixInsight
#   NUKEX_SIGN_KEYS        /home/scarter4work/projects/keys/scarter4work_keys.xssk
#   NUKEX_SIGN_PASS_FILE   /tmp/.pi_codesign_pass  (or NUKEX_SIGN_PASS env)
#   NUKEX_BUILD_DIR        <repo>/build
#
# Will refuse to proceed if the built .so is older than any src/ source
# file (i.e. the build is stale).

set -euo pipefail   # pipefail so failures of sha1sum / find are surfaced

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PI_DIR="${NUKEX_PI_DIR:-/opt/PixInsight}"
KEYS="${NUKEX_SIGN_KEYS:-/home/scarter4work/projects/keys/scarter4work_keys.xssk}"
BUILD_DIR="${NUKEX_BUILD_DIR:-${REPO}/build}"

MODULE_SO="${BUILD_DIR}/src/module/NukeX-pxm.so"
MODULE_XSGN="${BUILD_DIR}/src/module/NukeX-pxm.xsgn"
REPO_DIR="${REPO}/repository"
XRI="${REPO_DIR}/updates.xri"

if [ -n "${NUKEX_SIGN_PASS:-}" ]; then
    PASS="${NUKEX_SIGN_PASS}"
elif [ -r "${NUKEX_SIGN_PASS_FILE:-/tmp/.pi_codesign_pass}" ]; then
    PASS="$(cat "${NUKEX_SIGN_PASS_FILE:-/tmp/.pi_codesign_pass}")"
else
    echo "ERROR: neither NUKEX_SIGN_PASS env nor NUKEX_SIGN_PASS_FILE is set."
    echo "Set one, or put the password in /tmp/.pi_codesign_pass."
    exit 1
fi

if [ ! -f "${MODULE_SO}" ]; then
    echo "ERROR: module .so not found at ${MODULE_SO}"
    echo "Build first:  cd ${BUILD_DIR} && make NukeX-pxm -j"
    exit 1
fi
if [ ! -f "${KEYS}" ]; then
    echo "ERROR: keys file not found at ${KEYS}"
    exit 1
fi

# Staleness check: if any src/ file is newer than MODULE_SO, the build is stale.
STALE="$(find "${REPO}/src" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) \
           -newer "${MODULE_SO}" 2>/dev/null | head -1 || true)"
if [ -n "${STALE}" ]; then
    echo "ERROR: build looks stale — ${STALE} is newer than ${MODULE_SO}"
    echo "Rebuild:  cd ${BUILD_DIR} && make NukeX-pxm -j"
    exit 1
fi

sign_module() {
    echo "=== Signing module ==="
    "${PI_DIR}/bin/PixInsight.sh" \
        "--sign-module-file=${MODULE_SO}" \
        "--xssk-file=${KEYS}" \
        "--xssk-password=${PASS}" \
        >/dev/null 2>&1
    if [ ! -f "${MODULE_XSGN}" ]; then
        echo "ERROR: signing failed — ${MODULE_XSGN} not produced"
        exit 1
    fi
    echo "signed: ${MODULE_SO}"
    echo "        ${MODULE_XSGN}"
}

package_release() {
    sign_module

    echo "=== Staging to repository/bin/ ==="
    mkdir -p "${REPO_DIR}/bin"
    cp "${MODULE_SO}"   "${REPO_DIR}/bin/"
    cp "${MODULE_XSGN}" "${REPO_DIR}/bin/"

    DATE="$(date +%Y%m%d)"
    TAR="${REPO_DIR}/${DATE}-linux-x64-NukeX.tar.gz"
    echo "=== Creating tarball ${TAR} ==="
    tar -C "${REPO_DIR}" -czf "${TAR}" bin/
    NEW_SHA1="$(sha1sum "${TAR}" | awk '{print $1}')"
    TAR_NAME="$(basename "${TAR}")"
    echo "sha1: ${NEW_SHA1}"

    if [ ! -f "${XRI}" ]; then
        echo "ERROR: ${XRI} missing — cannot update"
        exit 1
    fi

    echo "=== Updating updates.xri sha1 + fileName + releaseDate ==="
    python3 - "$XRI" "$TAR_NAME" "$NEW_SHA1" "$DATE" <<'PY'
import re, sys, pathlib
xri_path, tar_name, sha1, date = sys.argv[1:5]
p = pathlib.Path(xri_path)
text = p.read_text()
# Strip any existing trailing <Signature> block — PI re-appends on sign.
text = re.sub(r'<Signature [^>]*>.*?</Signature>\s*$', '', text, flags=re.DOTALL).rstrip() + '\n'
# Patch the first package element's fileName/sha1/releaseDate attrs.
def sub(pattern, repl, s):
    new, n = re.subn(pattern, repl, s, count=1)
    if n != 1:
        sys.stderr.write(f"WARN: did not find pattern for {pattern!r}; skipped\n")
    return new
text = sub(r'fileName="[^"]+"',    f'fileName="{tar_name}"', text)
text = sub(r'sha1="[0-9a-fA-F]+"', f'sha1="{sha1}"',         text)
text = sub(r'releaseDate="\d+"',    f'releaseDate="{date}"',  text)
p.write_text(text)
print(f"  fileName    = {tar_name}")
print(f"  sha1        = {sha1}")
print(f"  releaseDate = {date}")
PY

    echo "=== Re-signing updates.xri ==="
    "${PI_DIR}/bin/PixInsight.sh" \
        "--sign-xml-file=${XRI}" \
        "--xssk-file=${KEYS}" \
        "--xssk-password=${PASS}" \
        >/dev/null 2>&1
    if ! tail -1 "${XRI}" | grep -q '<Signature '; then
        echo "ERROR: XRI re-signing failed; no <Signature> line at end of ${XRI}"
        exit 1
    fi
    echo "signed: ${XRI}"

    echo ""
    echo "Release artefacts ready:"
    echo "  ${TAR}"
    echo "  ${XRI}"
    echo ""
    echo "Next manual steps:"
    echo "  1. Verify CHANGELOG.md describes this release."
    echo "  2. git add .gitignore CHANGELOG.md src/module/NukeXModule.cpp \\"
    echo "            repository/updates.xri repository/$(basename "${TAR}")"
    echo "  3. git commit -m 'release: vX.Y.Z.W — ...'"
    echo "  4. git tag -a vX.Y.Z.W -m 'release notes'"
    echo "  5. git push origin main && git push origin vX.Y.Z.W"
}

case "${1:-package}" in
    sign)    sign_module ;;
    package) package_release ;;
    *)       echo "usage: $0 [sign|package]"; exit 2 ;;
esac
