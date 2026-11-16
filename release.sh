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
# A module tarball deploys bin/<Name>-pxm.{so,xsgn} and doc/tools/<DocId>/ for
# each of its processes.
# doc/tools is the deployment directory the PixInsight Update Repository
# Reference (section 4.2, Example 2) assigns to documentation of PCL modules,
# and the core's Browse Documentation opens doc/tools/<ProcessId>/<ProcessId>.html,
# so the folder and the page carry the process Id: <DocId> is the module name
# where the two agree ("NukeX"); module Planetary has two processes,
# "SERViewer" and "PlanetaryStack", so it ships two doc folders.
# doc/docs is for documents "not related to a particular module or script".
# The source of each is modules/<m>/doc/<DocId>/: <DocId>.html (CPD rule 6.1 --
# what the process does, its network connections, external programs and file
# locations) and THIRD_PARTY_NOTICES.txt (CPD rule 7.3 -- PCL License 2.0
# conditions 2 and 4, and every linked component's notice). Every folder under
# modules/<m>/doc/ must be one of the module's doc ids, so none is left unshipped.
NUKEX_DOC="$ROOT/modules/nukex/doc"
NUKEX_DOC_IDS=(NukeX)
PLANETARY_DOC="$ROOT/modules/planetary/doc"
PLANETARY_DOC_IDS=(SERViewer PlanetaryStack)
PCL_ACK='This product is based on software from the PixInsight project, developed by Pleiades Astrophoto and its contributors (https://pixinsight.com/).'

modver(){ sed -nE "s/^#define $2_MODULE_VERSION_(MAJOR|MINOR|REVISION|BUILD) +([0-9]+).*/\\2/p" "$1" | paste -sd. -; }

