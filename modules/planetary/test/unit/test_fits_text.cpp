#include <catch_amalgamated.hpp>

#include "FitsText.h"

using namespace planetary::fits;

TEST_CASE("bytes outside printable ASCII become '?'", "[fits]") {
    CHECK(AsciiFromBytes("ZWO ASI585MC") == "ZWO ASI585MC");
    CHECK(AsciiFromBytes("caf\xe9\tx") == "caf??x");  // Latin-1 e-acute, tab
}

TEST_CASE("each non-ASCII UTF-8 code point becomes one '?'", "[fits]") {
    CHECK(AsciiFromUtf8("Saturn.ser") == "Saturn.ser");
    CHECK(AsciiFromUtf8("J\xc3\xbcpiter \xe2\x80\x94 1.ser") == "J?piter ? 1.ser");  // u-umlaut, em dash
    CHECK(AsciiFromUtf8("\xf0\x9f\xaa\x90") == "?");                              // 4-byte code point
}

TEST_CASE("card text is split at spaces into pieces of at most 72 characters", "[fits]") {
    const std::string w = "SERViewer warning: header frame count is 0; using 2 complete frames found in the file";
    const auto parts = SplitCardText(w);
    REQUIRE(parts.size() == 2);
    CHECK(parts[0] == "SERViewer warning: header frame count is 0; using 2 complete frames");
    CHECK(parts[1] == "found in the file");
    std::string joined;
    for (const auto& p : parts) {
        CHECK(p.size() <= kCardTextWidth);
        CHECK(p.front() != ' ');
        CHECK(p.back() != ' ');
        joined += (joined.empty() ? "" : " ") + p;
    }
    CHECK(joined == w);
}

TEST_CASE("short card text is one piece, a word longer than a card is hard-split", "[fits]") {
    CHECK(SplitCardText("SERViewer: mono8.ser frame 1") == std::vector<std::string>{"SERViewer: mono8.ser frame 1"});
    CHECK(SplitCardText("").empty());
    const std::string longWord(100, 'x');
    const auto parts = SplitCardText("a " + longWord);
    REQUIRE(parts.size() == 3);
    CHECK(parts[0] == "a");
    CHECK(parts[1] == std::string(72, 'x'));
    CHECK(parts[2] == std::string(28, 'x'));
}
