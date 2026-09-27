#!/usr/bin/env bash
# PI Copilot self-test harness -- stale PixInsight swap-file cleaner (plan
# item 5, thist-hang-investigation.md).
#
# Every killed/timed-out headless PI run from before test/run-selftest.sh
# gave each run its own private TMPDIR (plan item 2) leaked its ~PI~*.swp
# image swap files straight into bare /tmp: PI only deletes them on a CLEAN
# window close. Investigation measured 16,940 such files (5.5 GB) sitting in
# /tmp on this box on 2026-09-26. Now that every run gets its own TMPDIR, a
# NEW run's swap files land there instead (confirmed live, 2026-09-26: a
# running self-test's ~PI~*.swp files showed up under its run's TMPDIR, none
# in bare /tmp) and get removed with it on exit -- so this script only ever
# has legacy debris (or a run from some OTHER, not-yet-updated harness copy)
# to deal with in bare /tmp.
#
# How PI names swap files (inspected 2026-09-26: `ls /tmp/~PI~*.swp`):
#   ~PI~<16-char base32-ish random id>~<window id>.swp
# e.g. ~PI~005CTVEXD074TR5Z~pcHrLong.swp
# The PID of the PI process that created a swap file is NOT encoded in its
# name anywhere.
#
# SAFETY (measured, not assumed): the obvious next idea -- "ask lsof/fuser
# which files are currently open, delete the rest" -- was tried and is WRONG.
# Live test (2026-09-26, a real self-test running under slot 90, mid-fixture,
# 1010 swap files already written for its still-open pcHrLong window): NONE
# of them showed up as open under `lsof -p <pid>`, `fuser`, OR as a mapped
# region in `/proc/<pid>/maps`. PixInsight writes a swap file and closes it
# per checkpoint; it does not hold the fd (or an mmap) open for as long as
# the window stays open. So "no open fd" does NOT mean "stale" -- it is true
# for every swap file of a live run just as much as for a dead one, and a
# per-file open-check would happily delete a live run's history out from
# under it. There is no OS-visible signal, anywhere, that ties a given
# ~PI~*.swp file to the specific process that is still using it.
#
# The one signal that IS reliable: whether ANY PixInsight process is running
# at all, anywhere on the box. If none are, nothing could possibly still want
# any ~PI~*.swp file in bare /tmp (a currently-running self-test's swap files
# no longer land there -- see above), so every one of them is unambiguously
# stale. If even one PixInsight process is running, we cannot tell its files
# apart from old debris, so this script refuses to delete ANYTHING and says
# so -- matching the instruction this was written under: never delete
# anything under /tmp while another run might be live.
#
# Usage:
#   clean-stale-swap.sh              # dry run (default): report only
#   clean-stale-swap.sh --dry-run    # same, explicit
#   clean-stale-swap.sh --apply      # actually delete (only when safe)
set -euo pipefail

MODE="dry-run"
case "${1:-}" in
   ""|--dry-run) MODE="dry-run" ;;
   --apply)      MODE="apply" ;;
   *) echo "usage: $0 [--dry-run|--apply]"; exit 1 ;;
esac

shopt -s nullglob
files=(/tmp/~PI~*.swp)
if [ ${#files[@]} -eq 0 ]; then
   echo "no PixInsight swap files found under /tmp"
   exit 0
fi

human()
{
   numfmt --to=iec-i --suffix=B -- "$1" 2>/dev/null || echo "$1 bytes"
}

total_bytes=0
for f in "${files[@]}"; do
   sz="$(stat -c%s "$f" 2>/dev/null || echo 0)"
   total_bytes=$(( total_bytes + sz ))
done
echo "total ~PI~*.swp files under /tmp: ${#files[@]}, $(human "$total_bytes")"

running_pids=()
while IFS= read -r pid; do
   [ -n "$pid" ] && running_pids+=("$pid")
done < <(pgrep -f '^/opt/PixInsight/bin/PixInsight ' 2>/dev/null || true)

if [ ${#running_pids[@]} -gt 0 ]; then
   echo "${#running_pids[@]} PixInsight process(es) currently running (pid(s): ${running_pids[*]})"
   echo "REFUSING to delete anything: a live run's swap files cannot be told apart"
   echo "from stale ones by inspection (see this script's header) -- re-run this"
   echo "once no PixInsight process is running anywhere on the box."
   exit 0
fi

echo "no PixInsight process is currently running -- every file above is stale"
if [ "$MODE" = "apply" ]; then
   deleted_count=0
   deleted_bytes=0
   for f in "${files[@]}"; do
      sz="$(stat -c%s "$f" 2>/dev/null || echo 0)"
      if rm -f -- "$f"; then
         deleted_count=$(( deleted_count + 1 ))
         deleted_bytes=$(( deleted_bytes + sz ))
      fi
   done
   echo "deleted: $deleted_count, $(human "$deleted_bytes")"
else
   echo "DRY RUN -- would delete: ${#files[@]}, $(human "$total_bytes")"
   echo "(re-run with --apply to actually delete)"
fi
