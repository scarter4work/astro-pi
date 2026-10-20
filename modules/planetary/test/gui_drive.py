#!/usr/bin/env python3
"""Drives the SERViewer interface in test/gui-smoke.sh's Xvfb PixInsight
(XTest via python-xlib) and checks the screenshots. Test-only.

  gui_drive.py --dismiss OUT          answer start-up notices with Return
  gui_drive.py --open OUT A B         01-empty.png, 01b-dialog.png, 02-loaded.png,
                                      03-last-frame.png, 04-second-file.png (+ preview.json)
  gui_drive.py --shot OUT NAME        one screenshot
  gui_drive.py --quit OUT             File > Quit PixInsight
  gui_drive.py --check OUT            pixel checks of the preview region (exit 1 on failure)

PixInsight draws its interface windows inside the main window (no X window to
query), so the preview is found in 01-empty.png: the empty preview is the only
large pure-black rectangle on screen (FramePreview paints black; measured: the
PI 1.9.5 desktop on Xvfb has ~1000 scattered black pixels, all in glyphs). The
interface layout (SERViewerInterface.cpp) puts the Open SER... button directly
above the preview and the frame row (label, slider, spin box) directly below
it, 6 px apart; the clicks are placed from the preview's edges. Native X
windows (the Qt file dialog) are found by name."""
import json, subprocess, sys, time
from collections import Counter

try:
    import numpy as np
    from PIL import Image
except ImportError as e:
    raise SystemExit("FAIL: the GUI smoke needs python3 numpy and PIL (%s)" % e)
from Xlib import X, XK, display
from Xlib.ext import xtest

MODE, OUT = sys.argv[1], sys.argv[2]
DIALOG = "Open SER video"
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
                res.append((name, -pos.x, -pos.y, g.width, g.height))
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
    for n, x, y, w, h in windows():
        log.write("  %r at %d,%d %dx%d\n" % (n, x, y, w, h))


def shot(name, settle=0.8):
    time.sleep(settle)
    subprocess.run(["import", "-window", "root", OUT + "/" + name], check=True)


def click(x, y):
    xtest.fake_input(d, X.MotionNotify, x=x, y=y); d.sync(); time.sleep(0.15)
    xtest.fake_input(d, X.ButtonPress, 1); d.sync(); time.sleep(0.05)
    xtest.fake_input(d, X.ButtonRelease, 1); d.sync()


def park():
    # Pointer onto empty workspace, so no tooltip covers the preview in a screenshot.
    xtest.fake_input(d, X.MotionNotify, x=1900, y=200); d.sync()


def press(keycode, shift=False):
    shift_kc = d.keysym_to_keycode(XK.XK_Shift_L)
    if shift:
        xtest.fake_input(d, X.KeyPress, shift_kc)
    xtest.fake_input(d, X.KeyPress, keycode); d.sync(); time.sleep(0.02)
    xtest.fake_input(d, X.KeyRelease, keycode)
    if shift:
        xtest.fake_input(d, X.KeyRelease, shift_kc)
    d.sync(); time.sleep(0.02)


def key(name):
    press(d.keysym_to_keycode(XK.string_to_keysym(name)))


def type_text(text):
    # Printable Latin-1 keysyms equal their code points; Shift when the
    # character is the keycode's second symbol (Xvfb's default US map).
    for ch in text:
        ks = ord(ch)
        kc = d.keysym_to_keycode(ks)
        if kc == 0 or ks > 0xff:
            raise SystemExit("FAIL: cannot type %r on the Xvfb keyboard (path %r)" % (ch, text))
        press(kc, shift=d.keycode_to_keysym(kc, 0) != ks)


def wait_for(pred, timeout, what):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if pred():
            return
        time.sleep(0.2)
    dump("timeout: " + what)
    shot("timeout.png", 0)
    raise SystemExit("FAIL: timed out (%d s) waiting for %s" % (timeout, what))


def dialog_open():
    return any(w[0] == DIALOG for w in windows())


def find_preview(png, min_w=350, min_h=250):
    """(x0, y0, x1, y1), exclusive ends: the tallest block of consecutive rows
    that each hold a pure-black horizontal span at least min_w wide; x0/x1 are
    the most common span ends (black glyphs touching an edge lengthen some rows)."""
    a = np.asarray(Image.open(png).convert("RGB")).astype(np.int32)
    black = a.sum(axis=2) == 0
    spans = []
    for row in black:
        p = np.concatenate(([0], row.astype(np.int8), [0]))
        dd = np.diff(p)
        starts, ends = np.where(dd == 1)[0], np.where(dd == -1)[0]
        if len(starts) == 0:
            spans.append(None)
            continue
        i = int((ends - starts).argmax())
        spans.append((int(starts[i]), int(ends[i])) if ends[i] - starts[i] >= min_w else None)
    best, y = None, 0
    while y < len(spans):
        if spans[y] is None:
            y += 1
            continue
        y0 = y
        while y < len(spans) and spans[y] is not None:
            y += 1
        if y - y0 >= min_h and (best is None or y - y0 > best[3] - best[1]):
            best = (Counter(s[0] for s in spans[y0:y]).most_common(1)[0][0], y0,
                    Counter(s[1] for s in spans[y0:y]).most_common(1)[0][0], y)
    return best