module_doc_check(){ # <doc root> <DocId>... : each <root>/<DocId>/ complete, and no other folder under <root>
  local root="$1" n d; shift
  [ $# -ge 1 ] || die "module_doc_check $root: no doc id"
  for n in "$@"; do
    d="$root/$n"
    [ -f "$d/$n.html" ] || die "$d/$n.html missing: every module package ships its user documentation (CPD rule 6.1)"
    [ -f "$d/THIRD_PARTY_NOTICES.txt" ] || die "$d/THIRD_PARTY_NOTICES.txt missing (CPD rule 7.3)"
    tr -s '[:space:]' ' ' < "$d/THIRD_PARTY_NOTICES.txt" | grep -qF "$PCL_ACK" \
      || die "$d/THIRD_PARTY_NOTICES.txt lacks the PCL License 2.0 condition-4 acknowledgment"
    grep -qF 'PixInsight Class Library License' "$d/THIRD_PARTY_NOTICES.txt" \
      && grep -qF 'Redistributions in binary form must reproduce the above copyright notice' "$d/THIRD_PARTY_NOTICES.txt" \
      || die "$d/THIRD_PARTY_NOTICES.txt lacks the full PCL licence text (PCL License 2.0 condition 2)"
  done
  for d in "$root"/*; do
    n="$(basename "$d")"
    printf '%s\n' "$@" | grep -qxF "$n" || die "$d is not one of the doc ids this package ships ($*): it would never be installed"
  done
  # One module, one set of notices: every doc folder carries the same file, so the copies cannot drift.
  for n in "${@:2}"; do
    cmp -s "$root/$1/THIRD_PARTY_NOTICES.txt" "$root/$n/THIRD_PARTY_NOTICES.txt" \
      || die "$root/$n/THIRD_PARTY_NOTICES.txt differs from $root/$1/THIRD_PARTY_NOTICES.txt: one module ships one set of notices"
  done
}

build_module_tgz(){ # <out.tar.gz> <Name> <module .so> <module .xsgn> <doc root> <DocId>...
  local out="$1" n="$2" so="$3" xsgn="$4" doc="$5" id st; shift 5
  [ "$(basename "$so")" = "$n-pxm.so" ] && [ "$(basename "$xsgn")" = "$n-pxm.xsgn" ] \
    || die "build_module_tgz $n: expected $n-pxm.so + $n-pxm.xsgn, got $so + $xsgn"
  module_doc_check "$doc" "$@"
  st="$(mktemp -d)"
  mkdir -p "$st/bin"
  cp "$so" "$xsgn" "$st/bin/"
  for id in "$@"; do
    mkdir -p "$st/doc/tools/$id"
    cp -R "$doc/$id/." "$st/doc/tools/$id/"
  done
  # World-readable whatever the build's umask: the updater installs as root
  # while PixInsight runs as the user (the published PICopilot 0.2.1.1
  # tarball carries a 0700 .so and a 0600 .xsgn).
  tar -C "$st" --sort=name --mode='u+rwX,go+rX,go-w' -czf "$out" bin doc
  rm -rf "$st"
}

module_pkg_check(){ # <tgz> <Name> <doc root> <DocId>... : deploys exactly bin/<Name>-pxm.{so,xsgn} + each doc/tools/<DocId>/ (Repository Reference 4.2), docs == source
  python3 - "$@" <<'PY'
import os,sys,tarfile
tgz,n,root=sys.argv[1:4]; ids=sys.argv[4:]
want={}
for i in ids:
    doc=os.path.join(root,i)
    for r,_,fs in os.walk(doc):
        for f in fs:
            p=os.path.join(r,f)
            want["doc/tools/%s/%s"%(i,os.path.relpath(p,doc))]=(open(p,'rb').read(),p)
need={"bin/%s-pxm.so"%n,"bin/%s-pxm.xsgn"%n}|set(want)
dirs={"bin","doc","doc/tools"}|{"doc/tools/"+i for i in ids}
errs=[]
with tarfile.open(tgz) as t:
    files={}
    for m in t.getmembers():
        name=m.name[2:] if m.name.startswith("./") else m.name
        if m.isdir():
            if name.rstrip("/") not in dirs and not any(name.startswith("doc/tools/%s/"%i) for i in ids): errs.append("unexpected directory "+name)
        elif m.isfile():
            files[name]=m
            if m.mode & 0o044 != 0o044: errs.append("%s is not world-readable (mode %o)"%(name,m.mode))
        else: errs.append("not a regular file or directory: "+name)
    outside=" and ".join("doc/tools/%s/"%i for i in ids)
    for f in sorted(set(files)-need): errs.append("deploys %s, outside bin/%s-pxm.* and %s"%(f,n,outside))
    for f in sorted(need-set(files)): errs.append("missing "+f)
    for f in sorted(set(want)&set(files)):
        if t.extractfile(files[f]).read()!=want[f][0]: errs.append("%s differs from %s"%(f,want[f][1]))
if errs:
    print("  "+os.path.basename(tgz)+":"); [print("    "+e) for e in errs]; sys.exit(1)
print("  OK  %s: bin/%s-pxm.{so,xsgn} + %d doc file(s) in %s, all world-readable, docs match source"%(os.path.basename(tgz),n,len(want),", ".join("doc/tools/%s/"%i for i in ids)))
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
published_pkg_reusable(){ # <tarball> <Name> <doc root> <DocId>...; 0 = new package, or published and up to date
  git -C "$ROOT" ls-files --error-unmatch "repository/$1" >/dev/null 2>&1 || { echo "  new package $1"; return 0; }
  local tgz="$1" tmp rc=0; shift; tmp="$(mktemp -d)"
  git -C "$ROOT" show ":repository/$tgz" > "$tmp/$tgz"
  module_pkg_check "$tmp/$tgz" "$@" || rc=1
  rm -rf "$tmp"
  return $rc
}

# ── Package descriptions in updates.xri ─────────────────────────────────────
# Every package's <description> is rewritten from its product's source on every
# release (CPD rule 6.1: a manifest description that outlives its release reads
# as documentation of a version that no longer ships; NukeX 5.2.0.1 was served
# with 5.1.0.3's text). A source is the inner XML of <description>, as served;
# the repository's own <description> comes from tools/repository-description.xml.
# A description names no version but its package's own: any dotted number of
# three or more parts, or v<n>.<n>..., must be that version (two-part numbers --
# glibc 2.34, Ubuntu 22.04 -- are not package versions). So a version bump
# fails the release until the description has been brought up to date.
EZ_NAMES=(EZStretch EZHazeKill)
# EZ script documentation (CPD rule 6.1; Repository Reference 4.2): scripts/ez-stretch/doc/<Name>/ holds
# <Name>.html (and anything it links), shipped as doc/scripts/<Name>/. Every EZ package must have it, and
# <Name>.js must declare "#feature-id <Name> : ...": PixInsight finds a script's documentation by that id
# (doc/scripts/<id>/<id>.html, as for its own scripts, e.g. ImageSolver).
EZDOC="$ROOT/scripts/ez-stretch/doc"
EZSRC="$ROOT/scripts/ez-stretch/src/scripts/EZ Stretch BSC"
# Every product's version is read from its source of truth (CLAUDE.md rule 2), never written here: a
# version held in this script has to be bumped by hand beside the real one (EZHazeKill 1.1.1 was).
#   srcver <file> <sed -E expression printing the version> <product> : exactly one dotted version, or die.
srcver(){
  local v; v="$(sed -nE "$2" "$1" 2>/dev/null)" || true
  [ "$(printf '%s\n' "$v" | grep -c .)" -eq 1 ] && [[ "$v" =~ ^[0-9]+(\.[0-9]+){2,3}$ ]] \
    || die "could not read $3's version from $1 (got '${v//$'\n'/ }')"
  echo "$v"
}
declare -A EZVER=()
for name in "${EZ_NAMES[@]}"; do EZVER[$name]="$(srcver "$EZSRC/$name.js" 's/^#define VERSION +"([^"]*)".*/\1/p' "$name")"; done
# Packages withdrawn from distribution (fileName regex, each matches at most one manifest entry). The
# first release after a withdrawal drops the entry from the work copy (and a <platform> block it leaves
# empty); once the new manifest is installed (step 6c), prune_repository deletes the package files it
# no longer names from repository/. Withdrawn, source in a private repo:
#   EZDonutRepair 2026-11-10 (scarter4work/ez-donut-repair);
#   PI Copilot 2026-11-16 (scarter4work/pi-copilot);
#   Gaia Depth Grade scripts and sidecar 2026-11-16 (scarter4work/gaia-depth-grade-pi).
RETIRED_PKG_RE=(
  'EZDonutRepair_v[0-9.]+\.zip'
  '[0-9]{8}-linux-x64-PICopilot(-[0-9.]+)?\.tar\.gz'
  'gaia-depth-grade_v[0-9.]+\.zip'
  '[0-9]{8}-linux-x64-GaiaDepthGradeSidecar-[0-9.]+\.tar\.gz'
)
# Installed files a package deletes when the updater installs it (Repository Reference 3, <remove>: files
# and directories relative to the PixInsight installation, removed completely before the package is
# installed; missing ones are ignored, with a warning in the updater's log). A package leaving the
# manifest uninstalls nothing, so a withdrawn or reorganised product's leftovers are removed through a
# package that stays: one entry per carrying package, '<fileName regex>=<item>[,<item>...]'. Step 3g
# writes exactly these <remove> elements into the manifest, and no others (manifest_removes). Each regex
# must match exactly one package of this release, each item must lie under bin/, src/scripts/,
# doc/scripts/ or doc/tools/, and no item may be or contain a file that another package of this release
# installs. The carrying package must be bumped in the same release (a reused package is not
# reinstalled), and its documentation must say what it removes.
#   EZStretch 1.0.13 (noarch, PixInsight 1.8.0-2.0.0: it reaches every installation the withdrawn
#   packages did):
#   - EZDonutRepair (withdrawn 2026-11-10; 1.0.3 and 1.0.4 installed exactly these two files);
#   - PI Copilot (withdrawn 2026-11-16; every package 20260923..0.2.2.3 installed bin/PICopilot-pxm.so
#     and .xsgn, and doc/tools/PICopilot/);
#   - Gaia Depth Grade (withdrawn 2026-11-16; scripts 1.0.0-1.0.13 installed src/scripts/GaiaDepthGrade/
#     and doc/scripts/GaiaDepthGrade/, sidecar 1.0.8 bin/gaia-depth-grade-sidecar and a notices file in
#     doc/scripts/GaiaDepthGrade/).
PKG_REMOVE=(
  'EZStretch_v[0-9.]+\.zip=src/scripts/scarter4work/EZDonutRepair.js,src/scripts/scarter4work/EZDonutRepair.xsgn,bin/PICopilot-pxm.so,bin/PICopilot-pxm.xsgn,doc/tools/PICopilot,src/scripts/GaiaDepthGrade,doc/scripts/GaiaDepthGrade,bin/gaia-depth-grade-sidecar'
)
REPO_DESC="$ROOT/tools/repository-description.xml"
declare -A PKG_DESC=(
  [NukeX]="$ROOT/modules/nukex/package-description.xml"
  [Planetary]="$ROOT/modules/planetary/package-description.xml"
  [EZStretch]="$ROOT/scripts/ez-stretch/package-description/EZStretch.xml"
  [EZHazeKill]="$ROOT/scripts/ez-stretch/package-description/EZHazeKill.xml"
)
# prune_repository <installed manifest> <repository dir> : delete every package file (*.zip, *.tar.gz) in
# the directory that the manifest does not name -- the version a bump supersedes, a withdrawn product --
# with git rm when it is tracked. A signed package stays downloadable for as long as it sits in
# repository/, served or not (CPD hygiene, 2026-11-16: 10 superseded packages had accumulated). Only
# package files go: updates.xri and the QE publication (qe_*) stay. Refuses, deleting nothing, when the
# manifest names no package or a package that is not on disk.
prune_repository(){
  local mf="$1" dir="$2" f b
  local -A keep=()
  while IFS= read -r b; do [ -n "$b" ] && keep[$b]=1; done < <(sed -nE 's/.*<package fileName="([^"]+)".*/\1/p' "$mf")
  [ "${#keep[@]}" -gt 0 ] || die "prune_repository: $mf names no package"
  for b in "${!keep[@]}"; do [ -f "$dir/$b" ] || die "prune_repository: $mf names $b, which is not in $dir"; done
  for f in "$dir"/*.zip "$dir"/*.tar.gz; do
    [ -e "$f" ] || continue
    b="$(basename "$f")"; [ -n "${keep[$b]:-}" ] && continue
    echo "  removing $b (not in the manifest)"
    if git -C "$dir" ls-files --error-unmatch -- "$b" >/dev/null 2>&1; then
      git -C "$dir" rm -q -f -- "$b" || die "prune_repository: git rm $b failed"
    else rm -f -- "$f" || die "prune_repository: rm $b failed"; fi
  done
}
# The packages' fileNames in a manifest, in order.
manifest_files(){ sed -nE 's/.*<package fileName="([^"]+)".*/\1/p' "$1"; }
# <fileName>=<source> for each fileName given (dies on one without a source).
desc_args(){
  local fn key
  for fn in "$@"; do
    case "$fn" in
      *-linux-x64-*.tar.gz) key="${fn#*-linux-x64-}"; key="${key%%-*}" ;;
      *_v*.zip) key="${fn%%_v*}" ;;
      *) key= ;;
    esac
    [ -n "$key" ] && [ -n "${PKG_DESC[$key]:-}" ] || die "no description source for package $fn"
    echo "$fn=${PKG_DESC[$key]}"
  done
}
# descriptions <check|apply|verify> <manifest> <fileName>=<source>...
#   check : every source is well-formed and names no version but its fileName's (manifest unused)
#   apply : check, then rewrite the manifest's <description> of each fileName (exactly one entry
#           each) and the repository's own; every package in the manifest must be listed
#   verify: the manifest's descriptions are exactly the sources (after a later step, e.g. signing)
descriptions(){
  python3 - "$REPO_DESC" "$@" <<'PY'
import re,sys
import xml.etree.ElementTree as ET
repo_desc,mode,mf=sys.argv[1:4]; pairs=[a.split("=",1) for a in sys.argv[4:]]
def version(fn):
    m=re.search(r'(?:-|_v)(\d+(?:\.\d+)+)\.(?:tar\.gz|zip)$',fn)
    if not m: sys.exit("descriptions: no version in package fileName "+fn)
    return m.group(1)
def body(src):
    t=open(src,encoding="utf-8").read().strip("\n")
    try: ET.fromstring("<description>"+t+"</description>")
    except ET.ParseError as e: sys.exit("descriptions: %s is not well-formed XML: %s"%(src,e))
    return t
def rendered(t,ind=12):  # as served: a package's description is indented 9, its lines 12; the repository's 3 and 6
    return "<description>\n"+"\n".join(" "*ind+l if l else l for l in t.split("\n"))+"\n"+" "*(ind-3)+"</description>"
errs=[]
srcs={}
for fn,src in pairs:
    t=body(src); v=version(fn); srcs[fn]=t; own=False
    for m in re.finditer(r'(?<![A-Za-z0-9.])(v?)(\d+(?:\.\d+)+)',t):
        num=m.group(2)
        if t[:m.start()].endswith(("PixInsight ","PCL ")): continue  # a PixInsight/PCL platform version, not the package's
        if num==v: own=True
        elif num.count(".")>=2 or m.group(1):
            errs.append("%s names version %s%s, but the package (%s) is %s"%(src,m.group(1),num,fn,v))
    # The package's version comes from its product's source (release.sh, srcver/modver); a description
    # that does not name it cannot be checked against it.
    if not own: errs.append("%s does not name its package's version %s (%s)"%(src,v,fn))
if errs: [print("  "+e) for e in errs]; sys.exit(1)
if mode=="check":
    print("  OK  %d package descriptions are well-formed and name only their own package's version"%len(pairs)); sys.exit(0)
s=open(mf,encoding="utf-8").read()
pkgs=re.findall(r'<package fileName="([^"]+)"',s)
for fn in pkgs:
    if fn not in srcs: errs.append("updates.xri package %s has no description source"%fn)
for fn in srcs:
    if pkgs.count(fn)!=1: errs.append("updates.xri has %d package entries for %s"%(pkgs.count(fn),fn))
if errs: [print("  "+e) for e in errs]; sys.exit(1)
def pkg_span(fn):
    m=re.search(r'<package fileName="'+re.escape(fn)+r'"[^>]*>.*?</package>',s,re.S)
    return m
def top_span():
    head=s[:s.index("<platform")]
    m=re.search(r'<description>.*?</description>',head,re.S)
    if not m: sys.exit("descriptions: updates.xri has no repository <description> before its first <platform>")
    return m
top=body(repo_desc)
if mode=="apply":
    for fn,t in srcs.items():
        m=pkg_span(fn); pk=m.group(0)
        if len(re.findall(r'<description>',pk))!=1: sys.exit("descriptions: package %s has not exactly one <description>"%fn)
        pk=re.sub(r'<description>.*?</description>',lambda _:rendered(t),pk,flags=re.S)
        s=s[:m.start()]+pk+s[m.end():]
    m=top_span(); s=s[:m.start()]+rendered(top,6)+s[m.end():]
    open(mf,"w",encoding="utf-8").write(s)
    print("  OK  wrote %d package descriptions and the repository description into %s"%(len(srcs),mf)); sys.exit(0)
for fn,t in srcs.items():
    d=re.search(r'<description>.*?</description>',pkg_span(fn).group(0),re.S).group(0)
    if d!=rendered(t): errs.append("updates.xri description of %s is not its source"%fn)
if top_span().group(0)!=rendered(top,6): errs.append("updates.xri repository description is not tools/repository-description.xml")
if errs: [print("  "+e) for e in errs]; sys.exit(1)
print("  OK  every description in %s is its source and names only its package's version"%mf)
PY
}

