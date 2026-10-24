#!/usr/bin/env python3
"""Drives the SERViewer interface in test/gui-smoke.sh's Xvfb PixInsight, and
the PlanetaryStack interface in test/gui-smoke-stack.sh's (XTest via
python-xlib), and checks the screenshots. Test-only.

  gui_drive.py --dismiss OUT          answer start-up notices with Return
  gui_drive.py --open OUT A B         01-empty.png, 01b-dialog.png, 02-loaded.png,
                                      03-last-frame.png, 03b-cfa.png, 03c-debayer-on.png,
                                      04-second-file.png (+ preview.json)
  gui_drive.py --open-avi OUT AVI SER Open... the AVI -> 08-avi.png; the Bayer pattern combo
                                      one item down (click, Down, Return) -> 09-bayer.png;
                                      Save as SER... (09b-save-dialog.png) to SER, typed without
                                      .ser -> 10-saved.png; Open... the AVI again -> 11-reopened.png
  gui_drive.py --save-pattern OUT AVI Open... AVI (no sidecar) -> 12-nosidecar.png; Bayer pattern
                                      RGGB -> 13-rggb.png; Debayer off -> 13a-debayer-off.png, on
                                      again; the last frame -> 13c-last-frame.png; Save pattern...,
                                      the confirmation
                                      (13b-confirm.png), Return -> 14-pattern-saved.png; Open...
                                      it again -> 15-reopened-sidecar.png
  gui_drive.py --shot OUT NAME        one screenshot
  gui_drive.py --close OUT            the interface's title-bar close button -> 06-closed.png
  gui_drive.py --quit OUT             File > Quit PixInsight (07-file-menu.png)
  gui_drive.py --check OUT [--expect-red] [--expect-avi-red] [--avi-is-a] [--expect-rb gt|lt]
                                      pixel checks of the preview region and the Save pattern...
                                      button's box (exit 1 on failure)

  PlanetaryStack (gui-smoke-stack.sh; screenshot names there):
  gui_drive.py --stack-open OUT FILE  01-opened.png (+ preview.json); Open... FILE -> 02-loaded.png
  gui_drive.py --stack-run OUT        Stack! mid-run, the preview at Fit / 100% / 200% / zoomed in /
                                      dragged / Fit again, Execute (03 ... 07)
  gui_drive.py --stack-unreadable OUT    Execute on an unreadable video: the error box, the stack kept (07b, 07c)
  gui_drive.py --stack-execute-again OUT Execute again, the video readable (07d)
  gui_drive.py --stack-cancel OUT     Stack! mid-run, Cancel (08, 09)
  gui_drive.py --stack-file OUT FILE NAME   Open... FILE, Stack!, NAME once it ends
  gui_drive.py --stack-bayer OUT AVI  Auto, then BGGR chosen and stacked (13, 13b, 14)
  gui_drive.py --stack-pattern OUT AVI   RGGB chosen, stacked, Save pattern..., Yes (15, 15b, 16)
  gui_drive.py --stack-close OUT FILE    Open... FILE, Stack! mid-run, the close button (17, 18)
  gui_drive.py --stack-quit OUT       the relaunched interface in place, Stack! mid-run (18b, 19)
  gui_drive.py --check-stack OUT      pixel checks of the preview, progress bar and Save pattern...
                                      boxes (exit 1 on failure)

PixInsight draws its interface windows inside the main window (no X window to
query), so the preview is found in 01-empty.png: the empty preview is the only
large pure-black rectangle on screen (FramePreview paints black; measured: the
PI 1.9.5 desktop on Xvfb has ~1000 scattered black pixels, all in glyphs). The
interface layout (SERViewerInterface.cpp) puts two rows of controls above the
preview (Open... / Debayer, then Bayer pattern [Save pattern...] / Save as SER...) and the frame
row (label, slider, spin box) directly below it; the clicks are placed from the
preview's edges (controls()). Native X windows (the Qt file dialogs, the Save pattern...
confirmation) are found by name. PlanetaryStack's controls are placed the same
way (stack_controls()); whether a stack is running is read from its progress
bar's fill colour.
Every mode but --check and --check-stack refuses (FAIL) unless $DISPLAY is served by Xvfb."""
import json, os, re, subprocess, sys, time
from collections import Counter

try:
    import numpy as np
    from PIL import Image
except ImportError as e:
    raise SystemExit("FAIL: the GUI smoke needs python3 numpy and PIL (%s)" % e)

MODE, OUT = sys.argv[1], sys.argv[2]
OPEN_DIALOG = "Open planetary video"
SAVE_DIALOG = "Save as SER"
CONFIRM_DIALOG = "SERViewer"   # Save pattern...'s MessageBox (interface windows have no X window)
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


if MODE not in ("--check", "--check-stack"):
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


def dialog_open(caption=OPEN_DIALOG):
    return any(w[0] == caption for w in windows())


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


def controls(box):
    """Click targets of the interface, from the preview's edges (x0, y0, x1, y1),
    measured from screenshots of the layout in SERViewerInterface.cpp: row 1
    Open... (left) / Debayer (right), row 2 the Bayer pattern combo (left) /
    Save as SER... (right), then the preview, then the frame row."""
    # Measured 2026-10-21 (PI 1.9.5, Xvfb) with the preview at (762, 317, 1162, 617):
    # Open... x 762..855 y 263..283; Debayer label ends at 1160 (y 273); Bayer pattern
    # combo x 848..959 y 290..310; Save as SER... x 1056..1161 y 291..311. Re-measured for 0.2.1.0
    # (the interface keeps room for Save pattern..., so the preview is 749..1175 wide): Save
    # pattern... x 953..1062 y 290..310, i.e. x0+204..x0+313, y0-27..y0-7 (PATTERN_BOX).
    x0, y0, x1, y1 = box
    return {
        "open": (x0 + 24, y0 - 44),          # Open... above the preview's left end, row 1
        "debayer": (x1 - 20, y0 - 44),       # the Debayer label ends above its right end, row 1
        "bayer": (x0 + 140, y0 - 17),        # the Bayer pattern combo, row 2
        "save": (x1 - 40, y0 - 17),          # Save as SER... above the preview's right end, row 2
        "pattern": (x0 + 258, y0 - 17),      # Save pattern..., right of the combo, row 2 (when shown)
        "handle": (x0 + 47, y1 + 18),        # the slider handle at frame 0 (measured: 809,621 for x0=762)
    }


