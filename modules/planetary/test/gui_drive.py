#!/usr/bin/env python3
"""Drives the SERViewer interface in test/gui-smoke.sh's Xvfb PixInsight
(XTest via python-xlib) and checks the screenshots. Test-only.

  gui_drive.py --dismiss OUT          answer start-up notices with Return
  gui_drive.py --open OUT A B         01-empty.png, 01b-dialog.png, 02-loaded.png,
                                      03-last-frame.png, 03b-cfa.png, 03c-debayer-on.png,
                                      04-second-file.png (+ preview.json)
  gui_drive.py --shot OUT NAME        one screenshot
  gui_drive.py --close OUT            the interface's title-bar close button -> 06-closed.png
  gui_drive.py --quit OUT             File > Quit PixInsight (07-file-menu.png)
  gui_drive.py --check OUT [--expect-red] [--expect-rb gt|lt]
                                      pixel checks of the preview region (exit 1 on failure)

PixInsight draws its interface windows inside the main window (no X window to
query), so the preview is found in 01-empty.png: the empty preview is the only
large pure-black rectangle on screen (FramePreview paints black; measured: the
PI 1.9.5 desktop on Xvfb has ~1000 scattered black pixels, all in glyphs). The
interface layout (SERViewerInterface.cpp) puts Open SER... above the preview's
left end, the Debayer check box above its right end and the frame row (label,
slider, spin box) directly below it, 6 px apart; the clicks are placed from the
preview's edges. Native X windows (the Qt file dialog) are found by name.
Every mode but --check refuses (FAIL) unless $DISPLAY is served by Xvfb."""
import json, os, re, subprocess, sys, time
from collections import Counter

try:
    import numpy as np
    from PIL import Image
except ImportError as e:
    raise SystemExit("FAIL: the GUI smoke needs python3 numpy and PIL (%s)" % e)

MODE, OUT = sys.argv[1], sys.argv[2]
DIALOG = "Open SER video"
# Interface title bars, measured (PI 1.9.5, Xvfb): active and inactive.
TITLE_COLOURS = ((86, 100, 175), (110, 110, 110))



