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
#  - manifest_names_check passes for exactly the manifest's names and fails for a missing or extra one;
#  - steps 0b and 3f and the name-set checks take the package names from one list, RELEASE_PKGS.
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

# 7. Steps 0b (description check), 3f (description write) and the name-set checks take the release's
#    package names from ONE list: RELEASE_PKGS, set once, before 0b's check (review round 2). A second
#    source of names lets 0b pass on names 3f never writes.
names_report="$(python3 - "$SCRIPT" <<'PY'
import re,sys
lines=open(sys.argv[1]).read().split("\n")
pt=[i for i,l in enumerate(lines) if l.startswith('if [ "${1:-}" = "--package-test" ]')]
end=next((j for j in range(pt[0],len(lines)) if lines[j]=="fi"),None) if pt else None
body=[(i,l) for i,l in enumerate(lines) if not l.lstrip().startswith("#") and not (pt and pt[0]<=i<=end)]
errs=[]
sets=[i for i,l in body if re.match(r'\s*RELEASE_PKGS=\(',l)]
if len(sets)!=1: errs.append("RELEASE_PKGS is set %d times, expected once"%len(sets))
calls=[(i,l) for i,l in body if re.search(r'desc_args ',l) and not re.match(r'\s*desc_args\(\)',l)]
for i,l in calls:
    if 'desc_args "${RELEASE_PKGS[@]}"' not in l: errs.append("line %d builds descriptions from other names: %s"%(i+1,l.strip()))
if not calls: errs.append("no desc_args call outside --package-test")
checks=[i for i,l in body if re.search(r'^\s*descriptions check ',l)]
if sets and checks and checks[0]<sets[0]: errs.append("step 0b's description check (line %d) runs before RELEASE_PKGS is set (line %d)"%(checks[0]+1,sets[0]+1))
for i,l in body:
    if re.search(r'manifest_names_check "\$REPO/updates.xri"',l) and '"${RELEASE_PKGS[@]}"' not in l:
        errs.append("line %d checks the manifest against other names: %s"%(i+1,l.strip()))
print("\n".join(errs) if errs else "OK %d desc_args call(s), one RELEASE_PKGS"%len(calls))
PY
)"
if [[ "$names_report" == OK* ]]; then ok "0b, 3f and the set checks share one name list ($names_report)"
else while IFS= read -r l; do bad "$l"; done <<< "$names_report"; fi

if [ "$fails" -eq 0 ]; then echo "PASS: release.sh manifest edits ($SCRIPT)"; exit 0; fi
echo "FAIL: $fails check(s) ($SCRIPT)"; exit 1
