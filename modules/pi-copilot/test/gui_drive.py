#!/usr/bin/env python3
"""Drives the PI Copilot panel in test/gui-smoke.sh's Xvfb PixInsight (XTest
via python-xlib) and screenshots each state into OUT:

  01-panel.png        the panel with the journey strip for pcGuiM42
  02-steps.png        the steps dialog (click on the strip)
  03-settings.png     the settings dialog with the journey fields
  04-keep-confirm.png the Keep journey confirm box (answered No)
  05-declined.png     the chat log line for the No
  windows.txt         the top-level windows seen at each state

Positions come from the panel's X geometry (its layout: 8 px margin, the top
row, then the strip row with Keep journey at its right end)."""
import subprocess, sys, time
from Xlib import X, XK, display
from Xlib.ext import xtest

OUT = sys.argv[-1]
d = display.Display()
root = d.screen().root
log = open(OUT + "/windows.txt", "a")


def windows():
    res = []
    def walk(w, depth):
        try:
            name = w.get_wm_name()
            attrs = w.get_attributes()
            g = w.get_geometry()
            pos = w.translate_coords(root, 0, 0)
            if name and attrs.map_state == X.IsViewable:
                res.append((name, -pos.x, -pos.y, g.width, g.height, w))
            if depth < 8:
                for c in w.query_tree().children:
                    walk(c, depth + 1)
        except Exception:
            pass
    for c in root.query_tree().children:
        walk(c, 0)
    return res


def dump(tag):
    log.write("== %s\n" % tag)
    for n, x, y, w, h, _ in windows():
        log.write("  %r at %d,%d %dx%d\n" % (n, x, y, w, h))


def dump_all(tag):
    """Every named window, mapped or not (diagnostics)."""
    log.write("== all windows: %s\n" % tag)
    def walk(w, depth):
        try:
            n = w.get_wm_name()
            if n:
                a = w.get_attributes(); g = w.get_geometry(); p = w.translate_coords(root, 0, 0)
                log.write("  %s%r map_state=%d at %d,%d %dx%d\n" % ("  " * depth, n, a.map_state, -p.x, -p.y, g.width, g.height))
            for c in w.query_tree().children:
                walk(c, depth + 1)
        except Exception:
            pass
    walk(root, 0)


def find(pred, timeout=10):
    t0 = time.time()
    while time.time() - t0 < timeout:
        for win in windows():
            if pred(win[0]):
                return win
        time.sleep(0.2)
    dump("find failed")
    dump_all("find failed")
    subprocess.run(["import", "-window", "root", OUT + "/find-failed.png"])
    raise SystemExit("window not found")


def click(x, y):
    xtest.fake_input(d, X.MotionNotify, x=x, y=y); d.sync(); time.sleep(0.15)
    xtest.fake_input(d, X.ButtonPress, 1); d.sync(); time.sleep(0.05)
    xtest.fake_input(d, X.ButtonRelease, 1); d.sync()


def key(name):
    kc = d.keysym_to_keycode(XK.string_to_keysym(name))
    xtest.fake_input(d, X.KeyPress, kc); d.sync(); time.sleep(0.05)
    xtest.fake_input(d, X.KeyRelease, kc); d.sync()


def shot(name):
    time.sleep(0.8)
    subprocess.run(["import", "-window", "root", OUT + "/" + name], check=True)


def gone(pred, timeout=10):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if not any(pred(w[0]) for w in windows()):
            return
        time.sleep(0.2)
    dump("still open")
    raise SystemExit("window did not close")


if len(sys.argv) > 2 and sys.argv[1] == "--dismiss":
    # Start-up notices: any viewable top-level window other than the main one gets Return.
    for n, x, y, w, h, _ in windows():
        if n and not n.startswith( "PixInsight (" ) and w < 1400:
            log.write("dismissing %r\n" % n)
            click(x + w // 2, y + 8)
            key("Return")
    sys.exit(0)

dump("start")
shot("00-ready.png")
if len(sys.argv) > 2 and sys.argv[1] == "--quit":
    # File > Quit PixInsight (the images were force-closed: no save prompt).
    click(34, 12)
    time.sleep(1.0)
    shot("07-file-menu.png")
    click(104, 583)
    sys.exit(0)

# The panel is not a native X window (PixInsight draws its interface windows
# inside the main window): its geometry comes from the gui.panel phase.
import json
g = json.load(open(OUT + "/panel.json"))
px, py, pw, ph = g["x"], g["y"], g["w"], g["h"]
if not g["visible"]:
    raise SystemExit("the panel is not visible: %r" % g)
# Measured on the 1920x1080 Xvfb screen (01-panel.png): title bar ~22 px, then
# the top row (mode, Include view, New chat, gear) and the journey strip row.
GEAR = (px + pw - 17, py + 44)
STRIP = (px + 90, py + 67)
KEEP = (px + pw - 55, py + 70)
shot("01-panel.png")

# The dialogs are drawn inside the main window too (no X window to wait
# for): click, give them time to paint, screenshot, answer with Escape
# (steps: Close; settings: Cancel, nothing saved; keep confirm: No).
click(*STRIP)
shot("02-steps.png")
key("Escape")
time.sleep(0.8)

click(*GEAR)
time.sleep(1.5)                                # the settings dialog reads the keyring first
shot("03-settings.png")
key("Escape")
time.sleep(0.8)

click(*KEEP)
shot("04-keep-confirm.png")
key("Escape")                                  # Esc = No (Ruling 17)
time.sleep(0.8)
shot("05-declined.png")
dump("end")
