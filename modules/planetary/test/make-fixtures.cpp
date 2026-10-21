// Test-only CLI: writes synthetic SER fixtures and the headless probe's input
// JSON (expected dimensions, samples and keywords) into a directory.
//
//   make-fixtures <out dir>   ->  <out dir>/*.ser + <out dir>/probe-in.json
//
// Expected values come from the same generator formula that fills the files,
// plus an independent bilinear demosaic written here, never from the reader
// under test. Never shipped.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "SerWriter.h"

using planetary::testing::SerSpec;
using planetary::testing::WriteSer;

namespace {

// Raw sample of (x, y, channel c) in frame f, LSB-aligned below 2^depth.
int Gen(int x, int y, int c, int f, int depth) {
    return (x * 7 + y * 13 + c * 29 + f * 31 + 3) % (1 << depth);
}

double Norm(int raw, int depth) { return raw / double((1 << depth) - 1); }

void Put16LE(std::vector<uint8_t>& b, int v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>(v >> 8));
}

// JSON string literal; non-ASCII UTF-8 goes out as \uXXXX (BMP only, which is
// all the fixtures use) so the probe's reader cannot mis-decode it.
std::string Quote(const std::string& s) {
    std::string o = "\"";
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '"' || c == '\\') {
            o += '\\';
            o += static_cast<char>(c);
        } else if (c < 0x80) {
            o += static_cast<char>(c);
        } else {
            unsigned cp = 0;
            int more = 0;
            if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; more = 1; }
            else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; more = 2; }
            else { std::fprintf(stderr, "make-fixtures: unsupported UTF-8 in %s\n", s.c_str()); std::exit(1); }
            for (int k = 0; k < more && i + 1 < s.size(); ++k) cp = (cp << 6) | (s[++i] & 0x3F);
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", cp);
            o += buf;
        }
    }
    return o + "\"";
}

std::string Num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.12g", v);
    return buf;
}

struct Point { int x, y, c; double v; };
struct Kw { std::string name, field, text; };  // field: value | comment | commentContains

struct Case {
    std::string name, path, expectId, expectError;
    int frame = 0;
    bool debayer = true;
    int width = 0, height = 0, channels = 0;
    std::vector<Point> samples;
    std::vector<Kw> keywords;
    std::vector<std::string> absent;
};

std::string ToJson(const Case& c) {
    std::ostringstream o;
    o << "{\"name\":" << Quote(c.name) << ",\"path\":" << Quote(c.path) << ",\"frame\":" << c.frame
      << ",\"debayer\":" << (c.debayer ? "true" : "false");
    if (!c.expectError.empty()) {
        o << ",\"expectError\":" << Quote(c.expectError) << "}";
        return o.str();
    }
    o << ",\"expectId\":" << Quote(c.expectId) << ",\"width\":" << c.width << ",\"height\":" << c.height
      << ",\"channels\":" << c.channels << ",\"samples\":[";
    for (size_t i = 0; i < c.samples.size(); ++i)
        o << (i ? "," : "") << "{\"x\":" << c.samples[i].x << ",\"y\":" << c.samples[i].y
          << ",\"c\":" << c.samples[i].c << ",\"v\":" << Num(c.samples[i].v) << "}";
    o << "],\"keywords\":[";
    for (size_t i = 0; i < c.keywords.size(); ++i)
        o << (i ? "," : "") << "{\"name\":" << Quote(c.keywords[i].name) << "," << Quote(c.keywords[i].field)
          << ":" << Quote(c.keywords[i].text) << "}";
    o << "],\"absentKeywords\":[";
    for (size_t i = 0; i < c.absent.size(); ++i) o << (i ? "," : "") << Quote(c.absent[i]);
    o << "]}";
    return o.str();
}

// Probe points: the four corners and two interior pixels.
std::vector<std::pair<int, int>> Points(int w, int h) {
    return {{0, 0}, {w - 1, 0}, {0, h - 1}, {w - 1, h - 1}, {5, 3}, {10, 7}};
}

