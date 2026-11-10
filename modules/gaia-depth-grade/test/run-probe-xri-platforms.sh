#!/usr/bin/env bash
# Update-repository probe (plan Task 2, decision D3): does the running
# PixInsight's updater accept a signed updates.xri with TWO linux/x64
# <platform> blocks -- the 1.8.0:2.0.0 block every package ships in today, plus
# a 1.9.6:2.0.0 block for modules built against PCL 2.13 -- and offer the
# packages of every block whose range holds the running core?
#
# A scratch copy of repository/updates.xri (repository/ is only read) gets
#   - PROBEALPHA   in the existing linux/x64 1.8.0:2.0.0 block,
#   - PROBEBRAVO   in a second linux/x64 block, 1.9.6:2.0.0,
#   - PROBECHARLIE in a third linux/x64 block, 1.9.7:2.0.0 (control: never offered by 1.9.6),
# is signed with the developer key, and served from 127.0.0.1 over plain HTTP.
# PixInsight runs on its own Xvfb, slot 95, with that URL as its ONLY update
# repository (insecure repositories allowed in that slot only), checks for
# updates at start-up, and the updates dialog is read back by OCR. Nothing is
# ever downloaded or installed: the probe opens the selection dialog from the
# updates notification and leaves it with Cancel (packages are only fetched on
# Apply), the server log must show no package request, and PixInsight is killed
# afterwards (an update only installs on a normal exit after one is scheduled).
#
#   bash run-probe-xri-platforms.sh <out-dir>   -> PASS / FAIL (exit 1) / BLOCKED (exit 2)
# <out-dir> receives the scratch manifest, the server log and the screenshots.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ASTROPI="$(cd "$HERE/../../.." && pwd)"
. "$HERE/harness.sh"
OUTDIR="${1:?usage: $0 <out-dir>}"
mkdir -p "$OUTDIR"; OUTDIR="$(realpath -e "$OUTDIR")"
for t in Xvfb import magick tesseract xprop xdotool; do command -v "$t" >/dev/null || { echo "FAIL: $t not found"; exit 1; }; done

gdg_require_signing_pass || exit 2
gdg_slot_setup || exit 1
gdg_private_tmp || exit 1

# ---- scratch repository ------------------------------------------------------
REPO="$GDG_RUN_DIR/repo"; mkdir -m 700 "$REPO"
for p in PROBEALPHA PROBEBRAVO PROBECHARLIE; do
   mkdir -p "$GDG_RUN_DIR/pkg-$p/doc/scripts/$p"
   echo "$p $(date +%s%N)" > "$GDG_RUN_DIR/pkg-$p/doc/scripts/$p/$p.txt"
   tar -C "$GDG_RUN_DIR/pkg-$p" -czf "$REPO/$p-1.0.0.tar.gz" doc
done
sed '/<Signature developerId=/d' "$ASTROPI/repository/updates.xri" > "$REPO/updates.xri"
python3 - "$REPO" <<'PY' || { echo "FAIL: could not build the scratch manifest"; exit 1; }
import hashlib,os,re,sys
repo=sys.argv[1]; mf=os.path.join(repo,"updates.xri"); s=open(mf).read()
def pkg(p):
    h=hashlib.sha1(open(os.path.join(repo,p+"-1.0.0.tar.gz"),"rb").read()).hexdigest()
    return ('\n      <package fileName="%s-1.0.0.tar.gz" sha1="%s" type="script" releaseDate="20261110">\n'
            '         <title>%s</title>\n         <description><p>%s platform probe package.</p></description>\n'
            '      </package>' % (p,h,p,p))
blocks=re.findall(r'<platform os="linux" arch="x64"[^>]*>',s)
if len(blocks)!=1 or 'version="1.8.0:2.0.0"' not in blocks[0]: sys.exit("expected one linux/x64 1.8.0:2.0.0 block, got %r"%blocks)
m=re.search(r'(<platform os="linux" arch="x64" version="1.8.0:2.0.0">.*?)(\n\s*</platform>)',s,re.S)
s=s[:m.start()]+m.group(1)+pkg("PROBEALPHA")+m.group(2)+s[m.end():]
end=s.index("</platform>",m.start())+len("</platform>")
extra=('\n   <platform os="linux" arch="x64" version="1.9.6:2.0.0">'+pkg("PROBEBRAVO")+'\n   </platform>'
       '\n   <platform os="linux" arch="x64" version="1.9.7:2.0.0">'+pkg("PROBECHARLIE")+'\n   </platform>')
