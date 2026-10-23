#!/usr/bin/env bash
# Test of release.sh's manifest edits (CPD re-audit ruling T10b), on a scratch copy of
# repository/updates.xri: no build, no signing, no PixInsight, repository/ untouched.
#
#   tools/test-release-manifest.sh [path/to/release.sh]   -> PASS (exit 0) / FAIL (exit 1)
#
# It takes release.sh's own functions (write_pkg, manifest_rename, manifest_names_check) out of the
# script, so it tests exactly what a release runs:
#  - a bumped EZ zip (EZStretch) and a bumped Gaia script zip are renamed in the manifest, exactly once each;
#  - write_pkg writes the sha1 and releaseDate of both new names;
#  - write_pkg of a fileName the manifest does not hold fails (it used to change nothing and succeed);
#  - manifest_rename fails when no entry matches;
#  - manifest_names_check passes for exactly the manifest's names and fails for a missing or extra one.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT="${1:-$ROOT/release.sh}"
[ -f "$SCRIPT" ] || { echo "FAIL: no $SCRIPT"; exit 1; }
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
MF="$WORK/updates.xri"
sed '/<Signature developerId=/d' "$ROOT/repository/updates.xri" > "$MF"

die(){ echo "ERROR: $*" >&2; exit 1; }
DATE=20991231
for f in write_pkg manifest_rename manifest_names_check; do
  body="$(sed -n "/^$f(){/,/^}/p" "$SCRIPT")"
  if [ -n "$body" ]; then eval "$body"; else echo "  (release.sh has no $f)"; fi
done

fails=0
ok(){ echo "  ok    $*"; }
bad(){ echo "  FAIL  $*"; fails=$((fails + 1)); }
have(){ declare -F "$1" >/dev/null; }

OLD_EZ="$(sed -nE 's/.*fileName="(EZStretch_v[0-9.]+\.zip)".*/\1/p' "$MF")"
OLD_GAIA="$(sed -nE 's/.*fileName="(gaia-depth-grade_v[0-9.]+\.zip)".*/\1/p' "$MF")"
[ -n "$OLD_EZ" ] && [ -n "$OLD_GAIA" ] || { echo "FAIL: the manifest has no EZStretch or gaia-depth-grade zip entry"; exit 1; }
NEW_EZ=EZStretch_v9.9.9.zip
NEW_GAIA=gaia-depth-grade_v9.9.9.zip
SHA_EZ=1111111111111111111111111111111111111111
SHA_GAIA=2222222222222222222222222222222222222222

# 1-2. The bump renames each entry exactly once.
if have manifest_rename && ( manifest_rename "$MF" 'EZStretch_v[0-9.]+\.zip' "$NEW_EZ" ) >/dev/null 2>&1 \
   && grep -qF "fileName=\"$NEW_EZ\"" "$MF" && ! grep -qF "fileName=\"$OLD_EZ\"" "$MF"; then
  ok "EZStretch renamed $OLD_EZ -> $NEW_EZ"
else bad "EZStretch was not renamed $OLD_EZ -> $NEW_EZ"; fi
if have manifest_rename && ( manifest_rename "$MF" 'gaia-depth-grade_v[0-9.]+\.zip' "$NEW_GAIA" ) >/dev/null 2>&1 \
   && grep -qF "fileName=\"$NEW_GAIA\"" "$MF" && ! grep -qF "fileName=\"$OLD_GAIA\"" "$MF"; then
  ok "Gaia script renamed $OLD_GAIA -> $NEW_GAIA"
else bad "Gaia script was not renamed $OLD_GAIA -> $NEW_GAIA"; fi

# 3. write_pkg writes both new names' sha1 and releaseDate.
for pair in "$NEW_EZ $SHA_EZ" "$NEW_GAIA $SHA_GAIA"; do
  set -- $pair
  if have write_pkg && ( write_pkg "$MF" "$1" "$2" ) >/dev/null 2>&1 \
     && grep -qE "fileName=\"$1\" sha1=\"$2\"[^>]*releaseDate=\"$DATE\"" "$MF"; then
    ok "write_pkg wrote $1: sha1 $2, releaseDate $DATE"
  else bad "write_pkg did not write $1's sha1 and releaseDate"; fi
done

# 4. write_pkg of an absent fileName fails.
before="$(sha1sum < "$MF")"
if have write_pkg && ! ( write_pkg "$MF" "NoSuchPackage_v1.0.0.zip" "$SHA_EZ" ) >/dev/null 2>&1; then
  ok "write_pkg of an absent fileName fails"
else bad "write_pkg of an absent fileName succeeded (a silent no-op)"; fi
[ "$(sha1sum < "$MF")" = "$before" ] && ok "  ... and leaves the manifest unchanged" || bad "write_pkg of an absent fileName changed the manifest"

# 5. manifest_rename with no matching entry fails.
if have manifest_rename && ! ( manifest_rename "$MF" 'NoSuchPackage_v[0-9.]+\.zip' "NoSuchPackage_v2.0.0.zip" ) >/dev/null 2>&1; then
  ok "manifest_rename with no matching entry fails"
else bad "manifest_rename with no matching entry did not fail"; fi

# 6. manifest_names_check: exactly the manifest's names.
mapfile -t NAMES < <(sed -nE 's/.*<package fileName="([^"]+)".*/\1/p' "$MF")
if have manifest_names_check && ( manifest_names_check "$MF" "${NAMES[@]}" ) >/dev/null 2>&1; then
  ok "manifest_names_check passes for the manifest's own ${#NAMES[@]} names"
else bad "manifest_names_check failed for the manifest's own names"; fi
if have manifest_names_check && ! ( manifest_names_check "$MF" "${NAMES[@]:1}" ) >/dev/null 2>&1; then
  ok "manifest_names_check fails when a manifest package is not expected (${NAMES[0]})"
else bad "manifest_names_check accepted a manifest package it was not given"; fi
if have manifest_names_check && ! ( manifest_names_check "$MF" "${NAMES[@]}" "Extra_v1.0.0.zip" ) >/dev/null 2>&1; then
  ok "manifest_names_check fails when an expected package is missing (Extra_v1.0.0.zip)"
else bad "manifest_names_check accepted a missing package"; fi

if [ "$fails" -eq 0 ]; then echo "PASS: release.sh manifest edits ($SCRIPT)"; exit 0; fi
echo "FAIL: $fails check(s) ($SCRIPT)"; exit 1
