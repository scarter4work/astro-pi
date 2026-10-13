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

# ── Module packages: bin/ + the module's documentation ──────────────────────
# A module tarball deploys bin/<Name>-pxm.{so,xsgn} and doc/tools/<Name>/.
# doc/tools is the deployment directory the PixInsight Update Repository
# Reference (section 4.2, Example 2) assigns to documentation of PCL modules,
# and the core's Browse Documentation opens doc/tools/<ProcessId>/<ProcessId>.html,
# so the folder and the page carry the process Id ("NukeX", "PICopilot").
# doc/docs is for documents "not related to a particular module or script".
# The source is modules/<m>/doc/<Name>/: <Name>.html (CPD rule 6.1 -- what the
# module does, its network connections, external programs and file locations)
# and THIRD_PARTY_NOTICES.txt (CPD rule 7.3 -- PCL License 2.0 conditions 2 and
# 4, and every linked component's notice).
NUKEX_DOC="$ROOT/modules/nukex/doc/NukeX"
PICOPILOT_DOC="$ROOT/modules/pi-copilot/doc/PICopilot"
PCL_ACK='This product is based on software from the PixInsight project, developed by Pleiades Astrophoto and its contributors (https://pixinsight.com/).'

modver(){ sed -nE "s/^#define $2_MODULE_VERSION_(MAJOR|MINOR|REVISION|BUILD) +([0-9]+).*/\\2/p" "$1" | paste -sd. -; }

module_doc_check(){ # <doc source dir> <Name>
  local d="$1" n="$2"
  [ -f "$d/$n.html" ] || die "$d/$n.html missing: every module package ships its user documentation (CPD rule 6.1)"
  [ -f "$d/THIRD_PARTY_NOTICES.txt" ] || die "$d/THIRD_PARTY_NOTICES.txt missing (CPD rule 7.3)"
  tr -s '[:space:]' ' ' < "$d/THIRD_PARTY_NOTICES.txt" | grep -qF "$PCL_ACK" \
    || die "$d/THIRD_PARTY_NOTICES.txt lacks the PCL License 2.0 condition-4 acknowledgment"
  grep -qF 'PixInsight Class Library License' "$d/THIRD_PARTY_NOTICES.txt" \
    && grep -qF 'Redistributions in binary form must reproduce the above copyright notice' "$d/THIRD_PARTY_NOTICES.txt" \
    || die "$d/THIRD_PARTY_NOTICES.txt lacks the full PCL licence text (PCL License 2.0 condition 2)"
}

build_module_tgz(){ # <out.tar.gz> <Name> <module .so> <module .xsgn> <doc source dir>
  local out="$1" n="$2" so="$3" xsgn="$4" doc="$5" st
  [ "$(basename "$so")" = "$n-pxm.so" ] && [ "$(basename "$xsgn")" = "$n-pxm.xsgn" ] \
    || die "build_module_tgz $n: expected $n-pxm.so + $n-pxm.xsgn, got $so + $xsgn"
  module_doc_check "$doc" "$n"
  st="$(mktemp -d)"
  mkdir -p "$st/bin" "$st/doc/tools/$n"
  cp "$so" "$xsgn" "$st/bin/"
  cp -R "$doc/." "$st/doc/tools/$n/"
  # World-readable whatever the build's umask: the updater installs as root
  # while PixInsight runs as the user (the published PICopilot 0.2.1.1
  # tarball carries a 0700 .so and a 0600 .xsgn).
  tar -C "$st" --sort=name --mode='u+rwX,go+rX,go-w' -czf "$out" bin doc
  rm -rf "$st"
}

