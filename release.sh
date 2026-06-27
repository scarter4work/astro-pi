#!/usr/bin/env bash
# astro-pi unified release: build -> native-sign -> package -> write ONE manifest -> sign manifest -> verify.
# Ordering is load-bearing: .xsgn embeds a timestamp, so hash AFTER packaging and sign the manifest LAST.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PI="${ASTROPI_PI_DIR:-/opt/PixInsight}/bin/PixInsight.sh"
KEYS="${ASTROPI_SIGN_KEYS:-/home/scarter4work/projects/keys/scarter4work_keys.xssk}"
PASS_FILE="${ASTROPI_SIGN_PASS_FILE:-/tmp/.pi_codesign_pass}"
REPO="$ROOT/repository"
DATE="$(date +%Y%m%d)"

die(){ echo "ERROR: $*" >&2; exit 1; }
[ -x "$PI" ]        || die "PixInsight.sh not executable at $PI"
[ -f "$KEYS" ]      || die "signing keys not found at $KEYS"
[ -f "$PASS_FILE" ] || die "password file not found at $PASS_FILE (create it 0600, never commit)"
# The EZ sign.sh reads the password from /tmp/.pi_codesign_pass directly; keep PASS_FILE aligned with it.
[ "$PASS_FILE" = "/tmp/.pi_codesign_pass" ] || die "EZ sign.sh expects /tmp/.pi_codesign_pass; set ASTROPI_SIGN_PASS_FILE accordingly"
PASS="$(cat "$PASS_FILE")"

sha1(){ python3 -c "import hashlib,sys;print(hashlib.sha1(open(sys.argv[1],'rb').read()).hexdigest())" "$1"; }

# write_pkg <manifest> <fileName> <sha1> : set sha1 + releaseDate for the package matching fileName.
write_pkg(){
  python3 - "$1" "$2" "$3" "$DATE" <<'PY'
import re,sys
mf,fn,h,date=sys.argv[1:5]
s=open(mf).read()
s=re.sub(r'(fileName="'+re.escape(fn)+r'"\s+sha1=")[0-9a-fA-F]{40}(")', r'\g<1>'+h+r'\g<2>', s)
s=re.sub(r'(fileName="'+re.escape(fn)+r'"[^>]*releaseDate=")\d{8}(")', r'\g<1>'+date+r'\g<2>', s)
open(mf,'w').write(s)
PY
}

echo "== 1/6 build NukeX module =="
cmake -B "$ROOT/modules/nukex/build" -S "$ROOT/modules/nukex" \
  -DPCLDIR="$HOME/PCL" -DNUKEX_BUILD_MODULE=ON -DNUKEX_BUILD_TESTS=ON >/dev/null
cmake --build "$ROOT/modules/nukex/build" -j"$(nproc)" >/dev/null
SO="$(find "$ROOT/modules/nukex/build" -name 'NukeX-pxm.so' -print -quit)"
[ -n "$SO" ] || die "NukeX-pxm.so not found after build"

echo "== 2/6 sign module =="
"$PI" --sign-module-file="$SO" --xssk-file="$KEYS" --xssk-password="$PASS"
XSGN="${SO%-pxm.so}-pxm.xsgn"
[ -f "$XSGN" ] || die "module signature $XSGN not produced"

echo "== 2b/6 native-sign EZ scripts =="
( cd "$ROOT/scripts/ez-stretch" && bash tools/sign.sh scripts )

echo "== 2c/6 native-sign gaia-depth-grade scripts =="
# Signs pi/*.js + the shared *.jsh -> *.xsgn and verifies each via getScriptSignature.
# Writes /tmp/.gaia_sign_result.json (automation-mode console never reaches stdout).
rm -f /tmp/.gaia_sign_result.json
LD_LIBRARY_PATH="${ASTROPI_PI_DIR:-/opt/PixInsight}/bin/lib:${ASTROPI_PI_DIR:-/opt/PixInsight}/bin" \
  "$PI" -n --automation-mode --no-startup-scripts --no-startup-check-updates \
        --no-startup-gui-messages -r="$ROOT/gaia-depth-grade/tools/SignGaiaScriptsNative.js" \
        --force-exit >/dev/null 2>&1 || true
python3 - /tmp/.gaia_sign_result.json <<'PY' || die "gaia script signing/verification failed"
import json,sys
r=json.load(open(sys.argv[1]))
assert r.get("ok"), r
print("  signed+verified:", ", ".join(r["verified"]))
PY

echo "== 3/6 package module tarball =="
mkdir -p "$REPO/bin"
cp "$SO" "$XSGN" "$REPO/bin/"
MOD_TGZ="$DATE-linux-x64-NukeX.tar.gz"
tar -C "$REPO" -czf "$REPO/$MOD_TGZ" bin/NukeX-pxm.so bin/NukeX-pxm.xsgn
python3 - "$REPO/updates.xri" "$MOD_TGZ" <<'PY'
import re,sys
mf,fn=sys.argv[1:3]
s=open(mf).read()
s=re.sub(r'fileName="[^"]*NukeX\.tar\.gz"', 'fileName="'+fn+'"', s)
open(mf,'w').write(s)
PY

