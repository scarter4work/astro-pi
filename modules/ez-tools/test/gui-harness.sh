# shellcheck shell=bash
# EZTools GUI smoke helpers (gui-smoke.sh); source after harness.sh.
# A REAL PixInsight GUI on a private Xvfb display (:100 + slot), isolated slot
# EZT_GUI_SLOT (default 92), driven with XTest by gui_drive.py. Adapted from
# modules/planetary/test/harness.sh's planetary_gui_* (see there for the measured
# reasons: no desktop session variables, a session bus that leads nowhere, start-up
# notices answered with Return). Call order:
#   ezt_gui_setup; ezt_sign; ezt_seed_slot_modules
#   ezt_gui_launch OUT SETUP.js     # Xvfb + PixInsight -m=SO -r=SETUP.js, ready, dev build mapped
#   ... gui_drive.py modes, ezt_gui_ipc SCRIPT.js TAG OUT ...
#   ezt_gui_quit OUT                # File > Quit; PixInsight must exit 0 within 60 s

ezt_gui_setup()
{
   local tool
   for tool in Xvfb xprop import; do
      command -v "$tool" >/dev/null || { echo "FAIL: $tool not found"; return 1; }
   done
   EZT_TEST_SLOT="${EZT_GUI_SLOT:-92}" ezt_slot_setup || return 1
   ezt_private_tmp || return 1
   EZT_DISPLAY_NO=$(( 100 + EZT_SLOT ))
   if [ -e "/tmp/.X11-unix/X$EZT_DISPLAY_NO" ]; then
      echo "FAIL: display :$EZT_DISPLAY_NO is already in use (another GUI smoke on slot $EZT_SLOT?)"; return 1
   fi
   EZT_GUI_XVFB_PID="" EZT_GUI_PI_PID=""
   ezt_on_exit ezt_gui_cleanup
}

ezt_gui_cleanup()
{
   [ -n "${EZT_GUI_PI_PID:-}" ] && kill -9 "$EZT_GUI_PI_PID" 2>/dev/null
   pkill -9 -f -- "^$EZT_PI_BIN_DIR/PixInsight -n=$EZT_SLOT " 2>/dev/null
   [ -n "${EZT_GUI_XVFB_PID:-}" ] && kill "$EZT_GUI_XVFB_PID" 2>/dev/null
   return 0
}

ezt_assert_only_dev_mapped()
{
   local so="$1" pid maps
   pid="$(pgrep -f -- "^$EZT_PI_BIN_DIR/PixInsight -n=$EZT_SLOT " | head -1)"
   [ -n "$pid" ] || { echo "FAIL: no running PixInsight for slot $EZT_SLOT to check"; return 1; }
   maps="$(grep -- 'EZTools-pxm\.so' "/proc/$pid/maps" | awk '{print $NF}' | sort -u | xargs -r -n1 realpath -m | sort -u)"
   if [ "$maps" != "$(realpath -e "$so")" ]; then
      echo "FAIL: PixInsight (slot $EZT_SLOT, pid $pid) maps [${maps//$'\n'/ }], expected ONLY the dev build $so"; return 1
   fi
   echo "only the dev module is mapped (pid $pid): $maps"
}

