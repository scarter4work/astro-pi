// Test-only CLI over the PCL-free reader, for the real-data AVI proof
// (test/run-avi-real.sh -> avi_real_check.py). Never shipped, not a ctest case.
//
//   avi-probe <avi> [--frames i,j,k] [--out DIR] [--export SER]
//
// Prints one JSON line:
//   {"frames":N,"open_s":t,"container":...,"indexKind":...,"colorId":...,"bayerSource":...,
//    "warnings":[...], "width":W,"height":H,"droppedFrames":D,"frameRate":F,
//    "sidecarBayer":...,"sidecarFlip":...[,"export_s":t,"ser_frames":N,"ser_colorId":...]}
// OpenVideo(path, Auto) is what open_s times. --frames with --out writes each requested
// frame's raw 8-bit samples (top-down, width*height bytes) to DIR/<i>.raw. --export runs
// ExportSer to SER (never overwriting), then reopens it with SerFile; with --frames/--out
// it also writes the SER's frames to DIR/ser-<i>.raw.
// Exit 0 on success; 1 with {"error":...} on any SerError/exception.
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "FrameSource.h"
#include "SerExport.h"
#include "SerFile.h"

using namespace planetary::ser;

namespace {

std::string Json(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
    return o + "\"";
}

const char* ContainerName(Container c) {
    switch (c) {
        case Container::Ser: return "SER";
        case Container::Avi: return "AVI";
        case Container::AviOpenDml: return "AVI (OpenDML)";
    }
    return "?";
}

double Since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void WriteRaw(const Frame& fr, const std::string& path) {
    if (fr.channels != 1) throw SerError("frame has " + std::to_string(fr.channels) + " channels, expected 1");
    std::vector<unsigned char> bytes(fr.samples.size());
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (fr.samples[i] > 255) throw SerError("sample " + std::to_string(i) + " exceeds 8 bits");
        bytes[i] = static_cast<unsigned char>(fr.samples[i]);
    }
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    o.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!o) throw SerError("cannot write " + path);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <avi> [--frames i,j,k] [--out DIR] [--export SER]\n", argv[0]);
        return 2;
    }
    const std::string path = argv[1];
    std::vector<int64_t> frames;
    std::string outDir, exportPath;
    for (int a = 2; a < argc; ++a) {
        const std::string k = argv[a];
        if (a + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", k.c_str()); return 2; }
        const std::string v = argv[++a];
        if (k == "--frames") {
            std::stringstream ss(v);
            for (std::string x; std::getline(ss, x, ',');) frames.push_back(std::stoll(x));
        } else if (k == "--out") outDir = v;
        else if (k == "--export") exportPath = v;
        else { std::fprintf(stderr, "unknown option %s\n", k.c_str()); return 2; }
    }
    if (!frames.empty() && outDir.empty()) { std::fprintf(stderr, "--frames needs --out\n"); return 2; }

    try {
        const auto t0 = std::chrono::steady_clock::now();
        const auto src = OpenVideo(path, BayerChoice::Auto);
        const double openS = Since(t0);
        const FileInfo& info = src->Info();

        std::string j = "{\"frames\":" + std::to_string(info.frameCount);
        char b[64];
        std::snprintf(b, sizeof b, "%.4f", openS);
        j += std::string(",\"open_s\":") + b;
        j += ",\"container\":" + Json(ContainerName(info.container));
        j += ",\"indexKind\":" + Json(info.avi ? info.avi->indexKind : "");
        j += ",\"colorId\":" + Json(ColorIdName(info.colorId));
        j += ",\"bayerSource\":" + Json(info.avi ? info.avi->bayerSource : "");
        j += ",\"warnings\":[";
        for (size_t i = 0; i < info.warnings.size(); ++i) j += (i ? "," : "") + Json(info.warnings[i]);
        j += "]";
        j += ",\"width\":" + std::to_string(info.header.width) + ",\"height\":" + std::to_string(info.header.height);
        if (info.avi) {
            std::snprintf(b, sizeof b, "%.6f", info.avi->frameRate);
            j += ",\"droppedFrames\":" + std::to_string(info.avi->droppedFrames) + ",\"frameRate\":" + b;
            if (info.avi->sidecar) {
                j += ",\"sidecarBayer\":" + Json(info.avi->sidecar->bayer);
                j += ",\"sidecarFlip\":" + Json(info.avi->sidecar->flip);
                j += ",\"sidecarCamera\":" + Json(info.avi->sidecar->camera);
            }
        }

        for (int64_t f : frames) WriteRaw(src->ReadFrame(f), outDir + "/" + std::to_string(f) + ".raw");

        if (!exportPath.empty()) {
            const auto t1 = std::chrono::steady_clock::now();
            const ExportResult r = ExportSer(*src, path, exportPath, false, ExportProgress());
            std::snprintf(b, sizeof b, "%.3f", Since(t1));
            j += std::string(",\"export_s\":") + b + ",\"export_frames\":" + std::to_string(r.framesWritten) +
                 ",\"export_bytes\":" + std::to_string(r.bytesWritten);
            const SerFile ser = SerFile::Open(exportPath);
            j += ",\"ser_frames\":" + std::to_string(ser.Info().frameCount) +
                 ",\"ser_colorId\":" + Json(ColorIdName(ser.Info().colorId));
            for (int64_t f : frames) WriteRaw(ser.ReadFrame(f), outDir + "/ser-" + std::to_string(f) + ".raw");
        }
        j += "}";
        std::printf("%s\n", j.c_str());
        return 0;
    } catch (const std::exception& e) {
        std::printf("{\"error\":%s}\n", Json(e.what()).c_str());
        return 1;
    }
}
