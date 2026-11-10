#!/usr/bin/env python3
"""Drives the EZHazeKill interface in test/gui-smoke.sh's Xvfb PixInsight (XTest via python-xlib)
and checks the screenshots. Test-only. Adapted from modules/planetary/test/gui_drive.py.

  gui_drive.py --dismiss OUT     answer start-up notices with Return
  gui_drive.py --shot OUT NAME   one screenshot
  gui_drive.py --quit OUT        File > Quit PixInsight

Every mode but --check refuses (FAIL) unless $DISPLAY is served by Xvfb."""
import json, os, re, subprocess, sys, time

try:
    import numpy as np
    from PIL import Image
except ImportError as e:
    raise SystemExit("FAIL: the GUI smoke needs python3 numpy and PIL (%s)" % e)

MODE, OUT = sys.argv[1], sys.argv[2]


def require_private_display():
    """FAIL unless $DISPLAY is served by an Xvfb process (as planetary's gui_drive.py and
    tools/pi-headless.sh's pi_require_private_display): XTest input on any other display would
    act on the user's screen."""
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
        raise SystemExit("FAIL: the GUI smoke needs python-xlib: %s" % e)
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
    time.sleep(0.4)


def park():
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
    for ch in text:
        ks = ord(ch)
        kc = d.keysym_to_keycode(ks)
        if kc == 0 or ks > 0xff:
            raise SystemExit("FAIL: cannot type %r on the Xvfb keyboard" % ch)
        press(kc, "Shift_L" if d.keycode_to_keysym(kc, 0) != ks else None)


def rgb(png):
    return np.asarray(Image.open(OUT + "/" + png).convert("RGB")).astype(int)


