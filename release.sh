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
# Every PixInsight launch below goes through pi_headless: a private Xvfb, no
# WAYLAND_DISPLAY, QT_QPA_PLATFORM=xcb -- never the user's desktop.
. "$ROOT/tools/pi-headless.sh"
python3 "$ROOT/tools/check-pi-launches.py" "$ROOT" || die "a script launches PixInsight outside pi_headless"
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

# ── NukeX camera (QE) database publication ──────────────────────────────────
# The module embeds modules/nukex/share/qe_database.json and fetches updates
# from $QE_URL (nukex::kQEUpdateBaseURL). The signed publication -- the four
# files below, Ed25519-signed with the key whose public half is pinned in
# qe_update.cpp -- lives in modules/nukex/repository/ and is copied verbatim to
# repository/. The module reads its embedded db_version from that manifest at
# BUILD time, so the publication must be settled before step 1.
#
# The QE key is a raw Ed25519 seed (0600, beside the .xssk), not the .xssk:
# the module verifies Ed25519, and PixInsight's code-signing identity cannot
# produce that. It is only needed to publish a CHANGED database; set
# ASTROPI_QE_DB_VERSION (> the published one) and ASTROPI_QE_DB_SUMMARY to do so.
QE_SRC="$ROOT/modules/nukex/repository"
QE_DB="$ROOT/modules/nukex/share/qe_database.json"
QE_KEY="${ASTROPI_QE_SIGN_KEY:-/home/scarter4work/projects/keys/nukex_qe_signing.key}"
QE_FILES=(qe_manifest.json qe_manifest.json.sig qe_database.json qe_database.json.sig)
QE_URL="https://raw.githubusercontent.com/scarter4work/astro-pi/main/repository"
# The URL this script publishes to must be the one the module fetches from.
grep -qF "kQEUpdateBaseURL =" "$ROOT/modules/nukex/src/lib/calibration/include/nukex/calibration/qe_update.hpp" \
  && grep -qF "\"$QE_URL\";" "$ROOT/modules/nukex/src/lib/calibration/include/nukex/calibration/qe_update.hpp" \
  || die "nukex::kQEUpdateBaseURL is not $QE_URL -- the updater would fetch from somewhere this release does not publish"

# qe_check <dir>: the publication in <dir> is complete, signed by the pinned key,
# its manifest names the exact bytes of the embedded database, and it carries
# that database verbatim. Independent of the C++ verifier (step 6 runs that too).
qe_check(){
  python3 - "$1" "$QE_DB" "$ROOT/modules/nukex/src/lib/calibration/src/qe_update.cpp" <<'PY'
import base64,hashlib,json,re,sys,os
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
d,db,src=sys.argv[1:4]
m=re.search(r'kQEPublicKey\[[^\]]*\]\s*=\s*\{([^}]*)\}', open(src).read())
if not m: sys.exit("cannot find kQEPublicKey in "+src)
pub=Ed25519PublicKey.from_public_bytes(bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})', m.group(1))))
bad=0
for name in ("qe_manifest.json","qe_database.json"):
    p=os.path.join(d,name)
    if not (os.path.isfile(p) and os.path.isfile(p+".sig")): print("  MISSING",name,"or its .sig in",d); bad=1; continue
    try: pub.verify(base64.b64decode(open(p+".sig").read().strip(), validate=True), open(p,'rb').read())
    except Exception as e: print("  BAD SIGNATURE",name,type(e).__name__); bad=1
if bad: sys.exit(1)
mf=json.load(open(os.path.join(d,"qe_manifest.json")))
body=open(db,'rb').read()
if mf["db_sha512"].lower()!=hashlib.sha512(body).hexdigest(): sys.exit("  manifest db_sha512 does not name the embedded share/qe_database.json")
if open(os.path.join(d,"qe_database.json"),'rb').read()!=body: sys.exit("  published qe_database.json differs from the embedded one")
if mf["db_bytes"]!=len(body): sys.exit("  manifest db_bytes is wrong")
print("  OK  qe db_version %d, %d bytes, signatures verify against the pinned key" % (mf["db_version"], len(body)))
PY
}

