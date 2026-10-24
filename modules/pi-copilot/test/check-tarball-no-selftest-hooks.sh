#!/usr/bin/env bash
# The PICopilot module INSIDE a package tarball must carry no self-test code: release.sh runs this on the
# tarball it ships -- a freshly built one, or an already-published one it would reuse byte for byte (the
# published 0.2.2.0 carries the old PICOPILOT_SELFTEST_* URL hooks, so re-shipping it must stop).
#
#   check-tarball-no-selftest-hooks.sh <...-linux-x64-PICopilot-<ver>.tar.gz>   exit 0 clean, 1 otherwise
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
TGZ="${1:?usage: check-tarball-no-selftest-hooks.sh <PICopilot tarball>}"
[ -f "$TGZ" ] && [ -r "$TGZ" ] && [ -s "$TGZ" ] || { echo "FAIL: missing, empty or unreadable tarball: $TGZ"; exit 1; }
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
tar -xzf "$TGZ" -C "$TMP" bin/PICopilot-pxm.so 2>"$TMP/tar.err" \
   || { echo "FAIL: $TGZ has no readable bin/PICopilot-pxm.so ($(head -c 300 "$TMP/tar.err"))"; exit 1; }
echo "$(basename "$TGZ"): bin/PICopilot-pxm.so"
"$HERE/check-no-selftest-hooks.sh" "$TMP/bin/PICopilot-pxm.so"
