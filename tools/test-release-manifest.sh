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
#  - steps 0b and 3f and the name-set checks take the package names from one list, RELEASE_PKGS;
#  - (section 12) the EZTools cut-over: EZTOOLS_RETIRES + EZTOOLS_REMOVE give the EZTools entry exactly the
#    EZHazeKill <remove>, refused while the script zip is still published;
#  - (final review, module m5) every manifest edit of steps 3-6 is made on a work copy, and
#    repository/updates.xri is written once, by install_manifest, after the copy is signed and
#    verified: install_manifest replaces the destination with a signed, verifying copy (keeping its
#    mode) and refuses an unsigned or non-verifying one, leaving the destination byte for byte as it was.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT="${1:-$ROOT/release.sh}"
[ -f "$SCRIPT" ] || { echo "FAIL: no $SCRIPT"; exit 1; }
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
MF="$WORK/updates.xri"
sed '/<Signature developerId=/d' "$ROOT/repository/updates.xri" > "$MF"

die(){ echo "ERROR: $*" >&2; exit 1; }
DATE=20991231
for f in write_pkg manifest_rename manifest_names_check install_manifest retire_manifest module_platform_block manifest_removes ez_pkg_check; do
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
    if re.search(r'^\s*manifest_names_check ',l) and '"${RELEASE_PKGS[@]}"' not in l:
        errs.append("line %d checks the manifest against other names: %s"%(i+1,l.strip()))
print("\n".join(errs) if errs else "OK %d desc_args call(s), one RELEASE_PKGS"%len(calls))
PY
)"
if [[ "$names_report" == OK* ]]; then ok "0b, 3f and the set checks share one name list ($names_report)"
else while IFS= read -r l; do bad "$l"; done <<< "$names_report"; fi

# 8. install_manifest (module m5): a signed copy whose every sha1 verifies against the files beside the
#    destination replaces it, keeping its mode; an unsigned or non-verifying one is refused and the
#    destination is left byte for byte as it was.
R="$WORK/repo"; mkdir -p "$R"
printf 'package bytes\n' > "$R/Pkg_v1.0.0.zip"
PKG_SHA="$(sha1sum "$R/Pkg_v1.0.0.zip" | cut -d' ' -f1)"
cp "$ROOT/repository/updates.xri" "$R/updates.xri"; chmod 0644 "$R/updates.xri"
ORIG="$(sha1sum < "$R/updates.xri")"
manifest_with(){ # <sha1> <signed 0|1>
  printf '<xri>\n<platform os="linux" arch="x64">\n<package fileName="Pkg_v1.0.0.zip" sha1="%s" type="script" releaseDate="20991231"/>\n</platform>\n' "$1"
  [ "$2" = 1 ] && printf '<Signature developerId="scarter4work" timestamp="x" encoding="Base64">AA==</Signature>\n'
  printf '</xri>\n'
}
manifest_with "$PKG_SHA" 0 > "$WORK/unsigned.xri"
manifest_with 3333333333333333333333333333333333333333 1 > "$WORK/badsha.xri"
manifest_with "$PKG_SHA" 1 > "$WORK/good.xri"
for c in "unsigned:an unsigned copy" "badsha:a copy whose sha1 does not match its package"; do
  f="${c%%:*}"; what="${c#*:}"
  if have install_manifest && ! ( install_manifest "$WORK/$f.xri" "$R/updates.xri" ) >/dev/null 2>&1 \
     && [ "$(sha1sum < "$R/updates.xri")" = "$ORIG" ]; then
    ok "install_manifest refuses $what; the destination is unchanged"
  else bad "install_manifest did not refuse $what, or changed the destination"; fi
done
if have install_manifest && ( install_manifest "$WORK/good.xri" "$R/updates.xri" ) >/dev/null 2>&1 \
   && cmp -s "$WORK/good.xri" "$R/updates.xri" && [ "$(stat -c %a "$R/updates.xri")" = 644 ] \
   && [ -z "$(find "$R" -name '.updates.xri.*')" ]; then
  ok "install_manifest installs a signed, verifying copy (mode 644 kept, no temporary left)"
else bad "install_manifest did not install a signed, verifying copy cleanly"; fi