def file_dialog(button, caption, path, first_shot=None):
    """Clicks `button`, waits for the Qt file dialog `caption`, types `path`
    into its name field and presses Return."""
    click(*button)
    park()
    wait_for(lambda: dialog_open(caption), 15, "the '%s' file dialog" % caption)
    time.sleep(0.8)
    if first_shot:
        shot(first_shot, 0)
    # The name field has focus; select all so a pre-filled name is replaced, not appended to.
    key("a", "Control_L")
    type_text(path)
    time.sleep(0.5)
    key("Return")
    wait_for(lambda: not dialog_open(caption), 15, "the '%s' dialog to close after Return (%s)" % (caption, path))


def open_file(path, first):
    file_dialog(CTL["open"], OPEN_DIALOG, path, "01b-dialog.png" if first else None)


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


def opened_shot(name):
    """The first screenshot of an interface just opened empty: its empty preview
    is found in it (find_preview) and recorded in preview.json for later modes.
    The Process Console starts slid out over the left of the workspace (an
    auto-hide panel reaching under the interface's Open... button); a click
    elsewhere only collapses it, so that click is spent on empty workspace first."""
    click(1500, 950)
    park()
    shot(name, 1.5)
    box = find_preview(OUT + "/" + name)
    if box is None:
        raise SystemExit("FAIL: no empty preview (a >=350x250 pure-black rectangle) in %s" % name)
    json.dump({"x0": box[0], "y0": box[1], "x1": box[2], "y1": box[3]}, open(OUT + "/preview.json", "w"))
    log.write("preview at %r\n" % (box,))
    return box


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
    x0, y0, x1, y1 = box = opened_shot("01-empty.png")
    CTL = controls(box)

    open_file(path_a, True)
    shot("02-loaded.png", 1.5)

    # Drag the handle past the slider's right end: the last frame, whatever the count. (A
    # click only steps toward the pointer and leaves keyboard focus on Open..., so a
    # following End key never reaches the slider.)
    drag(CTL["handle"], (x1 - 10, CTL["handle"][1]))
    park()
    shot("03-last-frame.png", 1.5)

    # Debayer off (raw CFA, grey mosaic), then on again (back to 03).
    click(*CTL["debayer"])
    park()
    shot("03b-cfa.png", 1.5)
    click(*CTL["debayer"])
    park()
    shot("03c-debayer-on.png", 1.5)

    open_file(path_b, False)
    shot("04-second-file.png", 1.5)
    dump("end")
    sys.exit(0)

if MODE == "--open-avi":
    # After the import step: the interface is open on fixture A (bayerPattern Auto).
    path_avi, out_ser = sys.argv[3], sys.argv[4]
    CTL = controls(preview_box())
    dump("open-avi start")
    open_file(path_avi, False)
    shot("08-avi.png", 1.5)

    # The combo shows Auto; one item down is None (mono): the raw CFA, grey. The click
    # opens the combo's list, Down moves its highlight, Return picks it. No screenshot
    # can show the list here: measured 2026-10-21, it is mapped (an override-redirect
    # 112x106 window under the combo) but stacked BELOW PixInsight's 32-bit interface
    # window on this WM-less Xvfb, and the combo still reads Auto after Down (the
    # highlight moved inside the hidden list); 09-bayer.png shows the result.
    click(*CTL["bayer"])
    time.sleep(0.5)
    key("Down")
    time.sleep(0.2)
    key("Return")
    park()
    shot("09-bayer.png", 1.5)

    # Typed WITHOUT its .ser suffix: Save as SER must add it.
    if not out_ser.endswith(".ser"):
        raise SystemExit("FAIL: --open-avi needs an output path ending in .ser, got %r" % out_ser)
    file_dialog(CTL["save"], SAVE_DIALOG, out_ser[:-4], "09b-save-dialog.png")
    wait_for(lambda: os.path.exists(out_ser) and not os.path.exists(out_ser + ".partial"), 30,
             "%s to be written (and %s.partial gone)" % (out_ser, out_ser))
    if os.path.exists(out_ser[:-4]):
        raise SystemExit("FAIL: Save as SER also wrote %s (no suffix)" % out_ser[:-4])
    park()
    shot("10-saved.png", 1.5)

    # Open... a file again: the Bayer pattern goes back to Auto (Ruling 9), so the
    # same AVI shows red again, exactly as 08.
    open_file(path_avi, False)
    shot("11-reopened.png", 1.5)
    dump("open-avi end")
    sys.exit(0)