// Independent bilinear demosaic of an RGGB mosaic at (x, y): neighbours are
// mirrored about the edge sample (-1 -> 1, n -> n-2), which keeps the parity.
double RggbExpected(int x, int y, int c, int f, int depth, int w, int h) {
    auto m = [](int i, int n) { return i < 0 ? -i : (i >= n ? 2 * (n - 1) - i : i); };
    auto s = [&](int dx, int dy) { return double(Gen(m(x + dx, w), m(y + dy, h), 0, f, depth)); };
    const bool evenRow = y % 2 == 0, evenCol = x % 2 == 0;
    const double here = s(0, 0);
    const double orth = (s(-1, 0) + s(1, 0) + s(0, -1) + s(0, 1)) / 4;
    const double diag = (s(-1, -1) + s(1, -1) + s(-1, 1) + s(1, 1)) / 4;
    const double horiz = (s(-1, 0) + s(1, 0)) / 2, vert = (s(0, -1) + s(0, 1)) / 2;
    double rgb[3];
    if (evenRow && evenCol) {        // red site
        rgb[0] = here; rgb[1] = orth; rgb[2] = diag;
    } else if (!evenRow && !evenCol) {  // blue site
        rgb[0] = diag; rgb[1] = orth; rgb[2] = here;
    } else if (evenRow) {            // green on a red row: red left/right, blue above/below
        rgb[0] = horiz; rgb[1] = here; rgb[2] = vert;
    } else {                         // green on a blue row: blue left/right, red above/below
        rgb[0] = vert; rgb[1] = here; rgb[2] = horiz;
    }
    return rgb[c] / ((1 << depth) - 1);
}

