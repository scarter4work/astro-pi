# shellcheck shell=bash
# Source me. Every PixInsight launch in this repo goes through here, so no
# caller has to remember how to keep PixInsight off the user's desktop.
#
# Why this is needed: xvfb-run alone is NOT enough. PixInsight 1.9.5 build
# 1706's PixInsight.sh no longer forces QT_QPA_PLATFORM=xcb, and Qt prefers
# its wayland plugin whenever WAYLAND_DISPLAY is set -- which every shell
# started from a Wayland desktop inherits. Such a "headless" PixInsight
# ignores xvfb-run's DISPLAY and opens on the real desktop (measured
# 2026-10-03: an e2e run under `xvfb-run -a` was connected to
# /run/user/1000/wayland-0, not to the Xvfb socket).
#
#   pi_headless <command> [args...]
#       Runs <command> (PixInsight.sh, the PixInsight binary, or a wrapper
#       such as `timeout ... PixInsight.sh`) on a private Xvfb display that
#       xvfb-run starts and tears down, with WAYLAND_DISPLAY removed and
#       QT_QPA_PLATFORM=xcb. pi_require_private_display runs INSIDE the launch,
#       on the environment PixInsight will inherit, and refuses to exec if it
#       could reach the desktop. Screen size: PI_HEADLESS_SCREEN.
#
#   pi_headless_env
#       For harnesses that start their own Xvfb: strip WAYLAND_DISPLAY and
#       force QT_QPA_PLATFORM=xcb in the current shell.
#
#   pi_require_headless_env
#       Returns 1 (with a message) if PixInsight launched from this
#       environment would pick Qt's wayland plugin: WAYLAND_DISPLAY set, or
#       QT_QPA_PLATFORM not xcb. Environment only -- usable before the
#       private display exists.
#
#   pi_require_private_display
#       Returns 1 (with a message) unless pi_require_headless_env holds AND
#       DISPLAY is served by an Xvfb process (not Xwayland or a real X
#       server). Call it immediately before any launch that does not go
#       through pi_headless (a harness running its own Xvfb).

PI_HEADLESS_LIB="$(realpath "${BASH_SOURCE[0]}")"
PI_HEADLESS_SCREEN="${PI_HEADLESS_SCREEN:-1920x1080x24}"

pi_headless_env()
{
   unset WAYLAND_DISPLAY
   export QT_QPA_PLATFORM=xcb
}

pi_require_headless_env()
{
   if [ -n "${WAYLAND_DISPLAY:-}" ] || [ "${QT_QPA_PLATFORM:-}" != "xcb" ]; then
      echo "FAIL: refusing to launch PixInsight: WAYLAND_DISPLAY='${WAYLAND_DISPLAY:-}'" \
           "QT_QPA_PLATFORM='${QT_QPA_PLATFORM:-}' -- it would open on the real desktop" \
           "(use pi_headless, or pi_headless_env under a private Xvfb)" >&2
      return 1
   fi
   return 0
}

pi_require_private_display()
{
   pi_require_headless_env || return 1
   local n="${DISPLAY:-}"
   n="${n#:}"; n="${n%%.*}"
   if [ -z "$n" ] || ! [[ "$n" =~ ^[0-9]+$ ]]; then
      echo "FAIL: refusing to launch PixInsight: DISPLAY='${DISPLAY:-}' is not a local X display" >&2
      return 1
   fi
   # Whoever listens on the display's socket must be Xvfb. The desktop's own
   # X server here is Xwayland (owned by the compositor): an xcb client on
   # that display is on the user's screen as surely as a wayland one.
   local owner
   owner="$(ss -xlpH 2>/dev/null | grep -E "[@ ]/tmp/\.X11-unix/X${n}( |$)" \
            | grep -oE 'users:\(\("[^"]+"' | head -1 | sed 's/users:(("//;s/"//')"
   if [ "$owner" != "Xvfb" ]; then
      echo "FAIL: refusing to launch PixInsight: DISPLAY=$DISPLAY is served by" \
           "'${owner:-nothing}', not a private Xvfb" >&2
      return 1
   fi
   return 0
}

pi_headless()
{
   [ $# -ge 1 ] || { echo "pi_headless: no command given" >&2; return 2; }
   command -v xvfb-run >/dev/null 2>&1 || {
      echo "FAIL: xvfb-run not found; refusing to launch PixInsight on the real display" >&2
      return 127
   }
   env -u WAYLAND_DISPLAY QT_QPA_PLATFORM=xcb PI_HEADLESS_LIB="$PI_HEADLESS_LIB" \
      xvfb-run -a -s "-screen 0 $PI_HEADLESS_SCREEN -nolisten tcp" \
      bash -c '. "$PI_HEADLESS_LIB" && pi_require_private_display || exit 97
               exec "$@"' pi_headless "$@"
}
