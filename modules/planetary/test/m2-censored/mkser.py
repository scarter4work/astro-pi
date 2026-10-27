#!/usr/bin/env python3
"""8-bit SER from a pal8 ASIAIR AVI (frames through ffmpeg's top-down pal8 rawvideo), with edits. Not shipped.

mkser.py in.avi out.ser W H [--gain G --gain-frames A:B] [--dead X,Y,V]
  --gain G         multiply frames A..B-1 (AVI frame index) by G, round, clip at 255
  --dead X,Y,V     set sensor site (X, Y) to V in every frame
The SER header's colour id is 8 (BAYER_RGGB, header offset 18); run.sh stacks it with stack-probe --bayer RGGB,
which names the same pattern.
"""
import struct
import subprocess
import sys

import numpy as np


def frames(src, w, h):
    p = subprocess.Popen(["ffmpeg", "-v", "error", "-i", src, "-f", "rawvideo", "-pix_fmt", "pal8", "-"],
                         stdout=subprocess.PIPE)
    fsz = w * h + 1024
    while True:
        b = p.stdout.read(fsz)
        if not b:
            break
        assert len(b) == fsz
        pal = np.frombuffer(b, dtype=np.uint8, offset=w * h).reshape(256, 4)
        assert (pal[:, :3] == np.arange(256)[:, None]).all(), "palette not identity"
        yield np.frombuffer(b, dtype=np.uint8, count=w * h).reshape(h, w)
    assert p.wait() == 0


def main():
    a = sys.argv[1:]
    src, dst, w, h = a[0], a[1], int(a[2]), int(a[3])
    opt = dict(zip(a[4::2], a[5::2]))
    gain = float(opt.get("--gain", 1))
    ga, gb = (int(v) for v in opt.get("--gain-frames", "0:0").split(":"))
    dead = [int(v) for v in opt["--dead"].split(",")] if "--dead" in opt else None
    lut = np.clip(np.round(np.arange(256) * gain), 0, 255).astype(np.uint8)
    hdr = bytearray(178)
    hdr[0:14] = b"LUCAM-RECORDER"
    for off, v in ((14, 0), (18, 8), (22, 0), (26, w), (30, h), (34, 8), (38, 0)):
        struct.pack_into("<i", hdr, off, v)
    n = 0
    with open(dst, "wb") as o:
        o.write(hdr)
        for im in frames(src, w, h):
            if ga <= n < gb:
                im = lut[im]
            if dead:
                im = np.array(im)
                im[dead[1], dead[0]] = dead[2]
            o.write(np.ascontiguousarray(im).tobytes())
            n += 1
        o.seek(38)
        o.write(struct.pack("<i", n))
    print("wrote %s: %d frames %dx%d" % (dst, n, w, h))


main()