# 9. Steps 3-6 edit only the work copy: outside --package-test and install_manifest, a line naming
#    repository/updates.xri may only copy it to WORK_MF, read it (0b's description check, before the
#    copy), or install the work copy -- once, after the copy is signed and its descriptions verified;
#    the signature goes on the work copy.
writes_report="$(python3 - "$SCRIPT" <<'PY'
import re,sys
lines=open(sys.argv[1]).read().split("\n")
def block(start_re, end_line):
    st=[i for i,l in enumerate(lines) if re.match(start_re,l)]
    if not st: return None
    en=next((j for j in range(st[0],len(lines)) if lines[j]==end_line),len(lines))
    return (st[0],en)
skip=[b for b in (block(r'if \[ "\$\{1:-\}" = "--package-test" \]',"fi"), block(r'install_manifest\(\)\{',"}")) if b]
body=[(i,l) for i,l in enumerate(lines) if not l.lstrip().startswith("#") and not any(a<=i<=b for a,b in skip)]
errs=[]
copy=[i for i,l in body if re.match(r'\s*cp "\$REPO/updates.xri" "\$WORK_MF"\s*$',l)]
inst=[i for i,l in body if re.match(r'\s*install_manifest "\$WORK_MF" "\$REPO/updates.xri"\s*$',l)]
if len(copy)!=1: errs.append('the work copy (cp "$REPO/updates.xri" "$WORK_MF") is made %d times, expected once'%len(copy))
if len(inst)!=1: errs.append('install_manifest "$WORK_MF" "$REPO/updates.xri" is called %d times, expected once'%len(inst))
for i,l in body:
    if "REPO/updates.xri" not in l or i in copy or i in inst: continue
    if re.match(r'\s*descriptions check "\$REPO/updates.xri" ',l) and copy and i<copy[0]: continue
    errs.append("line %d names repository/updates.xri outside the work-copy flow: %s"%(i+1,l.strip()))
sign=[i for i,l in body if "--sign-xml-file=" in l]
if not sign or any('--sign-xml-file="$WORK_MF"' not in lines[i] for i in sign): errs.append("the manifest is not signed as the work copy")
ver=[i for i,l in body if re.match(r'\s*descriptions verify "\$WORK_MF" ',l)]
if inst and (not ver or not sign or inst[0]<max(ver+sign)): errs.append("install_manifest runs before the work copy is signed and verified")
if copy and inst and copy[0]>inst[0]: errs.append("the manifest is installed before the work copy is made")
print("\n".join(errs) if errs else "OK one work copy, one install after signing and verification")
PY
)"
if [[ "$writes_report" == OK* ]]; then ok "steps 3-6 edit only the work copy ($writes_report)"
else while IFS= read -r l; do bad "$l"; done <<< "$writes_report"; fi

# 8. retire_manifest: a withdrawn package (RETIRED_PKG_RE in release.sh) is dropped from a work copy,
#    exactly once, and the remaining packages are exactly the release's.
RET_RE="$(sed -nE "s/^RETIRED_PKG_RE=\('(.*)'\)\s*$/\1/p" "$SCRIPT")"
if [ -n "$RET_RE" ] && have retire_manifest; then
  RM="$WORK/retire.xri"; cp "$MF" "$RM"
  before="$(grep -c '<package fileName=' "$RM")"
  if [ "$(grep -cE "fileName=\"$RET_RE\"" "$RM")" -eq 0 ]; then
    # The release that withdrew it already published a manifest without it (EZDonutRepair: release
    # f466df1f): nothing left to drop, and the run below must leave the manifest as it is.
    b="$(sha1sum < "$RM")"
    ( retire_manifest "$RM" "$RET_RE" ) >/dev/null 2>&1 && [ "$(sha1sum < "$RM")" = "$b" ] \
      && ok "retire_manifest: $RET_RE is already out of the published manifest; the manifest is unchanged" \
      || bad "retire_manifest changed a manifest that no longer holds $RET_RE"
  elif [ "$(grep -cE "fileName=\"$RET_RE\"" "$RM")" -eq 1 ] && ( retire_manifest "$RM" "$RET_RE" ) >/dev/null 2>&1 \
     && [ "$(grep -c '<package fileName=' "$RM")" -eq $((before - 1)) ] && ! grep -qE "fileName=\"$RET_RE\"" "$RM" \
     && python3 -c 'import sys,xml.dom.minidom as m; m.parse(sys.argv[1])' "$RM"; then
    ok "retire_manifest drops the withdrawn package ($RET_RE) once; the manifest stays well-formed"
  else bad "retire_manifest did not drop exactly one entry for $RET_RE"; fi
  if ( retire_manifest "$RM" "$RET_RE" ) >/dev/null 2>&1; then ok "retire_manifest is idempotent once the entry is gone"; else bad "retire_manifest failed on an already-retired manifest"; fi
