#pragma once
#include <optional>
#include <string>
#include <vector>

#include "FrameSource.h"

namespace planetary::ser {

// Parses an ASIAIR capture log (`<video>.avi.txt`): a `[<camera>]` section line, then
// `Key = Value` lines (CRLF or LF, spaces optional). Unknown keys are ignored. Text with
// neither a section line nor any `=` line yields nullopt (the caller treats the sidecar as
// unreadable; ResolveBayer words the warning). A `[SERViewer]` section (the sidecar
// WriteAsiairSidecar writes) is not a camera: camera stays empty, writtenBySERViewer is set.
std::optional<AsiairSidecar> ParseAsiairSidecar(const std::string& text);

// Every warning ResolveBayer gives when Auto finds no pattern is "<reason>" + this
// tail (worded for viewing); a caller that converts swaps the tail for what it writes.
// Reasons: "no Bayer pattern known (no .avi.txt sidecar)", "sidecar <name> could not be
// read[ (larger than 64 KiB)]; no Bayer pattern known", "sidecar <name> has no Bayer value",
// "sidecar Bayer value <v> is not recognised", "sidecar path <name> is not a file; ignored;
// no Bayer pattern known".
inline constexpr char kRawCfaAsMonoTail[] =
    "; showing the raw CFA as mono -- choose the pattern under Bayer pattern";

// Resolves the Bayer pattern of an AVI and says where it came from in `source`.
// Auto uses the sidecar's Bayer value as it stands. Flip "None" and "Vert(unimplemented)" are
// measured: neither changes the frame (ASIAIR records the Vert setting but does not apply it;
// spec amendment 2026-10-23, superseding Ruling 10). Any other non-empty Flip is unmeasured: the
// sidecar's pattern is used, with a warning saying so (Ruling 15). An odd StartX/StartY (never
// seen; ZWO documents no rule for it), or one that is present but not a sensor coordinate,
// likewise keeps the pattern with a warning. An explicit
// choice wins and warns of neither; None is Mono; Auto with nothing known
// is Mono with a warning naming why (`sidecarName` is the sidecar's file name, for warnings).
// `status` says why `sidecar` is empty -- none there, unreadable, or over the size cap -- so the
// warning is true; an unreadable sidecar is reported whatever the choice. When `sidecar` holds
// a value, `status` is ignored.
ColorId ResolveBayer(BayerChoice choice, const std::optional<AsiairSidecar>& sidecar,
                     std::string& source, std::vector<std::string>& warnings,
                     const std::string& sidecarName = "",
                     SidecarStatus status = SidecarStatus::Absent);

// Writes `<aviPath>.txt` (the sidecar OpenVideo reads) recording a Bayer pattern the user
// chose, exactly (LF, ASCII):
//   [SERViewer]
//   Bayer = <RG|GR|GB|BG>
//   Written By = SERViewer <writerVersion> (Bayer pattern chosen by the user)
// No Flip line, so Auto uses the pattern as it stands, and no camera (ParseAsiairSidecar reads
// the [SERViewer] section as "no camera", writtenBySERViewer; ResolveBayer's source is
// "sidecar (SERViewer)"). Throws SerError -- and writes nothing -- when the sidecar already
// exists ("<name> already exists; SERViewer never overwrites a sidecar"; a symbolic link counts)
// or `pattern` is not one of the four Bayer ids. Writes `<target>.partial`, then publishes it by hard link
// (PublishPartial: atomic no-replace; check + rename only without hard links); on
// any failure the partial is removed and the SerError names the path and the OS reason. A
// symbolic link or other non-regular file at the partial path is refused and left alone, never
// written through (CreatePartialFile).
void WriteAsiairSidecar(const std::string& aviPath, ColorId pattern, const std::string& writerVersion);

// "W * H" (the sidecar Capture Area Size) -> w, h; false when it does not parse.
bool ParseCaptureArea(const std::string& text, int& w, int& h);

// `YYYY-MM-DD-HHMMSS-...` -> "YYYY-MM-DD HH:MM:SS"; nullopt when the name does not start so.
std::optional<std::string> FilenameStart(const std::string& fileName);

}  // namespace planetary::ser