module_pkg_check(){ # <tgz> <Name> <doc source dir> : deploys exactly bin/<Name>-pxm.{so,xsgn} + doc/tools/<Name>/ (Repository Reference 4.2), docs == source
  python3 - "$1" "$2" "$3" <<'PY'
import os,sys,tarfile
tgz,n,doc=sys.argv[1:4]
want={}
for r,_,fs in os.walk(doc):
    for f in fs:
        p=os.path.join(r,f)
        want["doc/tools/%s/%s"%(n,os.path.relpath(p,doc))]=open(p,'rb').read()
need={"bin/%s-pxm.so"%n,"bin/%s-pxm.xsgn"%n}|set(want)
dirs={"bin","doc","doc/tools","doc/tools/"+n}
errs=[]
with tarfile.open(tgz) as t:
    files={}
    for m in t.getmembers():
        name=m.name[2:] if m.name.startswith("./") else m.name
        if m.isdir():
            if name.rstrip("/") not in dirs and not name.startswith("doc/tools/%s/"%n): errs.append("unexpected directory "+name)
        elif m.isfile():
            files[name]=m
            if m.mode & 0o044 != 0o044: errs.append("%s is not world-readable (mode %o)"%(name,m.mode))
        else: errs.append("not a regular file or directory: "+name)
    for f in sorted(set(files)-need): errs.append("deploys %s, outside bin/%s-pxm.* and doc/tools/%s/"%(f,n,n))
    for f in sorted(need-set(files)): errs.append("missing "+f)
    for f in sorted(set(want)&set(files)):
        if t.extractfile(files[f]).read()!=want[f]: errs.append("%s differs from %s"%(f,os.path.join(doc,f.split("/",3)[3])))
if errs:
    print("  "+os.path.basename(tgz)+":"); [print("    "+e) for e in errs]; sys.exit(1)
print("  OK  %s: bin/%s-pxm.{so,xsgn} + %d doc file(s) in doc/tools/%s/, all world-readable, docs match source"%(os.path.basename(tgz),n,len(want),n))
PY
}

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

# A published (committed) module tarball is reused byte for byte, never rebuilt
# (see package_module). That is only correct while it already deploys exactly
# the current documentation; otherwise the version must be bumped.
published_pkg_reusable(){ # <tarball> <Name> <doc source dir>; 0 = new package, or published and up to date
  git -C "$ROOT" ls-files --error-unmatch "repository/$1" >/dev/null 2>&1 || { echo "  new package $1"; return 0; }
  local tmp rc=0; tmp="$(mktemp -d)"
  git -C "$ROOT" show ":repository/$1" > "$tmp/$1"
  module_pkg_check "$tmp/$1" "$2" "$3" || rc=1
  rm -rf "$tmp"
  return $rc
}

# Packaging self-test: the same build_module_tgz + module_pkg_check the release
# uses, on any .so/.xsgn pair (e.g. a container build and a stand-in .xsgn),
# into a scratch directory, then the reuse verdict step 0b would give for the
# current module versions. No build, no signing, no manifest, repository/ untouched.
#   ./release.sh --package-test <outdir> <NukeX-pxm.so> <NukeX-pxm.xsgn> <PICopilot-pxm.so> <PICopilot-pxm.xsgn>
if [ "${1:-}" = "--package-test" ]; then
  [ $# -eq 6 ] || die "usage: $0 --package-test <outdir> <NukeX-pxm.so> <NukeX-pxm.xsgn> <PICopilot-pxm.so> <PICopilot-pxm.xsgn>"
  OUT="$2"; mkdir -p "$OUT"
  case "$(cd "$OUT" && pwd)/" in "$REPO/"*) die "--package-test must not write into repository/";; esac
  NV="$(modver "$ROOT/modules/nukex/src/module/NukeXVersion.h" NUKEX)"
  PV="$(modver "$ROOT/modules/pi-copilot/src/module/PICopilotVersion.h" PICOPILOT)"
  NT="$OUT/$DATE-linux-x64-NukeX-$NV.tar.gz"
  PT="$OUT/$DATE-linux-x64-PICopilot-$PV.tar.gz"
  build_module_tgz "$NT" NukeX "$3" "$4" "$NUKEX_DOC"
  module_pkg_check "$NT" NukeX "$NUKEX_DOC" || die "NukeX package check failed"
  build_module_tgz "$PT" PICopilot "$5" "$6" "$PICOPILOT_DOC"
  module_pkg_check "$PT" PICopilot "$PICOPILOT_DOC" || die "PICopilot package check failed"
  for t in "$NT" "$PT"; do echo "== $(basename "$t")"; tar -tzvf "$t"; done
  echo "== release step 0b verdict for the current versions"
  published_pkg_reusable "$(module_tgz NukeX "$NV")" NukeX "$NUKEX_DOC" \
    || echo "  release.sh would STOP: NukeX $NV is published without the current docs -- bump its version"
  published_pkg_reusable "$(module_tgz PICopilot "$PV")" PICopilot "$PICOPILOT_DOC" \
    || echo "  release.sh would STOP: PICopilot $PV is published without the current docs -- bump its version"
  exit 0