else echo "  (no RETIRED_PKG_RE / retire_manifest in release.sh)"; fi
# 10. module_platform_block (PCL 2.13: modules need PixInsight >= 1.9.6): on a fresh copy of the manifest,
#     the module packages move into ONE linux/x64 1.9.6:2.0.0 block placed after the 1.8.0 one, the
#     sidecar stays in the 1.8.0 block, the noarch block is untouched, a second run changes nothing,
#     and two entries for one module fail.
MB="$WORK/module-block.xri"
sed '/<Signature developerId=/d' "$ROOT/repository/updates.xri" > "$MB"
mb_report(){ python3 - "$MB" <<'PY'
import re,sys
s=open(sys.argv[1]).read()
blocks=[(m.group(1),m.group(2)) for m in re.finditer(r'<platform ([^>]*)>(.*?)</platform>',s,re.S)]
heads=[h for h,_ in blocks]
names=lambda b: re.findall(r'<package fileName="([^"]+)"',b)
out=[]
for h,b in blocks: out.append("%s: %s"%(h," ".join(sorted(names(b)))))
print("\n".join(out))
PY
}
noarch_before="$(mb_report | grep 'os="all"')"
if have module_platform_block && ( module_platform_block "$MB" NukeX PICopilot Planetary GaiaDepthGrade ) >/dev/null 2>&1; then
  rep="$(mb_report)"
  heads="$(grep -c 'os="linux" arch="x64"' <<< "$rep")"
  mods="$(grep 'version="1.9.6:2.0.0"' <<< "$rep")"
  old="$(grep 'os="linux" arch="x64" version="1.8.0:2.0.0"' <<< "$rep")"
  if [ "$heads" = 2 ] && grep -q 'NukeX' <<< "$mods" && grep -q 'PICopilot' <<< "$mods" && grep -q 'Planetary' <<< "$mods" \
     && ! grep -q 'NukeX\|PICopilot\|Planetary' <<< "$old" && grep -q 'GaiaDepthGradeSidecar' <<< "$old" \
     && ! grep -q 'GaiaDepthGradeSidecar' <<< "$mods" && [ "$(grep 'os="all"' <<< "$rep")" = "$noarch_before" ] \
     && [ "$(grep -n 'version=' <<< "$rep" | head -2 | tail -1 | grep -c '1.9.6:2.0.0')" = 1 ]; then
    ok "module_platform_block: NukeX, PICopilot, Planetary in the linux/x64 1.9.6:2.0.0 block; sidecar and scripts keep theirs"
  else bad "module_platform_block placed the packages wrongly:"; sed 's/^/        /' <<< "$rep"; fi
  before="$(sha1sum < "$MB")"
  ( module_platform_block "$MB" NukeX PICopilot Planetary GaiaDepthGrade ) >/dev/null 2>&1 && [ "$(sha1sum < "$MB")" = "$before" ] \
    && ok "module_platform_block is idempotent" || bad "a second module_platform_block run changed the manifest"
  python3 -c "import re,sys;p=sys.argv[1];s=open(p).read();m=re.search(r'\n[ \t]*<package fileName=\"[^\"]*-linux-x64-NukeX[^\"]*\".*?</package>',s,re.S);open(p,'w').write(s[:m.end()]+m.group(0)+s[m.end():])" "$MB"
  ( module_platform_block "$MB" NukeX ) >/dev/null 2>&1 && bad "module_platform_block accepted two NukeX entries" \
    || ok "module_platform_block fails on two entries for one module"
else bad "module_platform_block failed on the repository manifest"; fi

