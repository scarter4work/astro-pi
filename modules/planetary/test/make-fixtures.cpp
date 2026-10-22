// Test-only CLI: writes synthetic SER and AVI fixtures and the headless probe's input
// JSON (expected dimensions, samples and keywords) into a directory.
//
//   make-fixtures <out dir>   ->  <out dir>/*.ser, *.avi (+ .avi.txt sidecars) + <out dir>/probe-in.json
//   make-fixtures --stack <out dir>  ->  PlanetaryStack's captures + <out dir>/stack-cases.json
//                                        (test/run-probe-stack.sh; see WriteStackFixtures)
//   make-fixtures --stack-long <file.avi> <frames>  ->  the long Bayer capture with that many frames
//                                        (test/gui-smoke-stack.sh; see WriteLongCapture)
//
// SERViewer's expected values come from the same generator formula that fills the files,
// plus an independent bilinear demosaic written here, never from the reader
// under test. Never shipped.
#include <cmath>
#include <functional>
#include <random>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "AviWriter.h"
#include "PlanetaryVersion.h"
#include "SerWriter.h"
#include "StackSynth.h"

using planetary::testing::AviSpec;
using planetary::testing::SerSpec;
using planetary::testing::WriteAvi;
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

// The warning every Auto-resolved Flip=Vert RG sidecar carries (Ruling 10).
const char* const kVertWarning =
    "Bayer pattern inferred from Flip=Vert (BGGR); some ASIAIR captures are RGGB -- if red and blue look swapped, "
    "choose RGGB under Bayer pattern";

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
    std::vector<std::string> absentComments;  // no keyword comment may contain any of these
    std::string bayerPattern;   // SERViewer enum element id ("BayerRGGB"); "" leaves the default
    std::string saveSerPath;    // non-empty: a conversion case (or an error case) with this target
    std::vector<std::string> expectConsole;  // conversion: texts the console carries, in this order
    std::vector<std::string> consoleAbsent;  // conversion: texts the console must not carry
};

std::string JsonList(const std::vector<std::string>& v) {
    std::string o = "[";
    for (size_t i = 0; i < v.size(); ++i) o += (i ? "," : "") + Quote(v[i]);
    return o + "]";
}

std::string ToJson(const Case& c) {
    std::ostringstream o;
    o << "{\"name\":" << Quote(c.name) << ",\"path\":" << Quote(c.path) << ",\"frame\":" << c.frame
      << ",\"debayer\":" << (c.debayer ? "true" : "false");
    if (!c.bayerPattern.empty()) o << ",\"bayerPattern\":" << Quote(c.bayerPattern);
    if (!c.saveSerPath.empty()) o << ",\"saveSerPath\":" << Quote(c.saveSerPath);
    if (!c.expectError.empty()) {
        o << ",\"expectError\":" << Quote(c.expectError) << "}";
        return o.str();
    }
    if (!c.saveSerPath.empty()) {
        o << ",\"expectConsole\":" << JsonList(c.expectConsole) << ",\"consoleAbsent\":" << JsonList(c.consoleAbsent)
          << "}";
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
    o << "],\"absentKeywords\":" << JsonList(c.absent);
    if (!c.absentComments.empty()) o << ",\"absentComments\":" << JsonList(c.absentComments);
    o << "}";
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

// The same bilinear demosaic for any 2x2 pattern ("RGGB", "GRBG", "GBRG", "BGGR",
// read row by row): a red/blue site keeps its value, takes green from the four
// orthogonal neighbours and the opposite colour from the four diagonals; a green
// site takes the colour of its left/right neighbours from them and the colour of
// its above/below neighbours from those.
double BayerExpected(const std::string& pattern, int x, int y, int c, int f, int depth, int w, int h) {
    auto m = [](int i, int n) { return i < 0 ? -i : (i >= n ? 2 * (n - 1) - i : i); };
    auto s = [&](int dx, int dy) { return double(Gen(m(x + dx, w), m(y + dy, h), 0, f, depth)); };
    auto colourAt = [&](int px, int py) {
        const char k = pattern[(py % 2) * 2 + px % 2];
        return k == 'R' ? 0 : k == 'G' ? 1 : 2;
    };
    const int site = colourAt(x, y);
    double rgb[3];
    if (site != 1) {
        rgb[site] = s(0, 0);
        rgb[1] = (s(-1, 0) + s(1, 0) + s(0, -1) + s(0, 1)) / 4;
        rgb[2 - site] = (s(-1, -1) + s(1, -1) + s(-1, 1) + s(1, 1)) / 4;
    } else {
        rgb[1] = s(0, 0);
        rgb[colourAt(x + 1, y)] = (s(-1, 0) + s(1, 0)) / 2;
        rgb[colourAt(x, y + 1)] = (s(0, -1) + s(0, 1)) / 2;
    }
    return rgb[c] / ((1 << depth) - 1);
}

// 8-bit mono frames from the generator, top row first (what WriteAvi takes).
std::vector<std::vector<uint8_t>> GenFrames8(int w, int h, int n) {
    std::vector<std::vector<uint8_t>> frames(n);
    for (int f = 0; f < n; ++f)
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) frames[f].push_back(static_cast<uint8_t>(Gen(x, y, 0, f, 8)));
    return frames;
}

void WriteText(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
    if (!out) {
        std::fprintf(stderr, "make-fixtures: cannot write %s\n", path.c_str());
        std::exit(1);
    }
}

// 100-ns ticks since 0001-01-01 UTC, computed with Python's datetime:
// (datetime(2026,9,7,2,4,21,500000) - datetime(1,1,1)) in ticks, etc.
constexpr int64_t kHeaderUtc = 639243434615000000LL;   // 2026-09-07T02:04:21.500
constexpr int64_t kFrame0 = 639243438000000000LL;      // 2026-09-07T02:10:00.000
constexpr int64_t kFrameStep = 333330000LL;            // 33.333 s

// ---- PlanetaryStack fixtures (make-fixtures --stack <dir>) ----------------------------------------
//
// Synthetic captures rendered with StackSynth.h (the engine tests' scenes, seeing and frame model),
// written as real SER and AVI files, plus <dir>/stack-cases.json for test/run-probe-stack.sh. What a
// stack of them must look like cannot come from a formula here: the run script takes it from the
// engine itself (build/test/stack-probe on the same file: pixel values, frames used, the diagnosis and
// its message) and checks that every fixture still gives the outcome named here, so a fixture that
// stops producing its diagnosis fails loudly instead of testing nothing.
//
// stack-cases.json: { "cases": [ { name, path, kind, ... } ] }, kind:
//   image      bayerPattern? (PlanetaryStack enum element id), probeBayer (stack-probe --bayer),
//              expectId, sample: [[x, y]], keywords: [{name, value|comment|commentContains}],
//              absentKeywords: [name], absentComments: [text] (no keyword comment contains it),
//              frameTimes? (ISO UTC per frame, from the SER trailer),
//              consoleContains: [text], expectFocusWarning? (the engine must warn "may be out of focus")
//   diagnosis  probeBayer, expectKind (stack-probe's diagnosis kind)
//   error      expectError (text the thrown error carries)
//   cancel     probeBayer, abortAfter (seconds), minSeconds (the run must take at least this long
//              uninterrupted, so the abort lands mid-run)

using planetary::testing::Capture;
using planetary::testing::FrameSpec;
using planetary::testing::Scene;

// A capture's 16-bit frames as 8-bit (what an ASIAIR AVI holds) or as 16-bit little-endian SER bytes.
std::vector<std::vector<uint8_t>> Frames8(const Capture& cap) {
    std::vector<std::vector<uint8_t>> out;
    for (const auto& f : *cap.frames) {
        std::vector<uint8_t> b(f.size());
        for (size_t i = 0; i < f.size(); ++i) b[i] = static_cast<uint8_t>(std::lround(f[i] / 257.0));
        out.push_back(std::move(b));
    }
    return out;
}

std::vector<std::vector<uint8_t>> Frames16(const Capture& cap) {
    std::vector<std::vector<uint8_t>> out;
    for (const auto& f : *cap.frames) {
        std::vector<uint8_t> b;
        b.reserve(f.size() * 2);
        for (uint16_t v : f) Put16LE(b, v);
        out.push_back(std::move(b));
    }
    return out;
}

// A scene seen through an RGGB colour filter: each site keeps its own colour of the scene times a
// per-colour gain (R 1.0, G 0.8, B 0.55), like the engine tests' mosaics.
Capture Mosaic(const Capture& mono) {
    const int w = mono.info.header.width, h = mono.info.header.height;
    const double gain[3] = {1.0, 0.8, 0.55};
    Capture cap = planetary::testing::NewCapture(w, h);
    for (const auto& f : *mono.frames) {
        std::vector<float> v(f.size());
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int c = (y & 1) == 0 ? ((x & 1) == 0 ? 0 : 1) : ((x & 1) == 0 ? 1 : 2);
                const size_t i = static_cast<size_t>(y) * w + x;
                v[i] = static_cast<float>(gain[c] * f[i] / 65535.0);
            }
        cap.Add(v);
    }
    return cap;
}

