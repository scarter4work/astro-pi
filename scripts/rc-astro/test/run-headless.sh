#!/usr/bin/env bash
# Xvfb + PixInsight headless PJSR runner.
# Usage: run-headless.sh path/to/test.js [slot]
#
# [slot] is optional and pins the run to a specific PixInsight instance slot
# (`-n=<slot>`, range [1,256] per `PixInsight --help`) instead of the default
# `--new` (claims the first free slot). Use it when other headless PI runs
# may be in flight concurrently (this repo's worktree-based agents each run
# their own tests) and grabbing "first free" risks colliding with one of
# them; pick a slot known to be free at the time.
set -euo pipefail
dirname=/opt/PixInsight/bin
export LD_LIBRARY_PATH=/usr/local/cuda/lib64:$dirname/lib:$dirname
export CUDA_VISIBLE_DEVICES=0
export TF_CPP_MIN_LOG_LEVEL=1
export LC_ALL=en_US.utf8
export QT_PLUGIN_PATH=$dirname/lib/qt-plugins
export QT_QPA_PLATFORM_PLUGIN_PATH=$dirname/lib/qt-plugins/platforms
export QT_QPA_PLATFORM=xcb
export QT_LOGGING_RULES='*=false'
export AVAHI_COMPAT_NOWARN=1
export TMPDIR=/home/scarter4work/pixinsight-swap
mkdir -p "$TMPDIR"
script="$1"
slot="${2:-}"
newflag="--new"
if [ -n "$slot" ]; then newflag="--new=$slot"; fi
xvfb-run -a -s "-screen 0 1920x1080x24" \
  /opt/PixInsight/bin/PixInsight "$newflag" --automation-mode --force-exit -r="$script"