# retire_manifest <manifest> <fileName-regex>... : drop the package entry matching each regex (at most one
# each), then every <platform> block left with no package (an empty block is noise in the manifest; the
# Gaia sidecar was the only package of its linux/x64 1.8.0:2.0.0 block).
retire_manifest(){
  python3 - "$@" <<'PY' || die "retire_manifest failed"
import re,sys
mf=sys.argv[1]; s=open(mf).read()
for pat in sys.argv[2:]:
    rx=re.compile(r'[ \t]*<package fileName="'+pat+r'"[^>]*>.*?</package>[ \t]*\n',re.S)
    s,n=rx.subn('',s)
    if n>1: sys.exit("%s matches %d manifest entries"%(pat,n))
    print("  retired %s: %d manifest entr%s removed"%(pat,n,"y" if n==1 else "ies"))
s,n=re.subn(r'[ \t]*<platform [^>]*>\s*</platform>[ \t]*\n','',s)
if n: print("  dropped %d <platform> block(s) left without a package"%n)
open(mf,'w').write(s)
PY
}

# Packaging self-test: the same build_module_tgz + module_pkg_check the release
# uses, on any .so/.xsgn pair (e.g. a container build and a stand-in .xsgn),
# into a scratch directory, then the reuse verdict step 0b would give for the
# current module versions, and the description rewrite on a copy of the
# manifest (<outdir>/updates.xri, unsigned, withdrawn packages retired as step 3 does).
# No build, no signing, repository/ untouched.
#   ./release.sh --package-test <outdir> <NukeX-pxm.so> <NukeX-pxm.xsgn> <Planetary-pxm.so> <Planetary-pxm.xsgn>
if [ "${1:-}" = "--package-test" ]; then
  [ $# -eq 6 ] || die "usage: $0 --package-test <outdir> <NukeX-pxm.so> <NukeX-pxm.xsgn> <Planetary-pxm.so> <Planetary-pxm.xsgn>"
  OUT="$2"; mkdir -p "$OUT"
  case "$(cd "$OUT" && pwd)/" in "$REPO/"*) die "--package-test must not write into repository/";; esac
  NV="$(modver "$ROOT/modules/nukex/src/module/NukeXVersion.h" NUKEX)"
  LV="$(modver "$ROOT/modules/planetary/src/module/PlanetaryVersion.h" PLANETARY)"
  NT="$OUT/$DATE-linux-x64-NukeX-$NV.tar.gz"
  LT="$OUT/$DATE-linux-x64-Planetary-$LV.tar.gz"
  build_module_tgz "$NT" NukeX "$3" "$4" "$NUKEX_DOC" "${NUKEX_DOC_IDS[@]}"
  module_pkg_check "$NT" NukeX "$NUKEX_DOC" "${NUKEX_DOC_IDS[@]}" || die "NukeX package check failed"
  build_module_tgz "$LT" Planetary "$5" "$6" "$PLANETARY_DOC" "${PLANETARY_DOC_IDS[@]}"
  module_pkg_check "$LT" Planetary "$PLANETARY_DOC" "${PLANETARY_DOC_IDS[@]}" || die "Planetary package check failed"
  for t in "$NT" "$LT"; do echo "== $(basename "$t")"; tar -tzvf "$t"; done
  echo "== release step 0b verdict for the current versions"
  published_pkg_reusable "$(module_tgz NukeX "$NV")" NukeX "$NUKEX_DOC" "${NUKEX_DOC_IDS[@]}" \
    || echo "  release.sh would STOP: NukeX $NV is published without the current docs -- bump its version"
  published_pkg_reusable "$(module_tgz Planetary "$LV")" Planetary "$PLANETARY_DOC" "${PLANETARY_DOC_IDS[@]}" \
    || echo "  release.sh would STOP: Planetary $LV is published without the current docs -- bump its version"
  echo "== package descriptions, rewritten into a copy of the manifest: $OUT/updates.xri"
  # The manifest's fileNames, with each module entry renamed to the package this release would ship
  # (as step 3 does), so a bumped version is checked against its own description, each with its source.
  sed '/<Signature developerId=/d' "$REPO/updates.xri" > "$OUT/updates.xri"
  retire_manifest "$OUT/updates.xri" "${RETIRED_PKG_RE[@]}"
  for m in "NukeX $NV" "Planetary $LV"; do
    set -- $m
    sed -i -E "s#fileName=\"[^\"]*-linux-x64-$1(-[0-9.]+)?\.tar\.gz\"#fileName=\"$(module_tgz "$1" "$2")\"#" "$OUT/updates.xri"
  done
  DESC_LIST="$(desc_args $(manifest_files "$OUT/updates.xri"))" || die "a package in updates.xri has no description source"
  mapfile -t DESC_ARGS <<< "$DESC_LIST"
  descriptions apply "$OUT/updates.xri" "${DESC_ARGS[@]}" || die "description rewrite failed"
  descriptions verify "$OUT/updates.xri" "${DESC_ARGS[@]}" || die "description check failed"
  exit 0
