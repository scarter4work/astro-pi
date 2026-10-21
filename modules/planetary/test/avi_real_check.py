#!/usr/bin/env python3
"""Planetary real-data AVI proof (test-only, host-only: needs the NAS and ffmpeg).

    python3 -I avi_real_check.py <avi-probe> <work-dir>

Runs the C++ reader (through test/avi-probe) over every *.avi under /mnt/qnap/astro_data
(or $PLANETARY_AVI_ROOT; read-only, nothing is ever written there) and checks it against
ffmpeg/ffprobe, which are independent of the reader:

1. Frame count == ffprobe nb_frames, or the reader is short AND one of its warnings
   explains it ("... using the first N frames" or N + dropped zero-length frames).
2. Open time (OpenVideo, client page cache for the file dropped first) < 2.0 s;
   the slowest file is reported.
3. Frames 0, N//2, N-1 equal ffmpeg's `-f rawvideo -pix_fmt pal8` output byte for byte
   (the first w*h bytes; ffmpeg's unfiltered palette must be the identity grey ramp). ffmpeg's frame
   k is selected by pts == k (AVI pts counts index entries), after a coarse input seek.
4. Bayer phase: for sidecar files whose target (5th '-' field of the name) is Mars,
   Saturn, Jupiter, Moon or Betelgeuse: per-site means of the 2x2 CFA over the planet's
   disc in each of the first 20 frames (ffmpeg's bytes), averaged. Disc = superpixels whose
   2x2 sum is >= 50 % of that frame's 99.99th percentile and none of whose four sites is
   >= 250 (saturated). The pattern the reader resolved (Auto) must put its two green sites
   on the more equal diagonal and red on the brighter of the two non-green sites. The three
   captures in RGGB_EXCEPTIONS (Ruling 10: Flip=Vert files that measured RGGB) must still
   resolve to the Flip=Vert inference AND measure RGGB (red at (0,0)). Every Flip=Vert file
   must carry the reader's inference warning. A file whose disc has fewer than
   MIN_DISC_SUPERPIXELS (summed over the frames), or whose bright disc is more than
   MAX_SATURATED_SHARE saturated (what is left is the limb ring), is listed as unmeasurable
   with its numbers -- reported, never passed silently.
   Every file's numbers print.
5. Round trip: the smallest AVI over 1 GB -> avi-probe --export into <work-dir> (local
   disk) -> SerFile frame count == the AVI's, frames 0, N//2, N-1 read back by SerFile
   AND parsed here from the SER bytes equal the AVI's ffmpeg frames. The SER is deleted.

Prints PASS (exit 0) or FAIL lines with the numbers (exit 1). Never loosen a check.
"""
import json
import os
import shutil
import struct
import subprocess
import sys
from fractions import Fraction

ROOT = os.environ.get("PLANETARY_AVI_ROOT", "/mnt/qnap/astro_data")
OPEN_LIMIT_S = 2.0
BAYER_TARGETS = {"Mars", "Saturn", "Jupiter", "Moon", "Betelgeuse"}
BAYER_FRAMES = 20
DISC_PCT = 99.99           # the disc threshold is DISC_FRACTION of this percentile's 2x2 sum
DISC_FRACTION = 0.5
SATURATED = 250            # a superpixel with any site >= this is left out
MIN_DISC_SUPERPIXELS = 100  # summed over the frames; fewer -> unmeasurable (reported, not passed)
# When more than this share of the bright disc is saturated, the unsaturated part left is the
# limb ring, whose edge gradients bias the sites (measured: greens 5-8 % apart over 200 frames on
# the 2022-08-17 Jupiter captures, 88 % and 91 % saturated; the most saturated clean measurements
# are 53 % Betelgeuse and 56 % Moon) -> unmeasurable (reported, not passed).
MAX_SATURATED_SHARE = 0.8
# Flip=Vert captures measured RGGB (red at (0,0)) on the disc, against the BGGR inference
# (Ruling 10; disc s00/s11 1.325, 1.264, 1.163 when pinned). Matched by file-name prefix.
RGGB_EXCEPTIONS = ("2022-08-03-015840-Jupiter", "2022-08-03-021039-Saturn", "2022-08-17-004958-Moon")
VERT_WARNING = ("Bayer pattern inferred from Flip=Vert (%s); some ASIAIR captures are %s -- if red and blue "
                "look swapped, choose %s under Bayer pattern")
ROUND_TRIP_MIN = 1 << 30
SER_HEADER = 178
# ColorId -> letters of sites (0,0) (0,1) (1,0) (1,1)
PATTERNS = {"BAYER_RGGB": "RGGB", "BAYER_GRBG": "GRBG", "BAYER_GBRG": "GBRG", "BAYER_BGGR": "BGGR"}