ezt_gui_launch()
{
   local out="$1" setup="$2" n=0
   Xvfb ":$EZT_DISPLAY_NO" -screen 0 1920x1080x24 -nolisten tcp >"$out/xvfb.log" 2>&1 & EZT_GUI_XVFB_PID=$!
   export DISPLAY=":$EZT_DISPLAY_NO"
   for _ in $(seq 50); do xprop -root >/dev/null 2>&1 && break; sleep 0.1; done

   pi_headless_env
   unset XDG_CURRENT_DESKTOP DESKTOP_SESSION KDE_FULL_SESSION KDE_SESSION_VERSION GNOME_DESKTOP_SESSION_ID \
         QT_QPA_PLATFORMTHEME XDG_SESSION_TYPE
   export DBUS_SESSION_BUS_ADDRESS="unix:path=$EZT_RUN_DIR/no-session-bus"
   export XDG_DATA_HOME="$EZT_RUN_DIR/xdg"; mkdir -m 700 "$XDG_DATA_HOME"
   export EZT_GUI_DIR="$EZT_RUN_DIR/gui"; mkdir -m 700 "$EZT_GUI_DIR"

   pi_require_private_display || return 1   # this run's own Xvfb, never the desktop
   timeout 900 "$PI" -n="$EZT_SLOT" --no-startup-scripts --no-startup-check-updates -m="$SO" -r="$setup" \
      >"$out/pi.log" 2>&1 & EZT_GUI_PI_PID=$!
   for _ in $(seq 360); do
      [ -f "$EZT_GUI_DIR/ready" ] && break
      kill -0 "$EZT_GUI_PI_PID" 2>/dev/null || break
      n=$(( n + 1 ))
      (( n % 10 == 0 )) && { python3 "$HERE/gui_drive.py" --dismiss "$out" || { echo "FAIL: driver (dismiss)"; return 1; }; }
      sleep 0.5
   done
   if [ ! -f "$EZT_GUI_DIR/ready" ]; then
      import -window root "$out/not-ready.png" 2>/dev/null || true
      echo "FAIL: the GUI script never became ready (see $out/pi.log, $out/not-ready.png)"; return 1
   fi
   cp "$EZT_GUI_DIR/ready" "$out/ready.txt"
   grep -q "launchInterface=true" "$out/ready.txt" || { echo "FAIL: setup: $(cat "$out/ready.txt")"; return 1; }
   ezt_assert_only_dev_mapped "$SO" || return 1
   sleep 1
}

# Runs SCRIPT.js inside the running GUI PixInsight (-x IPC) and waits for it to write
# "$EZT_GUI_DIR/<tag>.done" (its last act); fails if it writes "<tag>.error". The script runs
# in the GUI PixInsight's environment (EZT_GUI_DIR), not this shell's: each IPC script has its
# own fixed tag.
ezt_gui_ipc()
{
   local script="$1" tag="$2" out="$3"
   rm -f "$EZT_GUI_DIR/$tag.done" "$EZT_GUI_DIR/$tag.error"
   pi_require_private_display || return 1
   timeout 60 "$PI" -x="$EZT_SLOT:$script" >>"$out/ipc.log" 2>&1 \
      || { echo "FAIL: IPC launch of $script failed or timed out (see $out/ipc.log)"; return 1; }
   for _ in $(seq 240); do
      [ -f "$EZT_GUI_DIR/$tag.done" ] && return 0
      [ -f "$EZT_GUI_DIR/$tag.error" ] && { echo "FAIL: $tag: $(cat "$EZT_GUI_DIR/$tag.error")"; return 1; }
      sleep 0.25
   done
   import -window root "$out/ipc-$tag-timeout.png" 2>/dev/null || true
   echo "FAIL: $tag: the IPC script never finished (60 s; see $out/ipc.log)"; return 1
}

ezt_gui_quit()
{
   local out="$1" rc=0
   python3 "$HERE/gui_drive.py" --quit "$out" || { echo "FAIL: driver (quit)"; return 1; }
   for _ in $(seq 240); do kill -0 "$EZT_GUI_PI_PID" 2>/dev/null || break; sleep 0.25; done
   if kill -0 "$EZT_GUI_PI_PID" 2>/dev/null; then
      import -window root "$out/hang.png" 2>/dev/null || true
      echo "FAIL: PixInsight still running 60 s after Quit (see $out/hang.png)"; return 1
   fi
   wait "$EZT_GUI_PI_PID" || rc=$?
   EZT_GUI_PI_PID=""
   [ "$rc" = 0 ] || { echo "FAIL: PixInsight exit code $rc (see $out/pi.log)"; return 1; }
}