fi

# Every PixInsight launch below goes through pi_headless: a private Xvfb, no
# WAYLAND_DISPLAY, QT_QPA_PLATFORM=xcb -- never the user's desktop.
. "$ROOT/tools/pi-headless.sh"
python3 "$ROOT/tools/check-pi-launches.py" "$ROOT" || die "a script launches PixInsight outside pi_headless"
"$ROOT/tools/test-release-manifest.sh" "$0" || die "release.sh's manifest edits fail their test (tools/test-release-manifest.sh)"
[ -x "$PI" ]        || die "PixInsight.sh not executable at $PI"
[ -f "$KEYS" ]      || die "signing keys not found at $KEYS"
[ -f "$PASS_FILE" ] || die "password file not found at $PASS_FILE (create it 0600, never commit)"
# The EZ sign.sh reads the password from /tmp/.pi_codesign_pass directly; keep PASS_FILE aligned with it.
[ "$PASS_FILE" = "/tmp/.pi_codesign_pass" ] || die "EZ sign.sh expects /tmp/.pi_codesign_pass; set ASTROPI_SIGN_PASS_FILE accordingly"
PASS="$(cat "$PASS_FILE")"

sha1(){ python3 -c "import hashlib,sys;print(hashlib.sha1(open(sys.argv[1],'rb').read()).hexdigest())" "$1"; }

# write_pkg <manifest> <fileName> <sha1> : set sha1 + releaseDate for the package matching fileName.
# Exactly one entry must match each: a fileName the manifest does not hold used to change nothing and
# succeed, so a bumped zip shipped the old one's entry (ruling T10b).
write_pkg(){
  python3 - "$1" "$2" "$3" "$DATE" <<'PY' || die "write_pkg: $2 is not exactly one package entry of $1"
import re,sys
mf,fn,h,date=sys.argv[1:5]
s=open(mf).read()
# A package whose bytes are the ones the entry already records is unchanged: it keeps its releaseDate
# (every release used to re-date every package, so the manifest changed on any later day's run).
old=re.findall(r'fileName="'+re.escape(fn)+r'"\s+sha1="([0-9a-fA-F]{40})"', s)
s,a=re.subn(r'(fileName="'+re.escape(fn)+r'"\s+sha1=")[0-9a-fA-F]{40}(")', r'\g<1>'+h+r'\g<2>', s)
if [o.lower() for o in old]!=[h.lower()]:
    s,b=re.subn(r'(fileName="'+re.escape(fn)+r'"[^>]*releaseDate=")\d{8}(")', r'\g<1>'+date+r'\g<2>', s)
else:
    b=len(re.findall(r'fileName="'+re.escape(fn)+r'"[^>]*releaseDate="\d{8}"', s))
if (a,b)!=(1,1): sys.exit("write_pkg: %s: %d sha1 and %d releaseDate entries, expected 1 and 1"%(fn,a,b))
open(mf,'w').write(s)
PY
}

# published_entries_check <published manifest> <work manifest> : every package the published manifest
# serves that this release still serves under the same fileName keeps its sha1 and releaseDate (a
# published fileName with new bytes is the stale-CDN hazard step 0b's naming avoids; a new releaseDate
# on unchanged bytes is a manifest edit with nothing behind it).
published_entries_check(){
  python3 - "$1" "$2" <<'PY'
import re,sys
def entries(p):
    return {fn:(h.lower(),d) for fn,h,d in re.findall(r'<package fileName="([^"]+)"\s+sha1="([0-9a-fA-F]{40})"[^>]*releaseDate="(\d{8})"',open(p,encoding="utf-8").read())}
pub,work=entries(sys.argv[1]),entries(sys.argv[2])
errs=["%s: sha1 %s releaseDate %s, published as sha1 %s releaseDate %s"%(fn,work[fn][0],work[fn][1],pub[fn][0],pub[fn][1])
      for fn in sorted(set(pub)&set(work)) if pub[fn]!=work[fn]]
if errs: [print("  "+e) for e in errs]; sys.exit(1)
print("  OK  %d package(s) served as published keep their sha1 and releaseDate; %d new"%(len(set(pub)&set(work)),len(set(work)-set(pub))))
PY
}

# manifest_rename <manifest> <regex of the old fileName> <new fileName> : point the one entry whose
# fileName matches at this release's file. No entry, or several, is an error (a first release of a
# package needs its entry added first).
manifest_rename(){
  python3 - "$1" "$2" "$3" <<'PY' || die "manifest_rename: $1 has not exactly one package entry matching $2 (for $3)"
import re,sys
mf,pat,fn=sys.argv[1:4]
s=open(mf).read()
s,n=re.subn(r'fileName="'+pat+r'"', 'fileName="'+fn+'"', s)
if n!=1: sys.exit("manifest_rename: %d entries match %s, expected 1"%(n,pat))
open(mf,'w').write(s)
PY
}

# module_platform_block <manifest> <Module>... : every module package lives in its own linux/x64
# <platform> block, version 1.9.6:2.0.0, created after the last linux/x64 block when missing.
# WHY: every module is built against PCL 2.13.0 (tools/build-env/Containerfile). A PCL module resolves
# the core's whole API table when it initialises and refuses to load if one entry is missing
# (src/pcl/APIInterface.cpp, Resolve), and PCL 2.13 adds five TreeBox functions the 1.9.5 core lacks;
# measured: PixInsight 1.9.5 build 1706 rejects a 2.13 module with "Unable to resolve PCL API interface".
# PixInsight 1.9.6's updater accepts two linux/x64 blocks and offers each block's packages by its own
# range (measured 2026-11 by an updater probe, run-probe-xri-platforms.sh, now in scarter4work/gaia-depth-grade-pi). Each <Module>'s entry
# (fileName *-linux-x64-<Module>[-<ver>].tar.gz) is moved into that block; none is fine (a first release
# inserts it there), more than one is an error. Everything else (the scripts) keeps its block.
module_platform_block(){
  python3 - "$@" <<'PY' || die "module_platform_block: could not place the module packages in the 1.9.6:2.0.0 block of $1"
import re,sys
mf=sys.argv[1]; mods=sys.argv[2:]
HEAD='<platform os="linux" arch="x64" version="1.9.6:2.0.0">'
s=open(mf).read()
if s.count(HEAD)>1: sys.exit("%d module platform blocks"%s.count(HEAD))
if HEAD not in s:
    last=None
    for m in re.finditer(r'<platform os="linux" arch="x64"[^>]*>.*?</platform>',s,re.S): last=m
    if last is None: sys.exit("no linux/x64 <platform> block")
    s=s[:last.end()]+"\n   "+HEAD+"\n   </platform>"+s[last.end():]
moved=0
for mod in mods:
    pat=r'\n[ \t]*<package fileName="[^"]*-linux-x64-'+re.escape(mod)+r'(?:-[0-9.]+)?\.tar\.gz"[^>]*>.*?</package>'
    hits=list(re.finditer(pat,s,re.S))
    if len(hits)>1: sys.exit("%d package entries for module %s"%(len(hits),mod))
    if not hits: continue
    b=s.index(HEAD); e=s.index("</platform>",b)
    if b<hits[0].start()<e: continue
    entry=hits[0].group(0)
    s=s[:hits[0].start()]+s[hits[0].end():]
    b=s.index(HEAD); e=s.index("</platform>",b)
    close=s.rindex("\n",b,e)
    s=s[:close]+entry+s[close:]
    moved+=1
open(mf,"w").write(s)
print("  OK  module packages in the linux/x64 1.9.6:2.0.0 block (%d moved there)"%moved)
PY
}

