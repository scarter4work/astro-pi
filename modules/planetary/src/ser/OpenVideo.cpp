#include <cstring>
#include <fstream>

#include "FrameSource.h"
#include "SerFile.h"

namespace planetary::ser {

std::unique_ptr<FrameSource> OpenVideo(const std::string& path, BayerChoice /*bayer*/) {
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
    if (std::memcmp(magic, "RIFF", 4) == 0 && std::memcmp(magic + 8, "AVI ", 4) == 0)
        throw SerError("AVI support is not built");
    throw SerError("not a SER or AVI video file: " + path);
}

}  // namespace planetary::ser
