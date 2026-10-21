#include "SerExport.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <system_error>
#include <vector>

#include "AsiairSidecar.h"
#include "PartialFile.h"
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
    const fs::path aa = fs::absolute(a, ec1), ba = fs::absolute(b, ec2);
    if (ec1 || ec2)
        throw SerError("cannot resolve path " + (ec1 ? a : b).string() + ": " +
                       (ec1 ? ec1 : ec2).message());
    return aa.lexically_normal() == ba.lexically_normal();
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
    return ExportSerWithSpace(src, sourcePath, outPath, allowOverwrite, progress,
                              [](const std::string& dir, std::error_code& ec) {
                                  return static_cast<uint64_t>(fs::space(dir, ec).available);
                              });
}

ExportResult ExportSerWithSpace(const FrameSource& src, const std::string& sourcePath,
                                const std::string& outPath, bool allowOverwrite,
                                const ExportProgress& progress, const AvailableSpaceFn& space) {
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
    const uint64_t available = space(dir.string(), ec);
    if (ec) throw SerError("cannot check free space in " + dir.string() + ": " + ec.message());
    if (available < static_cast<uint64_t>(need))
        throw SerError("not enough free space: need " + std::to_string(need) + " bytes, " +
                       std::to_string(available) + " available");

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
    // Never through a symbolic link at the partial path (CreatePartialFile). The guard is armed
    // only once this call has created the partial: a link or directory found there is reported
    // and left alone. Declared before `file`, so on any throw the file is closed, then removed.
    std::string whyNot;
    std::FILE* raw = CreatePartialFile(partial.string(), whyNot);
    if (!raw) throw SerError("cannot create " + partial.string() + ": " + whyNot);
    PartialGuard guard{partial};
    auto closeFile = [](std::FILE* f) { std::fclose(f); };
    std::unique_ptr<std::FILE, decltype(closeFile)> file(raw, closeFile);
    auto failed = [&partial](int err) {
        return SerError("write to " + partial.string() + " failed: " +
                        (err == 0 ? std::string("short write")
                                  : std::error_code(err, std::generic_category()).message()));
    };
    auto put = [&](const void* p, size_t len) {
        errno = 0;
        if (std::fwrite(p, 1, len, file.get()) != len) {
            const int err = errno;
            throw failed(err);
        }
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

    errno = 0;
    if (std::fclose(file.release()) != 0) {
        const int err = errno;
        throw failed(err);
    }
    // Without allowOverwrite, published by hard link: a file that appeared at outPath since the
    // check above is refused atomically, except on a filesystem without hard links (PublishPartial).
    const PublishResult published = PublishPartial(partial.string(), outPath, allowOverwrite, whyNot);
    if (published == PublishResult::TargetExists) throw SerError(outPath + " exists");
    if (published == PublishResult::Failed)
        throw SerError("cannot publish " + partial.string() + " as " + outPath + ": " + whyNot);
    guard.armed = false;
    return result;
}

}  // namespace planetary::ser