echo "== 3b/6 package EZ script zips (install under src/scripts/scarter4work) =="
declare -A EZVER=( [EZStretch]=1.0.10 [EZDonutRepair]=1.0.3 [EZHazeKill]=1.0.1 )
EZSRC="$ROOT/scripts/ez-stretch/src/scripts/EZ Stretch BSC"
for name in EZStretch EZDonutRepair EZHazeKill; do
  ver="${EZVER[$name]}"
  zipname="${name}_v${ver}.zip"
  # Stage into src/scripts/scarter4work/ so the menu and the on-disk folder both
  # read scarter4work (the #feature-id in each .js already points there).
  stage="$(mktemp -d)"; mkdir -p "$stage/src/scripts/scarter4work"
  cp "$EZSRC/$name.js" "$EZSRC/$name.xsgn" "$stage/src/scripts/scarter4work/"
  rm -f "$REPO/$zipname"
  ( cd "$stage" && zip -qr "$REPO/$zipname" src )
  rm -rf "$stage"
  [ -f "$REPO/$zipname" ] || die "zip $zipname not produced"
done

echo "== 3c/6 package gaia-depth-grade script zip =="
# Mirror PI's install layout (src/scripts/GaiaDepthGrade/) so the package extracts
# into PixInsight's scripts tree; bundle the 3 sources + their .xsgn.
GAIA_VER=1.0.11  # script package version; the frozen sidecar is independent (see *_lib.jsh pins)
GAIA_ZIP="gaia-depth-grade_v${GAIA_VER}.zip"
GAIA_STAGE="$(mktemp -d)"
mkdir -p "$GAIA_STAGE/src/scripts/GaiaDepthGrade"
cp "$ROOT/gaia-depth-grade/pi/gaia_depth_grade.js"        "$ROOT/gaia-depth-grade/pi/gaia_depth_grade.xsgn" \
   "$ROOT/gaia-depth-grade/pi/GaiaDepthGradeDialog.js"    "$ROOT/gaia-depth-grade/pi/GaiaDepthGradeDialog.xsgn" \
   "$ROOT/gaia-depth-grade/pi/gaia_depth_grade_lib.jsh"   "$ROOT/gaia-depth-grade/pi/gaia_depth_grade_lib.xsgn" \
   "$GAIA_STAGE/src/scripts/GaiaDepthGrade/"
rm -f "$REPO/$GAIA_ZIP"
( cd "$GAIA_STAGE" && zip -qr "$REPO/$GAIA_ZIP" src )
rm -rf "$GAIA_STAGE"
[ -f "$REPO/$GAIA_ZIP" ] || die "zip $GAIA_ZIP not produced"
# NOTE: the frozen sidecar binary is NOT packaged here — it lives on GitHub Releases
# (>100MB), pinned by SIDECAR_URL/SIDECAR_SHA256 in pi/gaia_depth_grade_lib.jsh.
# After bumping the sidecar, rebuild+upload it (gaia-depth-grade/tools/build-sidecar.sh)
# and update those pins; this script only ships the thin signed scripts.

echo "== 4/6 write fileName/sha1/releaseDate into ONE manifest =="
write_pkg "$REPO/updates.xri" "$MOD_TGZ"                  "$(sha1 "$REPO/$MOD_TGZ")"
write_pkg "$REPO/updates.xri" "EZStretch_v1.0.10.zip"     "$(sha1 "$REPO/EZStretch_v1.0.10.zip")"
write_pkg "$REPO/updates.xri" "EZDonutRepair_v1.0.3.zip"  "$(sha1 "$REPO/EZDonutRepair_v1.0.3.zip")"
write_pkg "$REPO/updates.xri" "EZHazeKill_v1.0.1.zip"     "$(sha1 "$REPO/EZHazeKill_v1.0.1.zip")"
write_pkg "$REPO/updates.xri" "$GAIA_ZIP"                 "$(sha1 "$REPO/$GAIA_ZIP")"

echo "== 5/6 sign manifest LAST =="
sed -i '/<Signature developerId=/d' "$REPO/updates.xri"
"$PI" --sign-xml-file="$REPO/updates.xri" --xssk-file="$KEYS" --xssk-password="$PASS"
grep -q '<Signature developerId="scarter4work"' "$REPO/updates.xri" || die "manifest signature not appended"

echo "== 6/6 integrity check: declared sha1 == on-disk =="
python3 - "$REPO/updates.xri" "$REPO" <<'PY'
import re,sys,hashlib,os
mf,repo=sys.argv[1:3]
s=open(mf).read()
bad=0
for fn,h in re.findall(r'fileName="([^"]+)"\s+sha1="([0-9a-fA-F]{40})"', s):
    p=os.path.join(repo,fn)
    if not os.path.exists(p): print("MISSING",fn); bad=1; continue
    actual=hashlib.sha1(open(p,'rb').read()).hexdigest()
    if actual!=h: print("MISMATCH",fn,h,actual); bad=1
    else: print("OK",fn)
sys.exit(1 if bad else 0)
PY

echo "RELEASE OK — repository/ ready to commit & push"