# 10. manifest_removes: release.sh's PKG_REMOVE specs land as <remove> elements, each on exactly one
#     package, and the manifest carries no other <remove>. Refused (manifest unchanged): a regex matching
#     no package or several, an item outside bin/ src/scripts/ doc/scripts/ doc/tools/ (or one of those
#     roots, or with ".." or a glob), and an item that would delete a file another package installs.
#     Package contents come from stand-in archives built here, one per manifest package.
mapfile -t REMOVE_SPECS < <(sed -n '/^PKG_REMOVE=(/,/^)/p' "$SCRIPT" | sed -nE "s/^[[:space:]]*'([^']+)'[[:space:]]*$/\1/p")
if have manifest_removes && [ "${#REMOVE_SPECS[@]}" -gt 0 ]; then
  RMF="$WORK/removes.xri"; sed '/<Signature developerId=/d' "$ROOT/repository/updates.xri" > "$RMF"
  # As in a release: withdrawn packages leave the work copy before step 3g (a <remove> may not delete
  # what a package still in the manifest installs, and EZDonutRepair is still in repository/updates.xri).
  [ -z "$RET_RE" ] || ( retire_manifest "$RMF" "$RET_RE" ) >/dev/null 2>&1 || bad "retire_manifest failed on the removes copy"
  PK="$WORK/pkgs"; mkdir -p "$PK"
  stand_in(){ # <fileName> <path inside the package>...
    local fn="$1" d; shift; d="$(mktemp -d "$WORK/si.XXXXXX")"
    for p in "$@"; do mkdir -p "$d/$(dirname "$p")"; printf 'x\n' > "$d/$p"; done
    case "$fn" in
      *.zip) ( cd "$d" && zip -qr "$PK/$fn" . ) ;;
      *.tar.gz) tar -C "$d" -czf "$PK/$fn" . ;;
    esac
  }
  while IFS= read -r fn; do
    case "$fn" in
      EZ*_v*.zip) n="${fn%%_v*}"; stand_in "$fn" "src/scripts/scarter4work/$n.js" "src/scripts/scarter4work/$n.xsgn" "doc/scripts/$n/$n.html" ;;
      gaia-depth-grade_v*.zip) stand_in "$fn" src/scripts/GaiaDepthGrade/GaiaDepthGrade.js doc/scripts/GaiaDepthGrade/GaiaDepthGrade.html ;;
      *-GaiaDepthGradeSidecar-*.tar.gz) stand_in "$fn" bin/gaia-depth-grade-sidecar ;;
      *-linux-x64-*.tar.gz) n="${fn#*-linux-x64-}"; n="${n%%-*}"; stand_in "$fn" "bin/$n-pxm.so" "bin/$n-pxm.xsgn" ;;
      *) stand_in "$fn" "doc/scripts/unknown/$fn.html" ;;
    esac
  done < <(sed -nE 's/.*<package fileName="([^"]+)".*/\1/p' "$RMF")

  removes_report(){ python3 - "$RMF" "$@" <<'PY'
import re,sys
s=open(sys.argv[1]).read(); specs=sys.argv[2:]
errs=[]
els=re.findall(r'<remove>(.*?)</remove>',s,re.S)
if len(els)!=len(specs): errs.append("%d <remove> elements, expected %d (one per PKG_REMOVE spec)"%(len(els),len(specs)))
for sp in specs:
    rx,items=sp.split("=",1); want=[i.strip() for i in items.split(",")]
    pk=[m for m in re.finditer(r'<package fileName="([^"]+)"[^>]*>(.*?)</package>',s,re.S) if re.fullmatch(rx,m.group(1))]
    if len(pk)!=1: errs.append("%s matches %d packages"%(rx,len(pk))); continue
    r=re.findall(r'<remove>(.*?)</remove>',pk[0].group(2),re.S)
    if len(r)!=1: errs.append("%s carries %d <remove> elements"%(pk[0].group(1),len(r))); continue
    got=[l.strip() for l in r[0].split("\n") if l.strip()]
    if got!=want: errs.append("%s removes %s, expected %s"%(pk[0].group(1),got,want))
import xml.dom.minidom as m
try: m.parseString(s)
except Exception as e: errs.append("not well-formed: %s"%e)
print("\n".join(errs) if errs else "OK")
PY
  }
  if ( manifest_removes "$RMF" "$PK" "${REMOVE_SPECS[@]}" ) >/dev/null 2>&1 && r="$(removes_report "${REMOVE_SPECS[@]}")" && [ "$r" = OK ]; then
    ok "PKG_REMOVE lands: ${#REMOVE_SPECS[@]} <remove> element(s), each on exactly one package (${REMOVE_SPECS[*]%%=*})"
  else bad "PKG_REMOVE did not land as one <remove> per spec on exactly one package: ${r:-manifest_removes failed}"; fi
  once="$(sha1sum < "$RMF")"
  ( manifest_removes "$RMF" "$PK" "${REMOVE_SPECS[@]}" ) >/dev/null 2>&1 && [ "$(sha1sum < "$RMF")" = "$once" ] \
    && ok "manifest_removes is idempotent" || bad "a second manifest_removes run changed the manifest"

  # The mechanism is reusable: a second carrying package (the Gaia cut-over shape) gets its own element.
  GAIA_SPEC='gaia-depth-grade_v[0-9.]+\.zip=src/scripts/GaiaDepthGrade/old_helper.js,doc/scripts/GaiaDepthGrade/old.html'
  cp "$RMF" "$WORK/two.xri"
  if ( manifest_removes "$WORK/two.xri" "$PK" "${REMOVE_SPECS[@]}" "$GAIA_SPEC" ) >/dev/null 2>&1 \
     && r="$(RMF="$WORK/two.xri"; removes_report "${REMOVE_SPECS[@]}" "$GAIA_SPEC")" && [ "$r" = OK ]; then
    ok "two carrying packages: each gets exactly its own <remove>"
  else bad "a second carrying package did not get its own <remove>: ${r:-manifest_removes failed}"; fi
  # Dropping a spec drops its element: no stale <remove> outlives PKG_REMOVE.
  cp "$RMF" "$WORK/none.xri"
  if ( manifest_removes "$WORK/none.xri" "$PK" ) >/dev/null 2>&1 && ! grep -q '<remove>' "$WORK/none.xri"; then
    ok "with no specs, every <remove> is dropped"
  else bad "a <remove> survived an empty PKG_REMOVE"; fi

  before="$(sha1sum < "$RMF")"
  for c in \
    'NoSuchPkg_v[0-9.]+\.zip=src/scripts/x.js|a regex matching no package' \
    'EZ[A-Za-z]+_v[0-9.]+\.zip=src/scripts/scarter4work/Old.js|a regex matching several packages' \
    'EZStretch_v[0-9.]+\.zip=src/scripts/../../bin/PixInsight|an item with ..' \
    'EZStretch_v[0-9.]+\.zip=/opt/PixInsight/bin/x|an absolute item' \
    'EZStretch_v[0-9.]+\.zip=src/scripts|a deployment root itself' \
    'EZStretch_v[0-9.]+\.zip=etc/x.conf|an item outside bin/ src/scripts/ doc/scripts/ doc/tools/' \
    'EZStretch_v[0-9.]+\.zip=src/scripts/scarter4work/*.js|a glob' \
    'EZStretch_v[0-9.]+\.zip=src/scripts/scarter4work|a directory holding another package'"'"'s files' \
    'gaia-depth-grade_v[0-9.]+\.zip=bin/gaia-depth-grade-sidecar|a file another package of the release installs'; do
    spec="${c%%|*}"; what="${c#*|}"
    if ! ( manifest_removes "$RMF" "$PK" "$spec" ) >/dev/null 2>&1 && [ "$(sha1sum < "$RMF")" = "$before" ]; then
      ok "manifest_removes refuses $what; the manifest is unchanged"
    else bad "manifest_removes accepted $what, or changed the manifest"; fi
  done