echo "== 0/6 settle the NukeX camera-database publication (before the build embeds its version) =="
if ! qe_check "$QE_SRC"; then
  [ -n "${ASTROPI_QE_DB_VERSION:-}" ] && [ -n "${ASTROPI_QE_DB_SUMMARY:-}" ] \
    || die "modules/nukex/share/qe_database.json is not the published camera database. To publish it, set ASTROPI_QE_DB_VERSION (greater than the current db_version) and ASTROPI_QE_DB_SUMMARY and re-run."
  [ -f "$QE_KEY" ] || die "QE signing key not found at $QE_KEY"
  OLD_QE_VER="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["db_version"])' "$QE_SRC/qe_manifest.json" 2>/dev/null || echo 0)"
  [ "$ASTROPI_QE_DB_VERSION" -gt "$OLD_QE_VER" ] || die "ASTROPI_QE_DB_VERSION=$ASTROPI_QE_DB_VERSION must exceed the published $OLD_QE_VER (clients refuse a rollback)"
  ( cd "$ROOT/modules/nukex" && python3 tools/sign_qe_database.py share/qe_database.json \
      --key "$QE_KEY" --db-version "$ASTROPI_QE_DB_VERSION" --summary "$ASTROPI_QE_DB_SUMMARY" --out repository ) \
    || die "QE database signing failed"
  qe_check "$QE_SRC" || die "freshly signed QE publication does not verify"
fi

echo "== 1/6 build NukeX + PICopilot modules (portable Rocky 9 container) =="
# NEVER ship a host build: the dev box's glibc/libstdc++ symbol versions and
# Fedora-only sonames (libceres/libglog/libgflags) end up in the module, which
# then loads on this machine and nowhere else. See tools/build-env/Containerfile.
"$ROOT/tools/build-env/build-modules.sh" || die "portable module build/tests failed"
SO="$(find "$ROOT/modules/nukex/build-portable" -name 'NukeX-pxm.so' -print -quit)"
[ -n "$SO" ] || die "NukeX-pxm.so not found after build"
PICOPILOT_SO="$(find "$ROOT/modules/pi-copilot/build-portable" -name 'PICopilot-pxm.so' -print -quit)"
[ -n "$PICOPILOT_SO" ] || die "PICopilot-pxm.so not found after build"

echo "== 1b/6 prove both modules load on stock older distros =="
"$ROOT/tools/build-env/verify-portable.sh" "$SO" "$PICOPILOT_SO" || die "a module would not load on a stock distro"

echo "== 2/6 sign NukeX module =="
XSGN="${SO%-pxm.so}-pxm.xsgn"
rm -f "$XSGN"   # a stale .xsgn from a prior build/self-test must not survive a failed sign
pi_headless "$PI" --sign-module-file="$SO" --xssk-file="$KEYS" --xssk-password="$PASS"
[ -f "$XSGN" ] || die "module signature $XSGN not produced"

echo "== 2a/6 sign PICopilot module =="
PICOPILOT_XSGN="${PICOPILOT_SO%-pxm.so}-pxm.xsgn"
rm -f "$PICOPILOT_XSGN"   # same guard as NukeX above
pi_headless "$PI" --sign-module-file="$PICOPILOT_SO" --xssk-file="$KEYS" --xssk-password="$PASS"
[ -f "$PICOPILOT_XSGN" ] || die "module signature $PICOPILOT_XSGN not produced"

echo "== 2b/6 native-sign EZ scripts =="
( cd "$ROOT/scripts/ez-stretch" && bash tools/sign.sh scripts )

echo "== 2c/6 native-sign gaia-depth-grade scripts =="
# Signs pi/*.js + the shared *.jsh -> *.xsgn and verifies each via getScriptSignature.
# Writes /tmp/.gaia_sign_result.json (automation-mode console never reaches stdout).
rm -f /tmp/.gaia_sign_result.json
LD_LIBRARY_PATH="${ASTROPI_PI_DIR:-/opt/PixInsight}/bin/lib:${ASTROPI_PI_DIR:-/opt/PixInsight}/bin" \
  pi_headless "$PI" -n --automation-mode --no-startup-scripts --no-startup-check-updates \
        --no-startup-gui-messages -r="$ROOT/gaia-depth-grade/tools/SignGaiaScriptsNative.js" \
        --force-exit >/dev/null 2>&1 || true
python3 - /tmp/.gaia_sign_result.json "$ROOT/gaia-depth-grade/pi" <<'PY' || die "gaia script signing/verification failed"
import json,os,sys
r=json.load(open(sys.argv[1]))
assert r.get("ok"), r
assert os.path.realpath(r.get("dir") or "") == os.path.realpath(sys.argv[2]), ("signed the wrong tree", r.get("dir"), sys.argv[2])
print("  signed+verified:", ", ".join(r["verified"]))
PY