def require_private_display():
    """FAIL unless $DISPLAY is served by an Xvfb process: XTest input and
    screenshots on any other display (the desktop's Xwayland, a real X server)
    would act on the user's screen. The same test as pi_require_private_display
    in tools/pi-headless.sh: whoever listens on the display's socket is Xvfb."""
    disp = os.environ.get("DISPLAY", "")
    m = re.fullmatch(r":(\d+)(\.\d+)?", disp)
    if not m:
        raise SystemExit("FAIL: refusing to drive DISPLAY=%r: not a local X display" % disp)
    sock = "/tmp/.X11-unix/X" + m.group(1)
    try:
        listing = subprocess.run(["ss", "-xlpH"], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        raise SystemExit("FAIL: refusing to drive DISPLAY=%s: cannot list the X sockets (%s)" % (disp, e))
    owner = None
    for line in listing.splitlines():
        if re.search(r"[@ ]" + re.escape(sock) + r"( |$)", line):
            u = re.search(r'users:\(\("([^"]+)"', line)
            if u:
                owner = u.group(1)
                break
    if owner != "Xvfb":
        raise SystemExit("FAIL: refusing to drive DISPLAY=%s: it is served by %r, not a private Xvfb"
                         % (disp, owner or "nothing"))


if MODE != "--check":
    require_private_display()
    try:
        from Xlib import X, XK, display
        from Xlib.ext import xtest
    except ImportError as e:
        raise SystemExit("FAIL: the GUI smoke needs python-xlib (python3 -m pip install python-xlib): %s" % e)
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
    # Leave the pointer where it is while PI handles the release: its title-bar buttons
    # test the live cursor position, so moving away at once (park) races the close
    # (measured 2026-10-21: ~1 close in 7 with an immediate park, every time with this pause).
    time.sleep(0.4)


def drag(a, b, steps=12):
    xtest.fake_input(d, X.MotionNotify, x=a[0], y=a[1]); d.sync(); time.sleep(0.3)
    xtest.fake_input(d, X.ButtonPress, 1); d.sync(); time.sleep(0.1)
    for i in range(1, steps + 1):
        xtest.fake_input(d, X.MotionNotify, x=a[0] + (b[0] - a[0]) * i // steps,
                         y=a[1] + (b[1] - a[1]) * i // steps); d.sync(); time.sleep(0.03)
    xtest.fake_input(d, X.ButtonRelease, 1); d.sync(); time.sleep(0.4)


def park():
    # Pointer onto empty workspace, so no tooltip covers the preview in a screenshot.
    xtest.fake_input(d, X.MotionNotify, x=1900, y=200); d.sync()


def press(keycode, modifier=None):
    mod_kc = d.keysym_to_keycode(XK.string_to_keysym(modifier)) if modifier else None
    if mod_kc:
        xtest.fake_input(d, X.KeyPress, mod_kc)
    xtest.fake_input(d, X.KeyPress, keycode); d.sync(); time.sleep(0.02)
    xtest.fake_input(d, X.KeyRelease, keycode)
    if mod_kc:
        xtest.fake_input(d, X.KeyRelease, mod_kc)
    d.sync(); time.sleep(0.02)


def key(name, modifier=None):
    press(d.keysym_to_keycode(XK.string_to_keysym(name)), modifier)


def type_text(text):
    # Printable Latin-1 keysyms equal their code points; Shift when the
    # character is the keycode's second symbol (Xvfb's default US map).
    for ch in text:
        ks = ord(ch)
        kc = d.keysym_to_keycode(ks)
        if kc == 0 or ks > 0xff:
            raise SystemExit("FAIL: cannot type %r on the Xvfb keyboard (path %r)" % (ch, text))
        press(kc, "Shift_L" if d.keycode_to_keysym(kc, 0) != ks else None)


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


def rgb(png):
    return np.asarray(Image.open(png).convert("RGB")).astype(np.int32)


def find_preview(png, min_w=350, min_h=250):
    """(x0, y0, x1, y1), exclusive ends: the tallest block of consecutive rows
    that each hold a pure-black horizontal span at least min_w wide; x0/x1 are
    the most common span ends (black glyphs touching an edge lengthen some rows)."""
    black = rgb(png).sum(axis=2) == 0
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


def preview_box():
    g = json.load(open(OUT + "/preview.json"))
    return g["x0"], g["y0"], g["x1"], g["y1"]


def open_file(path, first):
    click(*OPEN)
    park()
    wait_for(dialog_open, 15, "the '%s' file dialog after clicking Open SER..." % DIALOG)
    time.sleep(0.8)
    if first:
        shot("01b-dialog.png", 0)
    # The name field has focus; select all so a pre-filled name is replaced, not appended to.
    key("a", "Control_L")
    type_text(path)
    time.sleep(0.5)
    key("Return")
    wait_for(lambda: not dialog_open(), 15, "the file dialog to close after Return (%s)" % path)


def close_button(png):
    """Centre of the interface's title-bar close button, from the preview's
    position: the title bar is the nearest block of rows above the preview
    that are mostly title colour across the preview's width (title text and
    icons cover the rest), and the close button sits 12 px inside the
    interface's right edge (where the client background beside the preview's
    top ends), as measured on CurvesTransformation (title bar 764..1162,
    close glyph centre at 1150)."""
    a = rgb(png)
    x0, y0, x1, y1 = preview_box()
    band = a[:, x0:x1]
    title = np.zeros(band.shape[:2], dtype=bool)
    for c in TITLE_COLOURS:
        title |= (np.abs(band - np.array(c)) <= 6).all(axis=2)
    rows = title.mean(axis=1) >= 0.4
    y = y0 - 1
    while y > max(0, y0 - 200) and not rows[y]:
        y -= 1
    bottom = y
    while y > 0 and rows[y]:
        y -= 1
    top = y + 1
    if not rows[bottom] or bottom - top < 10:
        raise SystemExit("FAIL: no interface title bar found within 200 px above the preview")
    ty = (top + bottom) // 2
    bg = a[y0 - 3, x1 + 2]
    x = x1 + 2
    while x < a.shape[1] - 1 and np.abs(a[y0 - 3, x] - bg).max() <= 4:
        x += 1
    cx = x - 12
    win = a[ty - 5:ty + 6, cx - 5:cx + 6]
    if not (win.min(axis=2) >= 220).any():   # glyph: white when active, #E8E8E8 when inactive
        raise SystemExit("FAIL: no close-button glyph near (%d, %d) (title bar rows %d..%d, right edge %d)"
                         % (cx, ty, top, bottom, x))
    log.write("close button at %d,%d (title bar rows %d..%d, right edge %d)\n" % (cx, ty, top, bottom, x))
    return cx, ty


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

if MODE == "--close":
    park()
    shot("pre-close.png", 1.0)
    click(*close_button(OUT + "/pre-close.png"))
    park()
    shot("06-closed.png", 1.5)
    sys.exit(0)

if MODE == "--quit":
    # File > Quit PixInsight (measured on the 1920x1080 Xvfb screen, PI 1.9.5).
    click(34, 12)
    time.sleep(1.0)
    shot("07-file-menu.png", 0)
    click(104, 583)
    sys.exit(0)

if MODE == "--open":
    path_a, path_b = sys.argv[3], sys.argv[4]
    dump("start")
    # The Process Console starts slid out over the left of the workspace (an auto-hide
    # panel reaching under the interface's Open SER... button); a click elsewhere only
    # collapses it. Spend that click on empty workspace first.
    click(1500, 950)
    park()
    shot("01-empty.png", 1.5)
    box = find_preview(OUT + "/01-empty.png")
    if box is None:
        raise SystemExit("FAIL: no empty preview (a >=350x250 pure-black rectangle) in 01-empty.png")
    x0, y0, x1, y1 = box
    json.dump({"x0": x0, "y0": y0, "x1": x1, "y1": y1}, open(OUT + "/preview.json", "w"))
    log.write("preview at %r\n" % (box,))
    OPEN = (x0 + 24, y0 - 14)            # Open SER... sits above the preview's left end
    DEBAYER = (x1 - 20, y0 - 14)         # the Debayer label ends above its right end
    HANDLE = (x0 + 47, y1 + 18)          # the slider handle at frame 0 (measured: 809,621 for x0=762)

    open_file(path_a, True)
    shot("02-loaded.png", 1.5)

    # Drag the handle past the slider's right end: the last frame, whatever the count. (A
    # click only steps toward the pointer and leaves keyboard focus on Open SER..., so a
    # following End key never reaches the slider.)
    drag(HANDLE, (x1 - 10, HANDLE[1]))
    park()
    shot("03-last-frame.png", 1.5)

    # Debayer off (raw CFA, grey mosaic), then on again (back to 03).
    click(*DEBAYER)
    park()
    shot("03b-cfa.png", 1.5)
    click(*DEBAYER)
    park()
    shot("03c-debayer-on.png", 1.5)

    open_file(path_b, False)
    shot("04-second-file.png", 1.5)
    dump("end")
    sys.exit(0)

if MODE == "--check":
    expect_red = "--expect-red" in sys.argv[3:]
    expect_rb = sys.argv[sys.argv.index("--expect-rb") + 1] if "--expect-rb" in sys.argv[3:] else None
    if expect_rb not in (None, "gt", "lt"):
        raise SystemExit("--expect-rb takes gt or lt")
    x0, y0, x1, y1 = preview_box()
    inset = 2
    region = (x0 + inset, y0 + inset, x1 - inset, y1 - inset)

    def px(name):
        return np.asarray(Image.open(OUT + "/" + name).convert("RGB").crop(region)).astype(np.float64)

    # Thresholds hold for real planetary frames too (Task 5): a small bright
    # disc on black lights little of the preview (Saturn 2026-09-07 at 320x240:
    # 0.52-0.57%, measured 2026-10-21; an empty preview is 0%), and frame-to-frame
    # seeing changes only the pixels around its edges.
    def lit_mask(name):
        return px(name).max(axis=2) > 24

    def changed(a, b):
        return float((np.abs(px(a) - px(b)).max(axis=2) > 12).mean())

    fails = []
    lines = []
    for name in ("02-loaded.png", "03-last-frame.png", "03b-cfa.png", "03c-debayer-on.png",
                 "04-second-file.png", "05-import.png"):
        f = float(lit_mask(name).mean())
        lines.append("%s: %.2f%% of the preview lit" % (name, 100 * f))
        if f < 0.001:
            fails.append("%s: the preview is black (%.2f%% lit)" % (name, 100 * f))
    checks = [("02-loaded.png", "03-last-frame.png", "frame 0 vs the last frame", "differ"),
              ("03-last-frame.png", "03b-cfa.png", "debayered vs raw CFA", "differ"),
              ("03-last-frame.png", "03c-debayer-on.png", "debayer on again vs before", "same"),
              ("03-last-frame.png", "04-second-file.png", "fixture A vs fixture B", "differ"),
              ("03-last-frame.png", "05-import.png", "slider at the end vs ImportProcess of the last frame", "same"),
              ("05-import.png", "06-closed.png", "interface open vs closed", "differ")]
    for a, b, what, want in checks:
        v = changed(a, b)
        lines.append("%s vs %s (%s): %.3f%% of the preview changed" % (a, b, what, 100 * v))
        if want == "differ" and v < 0.002:
            fails.append("%s: %s and %s do not differ (%.3f%% changed)" % (what, a, b, 100 * v))
        if want == "same" and v > 0.0:
            fails.append("%s: %s and %s differ (%.3f%% changed)" % (what, a, b, 100 * v))

    # Debayer off shows one channel replicated: grey everywhere.
    cfa = px("03b-cfa.png")
    spread = float((cfa.max(axis=2) - cfa.min(axis=2)).max())
    lines.append("03b-cfa.png: largest channel spread %.0f (grey: <= 2)" % spread)
    if spread > 2:
        fails.append("03b-cfa.png: the raw CFA preview is not grey (channel spread %.0f)" % spread)

    if expect_red:
        # bayer-colour.ser: bright red sites, dim green, dark blue. An R/B swap
        # anywhere between the debayer and the screen turns this blue.
        img = px("03-last-frame.png")
        m = lit_mask("03-last-frame.png")
        r, g, b = (float(img[..., c][m].mean()) for c in range(3))
        lines.append("03-last-frame.png: mean R/G/B over lit pixels %.0f/%.0f/%.0f (red: R >= 2G, R >= 2B)" % (r, g, b))
        if not (r >= 2 * g and r >= 2 * b):
            fails.append("03-last-frame.png: not red-dominant (R/G/B %.0f/%.0f/%.0f)" % (r, g, b))

    if expect_rb:
        # Real data (Task 5): the scene is not strongly red, but its R/B balance is known from the
        # raw sensor samples (avi_to_ser.py: expected.json gui.rbOrder). An R/B swap anywhere
        # between the debayer and the screen flips it. The preview scales all channels by one factor.
        img = px("03-last-frame.png")
        m = lit_mask("03-last-frame.png")
        r, b = (float(img[..., c][m].mean()) for c in (0, 2))
        ratio = r / b if b else float("inf")
        lines.append("03-last-frame.png: mean R/B over lit pixels %.0f/%.0f = %.2f (expected R %s B)" % (r, b, ratio, "above" if expect_rb == "gt" else "below"))
        if (expect_rb == "gt" and not ratio >= 1.05) or (expect_rb == "lt" and not ratio <= 1 / 1.05):
            fails.append("03-last-frame.png: R/B order wrong (R/B %.2f, expected R %s B by at least 5%%)" % (ratio, "above" if expect_rb == "gt" else "below"))

    print("\n".join("  " + l for l in lines))
    open(OUT + "/checks.txt", "w").write("\n".join(lines + ["FAIL: " + f for f in fails]) + "\n")
    if fails:
        print("\n".join("FAIL: " + f for f in fails))
        sys.exit(1)
    sys.exit(0)

raise SystemExit("usage: gui_drive.py --dismiss|--open|--shot|--close|--quit|--check OUT ...")
