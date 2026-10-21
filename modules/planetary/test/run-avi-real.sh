#!/usr/bin/env bash
# Planetary real-data AVI proof over every .avi on Scott's NAS (test-only, host-only:
# needs /mnt/qnap and ffmpeg/ffprobe; no PixInsight).
#
#   bash modules/planetary/test/run-avi-real.sh   -> PASS (exit 0) / FAIL (exit 1)
#
# avi_real_check.py drives build/test/avi-probe (the C++ reader) and checks it against
# ffmpeg/ffprobe: frame counts, open time < 2 s (slowest reported), frames 0, N/2, N-1
# byte for byte, the Bayer phase of the planetary sidecar captures, and a >1 GB
# Save-as-SER round trip. The NAS is only read. The round-trip SER (>1 GB) goes to a
# private temp dir on local disk (build/, gitignored -- /tmp is RAM) and is deleted.
#
# PLANETARY_AVI_ROOT overrides the search root; PLANETARY_AVI_PROBE the probe binary;
# PLANETARY_AVI_WORK the parent of the temp dir.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

PROBE="${PLANETARY_AVI_PROBE:-$ROOT/build/test/avi-probe}"
AVI_ROOT="${PLANETARY_AVI_ROOT:-/mnt/qnap/astro_data}"
WORK_PARENT="${PLANETARY_AVI_WORK:-$ROOT/build}"

[ -x "$PROBE" ] || { echo "FAIL: avi-probe not built at $PROBE (cmake --build modules/planetary/build)"; exit 1; }
[ -d "$AVI_ROOT" ] || { echo "FAIL: AVI root not found: $AVI_ROOT"; exit 1; }
for tool in python3 ffmpeg ffprobe; do
   command -v "$tool" >/dev/null || { echo "FAIL: $tool not found"; exit 1; }
done
python3 -I -c 'import numpy' 2>/dev/null || { echo "FAIL: numpy not importable by python3 -I"; exit 1; }

mkdir -p "$WORK_PARENT"
WORK="$(mktemp -d "$WORK_PARENT/avi-real.XXXXXX")"
chmod 700 "$WORK"
trap 'rm -rf "$WORK"' EXIT

PLANETARY_AVI_ROOT="$AVI_ROOT" python3 -I "$HERE/avi_real_check.py" "$PROBE" "$WORK"
