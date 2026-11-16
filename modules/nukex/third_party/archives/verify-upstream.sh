#!/usr/bin/env bash
# Re-verify the vendored archives against upstream (NEEDS the network; the build
# never runs this). For each archive: the committed bytes match SHA256SUMS and
# a fresh download from upstream, and the contents match the upstream git tag
# (cfitsio: the whole tag tree; nlohmann/json: every file is the tag's file).
# Usage: verify-upstream.sh
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP:?}"' EXIT
fail=0
ok()  { echo "  ok   $*"; }
bad() { echo "  FAIL $*"; fail=1; }

(cd "$HERE" && sha256sum -c --quiet SHA256SUMS) && ok "SHA256SUMS matches the committed archives" || bad "SHA256SUMS"

# <file> <upstream url>
same_as_upstream() {
   curl -fsSL -o "$TMP/dl" "$2"
   cmp -s "$HERE/$1" "$TMP/dl" && ok "$1 == $2" || bad "$1 differs from $2"
}
# <repo url> <tag> <dir>: shallow checkout of the tag's tree (no .git) in <dir>
tag_tree() {
   git -c advice.detachedHead=false clone -q --depth 1 --branch "$2" "$1" "$3"
   echo "       $2 = commit $(git -C "$3" rev-parse HEAD)"
   rm -rf "${3:?}/.git"
}

echo "cfitsio-4.6.3.tar.gz"
same_as_upstream cfitsio-4.6.3.tar.gz \
   https://github.com/HEASARC/cfitsio/archive/refs/tags/cfitsio-4.6.3.tar.gz
tag_tree https://github.com/HEASARC/cfitsio.git cfitsio-4.6.3 "$TMP/cfitsio-tag"
mkdir "$TMP/cfitsio" && tar -C "$TMP/cfitsio" --strip-components=1 -xzf "$HERE/cfitsio-4.6.3.tar.gz"
diff -r "$TMP/cfitsio" "$TMP/cfitsio-tag" >/dev/null && ok "contents == tag tree" || bad "contents differ from the tag tree"

echo "nlohmann_json-v3.11.3.tar.xz"
same_as_upstream nlohmann_json-v3.11.3.tar.xz \
   https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
tag_tree https://github.com/nlohmann/json.git v3.11.3 "$TMP/json-tag"
mkdir "$TMP/json" && tar -C "$TMP/json" --strip-components=1 -xJf "$HERE/nlohmann_json-v3.11.3.tar.xz"
n=0; d=0
while IFS= read -r -d '' f; do
   n=$((n + 1)); cmp -s "$TMP/json/$f" "$TMP/json-tag/$f" || { echo "       differs: $f"; d=$((d + 1)); }
done < <(cd "$TMP/json" && find . -type f -print0)
[ "$n" -gt 0 ] && [ "$d" = 0 ] && ok "all $n files == the tag's files" || bad "$d of $n files differ from the tag"

echo "sqlite-amalgamation-3460100.zip"
same_as_upstream sqlite-amalgamation-3460100.zip \
   https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
sha3="$(openssl dgst -sha3-256 -r "$HERE/sqlite-amalgamation-3460100.zip" | cut -d' ' -f1)"
[ "$sha3" = af6aae8d3eccc608857c63cf56efbadc70da48b5c719446b353ed88dded1e288 ] \
   && ok "SHA3-256 == NukeX's earlier sqlite.org pin" || bad "SHA3-256 $sha3"

[ "$fail" = 0 ] && echo "PASS" || { echo "FAIL"; exit 1; }