// 100-ns ticks since 0001-01-01 UTC, computed with Python's datetime:
// (datetime(2026,9,7,2,4,21,500000) - datetime(1,1,1)) in ticks, etc.
constexpr int64_t kHeaderUtc = 639243434615000000LL;   // 2026-09-07T02:04:21.500
constexpr int64_t kFrame0 = 639243438000000000LL;      // 2026-09-07T02:10:00.000
constexpr int64_t kFrameStep = 333330000LL;            // 33.333 s

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <out dir>\n", argv[0]);
        return 2;
    }
    const std::filesystem::path dir = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(dir);
    std::vector<Case> cases;

    // mono8: 64x48, 3 frames, header strings and a header UTC time, no trailer.
    {
        const int w = 64, h = 48, d = 8, n = 3;
        SerSpec s;
        s.colorId = 0; s.width = w; s.height = h; s.depth = d; s.frameCount = n;
        s.dateTimeUtc = kHeaderUtc;
        s.observer = "Scott"; s.instrument = "ZWO ASI585MC"; s.telescope = "VC200L";
        std::vector<std::vector<uint8_t>> frames(n);
        for (int f = 0; f < n; ++f)
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) frames[f].push_back(static_cast<uint8_t>(Gen(x, y, 0, f, d)));
        const std::string path = (dir / "mono8.ser").string();
        WriteSer(path, s, frames);

        Case c;
        c.name = "mono8"; c.path = path; c.frame = 1; c.expectId = "mono8_f1";
        c.width = w; c.height = h; c.channels = 1;
        for (auto [x, y] : Points(w, h)) c.samples.push_back({x, y, 0, Norm(Gen(x, y, 0, 1, d), d)});
        c.keywords = {{"DATE-OBS", "value", "2026-09-07T02:04:21.500"},
                      {"OBSERVER", "value", "Scott"},
                      {"INSTRUME", "value", "ZWO ASI585MC"},
                      {"TELESCOP", "value", "VC200L"},
                      {"HISTORY", "comment", "SERViewer: mono8.ser frame 1"}};
        c.absent = {"BAYERPAT"};
        cases.push_back(c);

        Case bad;
        bad.name = "frame out of range"; bad.path = path; bad.frame = 5;
        bad.expectError = "frame index 5 out of range";
        cases.push_back(bad);
    }

    // rgb16: depth 12, little-endian (LittleEndian field 0), 40x30, 2 frames, empty header.
    {
        const int w = 40, h = 30, d = 12, n = 2;
        SerSpec s;
        s.colorId = 100; s.width = w; s.height = h; s.depth = d; s.frameCount = n;
        std::vector<std::vector<uint8_t>> frames(n);
        for (int f = 0; f < n; ++f)
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    for (int ch = 0; ch < 3; ++ch) Put16LE(frames[f], Gen(x, y, ch, f, d));
        const std::string path = (dir / "rgb16.ser").string();
        WriteSer(path, s, frames);

        Case c;
        c.name = "rgb16"; c.path = path; c.frame = 0; c.expectId = "rgb16_f0";
        c.width = w; c.height = h; c.channels = 3;
        for (auto [x, y] : Points(w, h))
            for (int ch = 0; ch < 3; ++ch) c.samples.push_back({x, y, ch, Norm(Gen(x, y, ch, 0, d), d)});
        c.keywords = {{"HISTORY", "comment", "SERViewer: rgb16.ser frame 0"}};
        c.absent = {"DATE-OBS", "OBSERVER", "INSTRUME", "TELESCOP", "BAYERPAT"};
        cases.push_back(c);
    }

    // bgr8: 32x24, 2 frames; stored B,G,R per pixel, must open as R,G,B.
    {
        const int w = 32, h = 24, d = 8, n = 2;
        SerSpec s;
        s.colorId = 101; s.width = w; s.height = h; s.depth = d; s.frameCount = n;
        std::vector<std::vector<uint8_t>> frames(n);
        for (int f = 0; f < n; ++f)
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    for (int ch = 2; ch >= 0; --ch) frames[f].push_back(static_cast<uint8_t>(Gen(x, y, ch, f, d)));
        const std::string path = (dir / "bgr8.ser").string();
        WriteSer(path, s, frames);

        Case c;
        c.name = "bgr8"; c.path = path; c.frame = 1; c.expectId = "bgr8_f1";
        c.width = w; c.height = h; c.channels = 3;
        for (auto [x, y] : Points(w, h))
            for (int ch = 0; ch < 3; ++ch) c.samples.push_back({x, y, ch, Norm(Gen(x, y, ch, 1, d), d)});
        c.keywords = {{"HISTORY", "comment", "SERViewer: bgr8.ser frame 1"}};
        c.absent = {"BAYERPAT", "DATE-OBS"};
        cases.push_back(c);
    }

    // Bayer RGGB, depth 12 in 16-bit samples, 32x24, 3 frames, with a timestamp
    // trailer (frame 2's ticks carry flag bits that must be masked off) and a
    // header UTC time the frame time must win over. The file name starts with a
    // digit and holds a space and hyphens, to exercise the window id rules.
    {
        const int w = 32, h = 24, d = 12, n = 3;
        SerSpec s;
        s.colorId = 8; s.width = w; s.height = h; s.depth = d; s.frameCount = n;
        s.dateTimeUtc = kHeaderUtc;
        std::vector<std::vector<uint8_t>> frames(n);
        for (int f = 0; f < n; ++f)
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) Put16LE(frames[f], Gen(x, y, 0, f, d));
        const std::vector<int64_t> ticks = {kFrame0, kFrame0 + kFrameStep,
                                            static_cast<int64_t>(static_cast<uint64_t>(kFrame0 + 2 * kFrameStep) |
                                                                 (uint64_t(1) << 62))};
        const std::string path = (dir / "2026-09-07 Saturn-rggb16.ser").string();
        WriteSer(path, s, frames, ticks);

        Case c;
        c.name = "bayerRGGB16 debayered"; c.path = path; c.frame = 2; c.debayer = true;
        c.expectId = "_2026_09_07_Saturn_rggb16_f2";
        c.width = w; c.height = h; c.channels = 3;
        for (auto [x, y] : Points(w, h))
            for (int ch = 0; ch < 3; ++ch) c.samples.push_back({x, y, ch, RggbExpected(x, y, ch, 2, d, w, h)});
        c.keywords = {{"DATE-OBS", "value", "2026-09-07T02:11:06.666"},
                      {"HISTORY", "comment", "SERViewer: 2026-09-07 Saturn-rggb16.ser frame 2"}};
        c.absent = {"BAYERPAT", "OBSERVER"};
        cases.push_back(c);

        Case raw = c;
        raw.name = "bayerRGGB16 raw CFA"; raw.debayer = false; raw.frame = 1;
        raw.expectId = "_2026_09_07_Saturn_rggb16_f1";
        raw.channels = 1;
        raw.samples.clear();
        for (auto [x, y] : Points(w, h)) raw.samples.push_back({x, y, 0, Norm(Gen(x, y, 0, 1, d), d)});
        raw.keywords = {{"DATE-OBS", "value", "2026-09-07T02:10:33.333"},
                        {"BAYERPAT", "value", "RGGB"},
                        {"HISTORY", "comment", "SERViewer: 2026-09-07 Saturn-rggb16.ser frame 1"}};
        raw.absent = {"OBSERVER"};
        cases.push_back(raw);
    }

    // GUI smoke only (test/gui-smoke.sh), no probe case: Bayer RGGB 8-bit, 64x48,
    // 3 frames. Red sites are bright in 8-pixel bars, one bar in three, that move
    // one bar per frame (period 3, so the last frame differs from frame 0), green
    // sites dim, blue sites dark, so the debayered preview is unmistakably red (an
    // R/B swap shows blue) and every frame differs; with debayer off it is a grey mosaic.
    {
        const int w = 64, h = 48, n = 3;
        SerSpec s;
        s.colorId = 8; s.width = w; s.height = h; s.depth = 8; s.frameCount = n;
        std::vector<std::vector<uint8_t>> frames(n);
        for (int f = 0; f < n; ++f)
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const int site = (y % 2) * 2 + x % 2;  // 0 R, 1 and 2 G, 3 B
                    const int v = site == 0 ? 60 + 150 * ((x / 8 + f) % 3 == 0) : site == 3 ? 12 : 40;
                    frames[f].push_back(static_cast<uint8_t>(v));
                }
        WriteSer((dir / "bayer-colour.ser").string(), s, frames);
    }

    // FrameCount 0 (a capture that crashed before finalising its header):
    // 16x16 mono8, 2 frames on disk. Opens with a recovery warning.
    {
        const int w = 16, h = 16, d = 8, n = 2;
        SerSpec s;
        s.colorId = 0; s.width = w; s.height = h; s.depth = d; s.frameCount = 0;
        std::vector<std::vector<uint8_t>> frames(n);
        for (int f = 0; f < n; ++f)
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) frames[f].push_back(static_cast<uint8_t>(Gen(x, y, 0, f, d)));
        const std::string path = (dir / "framecount0.ser").string();
        WriteSer(path, s, frames);

        Case c;
        c.name = "framecount0"; c.path = path; c.frame = 1; c.expectId = "framecount0_f1";
        c.width = w; c.height = h; c.channels = 1;
        for (auto [x, y] : Points(w, h)) c.samples.push_back({x, y, 0, Norm(Gen(x, y, 0, 1, d), d)});
        // The warning is longer than one card (72 characters): the probe finds
        // it whole only if the continuation card follows and joins back.
        c.keywords = {{"HISTORY", "comment", "SERViewer: framecount0.ser frame 1"},
                      {"HISTORY", "commentContains",
                       "SERViewer warning: header frame count is 0; using 2 complete frames found in the file"}};
        c.absent = {"DATE-OBS"};
        cases.push_back(c);
    }

    // Declared depth 6, but frame 0 holds full 8-bit values: every frame is
    // normalised by 255 (not 63) and the open reports it.
    {
        const int w = 16, h = 16, d = 6, n = 2;
        SerSpec s;
        s.colorId = 0; s.width = w; s.height = h; s.depth = d; s.frameCount = n;
        std::vector<std::vector<uint8_t>> frames(n);
        int max0 = 0;
        for (int f = 0; f < n; ++f)
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const int v = Gen(x, y, 0, f, 8);
                    if (f == 0 && v > max0) max0 = v;
                    frames[f].push_back(static_cast<uint8_t>(v));
                }
        const std::string path = (dir / "depth6-over.ser").string();
        WriteSer(path, s, frames);

        Case c;
        c.name = "depth 6, frame 0 over 6 bits"; c.path = path; c.frame = 1; c.expectId = "depth6_over_f1";
        c.width = w; c.height = h; c.channels = 1;
        for (auto [x, y] : Points(w, h)) c.samples.push_back({x, y, 0, Norm(Gen(x, y, 0, 1, 8), 8)});
        c.keywords = {{"HISTORY", "commentContains",
                       "SERViewer warning: samples exceed the declared 6-bit depth (frame 0 max " +
                           std::to_string(max0) + "); normalising to the full 8-bit range"}};
        cases.push_back(c);
    }

    // Declared depth 6, frame 0 fits it, frame 1 does not (pixel 0,0 = 100):
    // frame 1 opens with values above 1.0 and says so in HISTORY.
    {
        const int w = 16, h = 16, d = 6, n = 2;
        SerSpec s;
        s.colorId = 0; s.width = w; s.height = h; s.depth = d; s.frameCount = n;
        std::vector<std::vector<uint8_t>> frames(n);
        for (int f = 0; f < n; ++f)
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) frames[f].push_back(static_cast<uint8_t>(Gen(x, y, 0, f, d)));
        frames[1][0] = 100;
        const std::string path = (dir / "depth6-later.ser").string();
        WriteSer(path, s, frames);

        Case c;
        c.name = "depth 6, frame 1 over 6 bits"; c.path = path; c.frame = 1; c.expectId = "depth6_later_f1";
        c.width = w; c.height = h; c.channels = 1;
        for (auto [x, y] : Points(w, h))
            c.samples.push_back({x, y, 0, x == 0 && y == 0 ? 100.0 / 63 : Norm(Gen(x, y, 0, 1, d), d)});
        c.keywords = {{"HISTORY", "commentContains",
                       "SERViewer warning: frame 1 samples exceed the 6-bit range; values above 1.0"}};
        cases.push_back(c);
    }

    // Non-ASCII file name (UTF-8 "Jüpiter"): '_' in the window id, '?' in HISTORY.
    {
        const int w = 16, h = 16, d = 8;
        SerSpec s;
        s.colorId = 0; s.width = w; s.height = h; s.depth = d; s.frameCount = 1;
        std::vector<std::vector<uint8_t>> frames(1);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) frames[0].push_back(static_cast<uint8_t>(Gen(x, y, 0, 0, d)));
        const std::string path = (dir / "J\xc3\xbcpiter.ser").string();
        WriteSer(path, s, frames);

        Case c;
        c.name = "utf8 file name"; c.path = path; c.frame = 0; c.expectId = "J_piter_f0";
        c.width = w; c.height = h; c.channels = 1;
        for (auto [x, y] : Points(w, h)) c.samples.push_back({x, y, 0, Norm(Gen(x, y, 0, 0, d), d)});
        c.keywords = {{"HISTORY", "comment", "SERViewer: J?piter.ser frame 0"}};
        cases.push_back(c);
    }

    // Bad magic: refused with the real reason.
    {
        SerSpec s;
        s.fileId = "NOT-A-SERFILE.";
        s.colorId = 0; s.width = 8; s.height = 8; s.depth = 8; s.frameCount = 1;
        const std::string path = (dir / "badmagic.ser").string();
        WriteSer(path, s, {std::vector<uint8_t>(64, 0)});
        Case c;
        c.name = "bad magic"; c.path = path; c.expectError = "LUCAM-RECORDER";
        cases.push_back(c);
    }

    // Missing file and empty filePath.
    {
        Case c;
        c.name = "missing file"; c.path = (dir / "does-not-exist.ser").string();
        c.expectError = "cannot open SER file";
        cases.push_back(c);
        Case e;
        e.name = "empty filePath"; e.path = ""; e.expectError = "no SER file selected";
        cases.push_back(e);
    }

    std::ofstream out(dir / "probe-in.json");
    out << "{\"cases\":[";
    for (size_t i = 0; i < cases.size(); ++i) out << (i ? ",\n" : "\n") << ToJson(cases[i]);
    out << "\n]}\n";
    if (!out) {
        std::fprintf(stderr, "make-fixtures: cannot write %s\n", (dir / "probe-in.json").c_str());
        return 1;
    }
    std::printf("make-fixtures: %zu cases in %s\n", cases.size(), (dir / "probe-in.json").c_str());
    return 0;
}
