#!/usr/bin/env python3
"""Drives the GaiaDepthGrade interface in test/gui-smoke.sh's Xvfb PixInsight (XTest via
python-xlib) and checks it. Test-only tooling (ruling R20): never packaged, nothing shipped
imports it.

  gui_drive.py dismiss OUT              answer start-up notices with Return
  gui_drive.py quit OUT                 File > Quit PixInsight (99-file-menu.png)
  gui_drive.py shot OUT NAME            one screenshot
  gui_drive.py STEP OUT [ARGS]          one GUI smoke step (exit 0 PASS, 1 FAIL):
    controls_and_defaults OUT MASTER    every control's value, label, range and the group
                                        labels; Advanced expanded and collapsed; Browse...
                                        opens MASTER and makes it the target
    prepare_enables_preview OUT         Prepare: QA line, preview + inset painted, buttons on
    slider_marks_preview_stale OUT      a Brightness slider click: orange border; Preview / Update
    detect_edit_marks_prepare_stale OUT detect_threshold_sigma edited: Preview/Execute off;
                                        set back, Prepare again
    inset_click_moves_center OUT        clicks on the preview move the 1:1 inset (clamped)
    execute_creates_depthgraded OUT     Execute: "Done: created <id>_depthgraded"
    close_target_then_execute OUT       (after the shell closed the target) Execute: the
                                        "was closed ... Prepare again" error, no crash
    online_prepare_live OUT             Offline unchecked, Prepare against the ESA archive
    close_button OUT                    the Close button hides the interface (F9)

The interface's state comes from the test build's state file ($GDG_GUI_STATE, written by
GaiaDepthGradeInterface::WriteState in GDG_PROBES builds only): every control's value as the
control itself reports it, and its screen rectangle, so the clicks need no measured offsets.
What the user sees is checked on the screenshots (the preview's orange stale border, the
painted preview and inset). Every mode refuses (FAIL) unless $DISPLAY is served by Xvfb."""
import json, os, re, subprocess, sys, time

try:
    import numpy as np
    from PIL import Image
except ImportError as e:
    raise SystemExit("FAIL: the GUI smoke needs python3 numpy and PIL (%s)" % e)

MODE, OUT = sys.argv[1], sys.argv[2]
STATE = os.environ.get("GDG_GUI_STATE", "")
OPEN_DIALOG = "Open master"   # Browse...'s caption (the dialog's onBrowse)
DASH = "—"
ELLIPSIS = "…"
CLOSED_RE = r"^ERROR: The target image '(.+)' was closed after Prepare\. Select a target and Prepare again\.$"
QA_RE = (r"^Prepared: (\d+)/(\d+) matched \(rate (\d+\.\d\d), offset (\d+\.\d) px\)"
         r"(  ⚠ LOW MATCH — depth grade unreliable)?$")


def require_private_display():
    """FAIL unless $DISPLAY is served by an Xvfb process (the same test as
    pi_require_private_display in tools/pi-headless.sh; Planetary's gui_drive.py)."""
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


require_private_display()
try:
    from Xlib import X, XK, display
    from Xlib.ext import xtest
except ImportError as e:
    raise SystemExit("FAIL: the GUI smoke needs python-xlib (python3 -m pip install python-xlib): %s" % e)
d = display.Display()
root = d.screen().root
log = open(OUT + "/driver.log", "a")


def note(msg):
    log.write(msg + "\n")
    log.flush()
    print("        " + msg)


# ---- X primitives (Planetary's gui_drive.py) -------------------------------------------------

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


def shot(name, settle=0.8):
    time.sleep(settle)
    subprocess.run(["import", "-window", "root", OUT + "/" + name], check=True)
    return OUT + "/" + name


def click(x, y):
    xtest.fake_input(d, X.MotionNotify, x=x, y=y); d.sync(); time.sleep(0.15)
    xtest.fake_input(d, X.ButtonPress, 1); d.sync(); time.sleep(0.05)
    xtest.fake_input(d, X.ButtonRelease, 1); d.sync()
    time.sleep(0.4)


def park():
    # Pointer onto empty workspace, so no tooltip covers the interface in a screenshot.
    xtest.fake_input(d, X.MotionNotify, x=1900, y=1000); d.sync()


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
            raise SystemExit("FAIL: cannot type %r on the Xvfb keyboard (text %r)" % (ch, text))
        press(kc, "Shift_L" if d.keycode_to_keysym(kc, 0) != ks else None)


