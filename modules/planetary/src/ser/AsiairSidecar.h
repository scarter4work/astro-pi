#pragma once
#include <optional>
#include <string>
#include <vector>

#include "FrameSource.h"

namespace planetary::ser {

// Parses an ASIAIR capture log (`<video>.avi.txt`): a `[<camera>]` section line, then
// `Key = Value` lines (CRLF or LF, spaces optional). Unknown keys are ignored. Text with
// neither a section line nor any `=` line yields nullopt and the warning
// "sidecar <name> could not be read; ignored" (`name` is the sidecar's file name).
std::optional<AsiairSidecar> ParseAsiairSidecar(const std::string& text,
                                                std::vector<std::string>& warnings,
                                                const std::string& name = "");

// Every warning ResolveBayer gives when Auto finds no pattern is "<reason>" + this
// tail (worded for viewing); a caller that converts swaps the tail for what it writes.
// Reasons: "no Bayer pattern known (no .avi.txt sidecar)", "sidecar <name> has no Bayer
// value", "sidecar Bayer value <v> is not recognised".
inline constexpr char kRawCfaAsMonoTail[] =
    "; showing the raw CFA as mono -- choose the pattern under Bayer pattern";

// Resolves the Bayer pattern of an AVI and says where it came from in `source`.
// Auto uses the sidecar's Bayer value, rotated 180 degrees when Flip starts with "Vert"
// (measured on the ASIAIR files: most Flip=Vert captures carry the opposite phase, 3 do not,
// and nothing in the file tells them apart -- so it always adds a warning naming the inference
// and the sidecar's own pattern to choose if red and blue look swapped; Ruling 10). An explicit
// choice wins; None is Mono; Auto with nothing known is Mono with a warning naming why
// (`sidecarName` is the sidecar's file name, for that warning).
ColorId ResolveBayer(BayerChoice choice, const std::optional<AsiairSidecar>& sidecar,
                     std::string& source, std::vector<std::string>& warnings,
                     const std::string& sidecarName = "");

// "W * H" (the sidecar Capture Area Size) -> w, h; false when it does not parse.
bool ParseCaptureArea(const std::string& text, int& w, int& h);

// `YYYY-MM-DD-HHMMSS-...` -> "YYYY-MM-DD HH:MM:SS"; nullopt when the name does not start so.
std::optional<std::string> FilenameStart(const std::string& fileName);

}  // namespace planetary::ser
