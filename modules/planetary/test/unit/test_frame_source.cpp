#include <catch_amalgamated.hpp>

#include <fstream>

#include "FrameSource.h"
#include "SerFile.h"
#include "SerWriter.h"
#include "TestUtil.h"

using namespace planetary::ser;
using namespace planetary::testing;

TEST_CASE("OpenVideo opens a SER by content, not extension") {
    const std::vector<uint8_t> bytes = {1, 2, 3, 4, 5, 6};
    for (const char* ext : {".avi", ".dat"}) {
        TempDir dir;
        const std::string p = dir.NewPath(ext);
        WriteSer(p, Spec(0, 3, 2, 8, 1), {bytes});

        auto src = OpenVideo(p);
        REQUIRE(src != nullptr);
        CHECK(src->Info().container == Container::Ser);
        CHECK_FALSE(src->Info().avi.has_value());
        const Frame f = src->ReadFrame(0);
        REQUIRE(f.samples.size() == bytes.size());
        for (size_t i = 0; i < bytes.size(); ++i) CHECK(f.samples[i] == bytes[i]);
    }
}

TEST_CASE("OpenVideo rejects unknown magic") {
    TempDir dir;
    const std::string p = dir.NewPath(".bin");
    {
        std::ofstream out(p, std::ios::binary);
        const std::vector<char> zeros(200, 0);
        out.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    }
    try {
        OpenVideo(p);
        FAIL("expected SerError");
    } catch (const SerError& e) {
        CHECK(std::string(e.what()).find("not a SER or AVI video file") != std::string::npos);
    }
}

TEST_CASE("OpenVideo of a missing file") {
    TempDir dir;
    CHECK_THROWS_AS(OpenVideo(dir.NewPath(".ser")), SerError);
}
