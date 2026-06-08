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

echo "== 3b/6 package EZ script zips =="
declare -A EZVER=( [EZStretch]=1.0.9 [EZDonutRepair]=1.0.2 [EZHazeKill]=1.0.0 )
for name in EZStretch EZDonutRepair EZHazeKill; do
  ver="${EZVER[$name]}"
  zipname="${name}_v${ver}.zip"
  ( cd "$ROOT/scripts/ez-stretch" && \
    rm -f "$REPO/$zipname" && \
    zip -q "$REPO/$zipname" "src/scripts/EZ Stretch BSC/$name.js" "src/scripts/EZ Stretch BSC/$name.xsgn" )
  [ -f "$REPO/$zipname" ] || die "zip $zipname not produced"
done

echo "== 4/6 write fileName/sha1/releaseDate into ONE manifest =="
write_pkg "$REPO/updates.xri" "$MOD_TGZ"                  "$(sha1 "$REPO/$MOD_TGZ")"
write_pkg "$REPO/updates.xri" "EZStretch_v1.0.9.zip"      "$(sha1 "$REPO/EZStretch_v1.0.9.zip")"
write_pkg "$REPO/updates.xri" "EZDonutRepair_v1.0.2.zip"  "$(sha1 "$REPO/EZDonutRepair_v1.0.2.zip")"
write_pkg "$REPO/updates.xri" "EZHazeKill_v1.0.0.zip"     "$(sha1 "$REPO/EZHazeKill_v1.0.0.zip")"

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