if MODE == "--dismiss":
    for n, x, y, w, h in windows():
        if not n.startswith("PixInsight (") and w < 1400:
            log.write("dismissing %r\n" % n)
            click(x + w // 2, y + 8)
            key("Return")
    sys.exit(0)

if MODE == "--shot":
    park()
    shot(sys.argv[3], 1.5)
    dump(sys.argv[3])
    sys.exit(0)

if MODE == "--quit":
    # File > Quit PixInsight (measured on the 1920x1080 Xvfb screen; planetary's gui_drive.py).
    click(34, 12)
    time.sleep(1.0)
    shot("99-file-menu.png", 0)
    click(104, 583)
    sys.exit(0)

# ---------------------------------------------------------------------------------------------------
# Layout, measured on the 1920x1080 Xvfb screen with PixInsight 1.9.6 on a fresh slot (2026-11-10):
# the EZHazeKill interface opens at the workspace's top left (title bar y 62..82, x 29..466), its control
# bar at y 270 (triangle 43, Apply square 68, Real-Time Preview circle 93); the image window opens to its
# right, its title bar at y 73; the Real-Time Preview window opens under the interface (x 28..678, y 62..682)
# and shows the 3840x2160 image at 1:6 in x 60..677, y 195..541. Every step below checks what it clicked
# did what it should (the RTP button's active mark, the rendition's colour), so a moved layout FAILs.
RTP_BUTTON = (93, 270)
APPLY_BUTTON = (68, 270)
AMOUNT_EDIT = (133, 162)
NEUTRALIZE_EDIT = (133, 188)
IMAGE_TITLE = (1000, 73)
RTP_MAXIMIZE = (645, 73)              # the RTP window's title-bar "+"
RTP_RESTORE = (1885, 12)              # a maximized window's restore button
RTP_REGION = (62, 290, 676, 540)      # x0, y0, x1, y1: the rendition below the interface
RTP_MAX_REGION = (480, 300, 1700, 990)  # the maximized RTP's rendition right of / below the interface
VIEW_REGION = (700, 100, 1320, 790)   # the image window's view, right of the RTP window


def region(png, box):
    x0, y0, x1, y1 = box
    return rgb(png)[y0:y1, x0:x1]


def rtp_active(png):
    """The RTP button shows an orange mark while this interface owns the Real-Time Preview."""
    x, y = RTP_BUTTON
    a = rgb(png)[y - 5:y + 6, x - 5:x + 6]
    return bool(((a[..., 0] > 170) & (a[..., 0] - a[..., 2] > 80)).any())


def set_edit(pos, text):
    click(*pos)
    click(*pos)
    key("a", "Control_L")
    type_text(text)
    key("Return")


def activate_image():
    click(*IMAGE_TITLE)
    time.sleep(0.5)


if MODE == "--rtp":
    # The image window active, then the interface's Real-Time Preview button.
    activate_image()
    click(*RTP_BUTTON)
    park()
    shot("02-rtp.png", 5)
    if not rtp_active("02-rtp.png"):
        raise SystemExit("FAIL: the Real-Time Preview button shows no active mark after the click (02-rtp.png)")
    sys.exit(0)

if MODE == "--neutralize":
    set_edit(NEUTRALIZE_EDIT, sys.argv[3])
    park()
    shot(sys.argv[4], 5)
    sys.exit(0)

if MODE == "--zoom":
    # The RTP window has no zoom buttons and ignores the wheel; its rendition's zoom follows its size.
    # Maximize it (title-bar "+"): a new rendition at 1:2 instead of 1:6. Then restore it (the
    # maximized window's restore button at the workspace's top right): 1:6 again.
    click(*RTP_MAXIMIZE)
    park()
    shot("04-rtp-zoomed.png", 6)
    click(*RTP_RESTORE)
    park()
    shot("04b-rtp-unzoomed.png", 6)
    sys.exit(0)

if MODE == "--invert":
    # Another process changes the image under the RTP, from the GUI: Invert (Ctrl+I), then the inverted
    # image's own AutoStretch (Ctrl+A).
    activate_image()
    key("i", "Control_L")
    time.sleep(3)
    key("a", "Control_L")
    park()
    shot("05-rtp-inverted.png", 6)
    sys.exit(0)

if MODE == "--apply":
    activate_image()
    click(*APPLY_BUTTON)
    park()
    shot("06-applied.png", 6)
    sys.exit(0)

if MODE == "--check":
    fails = []
    res = {}

    def stat(name, box):
        r = region(name, box).reshape(-1, 3)
        med = np.median(r, axis=0)
        res[name + str(box)] = dict(mean=r.mean(axis=0).round(1).tolist(), median=med.tolist())
        return r.mean(axis=0), med

    m_a, med_a = stat("02-rtp.png", RTP_REGION)
    m_b, med_b = stat("03-rtp-neutralize0.png", RTP_REGION)
    m_z, med_z = stat("04-rtp-zoomed.png", RTP_MAX_REGION)
    m_u, _ = stat("04b-rtp-unzoomed.png", RTP_REGION)
    m_c, med_c = stat("05-rtp-inverted.png", RTP_REGION)
    m_v, med_v = stat("05-rtp-inverted.png", VIEW_REGION)
    m_o, med_o = stat("01-launched.png", VIEW_REGION)
    # A: Neutralize 1 -- the sky (the median pixel) is neutral grey, and lit (an empty RTP is flat 80 grey).
    if med_a.max() - med_a.min() > 12 or med_a.mean() < 15:
        fails.append("02: the default rendition's sky is not a lit neutral grey (median RGB %s)" % med_a.tolist())
    if region("02-rtp.png", RTP_REGION).std() < 5:
        fails.append("02: the RTP region is flat (no rendition shown)")
    # The original view (no RTP) is green-cast: what Neutralize removes.
    if med_o[1] - med_o[0] < 60:
        fails.append("01: the unprocessed view's sky is not green-cast (median RGB %s)" % med_o.tolist())
    # B: Neutralize 0 -- the cast is back: green sky, like the unprocessed view.
    if med_b[1] - med_b[0] < 60:
        fails.append("03: Neutralize 0 did not bring the green cast back (median RGB %s)" % med_b.tolist())
    da_b = float(np.abs(region("02-rtp.png", RTP_REGION) - region("03-rtp-neutralize0.png", RTP_REGION)).mean())
    res["mean_abs_02_vs_03"] = da_b
    # Zoom: the maximized RTP renders at 1:2 instead of 1:6 -- a new rendition, with the same sky (the
    # correction comes from the full image, so its sky colour does not depend on the zoom); restored,
    # the 1:6 rendition is A again. (Run before Neutralize 0: the neutral rendition cannot be mistaken for
    # the green-cast image window behind it.)
    du = float(np.abs(region("04b-rtp-unzoomed.png", RTP_REGION) - region("02-rtp.png", RTP_REGION)).mean())
    res["mean_abs_04b_vs_02"] = du
    if region("04-rtp-zoomed.png", RTP_MAX_REGION).std() < 5:
        fails.append("04: the maximized RTP shows no rendition")
    if np.abs(med_z - med_a).max() > 8:
        fails.append("04: the 1:2 rendition's sky (median RGB %s) differs from the 1:6 one's (%s)"
                     % (med_z.tolist(), med_a.tolist()))
    if du > 2:
        fails.append("04b: back at 1:6 the rendition differs from 02 (mean |d| %.2f)" % du)
    # C: after Invert + the inverted image's AutoStretch, a sky measured on the NEW image is pushed far
    # below the new shadows clip (black), while the old cached sky would leave the view's look.
    if m_c.max() > 15:
        fails.append("05: after Invert the RTP is not black (mean RGB %s): the cached sky of the old image "
                     "was used, or no rendition was made" % m_c.round(1).tolist())
    if np.abs(region("05-rtp-inverted.png", RTP_REGION).astype(int) - 80).max() == 0:
        fails.append("05: the RTP region is the empty-window grey: no rendition after the change")
    if m_v.mean() < 40:
        fails.append("05: the inverted view itself is dark (mean RGB %s): the discriminator needs it lit" % m_v.round(1).tolist())
    json.dump(res, open(OUT + "/checks.json", "w"), indent=1)
    for f in fails:
        print("  FAIL  " + f)
    for k, v in res.items():
        print("  info  %s: %s" % (k, v))
    sys.exit(1 if fails else 0)

raise SystemExit("FAIL: unknown mode %s" % MODE)
