#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <system_error>

#include "FrameSource.h"

namespace planetary::ser {

// Called once with (0, N) before the first frame and after every frame written.
// Return false to cancel.
using ExportProgress = std::function<bool(int64_t framesDone, int64_t frameCount)>;

struct ExportResult {
    int64_t framesWritten = 0;
    int64_t bytesWritten = 0;
};

class ExportCancelled : public SerError {
public:
    ExportCancelled() : SerError("export cancelled") {}
};

// Writes an 8-bit single-channel source (AVI, or 8-bit mono/Bayer SER) as a SER file:
// header ColorID from the source, depth 8, no trailer, DateTime_UTC 0, DateTime from the
// source file name's start time (else 0). Writes `<outPath>.partial` and renames it into
// place; on any error or cancel the partial file is removed. Holds one frame at a time.
// Throws SerError with the real reason, ExportCancelled on cancel.
ExportResult ExportSer(const FrameSource& src, const std::string& sourcePath,
                       const std::string& outPath, bool allowOverwrite,
                       const ExportProgress& progress);

// Test seam: ExportSer with the free-space query injected (bytes available in `dir`).
using AvailableSpaceFn = std::function<uint64_t(const std::string& dir, std::error_code& ec)>;
ExportResult ExportSerWithSpace(const FrameSource& src, const std::string& sourcePath,
                                const std::string& outPath, bool allowOverwrite,
                                const ExportProgress& progress, const AvailableSpaceFn& space);

// Local DateTime ticks (100 ns since 0001-01-01) of the file name's "YYYY-MM-DD-HHMMSS-"
// start time; 0 when the name does not start so.
int64_t FilenameStartTicks(const std::string& fileName);

}  // namespace planetary::ser