s=s[:end]+extra+s[end:]
open(mf,"w").write(s)
PY
pi_headless "$PI" --sign-xml-file="$REPO/updates.xri" --xssk-file="$GDG_KEYS" --xssk-password="$(cat "$GDG_PASS_FILE")" \
   >"$GDG_RUN_DIR/sign-xri.log" 2>&1 || { echo "FAIL: signing the scratch manifest failed"; exit 1; }
grep -q '<Signature developerId="scarter4work"' "$REPO/updates.xri" || { echo "FAIL: scratch manifest not signed"; exit 1; }
cp "$REPO/updates.xri" "$OUTDIR/updates.xri"
grep -n '<platform\|<package fileName="PROBE' "$REPO/updates.xri" | sed 's/^/  manifest: /'

# ---- local server --------------------------------------------------------------
PORT="$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')"
python3 -I -m http.server --bind 127.0.0.1 --directory "$REPO" "$PORT" >"$OUTDIR/http.log" 2>&1 & HTTP_PID=$!
XVFB_PID="" PI_PID=""
probe_cleanup()
{
   [ -n "$PI_PID" ] && kill -9 "$PI_PID" 2>/dev/null
   pkill -9 -f -- "^$GDG_PI_BIN_DIR/PixInsight -n=$GDG_SLOT " 2>/dev/null
   [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null
   kill "$HTTP_PID" 2>/dev/null
   return 0
}
gdg_on_exit probe_cleanup

# ---- slot: this repository only, insecure (http) allowed, check at start-up ------
gdg_seed_slot_modules || exit 1
python3 - "$GDG_SLOT_SETTINGS" "http://127.0.0.1:$PORT/" <<'PY' || { echo "FAIL: could not configure the slot"; exit 1; }
import sys
import xml.etree.ElementTree as ET
NS="http://www.pixinsight.com/xsdt"; ET.register_namespace("",NS)
ET.register_namespace("xsi","http://www.w3.org/2001/XMLSchema-instance")
path,url=sys.argv[1:3]
tree=ET.parse(path); root=tree.getroot()
def group(parent,*keys):
    for k in keys:
        g=next((c for c in parent.findall("{%s}i"%NS) if c.get("k")==k),None)
        if g is None: g=ET.SubElement(parent,"{%s}i"%NS,{"k":k})
        parent=g
    return parent
def setv(g,k,t,v):
    for c in list(g.findall("{%s}v"%NS)):
        if c.get("k")==k: g.remove(c)
    e=ET.SubElement(g,"{%s}v"%NS,{"k":k,"t":t})
    if t=="s": e.text=v
    else: e.set("v",v)
repos=group(root,"Global","UpdateManager","Repositories")
for c in list(repos): repos.remove(c)
setv(repos,"00000001","s",url)
setv(group(root,"Global","Preferences","Security"),"AllowInsecureRepositories","b","true")
setv(group(root,"Global","Preferences","MainWindow"),"CheckForUpdatesAtStartup","b","true")
# A fresh slot opens a modal "Risky Temporary Folders" notice that holds the
# start-up update check back; record it as answered, as the user's own slot does
# (nothing is ever typed into this PixInsight).
dsa=group(root,"DontShowAgain")
setv(dsa,"WarningOnSystemTempFolderBeingUsedForSwapFiles_Description","s","Warning on a system temporary folder being used for swap file storage")
setv(dsa,"WarningOnSystemTempFolderBeingUsedForSwapFiles_Value","b","false")
tree.write(path,encoding="UTF-8",xml_declaration=True)
PY

# ---- PixInsight GUI on a private display ----------------------------------------
DISP=$(( 100 + GDG_SLOT ))
[ ! -e "/tmp/.X11-unix/X$DISP" ] || { echo "FAIL: display :$DISP already in use"; exit 1; }
Xvfb ":$DISP" -screen 0 1920x1080x24 -nolisten tcp >"$GDG_RUN_DIR/xvfb.log" 2>&1 & XVFB_PID=$!
export DISPLAY=":$DISP"
for _ in $(seq 50); do xprop -root >/dev/null 2>&1 && break; sleep 0.1; done
pi_headless_env
unset XDG_CURRENT_DESKTOP DESKTOP_SESSION KDE_FULL_SESSION KDE_SESSION_VERSION GNOME_DESKTOP_SESSION_ID \
      QT_QPA_PLATFORMTHEME XDG_SESSION_TYPE
export DBUS_SESSION_BUS_ADDRESS="unix:path=$GDG_RUN_DIR/no-session-bus"
export XDG_DATA_HOME="$GDG_RUN_DIR/xdg"; mkdir -m 700 "$XDG_DATA_HOME"
pi_require_private_display || exit 1
timeout 300 "$PI" -n="$GDG_SLOT" --no-startup-scripts >"$OUTDIR/pi.log" 2>&1 & PI_PID=$!

# The start-up check fetches the manifest and, when it offers anything, posts
# a "You have N PixInsight updates" notification (measured on 1.9.6: no dialog
# opens by itself). The probe clicks that notification, which opens "Select and
# Download Updates" -- a selection list; nothing is fetched until Apply -- reads
# it by OCR, then presses Cancel. Words are located with tesseract's TSV output.
# word_xy <png> <word>: centre "x y" of the first OCR word equal to <word> (case-insensitive).
word_xy()
{
   tesseract "$1" - tsv 2>/dev/null \
      | awk -F'\t' -v w="$2" 'NR>1 && tolower($12)==tolower(w) {print int($7+$9/2), int($8+$10/2); exit}'
}
got=0
for i in $(seq 24); do
   sleep 5
   kill -0 "$PI_PID" 2>/dev/null || { echo "FAIL: PixInsight exited early (see $OUTDIR/pi.log)"; exit 1; }
   import -window root "$OUTDIR/shot-$i.png" 2>/dev/null || continue
   grep -q 'GET /updates.xri' "$OUTDIR/http.log" || continue
   xy="$(word_xy "$OUTDIR/shot-$i.png" Click)"
   [ -n "$xy" ] || continue
   xdotool mousemove $xy click 1
   sleep 3
   import -window root "$OUTDIR/final.png"
   tesseract "$OUTDIR/final.png" "$OUTDIR/final" >/dev/null 2>&1 || true
   if grep -q 'Select and Download Updates' "$OUTDIR/final.txt"; then
      # The package rows alternate shading; grey, 3x, sparse-text OCR reads every row
      # (measured: the plain screenshot drops PROBEBRAVO).
      magick "$OUTDIR/final.png" -colorspace Gray -resize 300% "$OUTDIR/final-ocr.png"
      tesseract "$OUTDIR/final-ocr.png" - --psm 11 > "$OUTDIR/final.txt" 2>/dev/null || true
      got=$i; break
   fi
done
if [ "$got" != 0 ]; then
   xy="$(word_xy "$OUTDIR/final.png" Cancel)"
   [ -n "$xy" ] && xdotool mousemove $xy click 1
   sleep 1
fi
{ grep 'GET ' "$OUTDIR/http.log" || true; } | sed 's/^/  http: /'
grep -q 'GET /updates.xri' "$OUTDIR/http.log" || { echo "FAIL: PixInsight never fetched updates.xri"; exit 1; }
[ "$got" != 0 ] || { echo "FAIL: no updates notification / Select and Download Updates dialog within 120 s (see $OUTDIR/shot-*.png)"; exit 1; }
txt="$OUTDIR/final.txt"
fails=0
grep -q 'Verified' "$txt" && grep -q 'scarter4work' "$txt" && echo "  ok    manifest signature verified (scarter4work)" \
   || { echo "  FAIL  the dialog does not report a verified signature"; fails=1; }
for p in PROBEALPHA PROBEBRAVO; do
   grep -qi "$p" "$txt" && echo "  ok    $p offered" || { echo "  FAIL  $p not offered"; fails=1; }
done
grep -qi PROBECHARLIE "$txt" && { echo "  FAIL  PROBECHARLIE (1.9.7:2.0.0) offered to $(sed -n 's/.*PixInsight Core \([0-9.]*\).*/\1/p' "$OUTDIR/pi.log" | head -1)"; fails=1; } \
   || echo "  ok    PROBECHARLIE (1.9.7:2.0.0 control) not offered"
grep -q 'GET /PROBE' "$OUTDIR/http.log" && { echo "  FAIL  a probe package was downloaded"; fails=1; } || echo "  ok    no package downloaded"
echo "  dialog: $OUTDIR/final.png"
[ "$fails" = 0 ] && echo "PASS: two linux/x64 platform blocks accepted; packages offered per block range" || { echo "FAIL"; exit 1; }