// A planet disc with seeing (drift, local displacement, a blur field around 1.2 px).
Capture SeeingDisc(int w, int h, double cx, double cy, double radius, int n, unsigned seed) {
    const Scene s = planetary::testing::MakeDisc(seed, cx, cy, radius);
    Capture cap = planetary::testing::NewCapture(w, h);
    std::mt19937 rng(seed + 1000);
    planetary::testing::RenderCapture(cap, s, n, [&](int) { return planetary::testing::Seeing(rng, w, h, 2.0, 28); });
    return cap;
}

// A disc of `radius` px centred in a (4 R + 80)-px square frame, with 1-px drift, whose limb is blurred
// by blur(rng) px in each frame, everywhere alike, and pixel noise `noise`.
Capture BlurredDisc(double radius, int n, double noise, const std::function<double(std::mt19937&)>& blur) {
    const int side = static_cast<int>(4 * radius + 80) / 2 * 2;
    const Scene s = planetary::testing::MakeDisc(61, side / 2.0 + 0.3, side / 2.0 - 0.2, radius);
    Capture cap = planetary::testing::NewCapture(side, side);
    std::mt19937 rng(68);
    std::normal_distribution<double> n01(0, 1);
    planetary::testing::RenderCapture(cap, s, n, [&](int) {
        FrameSpec f;
        f.noise = noise;
        f.gx = n01(rng);
        f.gy = n01(rng);
        f.hasLocal = true;
        f.dx = planetary::testing::RandomField(rng, side, side, 32, [] { return 0.0; });
        f.dy = planetary::testing::RandomField(rng, side, side, 32, [] { return 0.0; });
        const double b = blur(rng);
        f.sigma = planetary::testing::RandomField(rng, side, side, 32, [b] { return b; });
        return f;
    });
    return cap;
}

// A disc with small drift whose every pixel is scaled by `gain` (clipped at full scale).
Capture BrightDisc(int w, int h, double radius, int n, unsigned seed, double gain) {
    const Scene s = planetary::testing::MakeDisc(seed, w / 2.0 + 0.3, h / 2.0 - 0.2, radius);
    Capture cap = planetary::testing::NewCapture(w, h);
    std::mt19937 rng(seed + 7);
    std::normal_distribution<double> n01(0, 1);
    planetary::testing::RenderCapture(cap, s, n, [&](int) {
        FrameSpec f;
        f.gx = 1.0 * n01(rng);
        f.gy = 1.0 * n01(rng);
        f.gain = gain;
        return f;
    });
    return cap;
}

// "2026-09-07T02:10:00.000" plus k seconds (frame k of the mono SER, one per second from kFrame0).
std::string FrameIso(int k) {
    char b[40];
    std::snprintf(b, sizeof b, "2026-09-07T02:%02d:%02d.000", 10 + k / 60, k % 60);
    return b;
}

struct StackCase {
    std::string name, path, kind, bayerPattern, probeBayer = "auto", expectId, expectKind, expectError;
    std::vector<std::pair<int, int>> sample;
    std::vector<Kw> keywords;
    std::vector<std::string> absent, absentComments, frameTimes, consoleContains, consoleAbsent;
    double abortAfter = 0, minSeconds = 0;
    bool expectFocusWarning = false;  // image: the engine must warn "may be out of focus"
};