# install_manifest <signed work copy> <repository manifest> : the one write to repository/updates.xri.
# Refuses a copy that is not signed or does not verify (every declared sha1 against the file beside the
# destination); otherwise replaces the destination atomically (a temporary file in its directory, then
# mv), so on any failure the destination is untouched.
install_manifest(){
  local work="$1" dest="$2" tmp
  grep -q '<Signature developerId="scarter4work"' "$work" || die "install_manifest: $work is not signed"
  python3 - "$work" "$(dirname "$dest")" <<'PY' || die "install_manifest: $work does not verify against $(dirname "$dest")"
import re,sys,hashlib,os
mf,repo=sys.argv[1:3]
s=open(mf).read()
pkgs=re.findall(r'fileName="([^"]+)"\s+sha1="([0-9a-fA-F]{40})"', s)
if not pkgs: sys.exit("no package entries")
for fn,h in pkgs:
    p=os.path.join(repo,fn)
    if not os.path.exists(p) or hashlib.sha1(open(p,'rb').read()).hexdigest()!=h.lower(): sys.exit("%s: missing or not its sha1"%fn)
PY
  tmp="$(mktemp "$(dirname "$dest")/.updates.xri.XXXXXX")" || die "install_manifest: no temporary file beside $dest"
  # mktemp makes it 0600; the manifest keeps its own mode (0644 when it is new).
  if ! cp "$work" "$tmp" || ! { if [ -e "$dest" ]; then chmod --reference="$dest" "$tmp"; else chmod 0644 "$tmp"; fi; } \
     || ! mv -f "$tmp" "$dest"; then rm -f "$tmp"; die "install_manifest: could not replace $dest"; fi
  cmp -s "$work" "$dest" || die "install_manifest: $dest is not the signed manifest"
}

# manifest_removes <manifest> <package dir> [<fileName-regex>=<item>[,<item>...]]... : make the manifest's
# <remove> elements exactly these (PKG_REMOVE): every existing <remove> is dropped, then each spec's items
# go into the one package whose fileName matches its regex, after its <title>, one item per line. Refuses
# (manifest unchanged) a regex matching no package or several, an item that is empty, absolute, contains
# "..", a comma, a glob or a backslash, is not under bin/, src/scripts/, doc/scripts/ or doc/tools/ (or is
# one of those roots), or is (or is a directory containing) a file that another package in <package dir>
# installs -- the contents of every manifest package are read from <package dir> (.zip or .tar.gz).
manifest_removes(){
  python3 - "$@" <<'PY' || die "manifest_removes: refused (see above); $1 is unchanged"
import re,sys,os,zipfile,tarfile
mf,pkgdir=sys.argv[1:3]; specs=sys.argv[3:]
s=open(mf,encoding="utf-8").read()
roots=("bin/","src/scripts/","doc/scripts/","doc/tools/")
def contents(fn):
    p=os.path.join(pkgdir,fn)
    if not os.path.isfile(p): sys.exit("  package file %s is not in %s"%(fn,pkgdir))
    if fn.endswith(".zip"):
        with zipfile.ZipFile(p) as z: names=z.namelist()
    elif fn.endswith(".tar.gz"):
        with tarfile.open(p,"r:gz") as t: names=[m.name for m in t.getmembers() if not m.isdir()]
    else: sys.exit("  %s: not a .zip or .tar.gz"%fn)
    return [n[2:] if n.startswith("./") else n for n in names if not n.endswith("/")]
pkgs=re.findall(r'<package fileName="([^"]+)"',s)
errs=[]; plan={}
for spec in specs:
    if "=" not in spec: errs.append("%s: not <regex>=<items>"%spec); continue
    rx,items=spec.split("=",1)
    hit=[fn for fn in pkgs if re.fullmatch(rx,fn)]
    if len(hit)!=1: errs.append("%s matches %d manifest packages, expected 1"%(rx,len(hit))); continue
    fn=hit[0]; its=[i.strip() for i in items.split(",")]
    for it in its:
        if (not it or it.startswith("/") or ".." in it or re.search(r'[*?\[\]\\,]',it)
            or not it.startswith(roots) or it.rstrip("/")+"/" in roots):
            errs.append("%s: item %r is not a file or directory below bin/, src/scripts/, doc/scripts/ or doc/tools/"%(fn,it)); continue
        for other in pkgs:
            if other==fn: continue
            for n in contents(other):
                if n==it.rstrip("/") or n.startswith(it.rstrip("/")+"/"):
                    errs.append("%s: item %s would delete %s, which %s installs"%(fn,it,n,other))
    if fn in plan: errs.append("%s is named by two specs"%fn)
    plan[fn]=its
if errs: [print("  "+e) for e in errs]; sys.exit(1)
s,_=re.subn(r'\n[ \t]*<remove>.*?</remove>[ \t]*(?=\n)','',s,flags=re.S)
for fn,its in plan.items():
    m=re.search(r'(<package fileName="'+re.escape(fn)+r'"[^>]*>)(.*?)(</package>)',s,re.S)
    body=m.group(2); t=re.search(r'</title>',body)
    ind=re.search(r'\n([ \t]*)<',body); ind=ind.group(1) if ind else "         "
    el="\n"+ind+"<remove>\n"+"".join(ind+"   "+i+"\n" for i in its)+ind+"</remove>"
    at=t.end() if t else 0
    body=body[:at]+el+body[at:]
    s=s[:m.start(2)]+body+s[m.end(2):]
    print("  %s removes %s"%(fn,", ".join(its)))
open(mf,"w",encoding="utf-8").write(s)
PY
}

# ez_pkg_check <zip> <Name> : the EZ script package deploys exactly src/scripts/scarter4work/<Name>.{js,xsgn}
# and the current doc/scripts/<Name>/ (each doc file and the .js byte for byte as in the source tree).
# Run on an already-published zip before it is reused, and on every zip this release packages.
ez_pkg_check(){
  python3 - "$1" "$2" "$EZSRC" "$EZDOC" <<'PY'
import sys,os,zipfile
zp,name,src,doc=sys.argv[1:5]
want={"src/scripts/scarter4work/%s.js"%name: os.path.join(src,name+".js"),
      "src/scripts/scarter4work/%s.xsgn"%name: None}
d=os.path.join(doc,name)
if not os.path.isfile(os.path.join(d,name+".html")): sys.exit("  %s: no documentation %s/%s.html"%(name,d,name))
for root,_,files in os.walk(d):
    for f in files:
        p=os.path.join(root,f); want["doc/scripts/%s/%s"%(name,os.path.relpath(p,d))]=p
with zipfile.ZipFile(zp) as z:
    have={n for n in z.namelist() if not n.endswith("/")}
    errs=["%s: %s"%(os.path.basename(zp),e) for e in
          ["missing "+n for n in sorted(set(want)-have)]+["unexpected "+n for n in sorted(have-set(want))]]
    for n,p in want.items():
        if p and n in have and z.read(n)!=open(p,"rb").read(): errs.append("%s: %s differs from %s"%(os.path.basename(zp),n,p))
if errs: [print("  "+e) for e in errs]; sys.exit(1)
print("  OK  %s deploys %s.js/.xsgn and doc/scripts/%s/ (%d doc files)"%(os.path.basename(zp),name,name,len(want)-2))
PY
}