if MODE == "--save-pattern":
    # After --open-avi: the interface shows the AVI with a sidecar (Save pattern... hidden).
    # Open... an AVI with NO sidecar: Save pattern... appears, disabled under Auto.
    path_avi = sys.argv[3]
    sidecar = path_avi + ".txt"
    if os.path.lexists(sidecar):
        raise SystemExit("FAIL: --save-pattern needs an AVI without a sidecar; %s exists" % sidecar)
    CTL = controls(preview_box())
    dump("save-pattern start")
    open_file(path_avi, False)
    shot("12-nosidecar.png", 1.5)

    # Auto -> None (mono) -> RGGB: two items down (see --open-avi for why the list is not
    # visible in a screenshot). The button is enabled once a pattern is chosen.
    click(*CTL["bayer"])
    time.sleep(0.5)
    key("Down")
    time.sleep(0.2)
    key("Down")
    time.sleep(0.2)
    key("Return")
    park()
    shot("13-rggb.png", 1.5)

    # Debayer off: the raw CFA shows no colour to judge the pattern by, so the button is
    # disabled again; then on again (back to 13).
    click(*CTL["debayer"])
    park()
    shot("13a-debayer-off.png", 1.5)
    click(*CTL["debayer"])
    park()

    # The last frame, so the reopen after the write must keep it (14 == 13c, not frame 0).
    drag(CTL["handle"], (preview_box()[2] - 10, CTL["handle"][1]))
    park()
    shot("13c-last-frame.png", 1.5)

    # Save pattern... -> the confirmation (a native window captioned SERViewer) -> Return (Yes
    # is its default button) -> the sidecar is written and the AVI reopened with Auto.
    click(*CTL["pattern"])
    park()
    wait_for(lambda: dialog_open(CONFIRM_DIALOG), 15, "the '%s' confirmation" % CONFIRM_DIALOG)
    time.sleep(0.8)
    shot("13b-confirm.png", 0)
    key("Return")
    wait_for(lambda: not dialog_open(CONFIRM_DIALOG), 15, "the confirmation to close after Return")
    wait_for(lambda: os.path.exists(sidecar), 15, "%s to be written" % sidecar)
    if os.path.exists(sidecar + ".partial"):
        raise SystemExit("FAIL: %s.partial left behind" % sidecar)
    park()
    shot("14-pattern-saved.png", 1.5)

    # Open... the same AVI again: its sidecar is read (Auto -> RGGB), no Save pattern... button.
    open_file(path_avi, False)
    shot("15-reopened-sidecar.png", 1.5)
    dump("save-pattern end")
    sys.exit(0)

# ---- PlanetaryStack (test/gui-smoke-stack.sh) ------------------------------------------------

# The progress bar's fill (StackProgressBar in PlanetaryStackInterface.cpp paints it in exactly
# this colour; nothing else on the PI 1.9.5 desktop on Xvfb uses it, measured).
FILL = (58, 110, 165)
STACK_CONFIRM = "PlanetaryStack"   # Save pattern...'s MessageBox in the PlanetaryStack interface


def stack_controls(box):
    """Click targets and boxes of the PlanetaryStack interface, from the preview's
    edges (x0, y0, x1, y1), measured from screenshots of the layout in
    PlanetaryStackInterface.cpp: row 1 above the preview, Open... / Bayer pattern
    combo / Save pattern...; row 2, Stack! / the progress bar / Cancel; below the
    preview the zoom row (Fit, 100%, 200%, zoom out, zoom in), the text box, and
    Execute at the bottom right."""
    x0, y0, x1, y1 = box
    return {k: (x0 + dx if dx >= 0 else x1 + dx, (y0 if base == "top" else y1) + dy)
            for k, (dx, base, dy) in STACK_OFFSETS.items()}


# (x offset: >= 0 from the preview's left edge, < 0 from its right edge; "top" / "bottom":
# the preview edge the y offset is from). Measured 2026-10-22 (PI 1.9.5, Xvfb) with the preview at
# (695, 305, 1230, 605): Open... x 695..788 y 251..270; the Bayer combo x 892..1004; Stack! x 695..788
# y 278..297; the bar x 794..1130 y 277..298; Cancel x 1137..1230; the zoom row y 612..631 (Fit,
# 100%, 200%, -, + at x 695, 792, 889, 986, 1083, each 93 wide); Execute x 1137..1230, 249 px below
# the preview (the info box 200 px high).
STACK_OFFSETS = {
    "open": (46, "top", -45),
    "bayer": (253, "top", -45),
    "pattern": (361, "top", -45),
    "stack": (46, "top", -18),
    "cancel": (-47, "top", -18),
    "fit": (46, "bottom", 16),
    "z100": (143, "bottom", 16),
    "z200": (240, "bottom", 16),
    "zoomout": (337, "bottom", 16),
    "zoomin": (434, "bottom", 16),
    "execute": (-47, "bottom", 249),
}
# The progress bar's inside and Save pattern...'s box (x0, y0, x1, y1 relative as above).
BAR_BOX = ((101, "top", -25), (-102, "top", -9))
# Inside Stack! (x 697..786, y 279..296): its label is black (0) when enabled, grey (128 at its
# darkest) while a stack runs, measured.
STACK_BOX = ((2, "top", -26), (91, "top", -9))
PATTERN_BOX = ((317, "top", -52), (405, "top", -37))


def rel_box(spec, box):
    (ax, abase, ay), (bx, bbase, by) = spec
    x0, y0, x1, y1 = box
    px = lambda dx: x0 + dx if dx >= 0 else x1 + dx
    py = lambda base, dy: (y0 if base == "top" else y1) + dy
    return px(ax), py(abase, ay), px(bx), py(bbase, by)


def grab(region=None):
    """The screen (or a region of it) as an RGB array, straight from X (no PNG)."""
    x, y, w, h = (0, 0, 1920, 1080) if region is None else (region[0], region[1],
                                                              region[2] - region[0], region[3] - region[1])
    raw = root.get_image(x, y, w, h, X.ZPixmap, 0xffffffff)
    a = np.frombuffer(raw.data, dtype=np.uint8).reshape(h, w, 4)
    return a[..., [2, 1, 0]].astype(np.int32)   # BGRX on a 24-bit TrueColor Xvfb