failures = []


def fail(name, msg):
    line = "FAIL: %s: %s" % (name, msg)
    print(line, flush=True)
    failures.append(line)


def run(cmd):
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0:
        raise RuntimeError("%s exited %d: %s%s" % (os.path.basename(cmd[0]), r.returncode,
                                                   r.stdout.decode("utf8", "replace")[-400:],
                                                   r.stderr.decode("utf8", "replace")[-400:]))
    return r.stdout


def probe(exe, path, *args):
    out = subprocess.run([exe, path, *args], capture_output=True)
    text = out.stdout.decode("utf8", "replace").strip()
    try:
        j = json.loads(text.splitlines()[-1])
    except (ValueError, IndexError):
        raise RuntimeError("avi-probe printed no JSON (exit %d): %s %s" % (
            out.returncode, text[-300:], out.stderr.decode("utf8", "replace")[-300:]))
    if out.returncode != 0 or "error" in j:
        raise RuntimeError("avi-probe: %s" % j.get("error", "exit %d" % out.returncode))
    return j


def drop_cache(path):
    fd = os.open(path, os.O_RDONLY)
    try:
        os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
    finally:
        os.close(fd)


def ffprobe(path):
    out = run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
               "stream=width,height,nb_frames,time_base,pix_fmt,codec_name", "-of", "json", path])
    s = json.loads(out)["streams"][0]
    return s


def ff_frame(path, k, tb, w, h):
    """ffmpeg's frame with pts == k (pal8 indices, w*h bytes). The select filter zeroes the
    1024-byte palette ffmpeg appends, so the palette is proven by ff_first_frames instead."""
    pre = float(max(0, k - 3) * tb)
    # -max_error_rate 1: after a seek near the end, ffmpeg's demuxer turns the idx1 / trailing
    # bytes into 16-byte "00db" packets ("Packet too small") and, with few frames decoded, exits
    # 69 on its error-rate cap. The frame itself is still checked: exact length, byte for byte.
    out = run(["ffmpeg", "-nostdin", "-v", "error", "-max_error_rate", "1", "-ss", "%.9f" % pre, "-copyts", "-i", path,
               "-vf", "select=eq(pts\\,%d)" % k, "-frames:v", "1", "-fps_mode", "passthrough",
               "-f", "rawvideo", "-pix_fmt", "pal8", "-"])
    if len(out) != w * h + 1024:
        raise RuntimeError("ffmpeg returned %d bytes for frame %d, expected %d" % (len(out), k, w * h + 1024))
    return out[:w * h]


def check_palette(pal, k):
    for i in range(256):
        b, g, r = pal[4 * i], pal[4 * i + 1], pal[4 * i + 2]
        if not (b == g == r == i):
            raise RuntimeError("ffmpeg palette of frame %d is not the identity grey ramp (entry %d = %d,%d,%d)"
                               % (k, i, r, g, b))


def ff_first_frames(path, n, w, h):
    out = run(["ffmpeg", "-nostdin", "-v", "error", "-i", path, "-frames:v", str(n), "-fps_mode", "passthrough",
               "-f", "rawvideo", "-pix_fmt", "pal8", "-"])
    fsz = w * h + 1024
    if len(out) != n * fsz:
        raise RuntimeError("ffmpeg returned %d bytes for %d frames, expected %d" % (len(out), n, n * fsz))
    frames = []
    for k in range(n):
        check_palette(out[k * fsz + w * h:(k + 1) * fsz], k)
        frames.append(out[k * fsz:k * fsz + w * h])
    return frames


def site_means(frames, w, h):
    """Per-site means over the unsaturated disc, pooled over the frames; the number of disc
    superpixels (summed over the frames); and the saturated share of the bright disc."""
    import numpy as np
    sums = np.zeros(4)
    count = bright = 0
    for raw in frames:
        a = np.frombuffer(raw, np.uint8).reshape(h, w).astype(np.float64)
        a = a[:h - h % 2, :w - w % 2]
        s = [a[0::2, 0::2], a[0::2, 1::2], a[1::2, 0::2], a[1::2, 1::2]]
        tot = s[0] + s[1] + s[2] + s[3]
        disc = tot >= DISC_FRACTION * np.percentile(tot, DISC_PCT)
        mask = disc & (np.maximum.reduce(s) < SATURATED)
        sums += [x[mask].sum() for x in s]
        count += int(mask.sum())
        bright += int(disc.sum())
    return (sums / count if count else np.full(4, np.nan)), count, (1 - count / bright if bright else 1.0)