# ── What to sign: only products whose signed sources changed ────────────────
# An .xsgn embeds a timestamp, so re-signing an unchanged script rewrites a published signature: the
# tree then holds a signature no package ever shipped, and every release left EZStretch's and the Gaia
# scripts' .xsgn modified (2026-11-15: both releases restored them by hand; the Gaia .xsgn had been
# re-signed and committed four times since gaia-depth-grade_v1.0.13.zip shipped). Change is decided by
# content, against what is published: the package the committed manifest serves for the product (its
# committed bytes must be the sha1 the manifest records) carries the signed sources and their .xsgn.
#
# signed_state <product> <published manifest> <fileName regex> <this release's fileName> <dir in the zip> <src dir> <source>...
#   prints "changed <published fileName|->" (a source differs from the published one, or nothing is
#   published yet: sign it) or "unchanged <published fileName>" (every source is byte for byte the
#   published one, and so is its .xsgn: do not sign). Refuses (exit 1): a changed source under the
#   published version (bump it), a published package that is not committed or not the recorded sha1,
#   this release's fileName committed but not the one served, and an unchanged source whose tree .xsgn is
#   not the published one (a stray re-sign; the published signature is the one that shipped).
signed_state(){
  python3 - "$ROOT" "$@" <<'PY'
import re,sys,os,io,hashlib,subprocess,zipfile
root,prod,mf,rx,cur,inner,src=sys.argv[1:8]; files=sys.argv[8:]
def err(m): print("  %s: %s"%(prod,m),file=sys.stderr); sys.exit(1)
def committed(p):
    r=subprocess.run(["git","-C",root,"show",":repository/"+p],capture_output=True)
    return r.stdout if r.returncode==0 else None
ents=[(fn,h) for fn,h in re.findall(r'<package fileName="([^"]+)"\s+sha1="([0-9a-fA-F]{40})"',open(mf,encoding="utf-8").read()) if re.fullmatch(rx,fn)]
if len(ents)>1: err("the published manifest has %d entries matching %s"%(len(ents),rx))
if not ents: print("changed -"); sys.exit(0)
pub,h=ents[0]
b=committed(pub)
if b is None: err("the published manifest serves %s, which is not committed in repository/"%pub)
if hashlib.sha1(b).hexdigest()!=h.lower(): err("committed repository/%s is not the sha1 the published manifest records (%s)"%(pub,h))
if cur!=pub and committed(cur) is not None: err("%s is committed, but the published manifest serves %s (a version rollback?)"%(cur,pub))
z=zipfile.ZipFile(io.BytesIO(b)); names=set(z.namelist())
changed=[]; drift=[]
for f in files:
    sig=os.path.splitext(f)[0]+".xsgn"
    tp,ts=os.path.join(src,f),os.path.join(src,sig)
    if not os.path.isfile(tp): err("source %s not found"%tp)
    if inner+"/"+f not in names or open(tp,"rb").read()!=z.read(inner+"/"+f): changed.append(f); continue
    if inner+"/"+sig not in names: err("%s carries %s without its %s"%(pub,f,sig))
    if not os.path.isfile(ts) or open(ts,"rb").read()!=z.read(inner+"/"+sig): drift.append(ts)
if changed:
    if cur==pub: err("%s changed since %s was published, under the same version: bump %s's version"%(", ".join(changed),pub,prod))
    print("changed "+pub); sys.exit(0)
if drift: err("%s: not the signature published in %s for the same source (a stray re-sign) -- restore the published bytes from that package"%(", ".join(drift),pub))
print("unchanged "+pub)
PY
}

