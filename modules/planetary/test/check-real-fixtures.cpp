// Validates the C++ SER reader against the converter's independent expectations
// (expected-checks.tsv, derived from the AVI bytes by avi_to_ser.py), with no
// PixInsight. Test-only, never shipped.
//
//   check-real-fixtures <expected-checks.tsv>   -> exit 0 and "PASS: n checks", else FAIL lines
//
// S lines: path frame debayer channels x y c v   (sample of the displayed frame, tolerance 1e-6)
// T lines: path frame iso                        (trailer frame time as DATE-OBS text)
// W lines: path text                             (the reader must report a warning containing text)
// E lines: path frame text                       (reading the frame must throw SerError containing text)
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Debayer.h"
#include "SerTime.h"

using namespace planetary::ser;

namespace {
FloatImage Load(const SerFile& f, int64_t index, bool debayer) {
    const FileInfo& info = f.Info();
    const Frame fr = f.ReadFrame(index);
    if (debayer && IsBayer(info.colorId)) return DebayerBilinear(fr, info.colorId, info.normDepth);
    return Normalize(fr, info.normDepth);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: %s <expected-checks.tsv>\n", argv[0]); return 2; }
    std::ifstream in(argv[1]);
    if (!in) { std::fprintf(stderr, "FAIL: cannot open %s\n", argv[1]); return 1; }
    int checks = 0, fails = 0;
    auto bad = [&](const std::string& m) { std::printf("FAIL: %s\n", m.c_str()); ++fails; };
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::vector<std::string> t;
        std::stringstream ss(line);
        for (std::string x; std::getline(ss, x, '\t');) t.push_back(x);
        try {
            const SerFile f = SerFile::Open(t.at(1));
            ++checks;
            if (t[0] == "S") {
                const int64_t fi = std::stoll(t.at(2));
                const FloatImage img = Load(f, fi, std::stoi(t.at(3)) != 0);
                const int ch = std::stoi(t.at(4)), x = std::stoi(t.at(5)), y = std::stoi(t.at(6)), c = std::stoi(t.at(7));
                const double want = std::stod(t.at(8));
                if (img.channels != ch) { bad(t[1] + ": channels " + std::to_string(img.channels) + ", expected " + t[4]); continue; }
                const double got = img.data.at((static_cast<size_t>(y) * img.width + x) * img.channels + c);
                if (std::fabs(got - want) > 1e-6) {
                    char b[200];
                    std::snprintf(b, sizeof b, " frame %lld (%d,%d,%d) = %.9g, expected %.9g", (long long)fi, x, y, c, got, want);
                    bad(t[1] + b);
                }
            } else if (t[0] == "T") {
                const auto ts = f.FrameTimestamp(std::stoll(t.at(2)));
                const auto iso = ts ? TicksToIsoUtc(*ts) : std::nullopt;
                if (!iso || *iso != t.at(3)) bad(t[1] + " frame " + t[2] + " time " + (iso ? *iso : "none") + ", expected " + t[3]);
            } else if (t[0] == "W") {
                bool found = false;
                for (const auto& w : f.Info().warnings) found = found || w.find(t.at(2)) != std::string::npos;
                if (!found) bad(t[1] + ": no warning containing '" + t[2] + "'");
            } else if (t[0] == "E") {
                try { (void)Load(f, std::stoll(t.at(2)), true); bad(t[1] + ": frame " + t[2] + " read without error"); }
                catch (const SerError& e) {
                    if (std::string(e.what()).find(t.at(3)) == std::string::npos) bad(t[1] + ": error '" + e.what() + "' lacks '" + t[3] + "'");
                }
            } else bad("unknown check line: " + line);
        } catch (const std::exception& e) {
            bad(line.substr(0, 80) + ": " + e.what());
        }
    }
    if (fails) { std::printf("FAIL: %d of %d checks\n", fails, checks); return 1; }
    std::printf("PASS: %d checks\n", checks);
    return checks > 0 ? 0 : 1;
}