def wait_for(pred, timeout, what):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if pred():
            return time.time() - t0
        time.sleep(0.2)
    shot("timeout-%s.png" % MODE, 0)
    raise SystemExit("FAIL: timed out (%d s) waiting for %s (screenshot timeout-%s.png; state %s)"
                     % (timeout, what, MODE, json.dumps(state_or_none())[:600]))


def ocr_words(png, scale=2):
    """Tesseract's words on a whole screenshot, as (text, x, y, w, h) in screen pixels."""
    img = Image.open(png).convert("L")
    img = img.resize((img.width * scale, img.height * scale), Image.LANCZOS)
    tmp = OUT + "/ocr-input.png"
    img.save(tmp)
    r = subprocess.run(["tesseract", tmp, "stdout", "--psm", "11", "tsv"], capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("FAIL: tesseract failed on %s: %s" % (png, r.stderr.strip()))
    words = []
    for line in r.stdout.splitlines()[1:]:
        f = line.split("\t")
        if len(f) == 12 and f[11].strip():
            words.append((f[11].strip(), int(f[6]) // scale, int(f[7]) // scale, int(f[8]) // scale, int(f[9]) // scale))
    return words


def file_name_field():
    """PixInsight 1.9.6's own file dialog is drawn inside the main window (no X window to find
    by name): its "File name:" label is found on a screenshot; the field starts right of it.
    None while the dialog is not on screen."""
    words = ocr_words(shot("ocr-dialog.png", 0))
    for i, (t, x, y, w, h) in enumerate(words):
        if t == "name:" and i > 0 and words[i - 1][0] == "File" and abs(words[i - 1][2] - y) <= 4:
            return x + w + 120, y + h // 2
    return None


def rgb(png):
    return np.asarray(Image.open(png).convert("RGB")).astype(np.int32)


# ---- the interface's state -------------------------------------------------------------------

def state_or_none():
    try:
        with open(STATE) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def state():
    """The latest state the interface wrote (it rewrites the file on every change and twice a
    second; a missing or half-renamed file is retried)."""
    for _ in range(50):
        s = state_or_none()
        if s is not None:
            return s
        time.sleep(0.1)
    raise SystemExit("FAIL: no interface state at %r (GDG_GUI_STATE; a GDG_BUILD_PROBES build?)" % STATE)


def ctl(s, name):
    c = s["controls"].get(name)
    if c is None:
        raise SystemExit("FAIL: the state has no control %r" % name)
    return c


def centre(rect):
    x, y, w, h = rect
    return x + w // 2, y + h // 2


def click_ctl(name, s=None):
    s = s or state()
    c = ctl(s, name)
    if not c["visible"]:
        raise SystemExit("FAIL: %s is not visible (%r)" % (name, c))
    click(*centre(c["rect"]))


def edit_value(name, text):
    """Types `text` into a NumericEdit's edit field (select all first) and presses Return."""
    c = ctl(state(), name)
    click(*centre(c["edit"]))
    key("a", "Control_L")
    type_text(text)
    key("Return")
    park()


def actions():
    return state().get("actions", [])


def run_action(button, expect, timeout, while_busy=None):
    """Clicks `button` and waits until the interface recorded the actions `expect` (e.g.
    ["prepare ok", "preview ok"]) after the click and is idle again. Returns the new actions.
    `while_busy(state)`, when given, runs once the interface reports busy (FAIL if it never does)."""
    s = state()
    if not ctl(s, button)["enabled"]:
        raise SystemExit("FAIL: %s is disabled (prepared %r, status %r)" % (button, s["prepared"], s["status"]))
    n0 = len(s.get("actions", []))
    click_ctl(button, s)
    park()
    if while_busy:
        seen = []
        wait_for(lambda: seen.append(state_or_none() or {}) or seen[-1].get("busy", False), 10, "%s to go busy" % button)
        while_busy(seen[-1])
    def done():
        s = state_or_none() or {}
        a = s.get("actions", [])
        if s.get("busy", True) or len(a) <= n0:
            return False
        new = a[n0:]
        # Finished: as many actions as expected, or an error (nothing follows a failed action).
        return len(new) >= len(expect) or any(x.endswith(" error") for x in new)
    t = wait_for(done, timeout, "%s to finish" % button)
    new = actions()[n0:]
    note("%s -> %s in %.1f s; status %r" % (button, new, t, state()["status"]))
    if new[:len(expect)] != expect:
        for line in state()["statusHistory"][-20:]:
            note("  status: " + line)
        raise SystemExit("FAIL: %s gave actions %r, expected %r (status %r)" % (button, new, expect, state()["status"]))
    return new


# ---- screenshot checks -----------------------------------------------------------------------

def orange_fraction(png, rect):
    """Fraction of the preview control's top border row (2 px pen at y=1) that is the stale
    border's orange (0xa0ff8000 over the picture: red well above green, blue low)."""
    a = rgb(png)
    x, y, w, h = rect
    row = a[y + 1, x + 8:x + w - 8]
    orange = (row[:, 0] >= 140) & (row[:, 0] - row[:, 2] >= 100) & (row[:, 0] - row[:, 1] >= 40)
    return float(orange.mean())


def region_stats(png, rect, inset=6):
    a = rgb(png)
    x, y, w, h = rect
    r = a[y + inset:y + h - inset, x + inset:x + w - inset]
    return float(r.mean()), float(r.std())


def expect(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)
    note("ok " + what)


def expect_buttons(s, prepare, preview, execute):
    got = (ctl(s, "prepare")["enabled"], ctl(s, "preview")["enabled"], ctl(s, "execute")["enabled"])
    expect(got == (prepare, preview, execute),
           "buttons Prepare/Preview/Execute enabled %r (want %r)" % (got, (prepare, preview, execute)))


# ---- modes -----------------------------------------------------------------------------------

if MODE == "dismiss":
    # Start-up notices (e.g. "Risky Temporary Folders"): any viewable top-level window other
    # than the main one gets a click and Return.
    for n, x, y, w, h in windows():
        if not n.startswith("PixInsight (") and w < 1400:
            log.write("dismissing %r\n" % n)
            click(x + w // 2, y + 8)
            key("Return")
    sys.exit(0)

if MODE == "shot":
    shot(sys.argv[3], 1.0)
    sys.exit(0)

if MODE == "quit":
    # File > Quit PixInsight (measured on the 1920x1080 Xvfb screen; Planetary's and PI Copilot's
    # drivers). Every image window was force-closed first (gui-smoke.sh): no save prompt.
    click(34, 12)
    time.sleep(1.0)
    shot("99-file-menu.png", 0)
    click(104, 583)
    sys.exit(0)

LIVE_DEFAULTS = [
    # name, label, value, edit text, range (the dialog's liveSlider / prepEdit, F12-F24)
    ("brightness", "Brightness:", 0.65, "0.65", (0, 2)),
    ("size", "Size:", 0.85, "0.85", (0, 2)),
    ("contrast", "Contrast:", 0.12, "0.12", (0, 2)),
    ("saturation", "Saturation:", 0.4, "0.40", (0, 2)),
    ("haloSigma", "Halo sigma:", 2.0, "2.00", (0.5, 6)),
    ("pLow", "p_low:", 5.0, "5.0", (0, 50)),
    ("pHigh", "p_high:", 95.0, "95", (50, 100)),   # non-fixed precision: decimals = 1 - floor(log10 95) = 0, as PJSR's NumericEdit
    ("nebulaStrength", "Nebula depth:", 1.0, "1.00", (0, 2)),
    ("nebulaAtmos", "  atmospheric:", 1.0, "1.00", (0, 2)),
    ("nebulaStructure", "  structure:", 1.0, "1.00", (0, 2)),
    ("detect", "detect_threshold_sigma:", 6.0, "6.0", (1, 30)),
    ("match", "match_tolerance_px:", 4.0, "4.0", (1, 12)),
]
ADVANCED_DEFAULTS = [
    # the engine's config.py defaults (plan Global Constraints) and the G limit 18 (GDG_MAG_LIMIT)
    ("haloGain", 1.0), ("detectFWHM", 3.0), ("minMatchRate", 0.3), ("neutral", 0.0), ("magLimit", 18.0),
    ("parallaxSNR", 5.0), ("haloRadiusSigma", 8.0), ("haloMoffatBeta", 2.5),
]
OFFLINE_TIP = ("Checked: cone-query the local Gaia DR3 database (instant, offline; parallax distances). "
               "Unchecked: the online Bailer-Jones catalogue (more sources, but the ESA archive can be slow "
               "or down).")


def step_controls_and_defaults(master):
    # The Process Console starts slid out over the left of the workspace, under the interface
    # (an auto-hide panel; PixInsight draws an inactive interface translucent, so the console text
    # shows through it: 2026-11-12 01-opened.png). A click on empty workspace collapses it; a
    # click on the interface's status line then activates the interface.
    click(1500, 950)
    click(*centre(ctl(state(), "status")["rect"]))
    park()
    s = state()
    expect(s["interfaceVisible"] and s["windowTitle"] == "Gaia Depth Grade",
           "the interface is open, titled 'Gaia Depth Grade' (%r)" % s["windowTitle"])
    # F1: the default target is the active window (gui-smoke-setup.js made gdg_gui_dummy active).
    expect(s["target"] == "gdg_gui_dummy" and s["targetList"] == "gdg_gui_dummy",
           "default target = the active window gdg_gui_dummy (target %r, list %r)" % (s["target"], s["targetList"]))
    expect(s["status"] == "Target: gdg_gui_dummy %s click Prepare." % DASH, "status %r" % s["status"])
    expect(not s["prepared"] and not s["busy"], "not prepared, idle")
    expect_buttons(s, True, False, False)
    lab = s["labels"]
    expect(lab == {"live": "Look (live %s Preview to apply)" % DASH, "prep": "Matching (changing these needs re-Prepare)",
                   "inset": "1:1 inset", "target": "Target:"}, "group and row labels verbatim (%r)" % lab)
    for name, label, value, text, (lo, hi) in LIVE_DEFAULTS:
        c = ctl(s, name)
        expect(c["label"] == label and abs(c["value"] - value) < 1e-9 and c["text"] == text
               and abs(c["range"][0] - lo) < 1e-9 and abs(c["range"][1] - hi) < 1e-9,
               "%s: label %r value %r text %r range %r" % (name, c["label"], c["value"], c["text"], c["range"]))
    neb, off = ctl(s, "nebula"), ctl(s, "offline")
    expect(neb["text"] == "Grade the nebula too (depth from structure, scaled to Gaia)" and not neb["checked"],
           "nebula checkbox %r, unchecked" % neb["text"])
    expect(off["text"] == "Offline (local Gaia DR3) %s fast" % DASH and off["checked"] and off["toolTip"] == OFFLINE_TIP,
           "offline checkbox %r, checked, tooltip verbatim" % off["text"])
    texts = {b: ctl(s, b)["text"] for b in ("prepare", "preview", "execute", "close", "browse")}
    expect(texts == {"prepare": "Prepare", "preview": "Preview / Update", "execute": "Execute", "close": "Close",
                     "browse": "Browse" + ELLIPSIS}, "button texts %r" % texts)
    pv, ins = ctl(s, "previewControl")["rect"], ctl(s, "insetControl")["rect"]
    expect(pv[2:] == [760, 420] and ins[2:] == [256, 256], "preview 760x420 (%r), inset 256x256 (%r)" % (pv[2:], ins[2:]))
    for name, value in ADVANCED_DEFAULTS:
        expect(abs(ctl(s, name)["value"] - value) < 1e-9, "Advanced %s = %r" % (name, ctl(s, name)["value"]))
    expect(ctl(s, "magLimitEnabled")["checked"] and ctl(s, "haloMinRefs")["value"] == 8
           and ctl(s, "splitMode")["item"] == 0 and ctl(s, "outputMode")["item"] == 0,
           "Advanced: G limit on, halo refs 8, split StarXTerminator, output new window")
    expect(not ctl(s, "advanced")["expanded"], "Advanced starts collapsed")

    # The screen agrees with the state's geometry: the empty preview is the #101010 fill, with the
    # orange stale border (a target is selected, so the preview is stale, as the dialog's).
    png = shot("01-opened.png", 1.0)
    # PixInsight draws interface windows slightly translucent, so the #101010 fill reads a few
    # levels lighter over the workspace (measured mean 17-20): dark and flat inside the state's
    # rectangle, the light dialog background in the 8 px gap right of it (before the inset).
    m, sd = region_stats(png, pv, 10)
    gap, _ = region_stats(png, [pv[0] + pv[2] + 2, pv[1] + 20, 4, 200], 0)
    expect(m < 30 and sd < 3 and gap > 150, "empty preview painted dark at the state's rectangle (mean %.1f, sd %.1f; "
           "dialog background beside it %.1f)" % (m, sd, gap))
    of = orange_fraction(png, pv)
    expect(of > 0.9, "orange stale border on the empty preview (%.2f of the top row)" % of)

    # Advanced: expand, then collapse (the section bar's title).
    click_ctl("advanced")
    park()
    wait_for(lambda: ctl(state(), "advanced")["expanded"], 10, "Advanced to expand")
    s = state()
    expect(ctl(s, "splitMode")["visible"] and ctl(s, "haloGain")["visible"], "Advanced expanded: its controls visible")
    shot("01b-advanced.png", 1.0)
    click_ctl("advanced")
    park()
    wait_for(lambda: not ctl(state(), "advanced")["expanded"], 10, "Advanced to collapse")
    expect(not ctl(state(), "splitMode")["visible"], "Advanced collapsed again")

    # Review M4: picking <No View Selected> (the list's first item) clears the target.
    click_ctl("targetList")
    time.sleep(0.6)
    key("Home")
    key("Return")
    park()
    wait_for(lambda: (state_or_none() or {}).get("target") == "", 10, "the target to clear")
    s = state()
    expect(s["status"] == "Pick a target and click Prepare." and not s["prepared"], "no target: status %r" % s["status"])
    expect_buttons(s, False, False, False)
    shot("01d-no-target.png", 1.0)

    # F2: Browse... opens the master as a window and makes it the target.
    click_ctl("browse")
    park()
    field = []
    wait_for(lambda: field.append(file_name_field()) or field[-1] is not None, 30, "the '%s' file dialog" % OPEN_DIALOG)
    shot("01c-browse-dialog.png", 0)
    click(*field[-1])
    key("a", "Control_L")
    type_text(master)
    time.sleep(0.5)
    key("Return")
    park()
    wait_for(lambda: (state_or_none() or {}).get("target") not in (None, "", "gdg_gui_dummy"), 180, "the opened master as the target")
    time.sleep(2.0)   # anything the view lists do after the open must not take the target away
    s = state()
    expect(s["targetList"] == s["target"] and s["status"] == "Target: %s %s click Prepare." % (s["target"], DASH),
           "Browse... made %r the target (list %r, status %r)" % (s["target"], s["targetList"], s["status"]))
    expect_buttons(s, True, False, False)
    shot("02-browsed.png", 1.5)
    open(OUT + "/target.txt", "w").write(s["target"])


def session(s):
    if "session" not in s:
        raise SystemExit("FAIL: nothing is prepared (status %r)" % s["status"])
    return s["session"]


def check_prepared(tag):
    s = state()
    qa = [l for l in s["statusHistory"] if l.startswith("Prepared:")]
    expect(qa and re.match(QA_RE, qa[-1]), "QA summary line %r" % (qa[-1] if qa else None))
    m = re.match(QA_RE, qa[-1])
    se = session(s)
    expect(int(m.group(1)) == se["nMatched"] and int(m.group(2)) == se["nDetected"] and se["nDetected"] > 100,
           "QA numbers = the session's (%s/%s matched)" % (se["nMatched"], se["nDetected"]))
    expect(s["status"] == "Preview updated. Adjust the look, Preview again, or Execute.", "status %r" % s["status"])
    expect(s["prepared"] and not s["previewStale"], "prepared, preview fresh")
    expect_buttons(s, True, True, True)
    pw, ph = s["previewBitmap"][:2]
    # Review M9: fitted inside the 760 x 420 control, aspect kept, one side at its bound.
    fit_w, fit_h = (760, round(se["height"] * 760 / se["width"])) if se["width"] * 420 >= se["height"] * 760 \
        else (round(se["width"] * 420 / se["height"]), 420)
    expect((pw, ph) == (min(fit_w, se["width"]), min(fit_h, se["height"])) and s["insetBitmap"][:2] == [256, 256],
           "preview bitmap %dx%d fits 760x420 (want %dx%d), inset bitmap %r" % (pw, ph, fit_w, fit_h, s["insetBitmap"][:2]))
    # Review M9: the QA summary (and its LOW MATCH flag) stays on screen after the preview.
    qa_line = [l for l in s["statusHistory"] if l.startswith("Prepared:")][-1]
    expect(s.get("qa") == qa_line and ctl(s, "qaLabel")["visible"], "QA line kept visible: %r" % s.get("qa"))
    png = shot(tag, 1.5)
    pv = ctl(s, "previewControl")["rect"]
    m_, sd = region_stats(png, [pv[0], pv[1], min(pw, pv[2]), min(ph, pv[3])])
    expect(sd > 4 and m_ > 20, "preview painted (mean %.1f, sd %.1f)" % (m_, sd))
    of = orange_fraction(png, pv)
    expect(of < 0.2, "no stale border on a fresh preview (%.2f orange)" % of)
    im, isd = region_stats(png, ctl(s, "insetControl")["rect"], 2)
    expect(isd > 2, "inset painted (mean %.1f, sd %.1f)" % (im, isd))
    return s


INPUTS = ["brightness", "size", "contrast", "saturation", "haloSigma", "pLow", "pHigh", "nebula", "nebulaStrength",
          "nebulaAtmos", "nebulaStructure", "detect", "match", "offline", "advanced", "advancedPanel", "targetList",
          "browse", "prepare", "preview", "execute", "close", "previewControl"]


def busy_inputs_disabled(s):
    """Review I1: while busy, no input of the interface is enabled (an edit then could reset the
    session a render reads, or be overwritten by the Prepare that ends)."""
    missing = [n for n in INPUTS if n not in s["controls"]]
    on = [n for n in INPUTS if n in s["controls"] and s["controls"][n]["enabled"]]
    expect(not on and not missing, "while busy every input is disabled (enabled: %r; not reported: %r)" % (on, missing))
    png = shot("03a-busy.png", 0)
    # The interface's own control bar (outside its client area, not in the state file): its Apply
    # executes the instance on the active window (the master). Clicked while busy, it must never
    # start a second, nested Prepare (review round 2, item 5; the shell checks that no
    # <target>_depthgraded window exists after this Prepare). This does NOT prove the module's
    # busy gate: with the gate mutated off (2026-11-13) the click still did nothing, because it
    # lands while Prepare's PixInsight processes hold the GUI. The gate itself is tested headless
    # (run-probe.sh busy_refuses_execution).
    ax, ay = control_bar_apply(png, s)
    click(ax, ay)
    park()
    time.sleep(1.5)
    words = [w[0] for w in ocr_words(shot("03b-apply-while-busy.png", 0))]
    if "busy" in words:
        note("Apply while busy: PixInsight shows the refusal (OCR: ...%s...); dismissed with Return"
             % " ".join(words[max(0, words.index("busy") - 3):words.index("busy") + 6]))
        key("Return")
        park()
    else:
        note("Apply while busy: no message box on screen")
    # An edit attempted now must not reach a prepared session (typed into the disabled field).
    edit_value("detect", "9")


def control_bar_apply(png, s):
    """Centre of the interface control bar's Apply button (the filled blue square right of the
    New Instance triangle, bottom left of the interface, below the QA line): the right one of the
    two blue icon blobs in that strip of the screenshot."""
    a = rgb(png)
    qx, qy, qw, qh = ctl(s, "qaLabel")["rect"]
    y0, y1 = qy + qh, qy + qh + 45
    x0, x1 = max(0, qx - 20), qx + 80
    strip = a[y0:y1, x0:x1]
    blue = (strip[:, :, 2] >= 150) & (strip[:, :, 0] <= 80) & (strip[:, :, 1] <= 150)
    cols = np.where(blue.any(axis=0))[0]
    if len(cols) == 0:
        raise SystemExit("FAIL: no control bar icons below the QA line (strip x %d..%d, y %d..%d)" % (x0, x1, y0, y1))
    blobs, start = [], cols[0]
    for p, c in zip(cols, cols[1:]):
        if c != p + 1:
            blobs.append((start, p))
            start = c
    blobs.append((start, cols[-1]))
    if len(blobs) < 2:
        raise SystemExit("FAIL: expected the New Instance and Apply icons, found %d blob(s): %r" % (len(blobs), blobs))
    bx0, bx1 = blobs[1]
    rows = np.where(blue[:, bx0:bx1 + 1].any(axis=1))[0]
    cx, cy = x0 + (bx0 + bx1) // 2, y0 + (rows[0] + rows[-1]) // 2
    note("control bar Apply at %d,%d (icon blobs %r)" % (cx, cy, [(x0 + b0, x0 + b1) for b0, b1 in blobs]))
    return cx, cy


def step_prepare_enables_preview():
    s = state()
    expect_buttons(s, True, False, False)
    run_action("prepare", ["prepare ok", "preview ok"], 900, while_busy=busy_inputs_disabled)
    s = state()
    expect(abs(ctl(s, "detect")["value"] - 6) < 1e-9 and s["sessionMatchesSettings"],
           "the edit typed while busy did not land (detect %r; session matches the settings %r)"
           % (ctl(s, "detect")["value"], s["sessionMatchesSettings"]))
    s = check_prepared("03-prepared.png")
    se = session(s)
    cx, cy = (se["width"] + 1) // 2, (se["height"] + 1) // 2     # Math.round(W/2) for integer W
    expect(s["inset"] == [cx, cy], "inset centre defaults to the image centre %r (got %r)" % ([cx, cy], s["inset"]))


def step_slider_marks_preview_stale():
    s = state()
    before = ctl(s, "brightness")["value"]
    sum0 = s["previewBitmap"][2]
    x, y, w, h = ctl(s, "brightness")["slider"]
    t = time.time()
    click(x + int(w * 0.9), y + h // 2)     # a page step toward the click
    park()
    wait_for(lambda: abs(ctl(state(), "brightness")["value"] - before) > 1e-6, 10, "Brightness to change")
    s = state()
    expect(s["previewStale"] and s["prepared"], "Brightness %r -> %r: preview stale, still prepared"
           % (before, ctl(s, "brightness")["value"]))
    expect_buttons(s, True, True, True)
    png = shot("04-preview-stale.png", 1.0)
    of = orange_fraction(png, ctl(s, "previewControl")["rect"])
    expect(of > 0.9, "orange stale border drawn (%.2f of the top row)" % of)
    run_action("preview", ["preview ok"], 300)
    s = state()
    expect(not s["previewStale"] and s["previewBitmap"][2] != sum0,
           "Preview / Update: fresh, and the picture changed (checksum %d -> %d)" % (sum0, s["previewBitmap"][2]))
    png = shot("05-preview-updated.png", 1.5)
    of = orange_fraction(png, ctl(s, "previewControl")["rect"])
    expect(of < 0.2, "border gone after Preview / Update (%.2f orange)" % of)


def step_detect_edit_marks_prepare_stale():
    edit_value("detect", "7")
    wait_for(lambda: abs(ctl(state(), "detect")["value"] - 7) < 1e-9, 10, "detect_threshold_sigma = 7")
    s = state()
    expect(not s["prepared"] and s["previewStale"], "detect_threshold_sigma 7: not prepared, preview stale")
    expect(s.get("qa") == "", "the QA line is cleared with the session (%r)" % s.get("qa"))
    expect_buttons(s, True, False, False)
    png = shot("06-prepare-stale.png", 1.0)
    of = orange_fraction(png, ctl(s, "previewControl")["rect"])
    expect(of > 0.9, "orange stale border (%.2f)" % of)
    # A click on the disabled Execute does nothing.
    n0 = len(actions())
    click_ctl("execute")
    park()
    time.sleep(1.0)
    expect(len(actions()) == n0 and not state()["prepared"], "disabled Execute ignored")
    # Back to the default: still needs Prepare (the dialog's markPrepareStale on any edit).
    edit_value("detect", "6")
    wait_for(lambda: abs(ctl(state(), "detect")["value"] - 6) < 1e-9, 10, "detect_threshold_sigma = 6")
    expect_buttons(state(), True, False, False)
    run_action("prepare", ["prepare ok", "preview ok"], 900)
    check_prepared("07-reprepared.png")


def js_round(v):
    return int(np.floor(v + 0.5))


def step_inset_click_moves_center():
    s = state()
    se = session(s)
    W, H = se["width"], se["height"]
    pw = s["previewBitmap"][0]
    scale = W / pw
    x0, y0 = ctl(s, "previewControl")["rect"][:2]
    ph = s["previewBitmap"][1]
    shots = [("08-inset-click.png", pw // 4, ph // 4), ("09-inset-top-left.png", 2, 2),
             ("10-inset-bottom-right.png", pw - 3, ph - 3)]
    last = s["insetBitmap"][2]
    for png, px, py in shots:
        cx, cy = js_round(px * scale), js_round(py * scale)
        rx, ry = max(0, min(W - 256, cx - 128)), max(0, min(H - 256, cy - 128))
        n0 = len(actions())
        click(x0 + px, y0 + py)
        park()
        wait_for(lambda: len(actions()) > n0 and not state()["busy"], 300, "the inset re-render")
        s = state()
        expect(actions()[n0:] == ["preview ok"], "click at preview (%d, %d) re-rendered" % (px, py))
        expect(s["inset"] == [cx, cy] and s["insetRegion"] == [rx, ry, 256, 256],
               "inset centre %r (want %r), region %r (want %r)" % (s["inset"], [cx, cy], s["insetRegion"], [rx, ry, 256, 256]))
        expect(s["insetBitmap"][2] != last, "the inset picture changed")
        last = s["insetBitmap"][2]
        shot(png, 1.0)
    # A click on the control outside the picture (the fitted bitmap leaves room) is ignored.
    pv = ctl(s, "previewControl")["rect"]
    if pw + 6 < pv[2] or ph + 6 < pv[3]:
        ox, oy = (pw + 5, ph // 2) if pw + 6 < pv[2] else (pw // 2, ph + 5)
        n0, before = len(actions()), state()["inset"]
        click(x0 + ox, y0 + oy)
        park()
        time.sleep(1.5)
        expect(len(actions()) == n0 and state()["inset"] == before, "a click beside the picture (%d, %d) is ignored" % (ox, oy))


def step_execute_creates_depthgraded():
    target = state()["target"]
    run_action("execute", ["execute ok"], 600)
    s = state()
    expect(s["status"] == "Done: created %s_depthgraded" % target, "status %r" % s["status"])
    expect(s["prepared"] and not s["busy"], "still prepared after Execute (Preview / Execute again)")
    expect_buttons(s, True, True, True)
    shot("11-executed.png", 1.5)


def step_close_target_then_execute():
    s = state()
    target = open(OUT + "/target.txt").read().strip()
    expect(s["target"] == target, "the interface still names the closed target %r (%r)" % (target, s["target"]))
    # Closing the window does not disable Execute: the session is checked when it is used.
    expect(ctl(s, "execute")["enabled"], "Execute still enabled after the close")
    n0 = len(actions())
    click_ctl("execute")
    park()
    wait_for(lambda: len(actions()) > n0 and not state()["busy"], 60, "Execute to answer")
    expect(actions()[n0:] == ["execute error"], "Execute refused (%r)" % actions()[n0:])
    s = state()
    m = re.match(CLOSED_RE, s["status"])
    expect(m and m.group(1) == target, "status %r" % s["status"])
    expect(not s["prepared"], "the session is dropped (Prepare again)")
    expect_buttons(s, True, False, False)
    shot("12-target-closed.png", 1.0)
    # Prepare on the closed target: the same message, nothing made.
    run_action("prepare", ["prepare error"], 60)
    s = state()
    expect(re.match(CLOSED_RE, s["status"]), "Prepare on the closed target: %r" % s["status"])
    shot("13-prepare-closed.png", 1.0)


def step_online_prepare_live():
    # The Gaia archive cancels an anonymous async job after 60 s (its UWS executionDuration, capped
    # at 60 whatever the client asks; measured 2026-11-13 on this field): the Bailer-Jones join over
    # this 0.74 deg cone at G < 18 runs longer, at G < 14 it took 19 s. So: G limit 14 (Advanced).
    click_ctl("advanced")
    park()
    wait_for(lambda: ctl(state(), "advanced")["expanded"], 10, "Advanced to expand")
    edit_value("magLimit", "14")
    wait_for(lambda: abs(ctl(state(), "magLimit")["value"] - 14) < 1e-9, 10, "G limit = 14")
    click_ctl("advanced")
    park()
    wait_for(lambda: not ctl(state(), "advanced")["expanded"], 10, "Advanced to collapse")
    # A Qt check box toggles on its indicator or its text, not on the empty rest of its row.
    x, y, w, h = ctl(state(), "offline")["rect"]
    click(x + 8, y + h // 2)
    park()
    wait_for(lambda: not ctl(state(), "offline")["checked"], 10, "Offline to uncheck")
    s = state()
    expect(not s["prepared"], "Offline unchecked: Prepare needed")
    run_action("prepare", ["prepare ok", "preview ok"], 1800)
    s = check_prepared("14-online-prepared.png")
    expect(any("online Gaia query" in l for l in s["statusHistory"]), "the online catalogue was queried")


def step_close_button():
    """F9: the Close button hides the interface."""
    expect(state()["interfaceVisible"], "the interface is visible")
    click_ctl("close")
    park()
    wait_for(lambda: not (state_or_none() or {}).get("interfaceVisible", True), 10, "the interface to close")
    expect(True, "Close hid the interface")
    shot("15-closed.png", 1.0)


STEPS = {
    "controls_and_defaults": lambda: step_controls_and_defaults(sys.argv[3]),
    "prepare_enables_preview": step_prepare_enables_preview,
    "slider_marks_preview_stale": step_slider_marks_preview_stale,
    "detect_edit_marks_prepare_stale": step_detect_edit_marks_prepare_stale,
    "inset_click_moves_center": step_inset_click_moves_center,
    "execute_creates_depthgraded": step_execute_creates_depthgraded,
    "close_target_then_execute": step_close_target_then_execute,
    "online_prepare_live": step_online_prepare_live,
    "close_button": step_close_button,
}
if MODE not in STEPS:
    raise SystemExit("usage: gui_drive.py dismiss|quit|shot|%s OUT ..." % "|".join(STEPS))
note("== %s" % MODE)
STEPS[MODE]()
sys.exit(0)