else bad "release.sh has no manifest_removes or no PKG_REMOVE specs"; fi

# 11. ez_pkg_check: an EZ zip deploys exactly <Name>.js/.xsgn and the current doc/scripts/<Name>/, with the
#     .js and every doc file byte for byte as in the source tree (so a reused published zip with stale
#     docs fails and forces a version bump).
EZSRC="$(sed -nE 's/^EZSRC="\$ROOT\/(.*)"$/\1/p' "$SCRIPT")"; EZSRC="$ROOT/$EZSRC"
EZDOC="$(sed -nE 's/^EZDOC="\$ROOT\/(.*)"$/\1/p' "$SCRIPT")"; EZDOC="$ROOT/$EZDOC"
if have ez_pkg_check && [ -f "$EZSRC/EZStretch.js" ] && [ -f "$EZDOC/EZStretch/EZStretch.html" ]; then
  ez_zip(){ # <out.zip> <mode: good|nodoc|staledoc|extra|stalejs>
    local d; d="$(mktemp -d "$WORK/ez.XXXXXX")"
    mkdir -p "$d/src/scripts/scarter4work" "$d/doc/scripts"
    cp "$EZSRC/EZStretch.js" "$d/src/scripts/scarter4work/"; printf 'sig\n' > "$d/src/scripts/scarter4work/EZStretch.xsgn"
    cp -r "$EZDOC/EZStretch" "$d/doc/scripts/EZStretch"
    case "$2" in
      nodoc) rm -rf "$d/doc/scripts/EZStretch" ;;
      staledoc) printf '<p>old</p>\n' > "$d/doc/scripts/EZStretch/EZStretch.html" ;;
      extra) printf 'x\n' > "$d/src/scripts/scarter4work/EZDonutRepair.js" ;;
      stalejs) printf '// old\n' >> "$d/src/scripts/scarter4work/EZStretch.js" ;;
    esac
    ( cd "$d" && zip -qr "$1" . )
  }
  ez_zip "$WORK/ez-good.zip" good
  ( ez_pkg_check "$WORK/ez-good.zip" EZStretch ) >/dev/null 2>&1 && ok "ez_pkg_check accepts a zip with the current .js and docs" \
    || bad "ez_pkg_check rejected a correct EZStretch zip"
  for m in nodoc staledoc extra stalejs; do
    ez_zip "$WORK/ez-$m.zip" "$m"
    ! ( ez_pkg_check "$WORK/ez-$m.zip" EZStretch ) >/dev/null 2>&1 && ok "ez_pkg_check rejects a zip with $m" \
      || bad "ez_pkg_check accepted a zip with $m"
  done