def open_file(path, tag):
    click(*OPEN)
    park()
    wait_for(dialog_open, 15, "the '%s' file dialog after clicking Open SER..." % DIALOG)
    time.sleep(0.8)
    if tag == "A":
        shot("01b-dialog.png", 0)
    type_text(path)
    time.sleep(0.5)
    key("Return")
    wait_for(lambda: not dialog_open(), 15, "the file dialog to close after Return (%s)" % path)


if MODE == "--dismiss":
    # Start-up notices (e.g. "Risky Temporary Folders"): any viewable
    # top-level window other than the main one gets a click and Return.
    for n, x, y, w, h in windows():
        if not n.startswith("PixInsight (") and w < 1400:
            log.write("dismissing %r\n" % n)
            click(x + w // 2, y + 8)
            key("Return")
    sys.exit(0)

if MODE == "--shot":
    shot(sys.argv[3], 1.5)
    sys.exit(0)

if MODE == "--quit":
    # File > Quit PixInsight (measured on the 1920x1080 Xvfb screen, PI 1.9.5).
    click(34, 12)
    time.sleep(1.0)
    shot("06-file-menu.png", 0)
    click(104, 583)
    sys.exit(0)

if MODE == "--open":
    path_a, path_b = sys.argv[3], sys.argv[4]
    dump("start")
    park()
    shot("01-empty.png", 1.5)
    box = find_preview(OUT + "/01-empty.png")
    if box is None:
        raise SystemExit("FAIL: no empty preview (a >=350x250 pure-black rectangle) in 01-empty.png")
    x0, y0, x1, y1 = box
    json.dump({"x0": x0, "y0": y0, "x1": x1, "y1": y1}, open(OUT + "/preview.json", "w"))
    log.write("preview at %r\n" % (box,))
    OPEN = (x0 + 24, y0 - 14)            # Open SER... sits right above the preview
    SLIDER = ((x0 + x1) // 2, y1 + 18)   # the frame slider spans the middle of the row below

    open_file(path_a, "A")
    shot("02-loaded.png", 1.5)

    # Click the slider (it takes focus and steps toward the click), then End: last frame.
    click(*SLIDER)
    time.sleep(0.5)
    key("End")
    park()
    shot("03-last-frame.png", 1.5)

    open_file(path_b, "B")
    shot("04-second-file.png", 1.5)
    dump("end")
    sys.exit(0)

if MODE == "--check":
    g = json.load(open(OUT + "/preview.json"))
    inset = 2
    region = (g["x0"] + inset, g["y0"] + inset, g["x1"] - inset, g["y1"] - inset)

    def px(name):
        return np.asarray(Image.open(OUT + "/" + name).convert("RGB").crop(region)).astype(np.float64)

    # Thresholds hold for real planetary frames too (Task 5): a small bright
    # disc on black lights a few percent of the preview, and frame-to-frame
    # seeing changes only the pixels around its edges.
    def lit(name):
        return float((px(name).max(axis=2) > 24).mean())

    def changed(a, b):
        return float((np.abs(px(a) - px(b)).max(axis=2) > 12).mean())

    fails = []
    lines = []
    for name in ("02-loaded.png", "03-last-frame.png", "04-second-file.png", "05-import.png"):
        f = lit(name)
        lines.append("%s: %.2f%% of the preview lit" % (name, 100 * f))
        if f < 0.01:
            fails.append("%s: the preview is black (%.2f%% lit)" % (name, 100 * f))
    checks = [("02-loaded.png", "03-last-frame.png", "frame 0 vs the last frame", "differ"),
              ("03-last-frame.png", "04-second-file.png", "fixture A vs fixture B", "differ"),
              ("03-last-frame.png", "05-import.png", "slider End vs ImportProcess of the last frame", "same")]
    for a, b, what, want in checks:
        v = changed(a, b)
        lines.append("%s vs %s (%s): %.3f%% of the preview changed" % (a, b, what, 100 * v))
        if want == "differ" and v < 0.002:
            fails.append("%s: %s and %s do not differ (%.3f%% changed)" % (what, a, b, 100 * v))
        if want == "same" and v > 0.0:
            fails.append("%s: %s and %s differ (%.3f%% changed)" % (what, a, b, 100 * v))
    print("\n".join("  " + l for l in lines))
    open(OUT + "/checks.txt", "w").write("\n".join(lines + ["FAIL: " + f for f in fails]) + "\n")
    if fails:
        print("\n".join("FAIL: " + f for f in fails))
        sys.exit(1)
    sys.exit(0)

raise SystemExit("usage: gui_drive.py --dismiss|--open|--shot|--quit|--check OUT ...")
