#!/usr/bin/env python3
"""Planetary test-only converter: Scott's ASIAIR Saturn AVI -> SER fixtures.

    avi_to_ser.py <in.avi> <out-dir> [--frames N]

Source (default of run-real-data.sh): 2026-09-07-020421-Saturn-Bin1 Lqef 16.7F.avi,
ZWO ASI585MC Air, RAW8, Bayer RGGB (per its .avi.txt), 320x240, rawvideo `pal8`.
Writes into <out-dir> (never committed):

  saturn_rggb8.ser                BAYER_RGGB depth 8: the raw sensor samples
  saturn_rggb16_12bit.ser         BAYER_RGGB depth 12 in 16-bit LE samples, value x 16, flag 0
  saturn_rggb16_12bit_beflag.ser  the same little-endian bytes with the header flag set to 1:
                                  the reader must override the flag and warn
  saturn_rgb8.ser                 RGB depth 8: ffmpeg's debayer (bayer_rggb8 -> rgb24)
  saturn_mono8.ser                MONO depth 8: ffmpeg pal8 -> gray
  expected.json                   probe cases (make-fixtures format) + a "gui" block

Independence: the pixel values in expected.json come from this script's own RIFF/AVI
parse of the file's bytes plus a Python bilinear demosaic, never from the C++ reader.
The `pal8` palette is proven to be the identity grey ramp (entry i == (i,i,i)) before
the palette indices are treated as sensor values; otherwise this fails with the dump.

AVI row orientation (measured at run time, printed): an AVI `pal8` frame is a DIB.
A positive biHeight means rows are stored bottom-up, a negative one top-down, and each
row is padded to a 4-byte stride. The script reads biHeight from the file, un-flips if needed, and
verifies the result byte-for-byte against ffmpeg's own `-f rawvideo -pix_fmt pal8`
output (which ffmpeg already orients top-down) for every converted frame.

Timestamps are FIXTURE values, not authoritative capture times: the filename time
(2026-09-07 02:04:21, timezone unknown) is used as the SER header DateTime_UTC and
the trailer holds start + i x frame interval (ffprobe r_frame_rate).
"""
import datetime
import json
import os
import struct
import subprocess
import sys
from fractions import Fraction

START = datetime.datetime(2026, 9, 7, 2, 4, 21)  # from the file name; timezone unknown
TICKS_PER_SEC = 10_000_000
INSTRUMENT = "ZWO ASI585MC Air"                   # from the .avi.txt sidecar
PROBE_FRAMES = 100


def fail(msg):
    print("FAIL: " + msg, file=sys.stderr)
    sys.exit(1)


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, **kw)
    if r.returncode != 0:
        fail("%s failed (%d): %s" % (cmd[0], r.returncode, r.stderr.decode("utf8", "replace")[-600:]))
    return r.stdout