else bad "release.sh has no ez_pkg_check, or EZStretch has no source/documentation"; fi

# 12. The EZTools cut-over (EZTOOLS_MODULE_SHIPS=1 in release.sh), on a scratch manifest whatever the flag
#     says: with the EZHazeKill script zip retired (EZTOOLS_RETIRES) and an EZTools module entry in the
#     1.9.6:2.0.0 block, EZTOOLS_REMOVE lands as exactly one <remove> on the EZTools entry, no
#     EZHazeKill_v*.zip entry remains, and a second run changes nothing. While the script zip is still in
#     the manifest (the flag at 0), manifest_removes refuses that <remove>: it would delete what the zip
#     installs -- so the two can only ship together.
EZT_RET="$(sed -nE "s/^EZTOOLS_RETIRES='(.*)'\s*$/\1/p" "$SCRIPT")"
EZT_REM="$(sed -nE "s/^EZTOOLS_REMOVE='(.*)'\s*$/\1/p" "$SCRIPT")"
EZT_WANT='[0-9]{8}-linux-x64-EZTools-[0-9.]+\.tar\.gz=src/scripts/scarter4work/EZHazeKill.js,src/scripts/scarter4work/EZHazeKill.xsgn,doc/scripts/EZHazeKill'
if [ -z "$EZT_RET" ] || [ -z "$EZT_REM" ]; then
  bad "release.sh has no EZTOOLS_RETIRES / EZTOOLS_REMOVE (the EZTools cut-over is not wired)"
elif ! have manifest_removes || ! have retire_manifest || ! declare -F stand_in >/dev/null; then
  bad "release.sh has no manifest_removes / retire_manifest (or section 10 did not run)"