def rel(a, b):
    return abs(a - b) / ((a + b) / 2) if a + b > 0 else 0.0


def count_explained(j, ff_n):
    n = j["frames"]
    if n == ff_n:
        return True, ""
    if n > ff_n:
        return False, "reader has more frames (%d) than ffprobe (%d)" % (n, ff_n)
    for w in j["warnings"]:
        if ("using the first %d frames" % n) in w:
            return True, w
    if j.get("droppedFrames", 0) and n + j["droppedFrames"] == ff_n and any("dropped" in w for w in j["warnings"]):
        return True, "%d dropped frames" % j["droppedFrames"]
    return False, "reader %d frames vs ffprobe %d with no explaining warning: %s" % (n, ff_n, j["warnings"])


def main():
    if len(sys.argv) != 3:
        print("usage: avi_real_check.py <avi-probe> <work-dir>", file=sys.stderr)
        return 2
    exe, work = sys.argv[1], sys.argv[2]
    os.makedirs(work, exist_ok=True)
    avis = []
    for d, _, files in os.walk(ROOT):
        for f in files:
            if f.lower().endswith(".avi"):
                avis.append(os.path.join(d, f))
    avis.sort()
    print("files: %d under %s" % (len(avis), ROOT), flush=True)
    if not avis:
        fail(ROOT, "no .avi files found")

    results = []
    slowest = (0.0, "")
    bayer_rows = []
    for idx, path in enumerate(avis, 1):
        name = os.path.relpath(path, ROOT)
        try:
            drop_cache(path)
            j = probe(exe, path)
            s = ffprobe(path)
            w, h, ff_n = int(s["width"]), int(s["height"]), int(s["nb_frames"])
            tb = Fraction(s["time_base"])
            if s.get("codec_name") != "rawvideo" or s.get("pix_fmt") != "pal8":
                fail(name, "ffprobe says %s/%s, not rawvideo/pal8" % (s.get("codec_name"), s.get("pix_fmt")))
            if (j["width"], j["height"]) != (w, h):
                fail(name, "reader %dx%d vs ffprobe %dx%d" % (j["width"], j["height"], w, h))
            ok, why = count_explained(j, ff_n)
            if not ok:
                fail(name, why)
            if j["open_s"] >= OPEN_LIMIT_S:
                fail(name, "open took %.3f s (limit %.1f s)" % (j["open_s"], OPEN_LIMIT_S))
            if j["open_s"] > slowest[0]:
                slowest = (j["open_s"], name)

            n = j["frames"]
            # Identity palette (unfiltered decode of frame 0): palette indices are sensor values.
            ff_first_frames(path, 1, w, h)
            picks = sorted({0, n // 2, n - 1})
            fdir = os.path.join(work, "frames")
            shutil.rmtree(fdir, ignore_errors=True)
            os.makedirs(fdir)
            probe(exe, path, "--frames", ",".join(map(str, picks)), "--out", fdir)
            for k in picks:
                with open(os.path.join(fdir, "%d.raw" % k), "rb") as fh:
                    mine = fh.read()
                theirs = ff_frame(path, k, tb, w, h)
                if mine != theirs:
                    diff = sum(1 for a, b in zip(mine, theirs) if a != b) if len(mine) == len(theirs) else -1
                    fail(name, "frame %d differs from ffmpeg (%s bytes differ; reader %d bytes, ffmpeg %d)"
                         % (k, diff, len(mine), len(theirs)))

            note = "" if n == ff_n else "  (%d of ffprobe's %d: %s)" % (n, ff_n, why)
            print("[%3d/%d] %-80s %5d frames %4dx%-4d open %.3f s  %s/%s  frames %s ok%s"
                  % (idx, len(avis), name, n, w, h, j["open_s"], j["colorId"], j["bayerSource"] or "-",
                     ",".join(map(str, picks)), note), flush=True)

            check_vert_warning(name, j)
            target = os.path.basename(path).split("-")[4] if os.path.basename(path).count("-") >= 4 else ""
            if "sidecarBayer" in j and target in BAYER_TARGETS:
                bayer_rows.append(bayer_check(name, path, j, w, h))
            results.append((path, os.path.getsize(path), j, w, h, tb))
        except Exception as e:  # noqa: BLE001 -- every failure is reported with its file
            fail(name, str(e))

    print("\nBayer phase (2x2 site means over the unsaturated disc: superpixels >= %d %% of the %.2fth percentile"
          " sum, no site >= %d; first %d frames):" % (DISC_FRACTION * 100, DISC_PCT, SATURATED, BAYER_FRAMES))
    print("  %-62s %-22s %-6s %7s %7s %7s %7s %6s %6s %6s %8s %5s  %-9s %s" % (
        "file", "camera", "flip", "s00", "s01", "s10", "s11", "s00/11", "dG", "dRB", "disc", "sat", "resolved",
        "verdict"))
    rows = [r for r in bayer_rows if r]
    for row, _ in rows:
        print("  " + row)
    seen_exc = {e: 0 for e in RGGB_EXCEPTIONS}
    for path_name, verdict in bayer_verdicts:
        for e in RGGB_EXCEPTIONS:
            if os.path.basename(path_name).startswith(e):
                seen_exc[e] += 1
    for e, n in seen_exc.items():
        if n == 0:
            fail(e, "pinned RGGB exception not found among the measured captures")
    unmeasurable = [(p, v) for p, v in bayer_verdicts if v.startswith("unmeasurable")]
    print("Bayer verdicts (of %d): %d agree with the resolved pattern, %d pinned RGGB exceptions measured RGGB, %d unmeasurable%s"
          % (len(bayer_verdicts), sum(1 for _, v in bayer_verdicts if v == "agrees"),
             sum(1 for _, v in bayer_verdicts if v == "pinned RGGB exception, measures RGGB"),
             len(unmeasurable), "".join("\n  unmeasurable: %s (%s)" % (p, v) for p, v in unmeasurable)))

    round_trip(exe, work, results)

    print("\nslowest open: %.3f s  %s" % slowest)
    print("files: %d checked, %d Bayer-phase files" % (len(results), len(bayer_verdicts)))
    if failures:
        print("\nFAIL: %d check(s) failed:" % len(failures))
        for f in failures:
            print("  " + f)
        return 1
    print("PASS: %d/%d AVIs (frame counts, open < %.1f s, frames 0/N/2/N-1 == ffmpeg), Bayer phase, round trip"
          % (len(results), len(avis), OPEN_LIMIT_S))
    return 0


SIDECAR_PATTERNS = {"RG": "RGGB", "GR": "GRBG", "GB": "GBRG", "BG": "BGGR"}


def check_vert_warning(name, j):
    """Every Auto-resolved Flip=Vert file carries the reader's inference warning (Ruling 10)."""
    if not j.get("sidecarFlip", "").startswith("Vert") or j.get("sidecarBayer") not in SIDECAR_PATTERNS:
        return
    want = VERT_WARNING % (PATTERNS.get(j["colorId"], j["colorId"]), SIDECAR_PATTERNS[j["sidecarBayer"]],
                           SIDECAR_PATTERNS[j["sidecarBayer"]])
    if want not in j["warnings"]:
        fail(name, "Flip=Vert file lacks the inference warning %r; warnings %s" % (want, j["warnings"]))


bayer_verdicts = []  # (name, verdict)


def bayer_check(name, path, j, w, h):
    pat = PATTERNS.get(j["colorId"])
    if not pat:
        fail(name, "sidecar Bayer %r but the reader resolved %s (%s)" % (j["sidecarBayer"], j["colorId"], j["bayerSource"]))
        return None
    pinned = any(os.path.basename(path).startswith(e) for e in RGGB_EXCEPTIONS)
    if pinned and not (j.get("sidecarFlip", "").startswith("Vert") and pat == "BGGR"):
        fail(name, "pinned RGGB exception no longer resolves to the Flip=Vert inference BGGR (resolved %s, Flip %r)"
             % (pat, j.get("sidecarFlip")))
    expect = "RGGB" if pinned else pat   # what the frame data must say
    frames = ff_first_frames(path, min(BAYER_FRAMES, j["frames"]), w, h)
    m, count, sat = site_means(frames, w, h)
    greens = [i for i, c in enumerate(expect) if c == "G"]
    red, blue = expect.index("R"), expect.index("B")
    if count < MIN_DISC_SUPERPIXELS or sat > MAX_SATURATED_SHARE:
        verdict = "unmeasurable: %d unsaturated disc superpixels over %d frames (min %d), %.0f %% of the disc " \
                  "saturated (max %.0f %%)" % (count, len(frames), MIN_DISC_SUPERPIXELS, 100 * sat,
                                               100 * MAX_SATURATED_SHARE)
        if pinned:
            fail(name, "pinned RGGB exception is " + verdict)
        d_g = d_rb = float("nan")
    else:
        d_g = rel(m[greens[0]], m[greens[1]])
        d_rb = rel(m[red], m[blue])
        verdict = "pinned RGGB exception, measures RGGB" if pinned else "agrees"
    if not verdict.startswith("unmeasurable"):
        bad = []
        if not d_g < d_rb:
            bad.append("greens (sites %s) are not the equal diagonal: rel diff %.3f vs non-green %.3f"
                       % (greens, d_g, d_rb))
        if not m[red] > m[blue]:
            bad.append("red site %d (%.2f) is not brighter than blue site %d (%.2f)" % (red, m[red], blue, m[blue]))
        if bad:
            verdict = "DISAGREES with %s: %s" % ("RGGB (pinned exception)" if pinned else pat, "; ".join(bad))
    row = "%-62s %-22s %-6s %7.2f %7.2f %7.2f %7.2f %6.3f %6.3f %6.3f %8d %4.0f%%  %-9s %s" % (
        name[-62:], j.get("sidecarCamera", "")[:22], j.get("sidecarFlip", "")[:6],
        m[0], m[1], m[2], m[3], m[0] / m[3] if m[3] else float("inf"), d_g, d_rb, count, 100 * sat, pat, verdict)
    if verdict.startswith("DISAGREES"):
        fail(name, verdict + "; " + row)
    bayer_verdicts.append((name, verdict))
    return row, verdict


def round_trip(exe, work, results):
    big = sorted((r for r in results if r[1] > ROUND_TRIP_MIN), key=lambda r: r[1])
    if not big:
        fail("round trip", "no AVI over 1 GB")
        return
    path, size, j, w, h, tb = big[0]
    name = os.path.relpath(path, ROOT)
    n = j["frames"]
    picks = sorted({0, n // 2, n - 1})
    rt = os.path.join(work, "roundtrip")
    shutil.rmtree(rt, ignore_errors=True)
    os.makedirs(rt)
    ser = os.path.join(rt, "roundtrip.ser")
    try:
        free = shutil.disk_usage(rt).free
        if free < size + (256 << 20):
            fail("round trip", "%d bytes free in %s, need ~%d" % (free, rt, size))
            return
        e = probe(exe, path, "--export", ser, "--frames", ",".join(map(str, picks)), "--out", rt)
        ser_size = os.path.getsize(ser)
        with open(ser, "rb") as fh:
            hdr = fh.read(SER_HEADER)
            magic = hdr[:14]
            color, _le, sw, sh, depth, cnt = struct.unpack_from("<iiiiii", hdr, 18)
            ok = True
            if magic != b"LUCAM-RECORDER" or (sw, sh, depth, cnt) != (w, h, 8, n) or ser_size != SER_HEADER + n * w * h:
                fail("round trip", "SER header %r %dx%d depth %d frames %d size %d; want %dx%d depth 8 frames %d size %d"
                     % (magic, sw, sh, depth, cnt, ser_size, w, h, n, SER_HEADER + n * w * h))
                ok = False
            if e["ser_frames"] != n or e["export_frames"] != n:
                fail("round trip", "SerFile frames %d, exported %d, AVI %d" % (e["ser_frames"], e["export_frames"], n))
                ok = False
            if e["ser_colorId"] != j["colorId"]:
                fail("round trip", "SER ColorID %s vs the AVI's %s" % (e["ser_colorId"], j["colorId"]))
                ok = False
            for k in picks:
                theirs = ff_frame(path, k, tb, w, h)
                fh.seek(SER_HEADER + k * w * h)
                raw = fh.read(w * h)
                with open(os.path.join(rt, "ser-%d.raw" % k), "rb") as sf:
                    via_reader = sf.read()
                if raw != theirs or via_reader != theirs:
                    fail("round trip", "frame %d: SER bytes %s, SerFile %s the AVI's ffmpeg frame"
                         % (k, "==" if raw == theirs else "!=", "==" if via_reader == theirs else "!="))
                    ok = False
        print("\nround trip: %s (%.2f GB, %d frames %dx%d, %s) -> SER %.2f GB in %.1f s; SerFile reads %d frames; "
              "frames %s equal: %s"
              % (name, size / 1e9, n, w, h, j["colorId"], ser_size / 1e9, e["export_s"], e["ser_frames"],
                 ",".join(map(str, picks)), "yes" if ok else "NO"), flush=True)
    except Exception as ex:  # noqa: BLE001
        fail("round trip " + name, str(ex))
    finally:
        shutil.rmtree(rt, ignore_errors=True)
        if os.path.exists(rt):
            fail("round trip", "could not delete %s" % rt)


if __name__ == "__main__":
    sys.exit(main())