def bar_fill(box, png=None):
    """Fraction of the progress bar's width painted in the fill colour (its middle row), live
    from the screen or from a screenshot."""
    b = rel_box(BAR_BOX, box)
    a = grab(b) if png is None else rgb(png)[b[1]:b[3], b[0]:b[2]]
    row = a[a.shape[0] // 2]
    return float((np.abs(row - np.array(FILL)) <= 3).all(axis=1).mean())


def stack_enabled(box):
    """Stack! is enabled (no run): its label's darkest pixel is black, live from the screen."""
    return int(grab(rel_box(STACK_BOX, box)).max(axis=2).min()) < 64


def idle(box):
    return bar_fill(box) == 0.0 and stack_enabled(box)


def holds_video(samples=4, every=0.1):
    """PixInsight (PLANETARY_PI_PID, from gui-smoke-stack.sh) holds a .ser / .avi open in any of `samples`
    looks `every` s apart: a run is reading its video. (The interface itself never keeps one open; the
    engine opens readers per pass, so the looks span the short gaps between passes.)"""
    fd_dir = "/proc/%s/fd" % os.environ["PLANETARY_PI_PID"]
    for i in range(samples):
        if i:
            time.sleep(every)
        for fd in os.listdir(fd_dir):
            try:
                if os.readlink(os.path.join(fd_dir, fd)).endswith((".ser", ".avi")):
                    return True
            except OSError:
                pass  # closed between listdir and readlink
    return False


def stack_mid_run(name):
    """Stack!, then `name` shot once the bar shows the run under way; FAIL if the shot shows
    the run (nearly) over."""
    click(*CTL["stack"])
    park()
    wait_for(lambda: bar_fill(BOX) > 0.02, 15, "the progress bar to fill (Stack! running)")
    shot(name, 0)
    f = bar_fill(BOX, OUT + "/" + name)
    log.write("%s: bar %.1f%% full\n" % (name, 100 * f))
    if not 0.02 < f < 0.97:
        raise SystemExit("FAIL: %s does not show the run under way (bar %.0f%% full)" % (name, 100 * f))


def stack_and_wait(mid_shot=None, timeout=120):
    """Stack! (with mid_shot: shot mid-run, stack_mid_run), then waits until the bar is empty and Stack!
    is enabled again (the run ended) and at least 1.5 s have passed since the click (a small capture stacks
    in well under one tick of the interface's progress timer, so its bar may never fill)."""
    t0 = time.time()
    if mid_shot:
        stack_mid_run(mid_shot)
    else:
        click(*CTL["stack"])
        park()
    while time.time() - t0 < 1.5:
        time.sleep(0.1)
    wait_for(lambda: idle(BOX), timeout, "the run to end (the progress bar empty, Stack! enabled)")


def combo_down(n):
    # The combo's list is mapped below PI's interface window on this WM-less Xvfb (see
    # --open-avi): it is driven blind, Down n times, Return.
    click(*CTL["bayer"])
    time.sleep(0.5)
    for _ in range(n):
        key("Down")
        time.sleep(0.2)
    key("Return")
    park()


if MODE.startswith("--stack-"):
    if MODE == "--stack-open":
        BOX = opened_shot("01-opened.png")
    else:
        BOX = preview_box()
    CTL = stack_controls(BOX)
    dump(MODE)

if MODE == "--stack-open":
    file_dialog(CTL["open"], OPEN_DIALOG, sys.argv[3])
    shot("02-loaded.png", 1.5)
    sys.exit(0)

if MODE == "--stack-run":
    stack_and_wait("03-progress.png")
    shot("04-fit.png", 1.0)
    for name, ctl in (("05-zoom-100.png", "z100"), ("06-zoom-200.png", "z200"), ("06b-zoom-in.png", "zoomin")):
        click(*CTL[ctl])
        park()
        shot(name, 1.0)
    # Drag from the preview's centre toward its top-left corner: the view follows the pointer.
    cx, cy = (BOX[0] + BOX[2]) // 2, (BOX[1] + BOX[3]) // 2
    drag((cx, cy), (cx - 120, cy - 90))
    park()
    shot("06c-pan.png", 1.0)
    click(*CTL["fit"])
    park()
    shot("06d-fit-again.png", 1.0)
    click(*CTL["execute"])
    park()
    shot("07-execute.png", 1.5)
    sys.exit(0)

if MODE == "--stack-unreadable":
    # Final review, module m4: Execute while the video cannot be read (gui-smoke-stack.sh made it
    # unreadable) -> ReadInfo's error box, Return; the stack shown stays (07c), nothing opens.
    click(*CTL["execute"])
    park()
    wait_for(lambda: dialog_open(STACK_CONFIRM), 15, "the '%s' error box" % STACK_CONFIRM)
    time.sleep(0.8)
    shot("07b-unreadable.png", 0)
    key("Return")
    wait_for(lambda: not dialog_open(STACK_CONFIRM), 15, "the error box to close after Return")
    park()
    shot("07c-kept.png", 1.0)
    sys.exit(0)

if MODE == "--stack-execute-again":
    # The video readable again: Execute opens the stack kept, without stacking again.
    click(*CTL["execute"])
    park()
    shot("07d-execute-kept.png", 1.5)
    sys.exit(0)

if MODE == "--stack-cancel":
    stack_mid_run("08-cancel-progress.png")
    click(*CTL["cancel"])
    park()
    wait_for(lambda: idle(BOX), 15, "the run to stop after Cancel (the progress bar empty, Stack! enabled)")
    shot("09-cancelled.png", 1.0)
    sys.exit(0)

if MODE == "--stack-shade":
    # PixInsight's title-bar shade button (left of close, measured 21 px: glyphs at 1206 and 1227
    # in 03-progress.png) collapses the window to its title bar. A collapsed window is not a closed
    # one: collapsed mid-run, the run must go on and its stack be shown once the window is expanded.
    # Whether the collapse landed mid-run is read when it lands -- PixInsight holds the video open only
    # while a run reads it (holds_video) -- not inferred from how long a stack takes (bench6: the 7 s
    # minimum stack time this step needed failed on a faster machine). The window's return to its place
    # is read from its frame (the title bar's close button where it was), whatever the preview shows by
    # then: the run may have ended while the window was collapsed. A run that ended before the collapse
    # landed (between the mid-run shot and the click) proves nothing: the step is run again, at most 5 times.
    for attempt in range(1, 6):
        stack_mid_run("09b-shade-progress.png")
        cx, cy = close_button(OUT + "/09b-shade-progress.png")
        click(cx - 21, cy)
        running = holds_video()
        park()
        shot("09c-shaded.png", 1.5)
        # Collapsed, PixInsight moves the window to a bare title bar (~194 x 26) elsewhere (measured:
        # mid-screen): found by the title colour; its expand button is 34 px inside its right end,
        # the close button 13.
        a = rgb(OUT + "/09c-shaded.png")
        # The active title colour only: the inactive one is a neutral grey, as is much of the desktop.
        title = (np.abs(a - np.array(TITLE_COLOURS[0])) <= 6).all(axis=2)
        title[:80] = False     # the menu and tool bars
        title[1040:] = False   # the status bar
        title[:, :40] = False  # the side panels' tabs (blue icons)
        ys, xs = np.nonzero(title)
        if len(xs) < 500 or xs.max() - xs.min() > 400:
            raise SystemExit("FAIL: no collapsed title bar found in 09c-shaded.png")
        log.write("collapsed title bar at x %d..%d, y %d..%d\n" % (xs.min(), xs.max(), ys.min(), ys.max()))
        click(int(xs.max()) - 34, (int(ys.min()) + int(ys.max())) // 2)
        park()
        shot("09c2-expanded.png", 1.5)
        back = close_button(OUT + "/09c2-expanded.png")
        if back != (cx, cy):
            raise SystemExit("FAIL: the window did not expand back in place (09c2-expanded.png: close button at %r, "
                             "before collapsing %r)" % (back, (cx, cy)))
        wait_for(lambda: idle(BOX), 120, "the run to end after the window was expanded (the progress bar empty, Stack! enabled)")
        shot("09d-unshaded.png", 1.0)
        log.write("--stack-shade attempt %d: the run %s when the window collapsed\n"
                  % (attempt, "was reading the video" if running else "had ended"))
        if running:
            break
    else:
        raise SystemExit("FAIL: in 5 attempts the window never collapsed while the run was reading the video")
    sys.exit(0)

if MODE == "--stack-file":
    file_dialog(CTL["open"], OPEN_DIALOG, sys.argv[3])
    stack_and_wait()
    shot(sys.argv[4], 1.0)
    sys.exit(0)

if MODE == "--stack-bayer":
    file_dialog(CTL["open"], OPEN_DIALOG, sys.argv[3])
    stack_and_wait()
    shot("13-bayer-auto.png", 1.0)
    combo_down(5)          # Auto -> None (mono) -> RGGB -> GRBG -> GBRG -> BGGR
    shot("13b-bayer-changed.png", 1.5)
    stack_and_wait()
    shot("14-bayer-bggr.png", 1.0)
    sys.exit(0)

if MODE == "--stack-pattern":
    path_avi = sys.argv[3]
    file_dialog(CTL["open"], OPEN_DIALOG, path_avi)
    combo_down(2)          # Auto -> None (mono) -> RGGB
    stack_and_wait()
    shot("15-pattern-rggb.png", 1.0)
    click(*CTL["pattern"])
    park()
    wait_for(lambda: dialog_open(STACK_CONFIRM), 15, "the '%s' confirmation" % STACK_CONFIRM)
    time.sleep(0.8)
    shot("15b-confirm.png", 0)
    key("Return")
    wait_for(lambda: not dialog_open(STACK_CONFIRM), 15, "the confirmation to close after Return")
    wait_for(lambda: os.path.exists(path_avi + ".txt"), 15, "%s.txt to be written" % path_avi)
    park()
    shot("16-pattern-saved.png", 1.5)
    sys.exit(0)

if MODE == "--stack-close":
    file_dialog(CTL["open"], OPEN_DIALOG, sys.argv[3])
    stack_mid_run("17-close-progress.png")
    click(*close_button(OUT + "/17-close-progress.png"))
    park()
    shot("18-closed.png", 1.5)
    sys.exit(0)

if MODE == "--stack-quit":
    # Relaunched by IPC with filePath set (ImportProcess loads it, nothing stacked): PI puts the
    # interface back where it was, so its empty preview must be exactly the recorded one.
    park()
    shot("18b-relaunched.png", 1.5)
    if find_preview(OUT + "/18b-relaunched.png") != BOX:
        raise SystemExit("FAIL: the relaunched interface's preview is not at %r (found %r)"
                         % (BOX, find_preview(OUT + "/18b-relaunched.png")))
    stack_mid_run("19-quit-progress.png")
    sys.exit(0)

if MODE == "--check":
    expect_red = "--expect-red" in sys.argv[3:]
    expect_avi_red = "--expect-avi-red" in sys.argv[3:]
    avi_is_a = "--avi-is-a" in sys.argv[3:]
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
                 "04-second-file.png", "05-import.png", "08-avi.png", "09-bayer.png", "10-saved.png",
                 "11-reopened.png"):
        f = float(lit_mask(name).mean())
        lines.append("%s: %.2f%% of the preview lit" % (name, 100 * f))
        if f < 0.001:
            fails.append("%s: the preview is black (%.2f%% lit)" % (name, 100 * f))
    checks = [("02-loaded.png", "03-last-frame.png", "frame 0 vs the last frame", "differ"),
              ("03-last-frame.png", "03b-cfa.png", "debayered vs raw CFA", "differ"),
              ("03-last-frame.png", "03c-debayer-on.png", "debayer on again vs before", "same"),
              ("03-last-frame.png", "04-second-file.png", "fixture A vs fixture B", "differ"),
              ("03-last-frame.png", "05-import.png", "slider at the end vs ImportProcess of the last frame", "same"),
              ("05-import.png", "06-closed.png", "interface open vs closed", "differ"),
              ("05-import.png", "08-avi.png", "the imported last frame of A vs the opened AVI's frame 0", "differ"),
              ("08-avi.png", "09-bayer.png", "Bayer pattern Auto vs None (mono)", "differ"),
              ("09-bayer.png", "10-saved.png", "before vs after Save as SER (the viewer stays on the AVI)", "same"),
              ("08-avi.png", "11-reopened.png", "the AVI vs the AVI opened again (Bayer pattern back to Auto)", "same")]
    if avi_is_a:
        # bayer-colour.avi holds bayer-colour.ser's frames and its sidecar resolves to the
        # same RGGB: frame 0 of each, debayered, must look identical.
        checks.append(("02-loaded.png", "08-avi.png", "the AVI decodes as bayer-colour.ser frame 0", "same"))
    for a, b, what, want in checks:
        v = changed(a, b)
        lines.append("%s vs %s (%s): %.3f%% of the preview changed" % (a, b, what, 100 * v))
        if want == "differ" and v < 0.002:
            fails.append("%s: %s and %s do not differ (%.3f%% changed)" % (what, a, b, 100 * v))
        if want == "same" and v > 0.0:
            fails.append("%s: %s and %s differ (%.3f%% changed)" % (what, a, b, 100 * v))

    # Debayer off, and Bayer pattern None (mono), show one channel replicated: grey everywhere.
    for name, what in (("03b-cfa.png", "the raw CFA preview"), ("09-bayer.png", "the Bayer None (mono) preview")):
        cfa = px(name)
        spread = float((cfa.max(axis=2) - cfa.min(axis=2)).max())
        lines.append("%s: largest channel spread %.0f (grey: <= 2)" % (name, spread))
        if spread > 2:
            fails.append("%s: %s is not grey (channel spread %.0f)" % (name, what, spread))

    # bayer-colour.ser / .avi: bright red sites, dim green, dark blue. An R/B swap
    # anywhere between the debayer and the screen turns this blue.
    for name, wanted in (("03-last-frame.png", expect_red), ("08-avi.png", expect_avi_red)):
        if not wanted:
            continue
        img = px(name)
        m = lit_mask(name)
        r, g, b = (float(img[..., c][m].mean()) for c in range(3))
        lines.append("%s: mean R/G/B over lit pixels %.0f/%.0f/%.0f (red: R >= 2G, R >= 2B)" % (name, r, g, b))
        if not (r >= 2 * g and r >= 2 * b):
            fails.append("%s: not red-dominant (R/G/B %.0f/%.0f/%.0f)" % (name, r, g, b))

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

    # Save pattern... (Increment 2.1): the button's own screen box (controls(), measured), in the
    # whole screenshot, not the preview. Hidden: the row's plain background, as in 08 (an AVI with
    # a sidecar); shown disabled under Auto in 12, enabled once RGGB is chosen in 13 (and on the
    # last frame, 13c), disabled again with Debayer off (13a, the same pixels as 12); hidden again
    # once its sidecar exists (14 after the write, 15 reopened).
    bx0, by0, bx1, by1 = x0 + 206, y0 - 25, x0 + 311, y0 - 9

    def button(name):
        return np.asarray(Image.open(OUT + "/" + name).convert("RGB").crop((bx0, by0, bx1, by1))).astype(np.float64)

    def button_changed(a, b):
        return float((np.abs(button(a) - button(b)).max(axis=2) > 12).mean())

    for name in ("12-nosidecar.png", "13-rggb.png", "13a-debayer-off.png", "13c-last-frame.png",
                 "14-pattern-saved.png", "15-reopened-sidecar.png"):
        f = float(lit_mask(name).mean())
        lines.append("%s: %.2f%% of the preview lit" % (name, 100 * f))
        if f < 0.001:
            fails.append("%s: the preview is black (%.2f%% lit)" % (name, 100 * f))
    for a, b, what, want in (
            ("12-nosidecar.png", "13-rggb.png", "Auto (no sidecar: raw CFA) vs RGGB chosen", "differ"),
            ("13-rggb.png", "13c-last-frame.png", "RGGB frame 0 vs the last frame", "differ"),
            ("13c-last-frame.png", "14-pattern-saved.png",
             "the last frame chosen vs after the write (sidecar read back, frame kept)", "same"),
            ("13-rggb.png", "15-reopened-sidecar.png", "RGGB frame 0 vs Open... again (frame 0, Auto = RGGB)",
             "same")):
        v = changed(a, b)
        lines.append("%s vs %s (%s): %.3f%% of the preview changed" % (a, b, what, 100 * v))
        if want == "differ" and v < 0.002:
            fails.append("%s: %s and %s do not differ (%.3f%% changed)" % (what, a, b, 100 * v))
        if want == "same" and v > 0.0:
            fails.append("%s: %s and %s differ (%.3f%% changed)" % (what, a, b, 100 * v))
    cfa = px("13a-debayer-off.png")
    spread = float((cfa.max(axis=2) - cfa.min(axis=2)).max())
    lines.append("13a-debayer-off.png: largest channel spread %.0f (grey: <= 2)" % spread)
    if spread > 2:
        fails.append("13a-debayer-off.png: Debayer off is not grey (channel spread %.0f)" % spread)
    for a, b, what, want in (
            ("08-avi.png", "12-nosidecar.png", "hidden (AVI with a sidecar) vs shown (none)", "differ"),
            ("12-nosidecar.png", "13-rggb.png", "disabled under Auto vs enabled for RGGB", "differ"),
            ("12-nosidecar.png", "13a-debayer-off.png", "disabled under Auto vs disabled with Debayer off", "same"),
            ("13-rggb.png", "13c-last-frame.png", "enabled vs enabled on the last frame", "same"),
            ("08-avi.png", "14-pattern-saved.png", "hidden vs hidden after the write", "same"),
            ("08-avi.png", "15-reopened-sidecar.png", "hidden vs hidden on reopening", "same")):
        v = button_changed(a, b)
        lines.append("Save pattern... box, %s vs %s (%s): %.1f%% changed" % (a, b, what, 100 * v))
        if want == "differ" and v < 0.05:
            fails.append("Save pattern... %s: %s and %s do not differ (%.1f%% changed)" % (what, a, b, 100 * v))
        if want == "same" and v > 0.0:
            fails.append("Save pattern... %s: %s and %s differ (%.1f%% changed)" % (what, a, b, 100 * v))

    print("\n".join("  " + l for l in lines))
    open(OUT + "/checks.txt", "w").write("\n".join(lines + ["FAIL: " + f for f in fails]) + "\n")
    if fails:
        print("\n".join("FAIL: " + f for f in fails))
        sys.exit(1)
    sys.exit(0)

if MODE == "--check-stack":
    # gui-smoke-stack.sh's screenshots: the preview region (inset 2 px), the progress bar and
    # the Save pattern... box, from preview.json and the measured offsets above.
    box = preview_box()
    x0, y0, x1, y1 = box
    region = (x0 + 2, y0 + 2, x1 - 2, y1 - 2)

    def crop(name, r):
        return np.asarray(Image.open(OUT + "/" + name).convert("RGB").crop(r)).astype(np.float64)

    def px(name):
        return crop(name, region)

    def lit(name):
        return float((px(name).max(axis=2) > 24).mean())

    def changed(a, b, r=region):
        return float((np.abs(crop(a, r) - crop(b, r)).max(axis=2) > 12).mean())

    def spread(name):
        p = px(name)
        return float((p.max(axis=2) - p.min(axis=2)).max())

    def rb(name):
        p = px(name)
        m = p.max(axis=2) > 24
        return float(p[..., 0][m].mean()), float(p[..., 2][m].mean())

    fails, lines = [], []

    def need(ok, text):
        lines.append(("ok    " if ok else "FAIL  ") + text)
        if not ok:
            fails.append(text)

    # Nothing stacked: the preview black. 01 is where it was found (pure black by definition).
    # A dropped stack says why (13b: "Bayer pattern changed: click Stack! ..."): no stack drawn,
    # only the grey note.
    need(0 < lit("13b-bayer-changed.png") < 0.03 and spread("13b-bayer-changed.png") <= 2,
         "13b-bayer-changed.png: the stack is dropped, a grey note says why (%.2f%% lit, channel spread %.0f)"
         % (100 * lit("13b-bayer-changed.png"), spread("13b-bayer-changed.png")))
    # Final review, module m4: Execute on a video that could not be read (07b) kept the stack shown (07c).
    need(lit("07c-kept.png") > 0.05, "07c-kept.png: the stack stays after Execute on an unreadable video "
         "(%.1f%% lit)" % (100 * lit("07c-kept.png")))
    # Collapsing the window mid-run (09c) is not closing it: the run went on to its stack (09d).
    need(lit("09d-unshaded.png") > 0.05, "09d-unshaded.png: the run went on while collapsed; its stack is shown "
         "(%.1f%% lit)" % (100 * lit("09d-unshaded.png")))
    # The info box never has the focus (no focus ring, so no caret): the ring is (135, 179, 255),
    # measured, around the box (from 30 px below the preview to Execute's row).
    ibox = (x0 - 3, y1 + 30, x1 + 3, y1 + 236)
    for name in ("02-loaded.png", "03-progress.png", "04-fit.png", "07-execute.png", "08-cancel-progress.png",
                 "09-cancelled.png", "09d-unshaded.png", "10-diagnosis.png", "11-warning-mosaic.png",
                 "12-warning-focus.png", "13b-bayer-changed.png", "14-bayer-bggr.png", "16-pattern-saved.png"):
        ring = int((np.abs(crop(name, ibox) - np.array((135, 179, 255))) <= 6).all(axis=2).sum())
        need(ring == 0, "%s: the info box has no focus ring (%d ring pixels)" % (name, ring))
    for name in ("02-loaded.png", "09-cancelled.png"):
        need(lit(name) == 0.0, "%s: the preview is black (%.2f%% lit)" % (name, 100 * lit(name)))
    # Mid-run: the bar partly filled and, while it runs, no preview.
    for name in ("03-progress.png", "08-cancel-progress.png", "09b-shade-progress.png", "17-close-progress.png",
                 "19-quit-progress.png"):
        f = bar_fill(box, OUT + "/" + name)
        need(0.02 < f < 0.97, "%s: the progress bar is part filled (%.1f%%)" % (name, 100 * f))
        need(lit(name) == 0.0, "%s: no preview while it runs (%.2f%% lit)" % (name, 100 * lit(name)))
    # Not running: the bar empty.
    for name in ("02-loaded.png", "04-fit.png", "07-execute.png", "09-cancelled.png", "09d-unshaded.png", "10-diagnosis.png",
                 "11-warning-mosaic.png", "14-bayer-bggr.png", "16-pattern-saved.png"):
        f = bar_fill(box, OUT + "/" + name)
        need(f == 0.0, "%s: the progress bar is empty (%.1f%%)" % (name, 100 * f))

    # The stack of gui-long.avi (a disc of radius 180 centred in 1024 x 768, RGGB, R gain 1.0, B 0.55).
    f = lit("04-fit.png")
    need(0.05 < f < 0.6, "04-fit.png: the disc lights part of the preview at Fit (%.1f%%)" % (100 * f))
    p = px("04-fit.png")
    ys, xs = np.nonzero(p.max(axis=2) > 24)
    cxp, cyp = xs.mean() / p.shape[1], ys.mean() / p.shape[0]
    need(abs(cxp - 0.5) < 0.05 and abs(cyp - 0.5) < 0.05,
         "04-fit.png: the disc is centred in the preview (centroid at %.2f, %.2f of its size)" % (cxp, cyp))
    r, b = rb("04-fit.png")
    need(r > 1.2 * b, "04-fit.png: the RGGB stack is warm, R above B (mean R/B %.0f/%.0f)" % (r, b))
    # 100 %: the disc (360 px across) at full size in a preview wider than it, so more than twice
    # Fit's share; from 200 % the disc is larger than the preview both ways and fills it.
    need(lit("05-zoom-100.png") > 2 * f, "05-zoom-100.png: at 100%% the disc is larger than at Fit (%.1f%% lit, "
         "Fit %.1f%%)" % (100 * lit("05-zoom-100.png"), 100 * f))
    for name in ("06-zoom-200.png", "06b-zoom-in.png"):
        need(lit(name) > 0.9, "%s: zoomed into the disc, the preview is filled (%.1f%% lit)" % (name, 100 * lit(name)))
    for a, b_, what in (("04-fit.png", "05-zoom-100.png", "Fit vs 100%"), ("05-zoom-100.png", "06-zoom-200.png", "100% vs 200%"),
                        ("06-zoom-200.png", "06b-zoom-in.png", "200% vs zoomed in"),
                        ("06b-zoom-in.png", "06c-pan.png", "zoomed in vs dragged")):
        v = changed(a, b_)
        need(v > 0.01, "%s: %s and %s differ (%.2f%% changed)" % (what, a, b_, 100 * v))
    for a, b_, what in (("04-fit.png", "06d-fit-again.png", "Fit vs Fit again after zoom and pan"),
                        ("06d-fit-again.png", "07-execute.png", "the preview before vs after Execute (no new stack)")):
        v = changed(a, b_)
        need(v == 0.0, "%s: %s and %s are the same (%.3f%% changed)" % (what, a, b_, 100 * v))

    # Diagnosis: no stack drawn -- at most the preview's grey one-line note.
    need(lit("10-diagnosis.png") < 0.03 and spread("10-diagnosis.png") <= 2,
         "10-diagnosis.png: no preview, at most a grey note (%.2f%% lit, channel spread %.0f)"
         % (100 * lit("10-diagnosis.png"), spread("10-diagnosis.png")))
    # Warned stacks: made, and mono (the raw mosaic, the soft mono disc).
    for name in ("11-warning-mosaic.png", "12-warning-focus.png"):
        need(lit(name) > 0.01 and spread(name) <= 2,
             "%s: a grey (mono) stack is shown (%.2f%% lit, channel spread %.0f)" % (name, 100 * lit(name), spread(name)))
    # The Bayer pattern: Auto reads the RG sidecar (warm); BGGR chosen beats it (R and B swap).
    r, b = rb("13-bayer-auto.png")
    need(r > 1.2 * b, "13-bayer-auto.png: Auto (sidecar RG) stacks warm (R/B %.0f/%.0f)" % (r, b))
    r, b = rb("14-bayer-bggr.png")
    need(b > 1.2 * r, "14-bayer-bggr.png: BGGR chosen over the sidecar swaps R and B (R/B %.0f/%.0f)" % (r, b))
    # Save pattern...: shown disabled for the sidecar-less mosaic under Auto (11), enabled with
    # RGGB chosen and its stack shown (15), hidden once the sidecar exists (16, like 13: a sidecar).
    pbox = rel_box(PATTERN_BOX, box)
    for a, b_, what, want in (("13-bayer-auto.png", "11-warning-mosaic.png", "hidden (a sidecar) vs shown (none)", "differ"),
                              ("11-warning-mosaic.png", "15-pattern-rggb.png", "disabled under Auto vs enabled for RGGB", "differ"),
                              ("13-bayer-auto.png", "16-pattern-saved.png", "hidden vs hidden once the sidecar is written", "same")):
        v = changed(a, b_, pbox)
        need(v > 0.05 if want == "differ" else v == 0.0,
             "Save pattern... box, %s: %s vs %s (%.1f%% changed)" % (what, a, b_, 100 * v))
    r, b = rb("15-pattern-rggb.png")
    need(r > 1.2 * b, "15-pattern-rggb.png: the mosaic stacked as RGGB is in colour, warm (R/B %.0f/%.0f)" % (r, b))
    v = changed("15-pattern-rggb.png", "16-pattern-saved.png")
    need(v == 0.0, "16-pattern-saved.png: the stack is kept after Save pattern... (%.3f%% changed)" % (100 * v))
    v = changed("17-close-progress.png", "18-closed.png")
    need(v > 0.5, "18-closed.png: the interface is gone (%.1f%% of the preview region changed)" % (100 * v))

    print("\n".join("  " + l for l in lines))
    open(OUT + "/checks.txt", "w").write("\n".join(lines) + "\n")
    sys.exit(1 if fails else 0)

raise SystemExit("usage: gui_drive.py --dismiss|--open|--open-avi|--save-pattern|--shot|--close|--quit|--check OUT ...\n"
                 "       gui_drive.py --stack-open|--stack-run|--stack-unreadable|--stack-execute-again|--stack-cancel|--stack-file|--stack-bayer|--stack-pattern|"
                 "--stack-close|--stack-quit|--check-stack OUT ...")