else
  [ "$EZT_REM" = "$EZT_WANT" ] && ok "EZTOOLS_REMOVE is the script's three installed items on the EZTools package" \
    || bad "EZTOOLS_REMOVE is '$EZT_REM', expected '$EZT_WANT'"
  EZT_FN=20991231-linux-x64-EZTools-1.0.0.0.tar.gz
  stand_in "$EZT_FN" bin/EZTools-pxm.so bin/EZTools-pxm.xsgn doc/tools/EZHazeKill/EZHazeKill.html
  ezt_add(){ python3 - "$1" "$EZT_FN" <<'PY'
import re,sys
mf,fn=sys.argv[1:3]; s=open(mf).read()
entry=('\n      <package fileName="'+fn+'" sha1="0000000000000000000000000000000000000000" type="module" '
       'releaseDate="00000000">\n         <title>EZTools</title>\n         <description></description>\n      </package>\n')
s,n=re.subn(r'(<platform os="linux" arch="x64" version="1\.9\.6:2\.0\.0">.*?)(\s*</platform>)',
            lambda m: m.group(1)+entry+m.group(2), s, count=1, flags=re.DOTALL)
if n!=1: sys.exit("no 1.9.6:2.0.0 block")
open(mf,'w').write(s)
PY
  }
  # Flag at 0: the script zip is still published, so the <remove> is refused and the manifest unchanged.
  EZ0="$WORK/eztools-flag0.xri"; cp "$RMF" "$WORK/eztools-base.xri"; cp "$RMF" "$EZ0"
  ezt_add "$EZ0" || bad "could not add an EZTools entry to the scratch manifest"
  b0="$(sha1sum < "$EZ0")"
  if grep -qE "fileName=\"$EZT_RET\"" "$EZ0" && ! ( manifest_removes "$EZ0" "$PK" "${REMOVE_SPECS[@]}" "$EZT_REM" ) >/dev/null 2>&1 \
     && [ "$(sha1sum < "$EZ0")" = "$b0" ]; then
    ok "with the EZHazeKill zip still in the manifest, the EZTools <remove> is refused (it would delete the zip's files)"
  else bad "the EZTools <remove> was accepted while the EZHazeKill zip is still published (or the manifest has no such zip)"; fi
  # Flag at 1: retire, add, remove.
  EZ1="$WORK/eztools-flag1.xri"; cp "$WORK/eztools-base.xri" "$EZ1"
  if ( retire_manifest "$EZ1" "$RET_RE" "$EZT_RET" ) >/dev/null 2>&1 && ezt_add "$EZ1" \
     && ( manifest_removes "$EZ1" "$PK" "${REMOVE_SPECS[@]}" "$EZT_REM" ) >/dev/null 2>&1 \
     && r="$(RMF="$EZ1"; removes_report "${REMOVE_SPECS[@]}" "$EZT_REM")" && [ "$r" = OK ]; then
    ok "the EZTools entry carries exactly the EZHazeKill <remove> (and EZStretch keeps its own)"
  else bad "the EZTools <remove> did not land as exactly one element on the EZTools entry: ${r:-a step failed}"; fi
  ! grep -qE 'fileName="EZHazeKill_v[0-9.]+\.zip"' "$EZ1" && ok "no EZHazeKill_v*.zip entry remains" \
    || bad "an EZHazeKill_v*.zip entry survived the cut-over"
  once="$(sha1sum < "$EZ1")"
  ( retire_manifest "$EZ1" "$RET_RE" "$EZT_RET" ) >/dev/null 2>&1 && ( manifest_removes "$EZ1" "$PK" "${REMOVE_SPECS[@]}" "$EZT_REM" ) >/dev/null 2>&1 \
    && [ "$(sha1sum < "$EZ1")" = "$once" ] && ok "the cut-over's retire + <remove> are idempotent on a re-run" \
    || bad "a second retire + manifest_removes run changed the cut-over manifest"
  # And today's flag: unshipped, the script stays in the release.
  if grep -qE '^EZTOOLS_MODULE_SHIPS=0$' "$SCRIPT"; then
    grep -qE '^EZ_NAMES=\(EZStretch EZHazeKill\)' "$SCRIPT" && ok "EZTOOLS_MODULE_SHIPS=0: the EZHazeKill script still ships (EZ_NAMES)" \
      || bad "EZTOOLS_MODULE_SHIPS=0 but EZHazeKill is not in EZ_NAMES: nothing would deliver EZHazeKill"
  fi
fi

if [ "$fails" -eq 0 ]; then echo "PASS: release.sh manifest edits ($SCRIPT)"; exit 0; fi
echo "FAIL: $fails check(s) ($SCRIPT)"; exit 1
