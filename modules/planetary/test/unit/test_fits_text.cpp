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

TEST_CASE("UTF-8 text for a card: the reader's arrow becomes ->, other code points '?'", "[fits]") {
    CHECK(FitsAscii("BGGR (sidecar RG, Flip=Vert \xe2\x86\x92 BGGR)") == "BGGR (sidecar RG, Flip=Vert -> BGGR)");
    CHECK(FitsAscii("J\xc3\xbcpiter \xe2\x86\x92\xe2\x86\x92") == "J?piter ->->");
}

TEST_CASE("a string value longer than a card holds is cut, keeping its extension visible", "[fits]") {
    CHECK(kMaxStringValue == 68);
    CHECK(FitsStringLength("it's") == 5);  // the quote is doubled on the card
    CHECK(FitStringValue("2026-01-30-195830-Jupiter.avi") == "2026-01-30-195830-Jupiter.avi");
    const std::string exact(68, 'a');
    CHECK(FitStringValue(exact) == exact);

    const std::string name = std::string(80, 'J') + ".avi";
    const std::string cut = FitStringValue(name);
    CHECK(FitsStringLength(cut) == 68);
    CHECK(cut == std::string(61, 'J') + "....avi");

    // Doubled quotes count twice, and a quote is never split from its double.
    const std::string quoted = std::string(70, '\'') + ".ser";
    const std::string q = FitStringValue(quoted);
    CHECK(FitsStringLength(q) <= 68);
    CHECK(q.size() >= 4);
    CHECK(q.compare(q.size() - 7, 7, "....ser") == 0);

    // No extension worth keeping (none, or a long one): the start and "...".
    const std::string plain(90, 'x');
    CHECK(FitStringValue(plain) == std::string(65, 'x') + "...");
    const std::string longExt = "a." + std::string(90, 'e');
    CHECK(FitStringValue(longExt) == "a." + std::string(63, 'e') + "...");
}
