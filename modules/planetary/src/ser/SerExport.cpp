#include "SerExport.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#include "AsiairSidecar.h"
#include "SerFile.h"

namespace planetary::ser {

namespace fs = std::filesystem;

namespace {

constexpr int64_t kTicksPerSecond = 10000000;
constexpr int64_t kDaysTo1970 = 719162;  // 0001-01-01 -> 1970-01-01 (proleptic Gregorian)

// Days since 1970-01-01 of a civil date (Howard Hinnant's algorithm).
int64_t DaysFromCivil(int64_t y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void PutI32(std::vector<uint8_t>& b, size_t off, int32_t v) {
    for (int i = 0; i < 4; ++i) b[off + i] = static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i));
}
void PutI64(std::vector<uint8_t>& b, size_t off, int64_t v) {
    for (int i = 0; i < 8; ++i) b[off + i] = static_cast<uint8_t>(static_cast<uint64_t>(v) >> (8 * i));
}

// Removes the partial file on scope exit unless released.
struct PartialGuard {
    fs::path path;
    bool armed = true;
    ~PartialGuard() {
        if (armed) {
            std::error_code ec;
            fs::remove(path, ec);
        }
    }
};

bool SamePath(const fs::path& a, const fs::path& b) {
    std::error_code ec1, ec2;
    if (fs::exists(a, ec1) && fs::exists(b, ec2)) {
        std::error_code ec;
        const bool eq = fs::equivalent(a, b, ec);
        if (!ec) return eq;
    }
    return fs::absolute(a).lexically_normal() == fs::absolute(b).lexically_normal();
}

}  // namespace

int64_t FilenameStartTicks(const std::string& fileName) {
    const auto s = FilenameStart(fileName);
    if (!s) return 0;
    // "YYYY-MM-DD HH:MM:SS"
    auto n = [&](size_t at, size_t len) { return std::stoi(s->substr(at, len)); };
    const int64_t days = DaysFromCivil(n(0, 4), n(5, 2), n(8, 2)) + kDaysTo1970;
    const int64_t secs = days * 86400 + n(11, 2) * 3600 + n(14, 2) * 60 + n(17, 2);
    return secs * kTicksPerSecond;
}

ExportResult ExportSer(const FrameSource& src, const std::string& sourcePath,
                       const std::string& outPath, bool allowOverwrite,
                       const ExportProgress& progress) {
    const FileInfo& info = src.Info();
    if (info.planes != 1 || info.bytesPerSample != 1)
        throw SerError("Save as SER converts 8-bit single-channel video only");
    const int64_t n = info.frameCount;
    const int64_t w = info.header.width, h = info.header.height;
    if (n <= 0 || w <= 0 || h <= 0) throw SerError("the video has no frames to write");

    const fs::path out(outPath);
    if (SamePath(out, fs::path(sourcePath))) throw SerError("refusing to overwrite the source file");
    std::error_code ec;
    if (fs::exists(out, ec) && !allowOverwrite) throw SerError(outPath + " exists");

    const int64_t need = SerFile::kHeaderBytes + n * w * h;
    fs::path dir = out.parent_path();
    if (dir.empty()) dir = ".";
    const auto space = fs::space(dir, ec);
    if (ec) throw SerError("cannot check free space in " + dir.string() + ": " + ec.message());
    if (space.available < static_cast<uintmax_t>(need))
        throw SerError("not enough free space: need " + std::to_string(need) + " bytes, " +
                       std::to_string(space.available) + " available");

    std::vector<uint8_t> hdr(SerFile::kHeaderBytes, 0);
    std::memcpy(hdr.data(), "LUCAM-RECORDER", 14);
    PutI32(hdr, 14, 0);
    PutI32(hdr, 18, static_cast<int32_t>(info.colorId));
    PutI32(hdr, 22, 0);
    PutI32(hdr, 26, static_cast<int32_t>(w));
    PutI32(hdr, 30, static_cast<int32_t>(h));
    PutI32(hdr, 34, 8);
    PutI32(hdr, 38, static_cast<int32_t>(n));
    const std::string& inst = info.header.instrument;
    std::memcpy(hdr.data() + 82, inst.data(), std::min<size_t>(inst.size(), 40));
    PutI64(hdr, 162, FilenameStartTicks(fs::path(sourcePath).filename().string()));
    PutI64(hdr, 170, 0);

    const fs::path partial = outPath + ".partial";
    PartialGuard guard{partial};
    std::ofstream file(partial, std::ios::binary | std::ios::trunc);
    if (!file) throw SerError("cannot create " + partial.string());
    auto put = [&](const void* p, size_t len) {
        file.write(static_cast<const char*>(p), static_cast<std::streamsize>(len));
        if (!file) throw SerError("write to " + partial.string() + " failed");
    };
    put(hdr.data(), hdr.size());

    if (progress && !progress(0, n)) throw ExportCancelled();
    ExportResult result;
    result.bytesWritten = SerFile::kHeaderBytes;
    std::vector<uint8_t> row;
    for (int64_t i = 0; i < n; ++i) {
        const Frame f = src.ReadFrame(i);
        if (f.width != w || f.height != h || f.channels != 1 ||
            f.samples.size() != static_cast<size_t>(w * h))
            throw SerError("frame " + std::to_string(i) + " does not match the video size");
        row.resize(f.samples.size());
        for (size_t k = 0; k < row.size(); ++k) row[k] = static_cast<uint8_t>(f.samples[k]);
        put(row.data(), row.size());
        result.bytesWritten += static_cast<int64_t>(row.size());
        result.framesWritten = i + 1;
        if (progress && !progress(i + 1, n)) throw ExportCancelled();
    }

    file.close();
    if (!file) throw SerError("write to " + partial.string() + " failed");
    fs::rename(partial, out, ec);
    if (ec) throw SerError("cannot rename " + partial.string() + " to " + outPath + ": " + ec.message());
    guard.armed = false;
    return result;
}

}  // namespace planetary::ser