echo "== 3/6 package NukeX module tarball =="
mkdir -p "$REPO/bin"
cp "$SO" "$XSGN" "$REPO/bin/"
# The version goes in the tarball name (ported from nukex5 3868a85). Several
# releases on one day otherwise share "<date>-linux-x64-<Name>.tar.gz":
# raw.githubusercontent.com keeps serving the previous bytes for that path for
# minutes after a push while the signed manifest already names the new sha1,
# and PixInsight's updater records installed packages by fileName
# (/opt/PixInsight/etc/update/installed.xri). A name that changes with every
# release can be neither stale nor mistaken for the installed one.
modver(){ sed -nE "s/^#define $2_MODULE_VERSION_(MAJOR|MINOR|REVISION|BUILD) +([0-9]+).*/\\2/p" "$1" | paste -sd. -; }
NUKEX_VER="$(modver "$ROOT/modules/nukex/src/module/NukeXVersion.h" NUKEX)"
PICOPILOT_VER="$(modver "$ROOT/modules/pi-copilot/src/module/PICopilotVersion.h" PICOPILOT)"
[[ "$NUKEX_VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "could not read NukeX version (got '$NUKEX_VER')"
[[ "$PICOPILOT_VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "could not read PICopilot version (got '$PICOPILOT_VER')"
# The published tarball of a module VERSION, whatever date it was cut on
# (the name is <YYYYMMDD>-linux-x64-<Module>-<ver>.tar.gz), else a new dated
# name. Matching the dated name alone re-packaged an unchanged version under
# a new name on any later day, and the updater (which tracks installed
# packages by fileName) then offered the same version as an update.
module_tgz(){ # <Module> <ver>
  local hit
  hit="$(git -C "$ROOT" ls-files "repository/*-linux-x64-$1-$2.tar.gz" | sed 's#^repository/##')"
  [ "$(printf '%s\n' "$hit" | grep -c .)" -le 1 ] || die "several published tarballs for $1 $2: $hit"
  if [ -n "$hit" ]; then echo "$hit"; else echo "$DATE-linux-x64-$1-$2.tar.gz"; fi
}
MOD_TGZ="$(module_tgz NukeX "$NUKEX_VER")"
# A versioned module tarball that is already committed is PUBLISHED: never
# rebuild it (a re-sign changes its bytes under the same name, and the raw CDN
# would serve the old bytes against the new manifest sha1). Bump the module
# version to ship new bytes.
reuse_if_published(){ # <file in repository/>: restore the committed bytes of an already-published package
  if git -C "$ROOT" ls-files --error-unmatch "repository/$1" >/dev/null 2>&1; then
    git -C "$ROOT" checkout -- "repository/$1"
    echo "   $1 already published -- reusing the committed bytes"
  fi
}
package_module(){ # <tarball> <files...>
  local tgz="$1"; shift
  if git -C "$ROOT" ls-files --error-unmatch "repository/$tgz" >/dev/null 2>&1; then
    git -C "$ROOT" checkout -- "repository/$tgz"
    echo "   $tgz already published -- reusing the committed bytes"
  else
    tar -C "$REPO" -czf "$REPO/$tgz" "$@"
  fi
}
package_module "$MOD_TGZ" bin/NukeX-pxm.so bin/NukeX-pxm.xsgn
python3 - "$REPO/updates.xri" "$MOD_TGZ" <<'PY'
import re,sys
mf,fn=sys.argv[1:3]
s=open(mf).read()
s=re.sub(r'fileName="[^"]*-linux-x64-NukeX(-[0-9.]+)?\.tar\.gz"', 'fileName="'+fn+'"', s)
open(mf,'w').write(s)
PY

echo "== 3a/6 package PICopilot module tarball =="
cp "$PICOPILOT_SO" "$PICOPILOT_XSGN" "$REPO/bin/"
PICOPILOT_TGZ="$(module_tgz PICopilot "$PICOPILOT_VER")"
package_module "$PICOPILOT_TGZ" bin/PICopilot-pxm.so bin/PICopilot-pxm.xsgn
# Stale dated PICopilot tarballs (like stale dated NukeX tarballs) are not
# auto-deleted here -- they're pruned manually at commit time (see repository/
# git history, e.g. "Dropped stale repository/ artifacts ... 20260627 NukeX tarball").
python3 - "$REPO/updates.xri" "$PICOPILOT_TGZ" <<'PY'
import re,sys
mf,fn=sys.argv[1:3]
s=open(mf).read()
if re.search(r'fileName="[^"]*-linux-x64-PICopilot(-[0-9.]+)?\.tar\.gz"', s):
    # Entry already exists from a prior release -- just rename the dated
    # fileName, mirroring the NukeX rename above. Idempotent: re-running
    # with the same date is a no-op substitution.
    s=re.sub(r'fileName="[^"]*-linux-x64-PICopilot(-[0-9.]+)?\.tar\.gz"', 'fileName="'+fn+'"', s)
else:
    # First release: insert a new <package> entry into the linux/x64
    # platform block (the same block NukeX lives in). sha1/releaseDate
    # are placeholders -- step 4 (write_pkg) fills in the real values.
    entry = ('\n      <package fileName="' + fn + '" '
             'sha1="0000000000000000000000000000000000000000" '
             'type="module" releaseDate="00000000">\n'
             '         <title>PI Copilot</title>\n'
             '         <description>\n'
             '            <p>\n'
             '               <b>PI Copilot</b>\n'
             '            </p>\n'
             '            <p>PI Copilot — AI assistant panel for PixInsight '
             '(bring your own Anthropic API key).</p>\n'
             '         </description>\n'
             '      </package>\n')
    new_s,n=re.subn(
        r'(<platform os="linux" arch="x64"[^>]*>.*?)(\s*</platform>)',
        lambda m: m.group(1)+entry+m.group(2),
        s, count=1, flags=re.DOTALL)
    if n!=1:
        sys.exit("could not locate linux/x64 <platform> block to insert PICopilot package entry")
    s=new_s
open(mf,'w').write(s)
PY
[ -f "$REPO/$PICOPILOT_TGZ" ] || die "tarball $PICOPILOT_TGZ not produced"

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
  reuse_if_published "$zipname"
done

echo "== 3c/6 package gaia-depth-grade script zip =="
# Mirror PI's install layout (src/scripts/GaiaDepthGrade/) so the package extracts
# into PixInsight's scripts tree; bundle the 3 sources + their .xsgn.
GAIA_VER=1.0.12  # script package version; the frozen sidecar is independent (see *_lib.jsh pins)
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
reuse_if_published "$GAIA_ZIP"
# NOTE: the frozen sidecar binary is NOT packaged here — it lives on GitHub Releases
# (>100MB), pinned by SIDECAR_URL/SIDECAR_SHA256 in pi/gaia_depth_grade_lib.jsh.
# After bumping the sidecar, rebuild+upload it (gaia-depth-grade/tools/build-sidecar.sh)
# and update those pins; this script only ships the thin signed scripts.

echo "== 3e/6 publish the NukeX camera database (repository/qe_*) =="
for f in "${QE_FILES[@]}"; do cp "$QE_SRC/$f" "$REPO/$f"; done
qe_check "$REPO" || die "published QE set in repository/ does not verify"

echo "== 4/6 write fileName/sha1/releaseDate into ONE manifest =="
write_pkg "$REPO/updates.xri" "$MOD_TGZ"                  "$(sha1 "$REPO/$MOD_TGZ")"
write_pkg "$REPO/updates.xri" "$PICOPILOT_TGZ"            "$(sha1 "$REPO/$PICOPILOT_TGZ")"
write_pkg "$REPO/updates.xri" "EZStretch_v1.0.10.zip"     "$(sha1 "$REPO/EZStretch_v1.0.10.zip")"
write_pkg "$REPO/updates.xri" "EZDonutRepair_v1.0.3.zip"  "$(sha1 "$REPO/EZDonutRepair_v1.0.3.zip")"
write_pkg "$REPO/updates.xri" "EZHazeKill_v1.0.1.zip"     "$(sha1 "$REPO/EZHazeKill_v1.0.1.zip")"
write_pkg "$REPO/updates.xri" "$GAIA_ZIP"                 "$(sha1 "$REPO/$GAIA_ZIP")"

echo "== 5/6 sign manifest LAST =="
sed -i '/<Signature developerId=/d' "$REPO/updates.xri"
pi_headless "$PI" --sign-xml-file="$REPO/updates.xri" --xssk-file="$KEYS" --xssk-password="$PASS"
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

echo "== 6b/6 integrity check: camera-database publication, with the module's own verifier =="
qe_check "$REPO" || die "repository/ QE publication is stale, unsigned or does not match the embedded database"
# The C++ updater from THIS build, served exactly the files in repository/ at
# the URL the module fetches: signatures, digest, the full install path, and
# that a fresh install is not offered the database it already carries.
NUKEX_QE_PUBLISHED_DIR="$REPO" "$ROOT/modules/nukex/build-portable/test/test_qe_update" "[published]" \
  || die "the module's updater rejects the published camera database in repository/"

echo "RELEASE OK — repository/ ready to commit & push"
