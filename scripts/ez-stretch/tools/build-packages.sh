#!/bin/bash
# build-packages.sh - Atomic release: sign scripts -> zip -> write hashes -> sign manifest
#
# Usage: ./tools/build-packages.sh
#
# Does the whole release in the one correct order (mirrors NukeX `make package`),
# so package SHA1s and the signed manifest can never drift out of sync:
#   1. sign the .js scripts        (tools/sign.sh scripts)
#   2. zip each signed script       (-> repository/<name>_v<ver>.zip)
#   3. write each fresh SHA1 into repository/updates.xri  (no manual editing)
#   4. sign updates.xri natively    (tools/sign.sh xri)
#
# Why this order matters: .xsgn signatures embed a timestamp, so they are NOT
# deterministic - signing scripts AFTER zipping would change the file and
# invalidate the SHA1 already written to the manifest. Sign first, zip, hash,
# then sign the manifest (which no longer affects any zip).

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
SCRIPTS="$PROJECT_DIR/src/scripts/EZ Stretch BSC"
REPO="$PROJECT_DIR/repository"
XRI="$REPO/updates.xri"

# --- 1. Sign scripts (must happen before zipping) ---------------------------
echo "== Step 1/4: sign scripts =="
"$SCRIPT_DIR/sign.sh" scripts

# --- 2+3. Zip signed scripts, write SHA1s into updates.xri ------------------
echo ""
echo "== Step 2-3/4: build zips + write hashes into updates.xri =="
python3 << EOF
import zipfile, hashlib, os, glob, re

scripts = "$SCRIPTS"
repo = "$REPO"
xri_path = "$XRI"

install_base = "src/scripts/EZ Stretch BSC"
packages = {
    "EZStretch":     {"version": "1.0.9"},
    "EZDonutRepair": {"version": "1.0.2"},
    "EZHazeKill":    {"version": "1.0.0"},
}

with open(xri_path, "r", encoding="utf-8") as f:
    xri = f.read()

for name, cfg in packages.items():
    version = cfg["version"]
    zipname = f"{repo}/{name}_v{version}.zip"

    # Remove old versions of this package
    for old in glob.glob(f"{repo}/{name}_v*.zip"):
        os.remove(old)

    with zipfile.ZipFile(zipname, "w", zipfile.ZIP_DEFLATED) as zf:
        for ext in (".js", ".xsgn"):
            srcpath = f"{scripts}/{name}{ext}"
            if os.path.exists(srcpath):
                zf.write(srcpath, f"{install_base}/{name}{ext}")

    with open(zipname, "rb") as f:
        sha1 = hashlib.sha1(f.read()).hexdigest()

    # Rewrite this package's fileName + sha1 in updates.xri (matches by name).
    pat = re.compile(r'fileName="%s_v[^"]*\.zip" sha1="[^"]*"' % re.escape(name))
    repl = f'fileName="{name}_v{version}.zip" sha1="{sha1}"'
    xri, n = pat.subn(repl, xri)
    status = "ok" if n == 1 else f"WARNING: matched {n} entries"
    print(f"  {name}_v{version}.zip  {sha1}  [{status}]")

with open(xri_path, "w", encoding="utf-8") as f:
    f.write(xri)
EOF

# --- 4. Sign the finalized manifest -----------------------------------------
echo ""
echo "== Step 4/4: sign manifest =="
"$SCRIPT_DIR/sign.sh" xri

# --- verify -----------------------------------------------------------------
echo ""
echo "== Verify: declared SHA1 vs zip on disk =="
cd "$REPO"
fail=0
for z in *.zip; do
    actual=$(sha1sum "$z" | cut -d' ' -f1)
    declared=$(grep -oP "fileName=\"$z\" sha1=\"\K[^\"]+" updates.xri || echo "MISSING")
    if [ "$actual" = "$declared" ]; then echo "  OK   $z"; else echo "  FAIL $z (zip=$actual xri=$declared)"; fail=1; fi
done
[ "$fail" = 0 ] && echo "" && echo "Release ready." || { echo "" ; echo "INTEGRITY MISMATCH"; exit 1; }