std::string ToJson(const StackCase& c) {
    std::ostringstream o;
    o << "{\"name\":" << Quote(c.name) << ",\"path\":" << Quote(c.path) << ",\"kind\":" << Quote(c.kind)
      << ",\"probeBayer\":" << Quote(c.probeBayer);
    if (!c.bayerPattern.empty()) o << ",\"bayerPattern\":" << Quote(c.bayerPattern);
    if (!c.expectId.empty()) o << ",\"expectId\":" << Quote(c.expectId);
    if (!c.expectKind.empty()) o << ",\"expectKind\":" << Quote(c.expectKind);
    if (!c.expectError.empty()) o << ",\"expectError\":" << Quote(c.expectError);
    if (c.kind == "cancel") o << ",\"abortAfter\":" << Num(c.abortAfter) << ",\"minSeconds\":" << Num(c.minSeconds);
    if (c.expectFocusWarning) o << ",\"expectFocusWarning\":true";
    o << ",\"sample\":[";
    for (size_t i = 0; i < c.sample.size(); ++i) o << (i ? "," : "") << "[" << c.sample[i].first << "," << c.sample[i].second << "]";
    o << "],\"keywords\":[";
    for (size_t i = 0; i < c.keywords.size(); ++i)
        o << (i ? "," : "") << "{\"name\":" << Quote(c.keywords[i].name) << "," << Quote(c.keywords[i].field) << ":"
          << Quote(c.keywords[i].text) << "}";
    o << "],\"absentKeywords\":" << JsonList(c.absent) << ",\"absentComments\":" << JsonList(c.absentComments)
      << ",\"frameTimes\":" << JsonList(c.frameTimes)
      << ",\"consoleContains\":" << JsonList(c.consoleContains)
      << ",\"consoleAbsent\":" << JsonList(c.consoleAbsent) << "}";
    return o.str();
}

const char* const kAsiairSidecarRG =
    "[ZWO ASI585MC]\nCapture Area Size = %d * %d\nColour Format = RAW8\nFlip = None\nBayer = RG\n";

void WriteSidecarRG(const std::string& avi, int w, int h) {
    char text[256];
    std::snprintf(text, sizeof text, kAsiairSidecarRG, w, h);
    WriteText(avi + ".txt", text);
}

// The long Bayer capture (1024 x 768 RGGB AVI with its RG sidecar: a disc of radius 180 with seeing,
// 30 rendered frames repeated to n): the probe's cancel case (600 frames) and the GUI smoke's run
// (make-fixtures --stack-long, more frames so the progress bar and Cancel land mid-run).
void WriteLongCapture(const std::string& path, int n) {
    const int w = 1024, h = 768, unique = 30;
    const std::vector<std::vector<uint8_t>> rendered = Frames8(Mosaic(SeeingDisc(w, h, 512.4, 384.6, 180, unique, 81)));
    std::vector<std::vector<uint8_t>> frames;
    for (int k = 0; k < n; ++k) frames.push_back(rendered[static_cast<size_t>(k % unique)]);
    AviSpec a;
    a.width = w; a.height = h;
    WriteAvi(path, a, frames);
    WriteSidecarRG(path, w, h);
}