# ---------------------------------------------------------------- AVI parse (RIFF)
def walk_riff(data, pos, end, out):
    """Collect video-stream info: strf bytes, and every '##db'/'##dc' data chunk."""
    while pos + 8 <= end:
        fourcc = data[pos:pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        body = pos + 8
        if fourcc in (b"RIFF", b"LIST"):
            walk_riff(data, body + 4, min(body + size, end), out)
        elif fourcc == b"strh":
            out["strh"].append((data[body:body + 4], data[body + 4:body + 8]))
        elif fourcc == b"strf" and out["strh"] and out["strh"][-1][0] == b"vids" and "strf" not in out:
            out["strf"] = data[body:body + size]
        elif len(fourcc) == 4 and fourcc[:2] == b"00" and fourcc[2:] in (b"db", b"dc"):
            out["frames"].append(data[body:body + size])
        pos = body + size + (size & 1)


def parse_avi(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"AVI ":
        fail("%s is not a RIFF AVI" % path)
    out = {"strh": [], "frames": []}
    walk_riff(data, 0, len(data), out)
    if "strf" not in out:
        fail("no video stream format chunk in %s" % path)
    strf = out["strf"]
    (bi_size, width, height, planes, bits, comp, size_img, _, _, clr_used, _) = struct.unpack_from("<IiiHHIIiiII", strf, 0)
    if comp != 0 or bits != 8:
        fail("video is not uncompressed 8-bit palettised (biCompression=%d, bits=%d)" % (comp, bits))
    n_pal = clr_used or 256
    pal = [tuple(strf[bi_size + 4 * i + k] for k in (2, 1, 0)) for i in range(n_pal)]  # BGRX -> RGB
    return width, height, pal, out["frames"]


def check_identity_palette(pal):
    bad = [(i, c) for i, c in enumerate(pal) if c != (i, i, i)]
    if len(pal) != 256 or bad:
        dump = ", ".join("%d:%s" % (i, c) for i, c in enumerate(pal))
        fail("pal8 palette is not the identity grey ramp (%d entries, %d differ); palette dump: %s"
             % (len(pal), len(bad), dump))


def dib_rows(chunk, width, height_signed):
    """Top-down list of rows (bytes) from one DIB frame: stride is padded to 4 bytes,
    and a positive biHeight stores the bottom row first."""
    h = abs(height_signed)
    stride = (width + 3) & ~3
    if len(chunk) < stride * h:
        fail("AVI frame chunk is %d bytes, %dx%d at stride %d needs %d" % (len(chunk), width, h, stride, stride * h))
    rows = [chunk[r * stride:r * stride + width] for r in range(h)]
    return rows[::-1] if height_signed > 0 else rows


# ---------------------------------------------------------------- SER writer
def ser_header(color_id, le_field, width, height, depth, n, instrument, start_ticks):
    h = bytearray(178)
    h[0:14] = b"LUCAM-RECORDER"
    for off, v in ((14, 0), (18, color_id), (22, le_field), (26, width), (30, height), (34, depth), (38, n)):
        struct.pack_into("<i", h, off, v)
    h[82:82 + len(instrument)] = instrument.encode("ascii")
    struct.pack_into("<q", h, 162, 0)
    struct.pack_into("<q", h, 170, start_ticks)
    return bytes(h)


def write_ser(path, color_id, le_field, w, h, depth, frames, instrument, start_ticks, tick_step):
    with open(path, "wb") as f:
        f.write(ser_header(color_id, le_field, w, h, depth, len(frames), instrument, start_ticks))
        for fr in frames:
            f.write(fr)
        for i in range(len(frames)):
            f.write(struct.pack("<q", start_ticks + i * tick_step))


# ---------------------------------------------------------------- independent expectations
def rggb_bilinear(rows, w, h, x, y, c):
    """Bilinear RGGB demosaic of one sample with mirrored borders (-1 -> 1, n -> n-2).
    Same rule the module documents; written here from the mosaic, not from the C++."""
    def m(i, n):
        return -i if i < 0 else (2 * (n - 1) - i if i >= n else i)

    def s(dx, dy):
        return rows[m(y + dy, h)][m(x + dx, w)]

    er, ec = y % 2 == 0, x % 2 == 0
    here = s(0, 0)
    orth = (s(-1, 0) + s(1, 0) + s(0, -1) + s(0, 1)) / 4
    diag = (s(-1, -1) + s(1, -1) + s(-1, 1) + s(1, 1)) / 4
    horiz, vert = (s(-1, 0) + s(1, 0)) / 2, (s(0, -1) + s(0, 1)) / 2
    if er and ec:
        rgb = (here, orth, diag)
    elif not er and not ec:
        rgb = (diag, orth, here)
    elif er:
        rgb = (horiz, here, vert)
    else:
        rgb = (vert, here, horiz)
    return rgb[c]


def points(w, h):
    return [(0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1), (5, 3), (10, 7), (160, 120), (150, 100), (170, 140)]


def ticks_iso(ticks):
    t = datetime.datetime(1, 1, 1) + datetime.timedelta(microseconds=ticks // 10)
    return t.strftime("%Y-%m-%dT%H:%M:%S.") + "%03d" % ((ticks % TICKS_PER_SEC) // 10000)


def main():
    args = sys.argv[1:]
    n_frames = PROBE_FRAMES
    if "--frames" in args:
        i = args.index("--frames")
        n_frames = int(args[i + 1])
        del args[i:i + 2]
    if len(args) != 2:
        fail("usage: avi_to_ser.py <in.avi> <out-dir> [--frames N]")
    src, outdir = args
    if not os.path.isfile(src):
        fail("source AVI not found: %s" % src)
    os.makedirs(outdir, exist_ok=True)

    probe = json.loads(run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                            "stream=codec_name,pix_fmt,width,height,nb_frames,r_frame_rate",
                            "-of", "json", src]))["streams"][0]
    if probe["codec_name"] != "rawvideo" or probe["pix_fmt"] != "pal8":
        fail("expected rawvideo/pal8, ffprobe says %s/%s" % (probe["codec_name"], probe["pix_fmt"]))
    w, h = int(probe["width"]), int(probe["height"])
    rate = Fraction(probe["r_frame_rate"])
    tick_step = int(round(TICKS_PER_SEC / rate))
    start_ticks = int((START - datetime.datetime(1, 1, 1)).total_seconds()) * TICKS_PER_SEC

    aw, ah, pal, chunks = parse_avi(src)
    check_identity_palette(pal)
    if (aw, abs(ah)) != (w, h):
        fail("AVI header %dx%d disagrees with ffprobe %dx%d" % (aw, abs(ah), w, h))
    if n_frames > len(chunks):
        fail("asked for %d frames, the AVI holds %d" % (n_frames, len(chunks)))
    orientation = "bottom-up (biHeight=%d > 0)" % ah if ah > 0 else "top-down (biHeight=%d < 0)" % ah
    stride = (w + 3) & ~3
    print("AVI: %dx%d, %d frames, %s, row stride %d (width %d), palette = identity grey ramp"
          % (w, h, len(chunks), orientation, stride, w))

    # Own parse, top-down, first n frames.
    frames = [dib_rows(c, w, ah) for c in chunks[:n_frames]]

    # Cross-check against ffmpeg's top-down pal8 rawvideo, byte for byte.
    ff = run(["ffmpeg", "-v", "error", "-i", src, "-frames:v", str(n_frames), "-f", "rawvideo",
              "-pix_fmt", "pal8", "-"])
    # ffmpeg's pal8 rawvideo muxer output is index plane (w*h) followed by a 1024-byte palette per frame.
    fsz = w * h + 1024
    if len(ff) != fsz * n_frames:
        fail("ffmpeg pal8 output is %d bytes, expected %d x %d" % (len(ff), n_frames, fsz))
    mism = [i for i in range(n_frames) if ff[i * fsz:i * fsz + w * h] != b"".join(frames[i])]
    if mism:
        flipped = [i for i in mism[:3] if ff[i * fsz:i * fsz + w * h] == b"".join(frames[i][::-1])]
        fail("own parse differs from ffmpeg's top-down pal8 on %d/%d frames (first %s; row-flipped match: %s)"
             % (len(mism), n_frames, mism[:5], flipped))
    print("orientation proof: own %s parse == ffmpeg top-down pal8 on all %d frames" % (orientation, n_frames))

    raw = [b"".join(f) for f in frames]  # w*h bytes each, top-down: the sensor samples

    # ffmpeg debayer of the same raw samples, and ffmpeg pal8 -> gray.
    rgb = run(["ffmpeg", "-v", "error", "-f", "rawvideo", "-pix_fmt", "bayer_rggb8", "-s", "%dx%d" % (w, h),
               "-i", "-", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], input=b"".join(raw))
    if len(rgb) != 3 * w * h * n_frames:
        fail("ffmpeg debayer output is %d bytes, expected %d" % (len(rgb), 3 * w * h * n_frames))
    rgbf = [rgb[i * 3 * w * h:(i + 1) * 3 * w * h] for i in range(n_frames)]
    gray = run(["ffmpeg", "-v", "error", "-i", src, "-frames:v", str(n_frames), "-f", "rawvideo",
                "-pix_fmt", "gray", "-"])
    grayf = [gray[i * w * h:(i + 1) * w * h] for i in range(n_frames)]
    # pal8 -> gray goes through swscale's RGB->luma maths, so it is NOT the identity even for a
    # grey ramp (the mono fixture is ffmpeg's output and its expectations read those bytes).
    gdiff = max(abs(a - b) for g, r in zip(grayf, raw) for a, b in zip(g, r))
    print("ffmpeg pal8->gray vs palette indices: max |diff| = %d (swscale luma rounding)" % gdiff)

    # 12-bit: value x 16, little endian.
    def to12(fr):
        return b"".join(struct.pack("<H", v * 16) for v in fr)
    raw12 = [to12(fr) for fr in raw]

    files = {
        "saturn_rggb8.ser": (8, 0, 8, raw),
        "saturn_rggb16_12bit.ser": (8, 0, 12, raw12),
        "saturn_rggb16_12bit_beflag.ser": (8, 1, 12, raw12),
        "saturn_rgb8.ser": (100, 0, 8, rgbf),
        "saturn_mono8.ser": (0, 0, 8, grayf),
    }
    for name, (cid, le, depth, frs) in files.items():
        write_ser(os.path.join(outdir, name), cid, le, w, h, depth, frs, INSTRUMENT, start_ticks, tick_step)
    print("wrote %d SER files, %d frames each, start %s (fixture time, timezone unknown), step %d ticks"
          % (len(files), n_frames, START.isoformat(), tick_step))

    # ------------------------------------------------------------ expected.json
    pts = points(w, h)
    base_kw = lambda fname, fi: [
        {"name": "DATE-OBS", "value": ticks_iso(start_ticks + fi * tick_step)},
        {"name": "INSTRUME", "value": INSTRUMENT},
        {"name": "HISTORY", "comment": "SERViewer: %s frame %d" % (fname, fi)},
    ]

    def case(name, fname, fi, debayer, ch, samples, extra_kw=(), absent=()):
        return {"name": name, "path": os.path.join(outdir, fname), "frame": fi, "debayer": debayer,
                "expectId": "%s_f%d" % (fname[:-4], fi), "width": w, "height": h, "channels": ch,
                "samples": samples, "keywords": base_kw(fname, fi) + list(extra_kw),
                "absentKeywords": list(absent)}

    last = n_frames - 1
    cases = []
    # Raw CFA, 8-bit (BAYERPAT present).
    cases.append(case("saturn rggb8 raw CFA f%d" % last, "saturn_rggb8.ser", last, False, 1,
                      [{"x": x, "y": y, "c": 0, "v": frames[last][y][x] / 255} for x, y in pts],
                      [{"name": "BAYERPAT", "value": "RGGB"}]))
    # Debayered, 8-bit: frame 0 and the last frame.
    for fi in (0, last):
        cases.append(case("saturn rggb8 debayered f%d" % fi, "saturn_rggb8.ser", fi, True, 3,
                          [{"x": x, "y": y, "c": c, "v": rggb_bilinear(frames[fi], w, h, x, y, c) / 255}
                           for x, y in pts for c in range(3)], (), ["BAYERPAT"]))
    # 12-bit in 16-bit samples: normalised by 4095, raw x16 -> v*16/4095; debayered and raw.
    cases.append(case("saturn rggb16-12bit debayered f50", "saturn_rggb16_12bit.ser", min(50, last), True, 3,
                      [{"x": x, "y": y, "c": c, "v": 16 * rggb_bilinear(frames[min(50, last)], w, h, x, y, c) / 4095}
                       for x, y in pts for c in range(3)], (), ["BAYERPAT"]))
    cases.append(case("saturn rggb16-12bit raw f3", "saturn_rggb16_12bit.ser", 3, False, 1,
                      [{"x": x, "y": y, "c": 0, "v": 16 * frames[3][y][x] / 4095} for x, y in pts],
                      [{"name": "BAYERPAT", "value": "RGGB"}]))
    # Big-endian flag on little-endian data: values must still be right, and the override reported.
    cases.append(case("saturn rggb16-12bit beflag raw f3 (byte-order override)", "saturn_rggb16_12bit_beflag.ser", 3,
                      False, 1, [{"x": x, "y": y, "c": 0, "v": 16 * frames[3][y][x] / 4095} for x, y in pts],
                      [{"name": "BAYERPAT", "value": "RGGB"},
                       {"name": "HISTORY", "commentContains":
                        "SERViewer warning: byte order: header flag says big-endian, data is little-endian; using little-endian"}]))
    cases.append(case("saturn rggb16-12bit beflag debayered f%d" % last, "saturn_rggb16_12bit_beflag.ser", last, True, 3,
                      [{"x": x, "y": y, "c": c, "v": 16 * rggb_bilinear(frames[last], w, h, x, y, c) / 4095}
                       for x, y in pts for c in range(3)],
                      [{"name": "HISTORY", "commentContains": "using little-endian"}], ["BAYERPAT"]))
    # RGB 8 from ffmpeg's debayer (planes interleaved R,G,B per pixel).
    cases.append(case("saturn rgb8 f7", "saturn_rgb8.ser", 7, False, 3,
                      [{"x": x, "y": y, "c": c, "v": rgbf[7][3 * (y * w + x) + c] / 255} for x, y in pts for c in range(3)],
                      (), ["BAYERPAT"]))
    cases.append(case("saturn mono8 f%d" % last, "saturn_mono8.ser", last, True, 1,
                      [{"x": x, "y": y, "c": 0, "v": grayf[last][y * w + x] / 255} for x, y in pts], (), ["BAYERPAT"]))
    # Errors: a frame past the end.
    for fname in ("saturn_rggb8.ser", "saturn_rgb8.ser"):
        cases.append({"name": "%s frame %d out of range" % (fname, n_frames), "path": os.path.join(outdir, fname),
                      "frame": n_frames, "debayer": True, "expectError": "out of range"})

    # Library check list (check-real-fixtures.cpp validates the C++ reader against these, no PixInsight):
    #   S path frame debayer channels x y c v | T path frame iso | W path substring | E path frame substring
    tsv = []
    for c in cases:
        if "expectError" in c:
            tsv.append("E\t%s\t%d\t%s" % (c["path"], c["frame"], c["expectError"]))
            continue
        tsv.append("T\t%s\t%d\t%s" % (c["path"], c["frame"], c["keywords"][0]["value"]))
        for k in c["keywords"]:
            if k["name"] == "HISTORY" and "commentContains" in k:
                tsv.append("W\t%s\t%s" % (c["path"], k["commentContains"].replace("SERViewer warning: ", "")))
        for sm in c["samples"]:
            tsv.append("S\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%.12g" % (
                c["path"], c["frame"], int(c["debayer"]), c["channels"], sm["x"], sm["y"], sm["c"], sm["v"]))
    with open(os.path.join(outdir, "expected-checks.tsv"), "w") as f:
        f.write("\n".join(tsv) + "\n")

    # GUI hint: mean R vs mean B over the lit RGGB sites of the last frame, so the GUI
    # smoke can check channel order (an R/B swap flips it) when the scene is clearly not neutral.
    fr = frames[last]
    rsum = [fr[y][x] for y in range(0, h, 2) for x in range(0, w, 2)]
    bsum = [fr[y][x] for y in range(1, h, 2) for x in range(1, w, 2)]
    lit = [(r, b) for r, b in zip(rsum, bsum) if max(r, b) > 24]
    r_mean = sum(r for r, _ in lit) / len(lit) if lit else 0.0
    b_mean = sum(b for _, b in lit) / len(lit) if lit else 0.0
    ratio = r_mean / b_mean if b_mean else float("inf")
    order = "gt" if ratio >= 1.15 else ("lt" if ratio <= 1 / 1.15 else "none")
    json.dump({"cases": cases, "gui": {"lastFrame": last, "rMeanLit": r_mean, "bMeanLit": b_mean,
                                         "rbRatio": ratio, "rbOrder": order}},
              open(os.path.join(outdir, "expected.json"), "w"), indent=1)
    print("expected.json: %d cases; last-frame lit R/B site means %.1f / %.1f (ratio %.2f -> rbOrder=%s)"
          % (len(cases), r_mean, b_mean, ratio, order))


if __name__ == "__main__":
    main()
