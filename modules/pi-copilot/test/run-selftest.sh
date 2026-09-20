#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
PI=/opt/PixInsight/bin/PixInsight.sh
KEYS=/home/scarter4work/projects/keys/scarter4work_keys.xssk
PASS="$(cat /tmp/.pi_codesign_pass)"
SO="$ROOT/build/src/module/PICopilot-pxm.so"

[ -f "$SO" ] || { echo "FAIL: module not built at $SO"; exit 1; }
"$PI" --sign-module-file="$SO" --xssk-file="$KEYS" --xssk-password="$PASS"
[ -f "${SO%.so}.xsgn" ] || { echo "FAIL: signing produced no .xsgn"; exit 1; }

# Load the module headlessly and run the self-test harness. --force-exit only
# exits AFTER running -r= scripts, so a bare -m= with no -r= sits idle
# forever; the harness + timeout close that hole.
rm -f /tmp/.picopilot_selftest.json
if ! timeout 180 "$PI" -n --automation-mode --no-startup-scripts -m="$SO" -r="$HERE/selftest.js" --force-exit; then
   echo "FAIL: PI load timed out (180s) or exited non-zero"; exit 1
fi
R=/tmp/.picopilot_selftest.json
[ -f "$R" ] || { echo "FAIL: no result file"; exit 1; }
cat "$R"
python3 -c "import json,sys; d=json.load(open('$R')); sys.exit(0 if (d.get('evalOk') and d.get('evalResult')==3 and d.get('processInstanceValid')) else 1)" \
   || { echo "FAIL: self-test did not prove both execution paths"; exit 1; }
echo "PASS: EvaluateScript==3 and ProcessInstance valid"
