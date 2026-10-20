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

std::string Quote(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
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
        c.keywords = {{"HISTORY", "comment", "SERViewer: framecount0.ser frame 1"},
                      {"HISTORY", "commentContains", "SERViewer warning: header frame count is 0"}};
        c.absent = {"DATE-OBS"};
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
