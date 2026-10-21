#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "AsiairSidecar.h"
#include "AviFile.h"
#include "FrameSource.h"
#include "SerFile.h"

namespace planetary::ser {

namespace {

constexpr int64_t kMaxSidecarBytes = 64 * 1024;

// Adds everything that lives outside an AVI: the ASIAIR sidecar, the file-name start time and
// the resolved Bayer pattern (colorId becomes the pattern, so the debayer path is unchanged).
void ApplyAviContext(AviFile& avi, const std::string& path, BayerChoice bayer) {
    FileInfo& info = avi.MutableInfo();
    AviDetails& d = *info.avi;
    const std::filesystem::path p(path);

    const std::string sidecarPath = path + ".txt";
    const std::string sidecarName = p.filename().string() + ".txt";
    std::error_code ec;
    if (std::filesystem::is_regular_file(sidecarPath, ec)) {
        const auto size = std::filesystem::file_size(sidecarPath, ec);
        if (ec) {
            info.warnings.push_back("sidecar " + sidecarName + " could not be read; ignored");
        } else if (size > static_cast<uintmax_t>(kMaxSidecarBytes)) {
            info.warnings.push_back("sidecar " + sidecarName + " is larger than 64 KiB; ignored");
        } else {
            std::ifstream f(sidecarPath, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (!f && !f.eof())
                info.warnings.push_back("sidecar " + sidecarName + " could not be read; ignored");
            else
                d.sidecar = ParseAsiairSidecar(text, info.warnings, sidecarName);
        }
    }

    d.filenameStart = FilenameStart(p.filename().string());
    info.colorId = ResolveBayer(bayer, d.sidecar, d.bayerSource, info.warnings);

    if (d.sidecar) {
        info.header.instrument = d.sidecar->camera;
        int w = 0, h = 0;
        if (ParseCaptureArea(d.sidecar->captureArea, w, h) &&
            (w != info.header.width || h != info.header.height))
            info.warnings.push_back("sidecar Capture Area Size " + std::to_string(w) + " * " +
                                    std::to_string(h) + " differs from the video's " +
                                    std::to_string(info.header.width) + " x " +
                                    std::to_string(info.header.height));
    }
}

}  // namespace

std::unique_ptr<FrameSource> OpenVideo(const std::string& path, BayerChoice bayer) {
    char magic[12] = {};
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) throw SerError("cannot open SER file: " + path);
        f.read(magic, sizeof magic);
        if (f.gcount() != static_cast<std::streamsize>(sizeof magic))
            throw SerError("not a SER or AVI video file: " + path);
    }
    if (std::memcmp(magic, "LUCAM-RECORDER", 12) == 0)  // first 12 of the 14-byte SER id
        return std::make_unique<SerFile>(SerFile::Open(path));
    if (std::memcmp(magic, "RIFF", 4) == 0 && std::memcmp(magic + 8, "AVI ", 4) == 0) {
        auto avi = std::make_unique<AviFile>(AviFile::Open(path));
        ApplyAviContext(*avi, path, bayer);
        return avi;
    }
    throw SerError("not a SER or AVI video file: " + path);
}

}  // namespace planetary::ser