fi

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

echo "== 0b/6 module package names and documentation (before the long build) =="
# The version goes in the tarball name (ported from nukex5 3868a85). Several
# releases on one day otherwise share "<date>-linux-x64-<Name>.tar.gz":
# raw.githubusercontent.com keeps serving the previous bytes for that path for
# minutes after a push while the signed manifest already names the new sha1,
# and PixInsight's updater records installed packages by fileName
# (/opt/PixInsight/etc/update/installed.xri). A name that changes with every
# release can be neither stale nor mistaken for the installed one.
NUKEX_VER="$(modver "$ROOT/modules/nukex/src/module/NukeXVersion.h" NUKEX)"
PICOPILOT_VER="$(modver "$ROOT/modules/pi-copilot/src/module/PICopilotVersion.h" PICOPILOT)"
[[ "$NUKEX_VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "could not read NukeX version (got '$NUKEX_VER')"
[[ "$PICOPILOT_VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "could not read PICopilot version (got '$PICOPILOT_VER')"
MOD_TGZ="$(module_tgz NukeX "$NUKEX_VER")"
PICOPILOT_TGZ="$(module_tgz PICopilot "$PICOPILOT_VER")"
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
package_module(){ # <tarball> <Name> <module .so> <module .xsgn> <doc source dir>
  local tgz="$1"
  if git -C "$ROOT" ls-files --error-unmatch "repository/$tgz" >/dev/null 2>&1; then
    git -C "$ROOT" checkout -- "repository/$tgz"
    echo "   $tgz already published -- reusing the committed bytes"
  else
    build_module_tgz "$REPO/$tgz" "$2" "$3" "$4" "$5"
  fi
  module_pkg_check "$REPO/$tgz" "$2" "$5" || die "$tgz does not deploy exactly bin/$2-pxm.{so,xsgn} + the current doc/tools/$2/"
}
# Fail now, not after the build: the docs each package will ship must be
# complete, and an already-published version can only be reused if its
# tarball already carries exactly the current docs (else: bump the version).
module_doc_check "$NUKEX_DOC" NukeX
module_doc_check "$PICOPILOT_DOC" PICopilot
published_pkg_reusable "$MOD_TGZ" NukeX "$NUKEX_DOC" \
  || die "NukeX $NUKEX_VER is already published as $MOD_TGZ without the current doc/tools/NukeX/ -- bump NukeX's version so a new package is built"
published_pkg_reusable "$PICOPILOT_TGZ" PICopilot "$PICOPILOT_DOC" \
  || die "PICopilot $PICOPILOT_VER is already published as $PICOPILOT_TGZ without the current doc/tools/PICopilot/ -- bump PICopilot's version so a new package is built"

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
package_module "$MOD_TGZ" NukeX "$SO" "$XSGN" "$NUKEX_DOC"
python3 - "$REPO/updates.xri" "$MOD_TGZ" <<'PY'
import re,sys
mf,fn=sys.argv[1:3]
s=open(mf).read()
s=re.sub(r'fileName="[^"]*-linux-x64-NukeX(-[0-9.]+)?\.tar\.gz"', 'fileName="'+fn+'"', s)
open(mf,'w').write(s)
PY

echo "== 3a/6 package PICopilot module tarball =="
package_module "$PICOPILOT_TGZ" PICopilot "$PICOPILOT_SO" "$PICOPILOT_XSGN" "$PICOPILOT_DOC"
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
declare -A EZVER=( [EZStretch]=1.0.11 [EZDonutRepair]=1.0.4 [EZHazeKill]=1.0.2 )
EZSRC="$ROOT/scripts/ez-stretch/src/scripts/EZ Stretch BSC"
for name in EZStretch EZDonutRepair EZHazeKill; do
  ver="${EZVER[$name]}"
  zipname="${name}_v${ver}.zip"
  # Stage into src/scripts/scarter4work/ so the menu and the on-disk folder both
  # read scarter4work (the #feature-id in each .js already points there).
  stage="$(mktemp -d)"; mkdir -p "$stage/src/scripts/scarter4work"
  cp "$EZSRC/$name.js" "$EZSRC/$name.xsgn" "$stage/src/scripts/scarter4work/"
  # World-readable whatever the build's umask (mktemp -d is 0700): the updater
  # keeps archive modes and installs as root, so 0600/0700 entries would leave
  # the scripts unreadable to the user. (EZ <= 1.0.10/1.0.3/1.0.1 shipped 0600.)
  chmod -R u=rwX,go=rX "$stage"
  rm -f "$REPO/$zipname"
  ( cd "$stage" && zip -qrX "$REPO/$zipname" src )
  rm -rf "$stage"
  [ -f "$REPO/$zipname" ] || die "zip $zipname not produced"
  reuse_if_published "$zipname"
done

echo "== 3c/6 package gaia-depth-grade script zip =="
# Mirror PI's install layout: the 3 sources + their .xsgn in src/scripts/GaiaDepthGrade/,
# the user documentation in doc/scripts/GaiaDepthGrade/ (Repository Reference 4.2:
# doc/scripts is for script documentation; the folder and page carry the
# #feature-id "GaiaDepthGrade"). Modes are normalised world-readable: the updater
# installs as root while PixInsight runs as the user.
GAIA_VER=1.0.13  # script package version; the sidecar package (3d) has its own version
GAIA_ZIP="gaia-depth-grade_v${GAIA_VER}.zip"
GAIA_STAGE="$(mktemp -d)"
mkdir -p "$GAIA_STAGE/src/scripts/GaiaDepthGrade" "$GAIA_STAGE/doc/scripts/GaiaDepthGrade"
cp "$ROOT/gaia-depth-grade/pi/gaia_depth_grade.js"        "$ROOT/gaia-depth-grade/pi/gaia_depth_grade.xsgn" \
   "$ROOT/gaia-depth-grade/pi/GaiaDepthGradeDialog.js"    "$ROOT/gaia-depth-grade/pi/GaiaDepthGradeDialog.xsgn" \
   "$ROOT/gaia-depth-grade/pi/gaia_depth_grade_lib.jsh"   "$ROOT/gaia-depth-grade/pi/gaia_depth_grade_lib.xsgn" \
   "$GAIA_STAGE/src/scripts/GaiaDepthGrade/"
cp "$ROOT/gaia-depth-grade/doc/GaiaDepthGrade/GaiaDepthGrade.html" "$GAIA_STAGE/doc/scripts/GaiaDepthGrade/"
chmod -R u=rwX,go=rX "$GAIA_STAGE"
rm -f "$REPO/$GAIA_ZIP"
( cd "$GAIA_STAGE" && zip -qrX "$REPO/$GAIA_ZIP" src doc )
rm -rf "$GAIA_STAGE"
[ -f "$REPO/$GAIA_ZIP" ] || die "zip $GAIA_ZIP not produced"
reuse_if_published "$GAIA_ZIP"

echo "== 3d/6 package the gaia-depth-grade sidecar (linux-x64: bin/gaia-depth-grade-sidecar) =="
# The scripts run the sidecar only from <PixInsight>/bin, where this package puts
# it; they never download or install it. A published version (a committed
# <date>-linux-x64-GaiaDepthGradeSidecar-<ver>.tar.gz) is reused byte for byte,
# exactly like the module tarballs: bump gaia_depth_grade.__version__ to ship
# new bytes. Building one takes the portable Rocky 9 image and network access
# (pinned, hash-checked inputs); see gaia-depth-grade/tools/build-sidecar.sh.
SIDECAR_VER="$(sed -nE 's/^__version__ = "([0-9.]+)"$/\1/p' "$ROOT/gaia-depth-grade/src/gaia_depth_grade/__init__.py")"
[[ "$SIDECAR_VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "could not read the sidecar version (got '$SIDECAR_VER')"
grep -qE "^#define SIDECAR_VERSION +\"$SIDECAR_VER\"" "$ROOT/gaia-depth-grade/pi/gaia_depth_grade_lib.jsh" \
  || die "gaia_depth_grade_lib.jsh SIDECAR_VERSION is not $SIDECAR_VER: the scripts would refuse the sidecar this release ships"
SIDECAR_TGZ="$(git -C "$ROOT" ls-files "repository/*-linux-x64-GaiaDepthGradeSidecar-$SIDECAR_VER.tar.gz" | sed 's#^repository/##')"
[ "$(printf '%s\n' "$SIDECAR_TGZ" | grep -c .)" -le 1 ] || die "several published sidecar packages for $SIDECAR_VER: $SIDECAR_TGZ"
if [ -n "$SIDECAR_TGZ" ]; then
  git -C "$ROOT" checkout -- "repository/$SIDECAR_TGZ"
  echo "   $SIDECAR_TGZ already published -- reusing the committed bytes"
else
  SIDECAR_TGZ="$DATE-linux-x64-GaiaDepthGradeSidecar-$SIDECAR_VER.tar.gz"
  "$ROOT/gaia-depth-grade/tools/build-sidecar.sh" "$REPO/$SIDECAR_TGZ" || die "sidecar build/smoke test failed"
fi
[ "$(stat -c%s "$REPO/$SIDECAR_TGZ")" -lt 95000000 ] || die "$SIDECAR_TGZ is too close to GitHub's 100 MB per-file limit"
python3 - "$REPO/updates.xri" "$SIDECAR_TGZ" <<'PY' || die "updates.xri has no GaiaDepthGradeSidecar package entry to point at $SIDECAR_TGZ"
import re,sys
mf,fn=sys.argv[1:3]
s=open(mf).read()
s,n=re.subn(r'fileName="[^"]*-linux-x64-GaiaDepthGradeSidecar-[0-9.]+\.tar\.gz"', 'fileName="'+fn+'"', s)
if n!=1: sys.exit(1)
open(mf,'w').write(s)
PY

echo "== 3e/6 publish the NukeX camera database (repository/qe_*) =="
for f in "${QE_FILES[@]}"; do cp "$QE_SRC/$f" "$REPO/$f"; done
qe_check "$REPO" || die "published QE set in repository/ does not verify"

echo "== 4/6 write fileName/sha1/releaseDate into ONE manifest =="
write_pkg "$REPO/updates.xri" "$MOD_TGZ"                  "$(sha1 "$REPO/$MOD_TGZ")"
write_pkg "$REPO/updates.xri" "$PICOPILOT_TGZ"            "$(sha1 "$REPO/$PICOPILOT_TGZ")"
for name in EZStretch EZDonutRepair EZHazeKill; do
  zipname="${name}_v${EZVER[$name]}.zip"
  write_pkg "$REPO/updates.xri" "$zipname" "$(sha1 "$REPO/$zipname")"
done
write_pkg "$REPO/updates.xri" "$GAIA_ZIP"                 "$(sha1 "$REPO/$GAIA_ZIP")"
write_pkg "$REPO/updates.xri" "$SIDECAR_TGZ"              "$(sha1 "$REPO/$SIDECAR_TGZ")"

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