int WriteStackFixtures(const std::filesystem::path& dir) {
    std::vector<StackCase> cases;
    const std::string kMosaicWarning = "No Bayer pattern known (no .avi.txt sidecar), so the raw colour mosaic was stacked "
                                       "as mono";
    const std::string kMosaicRemedy = "set bayerPattern (or save the pattern with Save pattern... in PlanetaryStack or "
                                      "SERViewer) and stack again";

    // A Bayer AVI with an ASIAIR sidecar (RG, Flip None -> RGGB): 192 x 160, a disc of radius 40
    // sensor px with seeing, 40 frames.
    const std::string bayerAvi = (dir / "stack-bayer.avi").string();
    {
        const int w = 192, h = 160;
        const Capture cap = Mosaic(SeeingDisc(w, h, 96.4, 80.6, 40, 40, 31));
        AviSpec a;
        a.width = w; a.height = h;
        WriteAvi(bayerAvi, a, Frames8(cap));
        WriteSidecarRG(bayerAvi, w, h);

        StackCase c;
        c.name = "Bayer AVI with sidecar (Auto -> RGGB)"; c.path = bayerAvi; c.kind = "image";
        c.expectId = "stack_bayer_stack";
        c.sample = {{96, 80}, {97, 81}, {70, 64}, {120, 100}, {135, 80}, {10, 10}};
        c.keywords = {{"INSTRUME", "value", "ZWO ASI585MC"},
                      {"SRCVIDEO", "value", "stack-bayer.avi"},
                      {"HISTORY", "commentContains", "PlanetaryStack " PLANETARY_VERSION_STRING ": stacked stack-bayer.avi"},
                      {"HISTORY", "commentContains", "PlanetaryStack: Bayer RGGB (sidecar)"}};
        c.absent = {"DATE-OBS", "DATE-END", "BAYERPAT"};
        c.consoleContains = {"PlanetaryStack: Bayer RGGB (sidecar)"};
        cases.push_back(c);

        // A pattern set explicitly wins over the sidecar (as in SERViewer).
        StackCase o = c;
        o.name = "bayerPattern=BayerBGGR overrides the sidecar"; o.bayerPattern = "BayerBGGR"; o.probeBayer = "BGGR";
        o.keywords = {{"INSTRUME", "value", "ZWO ASI585MC"},
                      {"HISTORY", "commentContains", "PlanetaryStack: Bayer BGGR (chosen)"}};
        o.consoleContains = {"PlanetaryStack: Bayer BGGR (chosen)"};
        cases.push_back(o);
    }

    // Mono 16-bit SER with header strings and a timestamp trailer (one frame a second): 128 x 112, a
    // disc of radius 26 px with seeing, 60 frames.
    {
        const int w = 128, h = 112, n = 60;
        const Capture cap = SeeingDisc(w, h, 64.3, 55.7, 26, n, 11);
        SerSpec s;
        s.colorId = 0; s.width = w; s.height = h; s.depth = 16; s.frameCount = n;
        s.dateTimeUtc = kHeaderUtc;
        s.observer = "Scott"; s.instrument = "ZWO ASI462MM"; s.telescope = "VC200L";
        std::vector<int64_t> ticks;
        StackCase c;
        for (int k = 0; k < n; ++k) {
            ticks.push_back(kFrame0 + static_cast<int64_t>(k) * 10000000LL);
            c.frameTimes.push_back(FrameIso(k));
        }
        const std::string path = (dir / "stack-mono.ser").string();
        WriteSer(path, s, Frames16(cap), ticks);

        c.name = "mono SER"; c.path = path; c.kind = "image"; c.expectId = "stack_mono_stack";
        c.sample = {{64, 56}, {50, 40}, {80, 70}, {89, 56}, {5, 5}};
        c.keywords = {{"INSTRUME", "value", "ZWO ASI462MM"},
                      {"TELESCOP", "value", "VC200L"},
                      {"OBSERVER", "value", "Scott"},
                      {"SRCVIDEO", "value", "stack-mono.ser"}};
        c.absent = {"BAYERPAT"};
        cases.push_back(c);

        // The same frames with no timestamp trailer: DATE-OBS falls back to the header's capture start,
        // and there is no DATE-END.
        const std::string headerOnly = (dir / "stack-mono-header.ser").string();
        WriteSer(headerOnly, s, Frames16(cap));
        StackCase hdr = c;
        hdr.name = "mono SER without timestamps: DATE-OBS from the header"; hdr.path = headerOnly;
        hdr.expectId = "stack_mono_header_stack";
        hdr.frameTimes.clear();
        hdr.keywords = {{"DATE-OBS", "value", "2026-09-07T02:04:21.500"},
                        {"DATE-OBS", "comment", "UTC start of the capture (SER header)"},
                        {"SRCVIDEO", "value", "stack-mono-header.ser"}};
        hdr.absent = {"BAYERPAT", "DATE-END"};
        cases.push_back(hdr);
    }

    // A lunar-like surface filling the frame, blurred by 12 px: stacked (a frame-filling target is never
    // refused for focus, Ruling P16) with the region stage's "may be out of focus" warning, which must reach
    // HISTORY and the console. Mono 16-bit SER, 224 x 176, 40 frames (the engine test's soft surface).
    {
        const int w = 224, h = 176;
        const Scene scene = planetary::testing::MakeSurface(71, w, h);
        Capture cap = planetary::testing::NewCapture(w, h);
        std::mt19937 rng(7100);
        std::normal_distribution<double> n01(0, 1);
        planetary::testing::RenderCapture(cap, scene, 40, [&](int) {
            FrameSpec f;
            f.gx = 1.5 * n01(rng);
            f.gy = 1.5 * n01(rng);
            f.hasLocal = true;
            f.dx = planetary::testing::RandomField(rng, w, h, 40, [&] { return 0.3 * n01(rng); });
            f.dy = planetary::testing::RandomField(rng, w, h, 40, [&] { return 0.3 * n01(rng); });
            f.sigma = planetary::testing::RandomField(rng, w, h, 40, [&] { return 12.0 * std::exp(0.1 * n01(rng)); });
            return f;
        });
        SerSpec s;
        s.colorId = 0; s.width = w; s.height = h; s.depth = 16; s.frameCount = 40;
        const std::string path = (dir / "stack-soft-surface.ser").string();
        WriteSer(path, s, Frames16(cap));
        StackCase c;
        c.name = "soft frame-filling surface: stacked, focus warning in HISTORY"; c.path = path; c.kind = "image";
        c.expectId = "stack_soft_surface_stack";
        c.expectFocusWarning = true;
        c.sample = {{112, 88}, {30, 20}, {200, 150}};
        c.keywords = {{"HISTORY", "commentContains", "PlanetaryStack warning: The stack may be out of focus"}};
        c.absent = {"DATE-OBS", "BAYERPAT"};
        c.consoleContains = {"PlanetaryStack warning: The stack may be out of focus"};
        cases.push_back(c);
    }

    // A small disc (radius 8 px, under kMinJudgedRadius) blurred by 5 px: Select does not refuse it (its
    // small-disc rule never fired on measured data, Select.h step 7), the region stage warns (limb blur
    // over radius 0.53 against 0.4, measured) -- the disc warn-but-stack band. Mono 16-bit SER, 60 frames.
    {
        const Capture cap = BlurredDisc(8, 60, 0.01, [](std::mt19937&) { return 5.0; });
        SerSpec s;
        s.colorId = 0; s.width = cap.info.header.width; s.height = cap.info.header.height; s.depth = 16; s.frameCount = 60;
        const std::string path = (dir / "stack-soft-disc.ser").string();
        WriteSer(path, s, Frames16(cap));
        StackCase c;
        c.name = "soft small disc: stacked, focus warning in HISTORY"; c.path = path; c.kind = "image";
        c.expectId = "stack_soft_disc_stack";
        c.expectFocusWarning = true;
        c.sample = {{56, 56}, {60, 52}, {10, 10}};
        c.keywords = {{"HISTORY", "commentContains", "PlanetaryStack warning: The stack may be out of focus: the sharpest frames blur the limb"}};
        c.absent = {"DATE-OBS", "BAYERPAT"};
        c.consoleContains = {"PlanetaryStack warning: The stack may be out of focus: the sharpest frames blur the limb"};
        cases.push_back(c);
    }

    // An AVI of a colour mosaic with no sidecar, Auto (Ruling P9): stacked as raw mono, with the warning.
    {
        const int w = 192, h = 160;
        const Capture cap = Mosaic(SeeingDisc(w, h, 96.4, 80.6, 40, 40, 32));
        AviSpec a;
        a.width = w; a.height = h;
        const std::string path = (dir / "stack-mosaic.avi").string();
        WriteAvi(path, a, Frames8(cap));

        StackCase c;
        c.name = "undeclared mosaic AVI -> raw mono, warned"; c.path = path; c.kind = "image";
        c.expectId = "stack_mosaic_stack";
        c.sample = {{96, 80}, {97, 80}, {96, 81}, {120, 100}, {10, 10}};
        // One warning for the one condition (stackcards::ShownWarnings): the reader's reason, what was
        // stacked and why it cannot be undone, and the process's remedy. The reader's own line and the
        // engine's P9 text (with its SERViewer-only remedy) are merged into it, never shown as well.
        c.keywords = {{"HISTORY", "commentContains", "PlanetaryStack warning: " + kMosaicWarning},
                      {"HISTORY", "commentContains", "cannot be debayered afterwards"},
                      {"HISTORY", "commentContains", kMosaicRemedy}};
        c.absent = {"BAYERPAT", "INSTRUME", "DATE-OBS"};
        c.absentComments = {"showing the raw CFA", "the raw CFA is stacked as mono", "No Bayer pattern is known for this AVI",
                            "SERViewer's Save pattern... remembers it"};
        c.consoleContains = {kMosaicWarning, "cannot be debayered afterwards", kMosaicRemedy};
        c.consoleAbsent = {"showing the raw CFA", "the raw CFA is stacked as mono", "No Bayer pattern is known for this AVI",
                           "SERViewer's Save pattern... remembers it"};
        cases.push_back(c);
    }

    // Diagnoses: captures that must give no image, each with the engine's own message.
    auto diagnosis = [&](const std::string& name, const std::string& file, const Capture& cap, const std::string& kind) {
        const std::string path = (dir / file).string();
        SerSpec s;
        s.colorId = 0; s.width = cap.info.header.width; s.height = cap.info.header.height; s.depth = 16;
        s.frameCount = static_cast<int32_t>(cap.frames->size());
        WriteSer(path, s, Frames16(cap));
        StackCase c;
        c.name = name; c.path = path; c.kind = "diagnosis"; c.expectKind = kind;
        cases.push_back(c);
    };
    {
        // Flat 2% sky and noise: the 2026-01-30 Saturn AVI in miniature.
        Capture cap = planetary::testing::NewCapture(96, 80);
        Scene sky;
        sky.level = 0.02;
        planetary::testing::RenderCapture(cap, sky, 40, [](int) {
            FrameSpec f;
            f.noise = 0.004;
            return f;
        });
        diagnosis("diagnosis: no target", "diag-notarget.ser", cap, "NoTarget");
    }
    {
        // The disc sits on the left edge (half cut off) in 36 of 40 frames.
        const int w = 160, h = 120;
        const Scene inside = planetary::testing::MakeDisc(41, 80, 60, 20);
        const Scene edge = planetary::testing::MakeDisc(41, 6, 60, 20);
        Capture cap = planetary::testing::NewCapture(w, h);
        for (int k = 0; k < 40; ++k) {
            FrameSpec f;
            cap.Add(planetary::testing::RenderFrame(k < 4 ? inside : edge, w, h, f, 500u + k));
        }
        diagnosis("diagnosis: target leaves the frame", "diag-edge.ser", cap, "TargetLeavesFrame");
    }
    {
        // A disc overexposed to full scale in every frame.
        Capture cap = BrightDisc(128, 112, 26, 40, 51, 3.0);
        diagnosis("diagnosis: clipped", "diag-clipped.ser", cap, "Clipped");
    }
    // Focus and seeing (Ruling P23): only Select refuses them, and only for a disc of half-contrast radius
    // >= kMinJudgedRadius blurred past kGrossBlurLimit (Select.h step 7). A disc of radius 24 px in low
    // noise (0.003; at 0.01 the reading levels off near the limit): measured 2026-10-22, the sharpest frames
    // read 37-38% of the radius against the 25% limit.
    {
        // The same blur, 16 px, in all 60 frames: steady -- "the optics, not the air".
        Capture cap = BlurredDisc(24, 60, 0.003, [](std::mt19937&) { return 16.0; });
        diagnosis("diagnosis: out of focus", "diag-defocus.ser", cap, "OutOfFocus");
    }
    {
        // 26 px x a 10% log-normal spread over 200 frames: the blur changes from frame to frame (spread at
        // least 26% beyond the noise, measured) and even the sharpest frames read 37% of the radius.
        Capture cap = BlurredDisc(24, 200, 0.003, [](std::mt19937& rng) {
            std::normal_distribution<double> n01(0, 1);
            return 26.0 * std::exp(0.1 * n01(rng));
        });
        diagnosis("diagnosis: poor seeing", "diag-seeing.ser", cap, "PoorSeeing");
    }
    {
        // Every frame a different lunar field: no frame aligns to the others.
        const int w = 160, h = 128;
        Capture cap = planetary::testing::NewCapture(w, h);
        for (int k = 0; k < 30; ++k) {
            FrameSpec f;
            cap.Add(planetary::testing::RenderFrame(planetary::testing::MakeSurface(900u + k, w, h), w, h, f, 77u + k));
        }
        diagnosis("diagnosis: unaligned", "diag-unaligned.ser", cap, "Unaligned");
    }

    // Files that cannot be stacked at all.
    {
        StackCase m;
        m.name = "missing file"; m.path = (dir / "does-not-exist.avi").string(); m.kind = "error";
        m.expectError = "cannot open video file";
        cases.push_back(m);

        const std::string junk = (dir / "not-a-video.avi").string();
        WriteText(junk, std::string(4096, 'x'));
        StackCase j;
        j.name = "not a video file"; j.path = junk; j.kind = "error"; j.expectError = "not-a-video.avi";
        cases.push_back(j);

        StackCase e;
        e.name = "empty filePath"; e.path = ""; e.kind = "error"; e.expectError = "no video file selected";
        cases.push_back(e);
    }

    // Cancel: a longer Bayer capture (1024 x 768, 600 frames: 30 rendered frames, repeated) aborted from
    // the Process Console while it runs; afterwards the Bayer AVI above must still stack (the run script
    // runs it again after the cancel).
    {
        const std::string path = (dir / "stack-long.avi").string();
        WriteLongCapture(path, 600);
        StackCase c;
        c.name = "cancel mid-run from the console"; c.path = path; c.kind = "cancel";
        c.abortAfter = 0.5; c.minSeconds = 1.5;
        cases.push_back(c);
    }

    std::ofstream out(dir / "stack-cases.json");
    out << "{\"cases\":[";
    for (size_t i = 0; i < cases.size(); ++i) out << (i ? ",\n" : "\n") << ToJson(cases[i]);
    out << "\n]}\n";
    if (!out) {
        std::fprintf(stderr, "make-fixtures: cannot write %s\n", (dir / "stack-cases.json").c_str());
        return 1;
    }
    std::printf("make-fixtures: %zu PlanetaryStack cases in %s\n", cases.size(), (dir / "stack-cases.json").c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--stack") {
        const std::filesystem::path dir = std::filesystem::absolute(argv[2]);
        std::filesystem::create_directories(dir);
        return WriteStackFixtures(dir);
    }
    if (argc == 4 && std::string(argv[1]) == "--stack-long") {
        const int n = std::atoi(argv[3]);
        if (n < 1) {
            std::fprintf(stderr, "make-fixtures: --stack-long needs a frame count >= 1, got '%s'\n", argv[3]);
            return 2;
        }
        WriteLongCapture(argv[2], n);
        std::printf("make-fixtures: %s, %d frames\n", argv[2], n);
        return 0;
    }
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <out dir>\n       %s --stack <out dir>\n       %s --stack-long <file.avi> <frames>\n",
                     argv[0], argv[0], argv[0]);
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
    // The same frames go into bayer-colour.avi (top-down, grey palette) with an
    // ASIAIR sidecar saying Bayer RG, Flip None, so Auto resolves to RGGB and the
    // AVI previews red too; Save as SER of it must write these frame bytes.
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

        AviSpec a;
        a.width = w; a.height = h;
        const std::string avi = (dir / "bayer-colour.avi").string();
        WriteAvi(avi, a, frames);
        WriteText(avi + ".txt",
                  "[ZWO ASI585MC]\nCapture Area Size = 64 * 48\nColour Format = RAW8\nFlip = None\nBayer = RG\n");
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

    // The generalised demosaic must agree with the RGGB one the SER cases use.
    for (int y = 0; y < 9; ++y)
        for (int x = 0; x < 13; ++x)
            for (int ch = 0; ch < 3; ++ch)
                if (BayerExpected("RGGB", x, y, ch, 1, 8, 13, 9) != RggbExpected(x, y, ch, 1, 8, 13, 9)) {
                    std::fprintf(stderr, "make-fixtures: BayerExpected disagrees with RggbExpected\n");
                    return 1;
                }

    // AVI with an ASIAIR sidecar saying Bayer RG, Flip Vert: Auto resolves to BGGR.
    // 32x24, 3 frames, BI_RGB grey palette, stored bottom-up.
    const std::string aviVert = (dir / "avi-vert.avi").string();
    {
        const int w = 32, h = 24, d = 8;
        AviSpec s;
        s.width = w; s.height = h; s.bottomUp = true;
        WriteAvi(aviVert, s, GenFrames8(w, h, 3));
        WriteText(aviVert + ".txt",
                  "[ZWO ASI585MC]\r\nCapture Area Size = 32 * 24\r\nColour Format = RAW8\r\n"
                  "Flip = Vert(unimplemented)\r\nBayer = RG\r\nGain = 200\r\nExposure = 10ms\r\n");

        Case c;
        c.name = "avi sidecar Auto debayered (RG + Flip=Vert -> BGGR)"; c.path = aviVert; c.frame = 2;
        c.expectId = "avi_vert_f2";
        c.width = w; c.height = h; c.channels = 3;
        for (auto [x, y] : Points(w, h))
            for (int ch = 0; ch < 3; ++ch) c.samples.push_back({x, y, ch, BayerExpected("BGGR", x, y, ch, 2, d, w, h)});
        c.keywords = {{"INSTRUME", "value", "ZWO ASI585MC"},
                      {"HISTORY", "comment", "SERViewer: avi-vert.avi frame 2"},
                      {"HISTORY", "commentContains", "SERViewer: Bayer BGGR (sidecar RG, Flip=Vert -> BGGR)"},
                      // Ruling 10: the Flip=Vert pattern is labelled an inference.
                      {"HISTORY", "commentContains", std::string("SERViewer warning: ") + kVertWarning}};
        c.absent = {"DATE-OBS", "BAYERPAT", "OBSERVER", "TELESCOP"};
        cases.push_back(c);

        Case o;
        o.name = "avi bayerPattern=BayerRGGB overrides the sidecar"; o.path = aviVert; o.frame = 1;
        o.bayerPattern = "BayerRGGB";
        o.expectId = "avi_vert_f1";
        o.width = w; o.height = h; o.channels = 3;
        for (auto [x, y] : Points(w, h))
            for (int ch = 0; ch < 3; ++ch) o.samples.push_back({x, y, ch, BayerExpected("RGGB", x, y, ch, 1, d, w, h)});
        o.keywords = {{"INSTRUME", "value", "ZWO ASI585MC"},
                      {"HISTORY", "comment", "SERViewer: Bayer RGGB (chosen)"}};
        o.absent = {"DATE-OBS", "BAYERPAT"};
        o.absentComments = {"inferred from Flip=Vert"};  // a chosen pattern is no inference
        cases.push_back(o);

        Case r;
        r.name = "avi sidecar raw CFA"; r.path = aviVert; r.frame = 0; r.debayer = false;
        r.expectId = "avi_vert_f0";
        r.width = w; r.height = h; r.channels = 1;
        for (auto [x, y] : Points(w, h)) r.samples.push_back({x, y, 0, Norm(Gen(x, y, 0, 0, d), d)});
        r.keywords = {{"BAYERPAT", "value", "BGGR"}, {"INSTRUME", "value", "ZWO ASI585MC"}};
        r.absent = {"DATE-OBS"};
        cases.push_back(r);
    }

    // AVI without a sidecar: Auto shows the raw CFA as mono and says why.
    {
        const int w = 16, h = 16, d = 8;
        AviSpec s;
        s.width = w; s.height = h;
        const std::string path = (dir / "avi-nosidecar.avi").string();
        WriteAvi(path, s, GenFrames8(w, h, 2));

        Case c;
        c.name = "avi no sidecar, Auto -> mono with a warning"; c.path = path; c.frame = 1;
        c.expectId = "avi_nosidecar_f1";
        c.width = w; c.height = h; c.channels = 1;
        for (auto [x, y] : Points(w, h)) c.samples.push_back({x, y, 0, Norm(Gen(x, y, 0, 1, d), d)});
        c.keywords = {{"HISTORY", "comment", "SERViewer: avi-nosidecar.avi frame 1"},
                      {"HISTORY", "commentContains",
                       "SERViewer warning: no Bayer pattern known (no .avi.txt sidecar); showing the raw CFA as "
                       "mono -- choose the pattern under Bayer pattern"}};
        c.absent = {"DATE-OBS", "INSTRUME", "BAYERPAT"};
        cases.push_back(c);
    }

    // AVI whose sidecar has no Bayer line: mono, and the warning names the sidecar
    // (never "no .avi.txt sidecar"); converting it writes MONO and says so.
    {
        const int w = 16, h = 16, d = 8;
        AviSpec s;
        s.width = w; s.height = h;
        const std::string path = (dir / "avi-nobayer.avi").string();
        WriteAvi(path, s, GenFrames8(w, h, 2));
        WriteText(path + ".txt", "[ZWO ASI120MM Mini]\nCapture Area Size = 16 * 16\nColour Format = RAW8\nGain = 50\n");

        Case c;
        c.name = "avi sidecar without a Bayer line -> mono, says so"; c.path = path; c.frame = 0;
        c.expectId = "avi_nobayer_f0";
        c.width = w; c.height = h; c.channels = 1;
        for (auto [x, y] : Points(w, h)) c.samples.push_back({x, y, 0, Norm(Gen(x, y, 0, 0, d), d)});
        c.keywords = {{"INSTRUME", "value", "ZWO ASI120MM Mini"},
                      {"HISTORY", "commentContains",
                       "SERViewer warning: sidecar avi-nobayer.avi.txt has no Bayer value; showing the raw CFA as "
                       "mono -- choose the pattern under Bayer pattern"}};
        c.absent = {"DATE-OBS", "BAYERPAT"};
        c.absentComments = {"no .avi.txt sidecar", "SERViewer: Bayer"};
        cases.push_back(c);

        const std::string out = (dir / "avi-nobayer-converted.ser").string();
        std::error_code ec;
        std::filesystem::remove(out, ec);
        Case conv;
        conv.name = "saveSerPath, sidecar without a Bayer line, writes MONO"; conv.path = path; conv.saveSerPath = out;
        conv.expectConsole = {"converting avi-nobayer.avi to SER; Bayer none (no sidecar pattern",
                              "sidecar avi-nobayer.avi.txt has no Bayer value; the SER will be written as MONO (raw CFA)",
                              "SERViewer: wrote 2 frames (MONO) to " + out};
        conv.consoleAbsent = {"showing the raw CFA", "no .avi.txt sidecar"};
        cases.push_back(conv);
    }

    // AVI whose sidecar is there but unreadable (no section or key lines): mono, and the
    // warning says the sidecar could not be read -- never "no .avi.txt sidecar".
    {
        const int w = 16, h = 16, d = 8;
        AviSpec s;
        s.width = w; s.height = h;
        const std::string path = (dir / "avi-badsidecar.avi").string();
        WriteAvi(path, s, GenFrames8(w, h, 2));
        WriteText(path + ".txt", "not a capture log\n");

        Case c;
        c.name = "avi unreadable sidecar -> mono, says it could not be read"; c.path = path; c.frame = 0;
        c.expectId = "avi_badsidecar_f0";
        c.width = w; c.height = h; c.channels = 1;
        for (auto [x, y] : Points(w, h)) c.samples.push_back({x, y, 0, Norm(Gen(x, y, 0, 0, d), d)});
        c.keywords = {{"HISTORY", "commentContains",
                       "SERViewer warning: sidecar avi-badsidecar.avi.txt could not be read; no Bayer pattern known; "
                       "showing the raw CFA as mono -- choose the pattern under Bayer pattern"}};
        c.absent = {"DATE-OBS", "INSTRUME", "BAYERPAT"};
        c.absentComments = {"no .avi.txt sidecar", "SERViewer: Bayer"};
        cases.push_back(c);

        const std::string out = (dir / "avi-badsidecar-converted.ser").string();
        std::error_code ec;
        std::filesystem::remove(out, ec);
        Case conv;
        conv.name = "saveSerPath, unreadable sidecar, writes MONO"; conv.path = path; conv.saveSerPath = out;
        conv.expectConsole = {"converting avi-badsidecar.avi to SER; Bayer none (sidecar could not be read",
                              "sidecar avi-badsidecar.avi.txt could not be read; no Bayer pattern known; the SER will "
                              "be written as MONO (raw CFA)",
                              "SERViewer: wrote 2 frames (MONO) to " + out};
        conv.consoleAbsent = {"showing the raw CFA", "no .avi.txt sidecar"};
        cases.push_back(conv);
    }

    // OpenDML: Y800, 5 frames in RIFF AVI + 2 RIFF AVIX segments (2 frames each but the last).
    {
        const int w = 24, h = 16, d = 8, n = 5;
        AviSpec s;
        s.width = w; s.height = h; s.fourcc = "Y800";
        s.index = AviSpec::OpenDml; s.framesPerRiff = 2;
        const std::string path = (dir / "avi-opendml.avi").string();
        WriteAvi(path, s, GenFrames8(w, h, n));

        Case c;
        c.name = "avi OpenDML last frame"; c.path = path; c.frame = n - 1;
        c.expectId = "avi_opendml_f4";
        c.width = w; c.height = h; c.channels = 1;
        for (auto [x, y] : Points(w, h)) c.samples.push_back({x, y, 0, Norm(Gen(x, y, 0, n - 1, d), d)});
        c.keywords = {{"HISTORY", "comment", "SERViewer: avi-opendml.avi frame 4"}};
        c.absent = {"DATE-OBS", "INSTRUME"};
        cases.push_back(c);
    }

    // Odd size 13x9 (rows padded to 16 bytes), sidecar Bayer GR, Flip None -> GRBG.
    {
        const int w = 13, h = 9, d = 8;
        AviSpec s;
        s.width = w; s.height = h;
        const std::string path = (dir / "avi-odd.avi").string();
        WriteAvi(path, s, GenFrames8(w, h, 1));
        WriteText(path + ".txt",
                  "[ZWO ASI224MC]\nCapture Area Size = 13 * 9\nColour Format = RAW8\nFlip = None\nBayer = GR\n");

        Case c;
        c.name = "avi odd size debayered (GRBG from the sidecar)"; c.path = path; c.frame = 0;
        c.expectId = "avi_odd_f0";
        c.width = w; c.height = h; c.channels = 3;
        for (auto [x, y] : Points(w, h))
            for (int ch = 0; ch < 3; ++ch) c.samples.push_back({x, y, ch, BayerExpected("GRBG", x, y, ch, 0, d, w, h)});
        c.keywords = {{"INSTRUME", "value", "ZWO ASI224MC"},
                      {"HISTORY", "comment", "SERViewer: Bayer GRBG (sidecar)"}};
        c.absent = {"DATE-OBS", "BAYERPAT"};
        cases.push_back(c);

        // Every explicit bayerPattern element on the same file: each one must reach
        // the reader as its own pattern ("(chosen)" tells GRBG apart from the
        // sidecar's GRBG), so any swap in the enum/element ids/mapping fails a case.
        for (const char* pattern : {"GRBG", "GBRG", "BGGR"}) {
            Case e = c;
            e.name = std::string("avi bayerPattern=Bayer") + pattern;
            e.bayerPattern = std::string("Bayer") + pattern;
            e.samples.clear();
            for (auto [x, y] : Points(w, h))
                for (int ch = 0; ch < 3; ++ch) e.samples.push_back({x, y, ch, BayerExpected(pattern, x, y, ch, 0, d, w, h)});
            e.keywords = {{"INSTRUME", "value", "ZWO ASI224MC"},
                          {"HISTORY", "comment", std::string("SERViewer: Bayer ") + pattern + " (chosen)"}};
            cases.push_back(e);
        }

        Case none = c;
        none.name = "avi bayerPattern=BayerNone -> mono, no Bayer record";
        none.bayerPattern = "BayerNone";
        none.channels = 1;
        none.samples.clear();
        for (auto [x, y] : Points(w, h)) none.samples.push_back({x, y, 0, Norm(Gen(x, y, 0, 0, d), d)});
        none.keywords = {{"INSTRUME", "value", "ZWO ASI224MC"},
                         {"HISTORY", "comment", "SERViewer: avi-odd.avi frame 0"}};
        none.absent = {"DATE-OBS", "BAYERPAT"};
        none.absentComments = {"SERViewer: Bayer", "no Bayer pattern known"};
        cases.push_back(none);
    }

    // saveSerPath: avi-vert.avi -> SER (BGGR from the sidecar), no window; then the
    // written SER read back through SERViewer must hold the same frames.
    {
        const int w = 32, h = 24, d = 8;
        const std::string out = (dir / "avi-vert-converted.ser").string();
        std::error_code ec;
        std::filesystem::remove(out, ec);  // a rerun into the same directory starts clean

        // The context (layout, Bayer source) is on the console before the export
        // starts, and the result line names the layout written.
        Case c;
        c.name = "saveSerPath converts the AVI"; c.path = aviVert; c.saveSerPath = out;
        c.expectConsole = {"converting avi-vert.avi to SER; Bayer BGGR (sidecar RG, Flip=Vert",
                           kVertWarning,
                           "SERViewer: writing avi-vert-converted.ser",
                           "SERViewer: wrote 3 frames (BAYER_BGGR) to " + out};
        cases.push_back(c);

        for (int f = 0; f < 2; ++f) {
            Case raw;
            raw.name = "converted SER reads back: raw frame " + std::to_string(f);
            raw.path = out; raw.frame = f; raw.debayer = false;
            raw.expectId = "avi_vert_converted_f" + std::to_string(f);
            raw.width = w; raw.height = h; raw.channels = 1;
            for (auto [x, y] : Points(w, h)) raw.samples.push_back({x, y, 0, Norm(Gen(x, y, 0, f, d), d)});
            raw.keywords = {{"BAYERPAT", "value", "BGGR"}, {"INSTRUME", "value", "ZWO ASI585MC"}};
            raw.absent = {"DATE-OBS"};
            cases.push_back(raw);
        }

        Case deb;
        deb.name = "converted SER reads back: debayered last frame"; deb.path = out; deb.frame = 2;
        deb.expectId = "avi_vert_converted_f2";
        deb.width = w; deb.height = h; deb.channels = 3;
        for (auto [x, y] : Points(w, h))
            for (int ch = 0; ch < 3; ++ch)
                deb.samples.push_back({x, y, ch, BayerExpected("BGGR", x, y, ch, 2, d, w, h)});
        deb.keywords = {{"HISTORY", "comment", "SERViewer: avi-vert-converted.ser frame 2"}};
        deb.absent = {"DATE-OBS", "BAYERPAT"};
        cases.push_back(deb);

        // No sidecar, Auto: written as MONO, and the warning says so in conversion terms.
        const std::string monoOut = (dir / "avi-nosidecar-converted.ser").string();
        std::filesystem::remove(monoOut, ec);
        Case mono;
        mono.name = "saveSerPath with no Bayer pattern writes MONO"; mono.path = (dir / "avi-nosidecar.avi").string();
        mono.saveSerPath = monoOut;
        mono.expectConsole = {"converting avi-nosidecar.avi to SER; Bayer none (no sidecar pattern",
                              "the SER will be written as MONO (raw CFA)",
                              "SERViewer: wrote 2 frames (MONO) to " + monoOut};
        mono.consoleAbsent = {"showing the raw CFA"};
        cases.push_back(mono);

        Case monoBack;
        monoBack.name = "MONO SER reads back: frame 1"; monoBack.path = monoOut; monoBack.frame = 1;
        monoBack.expectId = "avi_nosidecar_converted_f1";
        monoBack.width = 16; monoBack.height = 16; monoBack.channels = 1;
        for (auto [x, y] : Points(16, 16)) monoBack.samples.push_back({x, y, 0, Norm(Gen(x, y, 0, 1, d), d)});
        monoBack.keywords = {{"HISTORY", "comment", "SERViewer: avi-nosidecar-converted.ser frame 1"}};
        monoBack.absent = {"DATE-OBS", "BAYERPAT", "INSTRUME"};
        cases.push_back(monoBack);

        Case serIn;
        serIn.name = "saveSerPath on SER input"; serIn.path = (dir / "mono8.ser").string();
        serIn.saveSerPath = (dir / "from-ser.ser").string();
        serIn.expectError = "saveSerPath converts AVI input only";
        cases.push_back(serIn);

        Case self;
        self.name = "saveSerPath = the source path"; self.path = aviVert; self.saveSerPath = aviVert;
        self.expectError = "refusing to overwrite the source file";
        cases.push_back(self);

        Case exists;
        exists.name = "saveSerPath names an existing file"; exists.path = aviVert;
        exists.saveSerPath = (dir / "mono8.ser").string();
        exists.expectError = exists.saveSerPath + " exists";
        cases.push_back(exists);
    }

    // Missing file and empty filePath.
    {
        Case c;
        c.name = "missing file"; c.path = (dir / "does-not-exist.ser").string();
        c.expectError = "cannot open video file";
        cases.push_back(c);
        Case e;
        e.name = "empty filePath"; e.path = ""; e.expectError = "no video file selected";
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