# manifest_names_check <manifest> <fileName>... : the manifest's packages are exactly these, each once.
manifest_names_check(){
  python3 - "$@" <<'PY' || die "updates.xri does not list exactly this release's packages"
import re,sys
mf=sys.argv[1]; want=sys.argv[2:]
have=re.findall(r'<package fileName="([^"]+)"',open(mf).read())
errs=[]
for fn in sorted(set(have)|set(want)):
    if have.count(fn)!=1 or fn not in want:
        errs.append("%s: %d manifest entr%s, %s this release"%(fn,have.count(fn),"y" if have.count(fn)==1 else "ies","in" if fn in want else "not in"))
if len(want)!=len(set(want)): errs.append("a package is named twice in this release's list")
if errs: [print("  "+e) for e in errs]; sys.exit(1)
print("  OK  updates.xri lists exactly this release's %d packages, each once"%len(want))
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
PLANETARY_VER="$(modver "$ROOT/modules/planetary/src/module/PlanetaryVersion.h" PLANETARY)"
[[ "$NUKEX_VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "could not read NukeX version (got '$NUKEX_VER')"
[[ "$PLANETARY_VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "could not read Planetary version (got '$PLANETARY_VER')"
MOD_TGZ="$(module_tgz NukeX "$NUKEX_VER")"
PLANETARY_TGZ="$(module_tgz Planetary "$PLANETARY_VER")"
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
package_module(){ # <tarball> <Name> <module .so> <module .xsgn> <doc root> <DocId>...
  local tgz="$1" n="$2" so="$3" xsgn="$4" doc="$5"; shift 5
  if git -C "$ROOT" ls-files --error-unmatch "repository/$tgz" >/dev/null 2>&1; then
    git -C "$ROOT" checkout -- "repository/$tgz"
    echo "   $tgz already published -- reusing the committed bytes"
  else
    build_module_tgz "$REPO/$tgz" "$n" "$so" "$xsgn" "$doc" "$@"
  fi
  module_pkg_check "$REPO/$tgz" "$n" "$doc" "$@" || die "$tgz does not deploy exactly bin/$n-pxm.{so,xsgn} + the current doc/tools/{$*}/"
}
# Fail now, not after the build: the docs each package will ship must be
# complete, and an already-published version can only be reused if its
# tarball already carries exactly the current docs (else: bump the version).
module_doc_check "$NUKEX_DOC" "${NUKEX_DOC_IDS[@]}"
module_doc_check "$PLANETARY_DOC" "${PLANETARY_DOC_IDS[@]}"
published_pkg_reusable "$MOD_TGZ" NukeX "$NUKEX_DOC" "${NUKEX_DOC_IDS[@]}" \
  || die "NukeX $NUKEX_VER is already published as $MOD_TGZ without the current doc/tools/NukeX/ -- bump NukeX's version so a new package is built"
published_pkg_reusable "$PLANETARY_TGZ" Planetary "$PLANETARY_DOC" "${PLANETARY_DOC_IDS[@]}" \
  || die "Planetary $PLANETARY_VER is already published as $PLANETARY_TGZ without the current doc/tools/{${PLANETARY_DOC_IDS[*]}}/ -- bump Planetary's version so a new package is built"
# The descriptions this release will write (step 3f) must already name only
# these versions: a bumped version with last release's text stops here.
EZ_ZIPS=(); for name in "${EZ_NAMES[@]}"; do EZ_ZIPS+=("${name}_v${EZVER[$name]}.zip"); done
# EZ scripts (versions: each .js's #define VERSION): the documentation exists, and an already-published
# zip of that version carries exactly the current .js and documentation (else: bump the version).
for name in "${EZ_NAMES[@]}"; do
  [ -f "$EZDOC/$name/$name.html" ] || die "$name has no documentation: $EZDOC/$name/$name.html (CPD rule 6.1)"
  grep -qE "^#feature-id +$name +:" "$EZSRC/$name.js" \
    || die "$name.js has no '#feature-id $name : ...' directive: PixInsight would not find doc/scripts/$name/$name.html"
  ! git -C "$ROOT" ls-files --error-unmatch "repository/${name}_v${EZVER[$name]}.zip" >/dev/null 2>&1 \
    || ez_pkg_check "$REPO/${name}_v${EZVER[$name]}.zip" "$name" \
    || die "$name ${EZVER[$name]} is already published without the current $name.js and doc/scripts/$name/ -- bump $name's version so a new package is built"
done

# What this release signs (steps 2-2b): a module only when its package is new (a published version's
# tarball is reused byte for byte, so its signature would ship nowhere); a script product only when a
# signed source differs from the one its published package carries (signed_state). The published
# manifest is the committed one: the state every package is judged against.
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT
PUB_MF="$WORK_DIR/published.xri"
git -C "$ROOT" show ":repository/updates.xri" > "$PUB_MF" || die "repository/updates.xri is not committed"
declare -A MODULE_NEW=() SIGN_STATE=() SIGN_PUB=()
for pair in "NukeX $MOD_TGZ" "Planetary $PLANETARY_TGZ"; do
  read -r m t <<< "$pair"
  MODULE_NEW[$m]=0
  [ -n "${t:-}" ] && ! git -C "$ROOT" ls-files --error-unmatch "repository/$t" >/dev/null 2>&1 && MODULE_NEW[$m]=1
  echo "   module $m: $([ "${MODULE_NEW[$m]}" = 1 ] && echo "new package ${t} -- signed" || echo "${t:-not packaged}${t:+ published} -- not signed")"
done
SCRIPT_PRODUCTS=("${EZ_NAMES[@]}")
product_state(){ # <product> : signed_state of a script product against the published manifest
  signed_state "$1" "$PUB_MF" "${1}_v[0-9.]+\.zip" "${1}_v${EZVER[$1]}.zip" src/scripts/scarter4work "$EZSRC" "$1.js"
}
for p in "${SCRIPT_PRODUCTS[@]}"; do
  out="$(product_state "$p")" || die "$p: cannot decide whether to sign it (see above)"
  SIGN_STATE[$p]="${out%% *}"; SIGN_PUB[$p]="${out#* }"
  echo "   $p: signed sources ${SIGN_STATE[$p]} since ${SIGN_PUB[$p]} -- $([ "${SIGN_STATE[$p]}" = changed ] && echo signed || echo "not signed")"
done
needs_signing(){ [ "${SIGN_STATE[$1]}" = changed ]; }   # <product>
# This release's packages, ONE list: step 0b's description check, 3f's description write and the
# manifest name-set checks (after 3f and after step 4) all take their names from it.
RELEASE_PKGS=("$MOD_TGZ" "$PLANETARY_TGZ" "${EZ_ZIPS[@]}")
DESC_LIST="$(desc_args "${RELEASE_PKGS[@]}")" || die "a package has no description source"
mapfile -t DESC_ARGS <<< "$DESC_LIST"
descriptions check "$REPO/updates.xri" "${DESC_ARGS[@]}" \
  || die "a package description names a version other than its package's -- bring it up to date for this release"

echo "== 1/6 build NukeX + Planetary modules (portable Rocky 9 container, PCL 2.13.0) =="
# NEVER ship a host build: the dev box's glibc/libstdc++ symbol versions and
# Fedora-only sonames (libceres/libglog/libgflags) end up in the module, which
# then loads on this machine and nowhere else. See tools/build-env/Containerfile.
"$ROOT/tools/build-env/build-modules.sh" || die "portable module build/tests failed"
SO="$(find "$ROOT/modules/nukex/build-portable" -name 'NukeX-pxm.so' -print -quit)"
[ -n "$SO" ] || die "NukeX-pxm.so not found after build"
PLANETARY_SO="$(find "$ROOT/modules/planetary/build-portable" -name 'Planetary-pxm.so' -print -quit)"
[ -n "$PLANETARY_SO" ] || die "Planetary-pxm.so not found after build"

echo "== 1b/6 prove both modules load on stock older distros =="
"$ROOT/tools/build-env/verify-portable.sh" "$SO" "$PLANETARY_SO" || die "a module would not load on a stock distro"

# sign_module <Module> <module .so> : sign a module whose package is new (MODULE_NEW, step 0b); a published
# version's tarball is reused byte for byte, so its build is not signed. Either way no .xsgn from a prior
# build or self-test survives beside the .so (a failed sign must not leave one to package).
sign_module(){
  local m="$1" so="$2" xsgn="${2%-pxm.so}-pxm.xsgn"
  rm -f "$xsgn"
  if [ "${MODULE_NEW[$m]}" = 1 ]; then
    pi_headless "$PI" --sign-module-file="$so" --xssk-file="$KEYS" --xssk-password="$PASS"
    [ -f "$xsgn" ] || die "module signature $xsgn not produced"
  else
    echo "   $m: package published -- not signed"
  fi
}
echo "== 2/6 sign NukeX module (only a new package) =="
XSGN="${SO%-pxm.so}-pxm.xsgn"
sign_module NukeX "$SO"

echo "== 2a2/6 sign Planetary module (only a new package) =="
PLANETARY_XSGN="${PLANETARY_SO%-pxm.so}-pxm.xsgn"
sign_module Planetary "$PLANETARY_SO"

echo "== 2b/6 native-sign the EZ scripts whose source changed =="
EZ_SIGN=(); for name in "${EZ_NAMES[@]}"; do needs_signing "$name" && EZ_SIGN+=("$name"); done
if [ ${#EZ_SIGN[@]} -gt 0 ]; then
  ( cd "$ROOT/scripts/ez-stretch" && bash tools/sign.sh scripts "${EZ_SIGN[@]}" ) || die "EZ script signing failed"
else
  echo "   no EZ script changed -- none signed"
fi

echo "== 2d/6 nothing unchanged was re-signed =="
# Each script product judged unchanged in step 0b must still carry exactly its published .xsgn: a signer
# that wrote one anyway (or a decision that let it) stops the release here, before anything is packaged.
for p in "${SCRIPT_PRODUCTS[@]}"; do
  [ "${SIGN_STATE[$p]}" = unchanged ] || continue
  out="$(product_state "$p")" || die "$p was re-signed although its sources are unchanged since ${SIGN_PUB[$p]}"
  [ "${out%% *}" = unchanged ] || die "$p: its sources changed during the release (now: $out)"
  echo "  OK  $p: .xsgn still the one published in ${SIGN_PUB[$p]}"
done

# Every manifest edit from here on (steps 3-6) is made on a WORK COPY of repository/updates.xri: the
# repository's manifest is replaced only by step 6c, once the copy is fully edited, hashed, signed and
# verified. A failure anywhere before that (a build, a check) leaves
# repository/updates.xri exactly as it was -- never a renamed, re-described manifest under its old,
# now invalid signature, which PixInsight would reject as a whole (final review, module m5).
WORK_MF="$WORK_DIR/updates.xri"   # WORK_DIR: made in step 0b
cp "$REPO/updates.xri" "$WORK_MF"
cp "$WORK_MF" "$WORK_DIR/before.xri"   # step 5 signs the copy only if this release changed it
retire_manifest "$WORK_MF" "${RETIRED_PKG_RE[@]}"
# A module package NEW in this release is a PCL 2.13 build: it goes to the 1.9.6:2.0.0 block. A published
# package reused byte for byte (an unbumped module) keeps its block: older builds still load on 1.9.5.
NEW_MODULES=(); for m in NukeX Planetary; do [ "${MODULE_NEW[$m]}" = 1 ] && NEW_MODULES+=("$m"); done
if [ ${#NEW_MODULES[@]} -gt 0 ]; then module_platform_block "$WORK_MF" "${NEW_MODULES[@]}"; fi

echo "== 3/6 package NukeX module tarball =="
package_module "$MOD_TGZ" NukeX "$SO" "$XSGN" "$NUKEX_DOC" "${NUKEX_DOC_IDS[@]}"
python3 - "$WORK_MF" "$MOD_TGZ" <<'PY'
import re,sys
mf,fn=sys.argv[1:3]
s=open(mf).read()
s,n=re.subn(r'fileName="[^"]*-linux-x64-NukeX(-[0-9.]+)?\.tar\.gz"', 'fileName="'+fn+'"', s)
if n!=1: sys.exit("updates.xri has %d NukeX package entries, expected 1"%n)
open(mf,'w').write(s)
PY

echo "== 3a2/6 package Planetary module tarball (docs deploy as doc/tools/{${PLANETARY_DOC_IDS[*]}}/) =="
package_module "$PLANETARY_TGZ" Planetary "$PLANETARY_SO" "$PLANETARY_XSGN" "$PLANETARY_DOC" "${PLANETARY_DOC_IDS[@]}"
python3 - "$WORK_MF" "$PLANETARY_TGZ" <<'PY'
import re,sys
mf,fn=sys.argv[1:3]
s=open(mf).read()
pat=r'fileName="[^"]*-linux-x64-Planetary(-[0-9.]+)?\.tar\.gz"'
if re.search(pat, s):
    # Entry exists from a prior release -- rename the fileName, as for NukeX.
    s,n=re.subn(pat, 'fileName="'+fn+'"', s)
    if n!=1: sys.exit("updates.xri has %d Planetary package entries, expected 1"%n)
else:
    # First release: a new <package> in the linux/x64 1.9.6:2.0.0 module block, with
    # placeholder sha1/releaseDate that step 4 (write_pkg) fills in, and an
    # empty description that step 3f writes from its source.
    entry = ('\n      <package fileName="' + fn + '" '
             'sha1="0000000000000000000000000000000000000000" '
             'type="module" releaseDate="00000000">\n'
             '         <title>Planetary</title>\n'
             '         <description></description>\n'
             '      </package>\n')
    new_s,n=re.subn(
        r'(<platform os="linux" arch="x64" version="1\.9\.6:2\.0\.0">.*?)(\s*</platform>)',
        lambda m: m.group(1)+entry+m.group(2),
        s, count=1, flags=re.DOTALL)
    if n!=1:
        sys.exit("could not locate linux/x64 <platform> block to insert Planetary package entry")
    s=new_s
open(mf,'w').write(s)
PY
[ -f "$REPO/$PLANETARY_TGZ" ] || die "tarball $PLANETARY_TGZ not produced"

echo "== 3b/6 package EZ script zips (src/scripts/scarter4work + doc/scripts/<Name>) =="
for name in "${EZ_NAMES[@]}"; do
  ver="${EZVER[$name]}"
  zipname="${name}_v${ver}.zip"
  # A published version's zip is reused byte for byte (never rebuilt: a rebuild under the same name is
  # new bytes behind a fileName the CDN already serves); step 0b checked it carries the current .js and
  # documentation, and step 2d that the tree's .xsgn is the one inside it.
  if git -C "$ROOT" ls-files --error-unmatch "repository/$zipname" >/dev/null 2>&1; then
    reuse_if_published "$zipname"
    ez_pkg_check "$REPO/$zipname" "$name" || die "$zipname does not deploy exactly $name.js/.xsgn and the current doc/scripts/$name/"
    manifest_rename "$WORK_MF" "${name}_v[0-9.]+\.zip" "$zipname"
    continue
  fi
  # Stage into src/scripts/scarter4work/ so the menu and the on-disk folder both
  # read scarter4work (the #feature-id in each .js already points there); the
  # documentation goes to doc/scripts/<Name>/ (Repository Reference 4.2).
  stage="$(mktemp -d)"; mkdir -p "$stage/src/scripts/scarter4work" "$stage/doc/scripts"
  cp "$EZSRC/$name.js" "$EZSRC/$name.xsgn" "$stage/src/scripts/scarter4work/"
  mkdir -p "$stage/doc/scripts/$name"
  cp -R "$EZDOC/$name/." "$stage/doc/scripts/$name/"
  # World-readable whatever the build's umask (mktemp -d is 0700): the updater
  # keeps archive modes and installs as root, so 0600/0700 entries would leave
  # the scripts unreadable to the user. (EZ <= 1.0.10/1.0.3/1.0.1 shipped 0600.)
  chmod -R u=rwX,go=rX "$stage"
  rm -f "$REPO/$zipname"
  ( cd "$stage" && zip -qrX "$REPO/$zipname" src doc )
  rm -rf "$stage"
  [ -f "$REPO/$zipname" ] || die "zip $zipname not produced"
  ez_pkg_check "$REPO/$zipname" "$name" || die "$zipname does not deploy exactly $name.js/.xsgn and the current doc/scripts/$name/"
  manifest_rename "$WORK_MF" "${name}_v[0-9.]+\.zip" "$zipname"
done

echo "== 3e/6 publish the NukeX camera database (repository/qe_*) =="
for f in "${QE_FILES[@]}"; do cp "$QE_SRC/$f" "$REPO/$f"; done
qe_check "$REPO" || die "published QE set in repository/ does not verify"

echo "== 3f/6 write every package description from its source (and the repository's) =="
# Every fileName is final by now; each entry gets its product's text, so none
# outlives the release it was written for. The same explicit list as step 0b
# (RELEASE_PKGS), so 0b, 3f and 6a check identical names.
manifest_names_check "$WORK_MF" "${RELEASE_PKGS[@]}"
DESC_LIST="$(desc_args "${RELEASE_PKGS[@]}")" || die "a package has no description source"
mapfile -t DESC_ARGS <<< "$DESC_LIST"
descriptions apply "$WORK_MF" "${DESC_ARGS[@]}" || die "could not write the package descriptions"

echo "== 3g/6 write the <remove> elements (PKG_REMOVE), and no others =="
manifest_removes "$WORK_MF" "$REPO" "${PKG_REMOVE[@]}"

echo "== 4/6 write fileName/sha1/releaseDate into ONE manifest =="
write_pkg "$WORK_MF" "$MOD_TGZ"                  "$(sha1 "$REPO/$MOD_TGZ")"
write_pkg "$WORK_MF" "$PLANETARY_TGZ"            "$(sha1 "$REPO/$PLANETARY_TGZ")"
for name in "${EZ_NAMES[@]}"; do
  zipname="${name}_v${EZVER[$name]}.zip"
  write_pkg "$WORK_MF" "$zipname" "$(sha1 "$REPO/$zipname")"
done
manifest_names_check "$WORK_MF" "${RELEASE_PKGS[@]}"

echo "== 4a/6 packages served as published keep their sha1 and releaseDate =="
published_entries_check "$PUB_MF" "$WORK_MF" || die "this release rewrote a published package's manifest entry"

echo "== 5/6 sign manifest LAST (only if this release changed it) =="
# A signature embeds a timestamp: re-signing a manifest whose content this release left byte for byte as
# it was would rewrite repository/updates.xri for nothing. The work copy still carries the published
# signature then, and it is valid for exactly these bytes.
if cmp -s "$WORK_MF" "$WORK_DIR/before.xri"; then
  echo "   updates.xri unchanged -- keeping its signature"
else
  sed -i '/<Signature developerId=/d' "$WORK_MF"
  pi_headless "$PI" --sign-xml-file="$WORK_MF" --xssk-file="$KEYS" --xssk-password="$PASS"
fi
grep -q '<Signature developerId="scarter4work"' "$WORK_MF" || die "manifest signature not appended"

echo "== 6/6 integrity check: declared sha1 == on-disk =="
python3 - "$WORK_MF" "$REPO" <<'PY'
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

echo "== 6a/6 the signed manifest's descriptions are their sources and name only their own versions =="
descriptions verify "$WORK_MF" "${DESC_ARGS[@]}" || die "updates.xri carries a stale package description"

echo "== 6b/6 integrity check: camera-database publication, with the module's own verifier =="
qe_check "$REPO" || die "repository/ QE publication is stale, unsigned or does not match the embedded database"
# The C++ updater from THIS build, served exactly the files in repository/ at
# the URL the module fetches: signatures, digest, the full install path, and
# that a fresh install is not offered the database it already carries.
NUKEX_QE_PUBLISHED_DIR="$REPO" "$ROOT/modules/nukex/build-portable/test/test_qe_update" "[published]" \
  || die "the module's updater rejects the published camera database in repository/"

echo "== 6b2/6 every published package in repository/ is its committed bytes =="
for p in "${RELEASE_PKGS[@]}"; do
  git -C "$ROOT" ls-files --error-unmatch "repository/$p" >/dev/null 2>&1 || continue
  git -C "$ROOT" diff --quiet -- "repository/$p" || die "repository/$p is published but this release changed its bytes"
done
echo "  OK  no published package rewritten"

echo "== 6c/6 install the signed, verified manifest as repository/updates.xri =="
install_manifest "$WORK_MF" "$REPO/updates.xri"
# Superseded and withdrawn packages leave repository/ only now that the manifest that no longer names
# them is installed.
prune_repository "$REPO/updates.xri" "$REPO"

echo "RELEASE OK — repository/ ready to commit & push"
