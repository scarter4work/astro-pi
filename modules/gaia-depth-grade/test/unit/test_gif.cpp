// Task 24 (Phase G2): the native GIF89a writer. Files are decoded by the test-only reader
// test/util/GifRead.h (an independent LZW decoder); the encoder was also cross-checked against
// ffmpeg and ImageMagick during development (task report).
#include <catch_amalgamated.hpp>

#include "GifRead.h"
#include "GlbRead.h"
#include "TestFs.h"
#include "core/Gif.h"

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

using gdg::encode_gif;
using gdg::write_gif;
using gdg::test::GifFile;
using gdg::test::no_temps;
using gdg::test::parse_gif;
using gdg::test::TempDir;
using gdg::test::thrown_message;

namespace {

namespace fs = std::filesystem;
using Frames = std::vector<std::vector<std::uint8_t>>;

/// `n` distinct pseudo-random colours (fixed seed).
std::vector<std::uint32_t> distinct_colours(std::size_t n, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::set<std::uint32_t> seen;
    std::vector<std::uint32_t> out;
    while (out.size() < n) {
        const auto c = static_cast<std::uint32_t>(rng() & 0xFFFFFF);
        if (seen.insert(c).second) out.push_back(c);
    }
    return out;
}

/// A w x h frame whose pixels pick colours from `palette` by `pick(x, y)`.
template <typename Pick>
std::vector<std::uint8_t> frame_of(int w, int h, const std::vector<std::uint32_t>& palette, Pick pick) {
    std::vector<std::uint8_t> f(static_cast<std::size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::uint32_t c = palette[pick(x, y) % palette.size()];
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 3;
            f[i] = static_cast<std::uint8_t>(c >> 16);
            f[i + 1] = static_cast<std::uint8_t>(c >> 8);
            f[i + 2] = static_cast<std::uint8_t>(c);
        }
    return f;
}

/// Decodes and checks the fixed container layout of Gif.h.
GifFile decode_checked(const std::vector<std::uint8_t>& bytes, int w, int h, std::size_t frames) {
    const GifFile g = parse_gif(bytes);
    CHECK(g.signature == "GIF89a");
    CHECK(g.width == w);
    CHECK(g.height == h);
    CHECK(g.lsd_packed == 0xF7);  // global table, 8-bit colour resolution, 256 entries
    CHECK(g.global_table.size() == 768);
    CHECK(g.netscape_count == 1);
    CHECK(g.trailer);
    CHECK(g.trailing_bytes == 0);
    REQUIRE(g.frames.size() == frames);
    for (const auto& f : g.frames) {
        CHECK(f.gce_count == 1);
        CHECK(f.left == 0);
        CHECK(f.top == 0);
        CHECK(f.width == w);
        CHECK(f.height == h);
        CHECK_FALSE(f.local_table);
        CHECK_FALSE(f.interlaced);
        CHECK_FALSE(f.transparent);
        CHECK(f.min_code_size == 8);
        CHECK(f.clear_codes >= 1);
        CHECK(f.max_code_width <= 12);
        CHECK(f.max_sub_block <= 255);
    }
    return g;
}

}  // namespace

TEST_CASE("gif_roundtrip_exact_for_256_colours", "[gif]") {
    SECTION("exactly 256 colours, spread over three frames") {
        const auto pal = distinct_colours(256, 7);
        const int w = 64, h = 48;
        // Frame 0 holds colours 0..127 only, frame 1 128..255, frame 2 every one: the palette is global.
        const Frames frames = {frame_of(w, h, pal, [](int x, int y) { return (x * 7 + y * 3) % 128; }),
                               frame_of(w, h, pal, [](int x, int y) { return 128 + (x + y * 5) % 128; }),
                               frame_of(w, h, pal, [w](int x, int y) { return (y * w + x) * 37; })};
        const GifFile g = decode_checked(encode_gif(frames, w, h, 5), w, h, 3);
        for (std::size_t k = 0; k < frames.size(); ++k) CHECK(g.frames[k].rgb == frames[k]);
    }
    SECTION("few colours, uneven counts, extreme values") {
        const std::vector<std::uint32_t> pal = {0x000000, 0xFFFFFF, 0xFF0000, 0x00FF01, 0x0000FE};
        const int w = 37, h = 11;
        const Frames frames = {frame_of(w, h, pal, [](int x, int y) { return x < 30 ? 0 : (x + y) % 5; })};
        const GifFile g = decode_checked(encode_gif(frames, w, h, 0), w, h, 1);
        CHECK(g.frames[0].rgb == frames[0]);
    }
    SECTION("edge shapes: one pixel, one colour (long KwKwK runs), one row, one column") {
        const std::vector<std::uint32_t> one = {0x123456};
        for (const auto& [w, h] : {std::pair{1, 1}, std::pair{300, 200}, std::pair{1000, 1}, std::pair{1, 700}}) {
            const Frames frames = {frame_of(w, h, one, [](int, int) { return 0; })};
            const GifFile g = decode_checked(encode_gif(frames, w, h, 1), w, h, 1);
            CHECK(g.frames[0].rgb == frames[0]);
        }
    }
    SECTION("a 256-colour ramp of single runs (every code width)") {
        const auto pal = distinct_colours(256, 11);
        const int w = 512, h = 40;
        const Frames frames = {frame_of(w, h, pal, [](int x, int y) { return (x / (1 + y % 4)) % 256; })};
        const GifFile g = decode_checked(encode_gif(frames, w, h, 1), w, h, 1);
        CHECK(g.frames[0].rgb == frames[0]);
        CHECK(g.frames[0].max_code_width == 12);
    }
}

TEST_CASE("gif_lzw_table_reset", "[gif]") {
    // 320 x 240 uniform noise over 256 colours: almost every code adds a table entry, so the
    // 4096-code table fills many times and must be cleared each time.
    const auto pal = distinct_colours(256, 3);
    std::mt19937_64 rng(99);
    std::vector<int> picks(320 * 240);
    for (int& p : picks) p = static_cast<int>(rng() % 256);
    const Frames frames = {frame_of(320, 240, pal, [&](int x, int y) { return picks[y * 320 + x]; }),
                           frame_of(320, 240, pal, [&](int x, int y) { return picks[(239 - y) * 320 + x] ^ 0x55; })};
    const GifFile g = decode_checked(encode_gif(frames, 320, 240, 3), 320, 240, 2);
    for (std::size_t k = 0; k < frames.size(); ++k) {
        CHECK(g.frames[k].rgb == frames[k]);
        CHECK(g.frames[k].clear_codes > 10);  // ~76 800 codes / ~3 838 per table
        CHECK(g.frames[k].max_code_width == 12);
    }
}

TEST_CASE("gif_frame_count_delay_and_loop", "[gif]") {
    const auto pal = distinct_colours(16, 5);
    Frames frames;
    for (int k = 0; k < 7; ++k) frames.push_back(frame_of(20, 10, pal, [k](int x, int y) { return x + y + k; }));
    {
        const GifFile g = decode_checked(encode_gif(frames, 20, 10, 8), 20, 10, 7);
        CHECK(g.loops == 0);  // the default: loop forever
        for (std::size_t k = 0; k < frames.size(); ++k) {
            CHECK(g.frames[k].delay_cs == 8);
            CHECK(g.frames[k].disposal == 1);
            CHECK(g.frames[k].rgb == frames[k]);
        }
    }
    {
        const GifFile g = decode_checked(encode_gif(frames, 20, 10, 65535, 3), 20, 10, 7);
        CHECK(g.loops == 3);
        for (const auto& f : g.frames) CHECK(f.delay_cs == 65535);
    }
    CHECK(parse_gif(encode_gif(frames, 20, 10, 0, 65535)).loops == 65535);
}

TEST_CASE("gif_quantization_error_bounded", "[gif]") {
    // A 256 x 64 truecolour gradient, one level per pixel along each axis (16 384 distinct
    // colours): R rises and B falls along x, G rises along y and x. (A field that spreads its 64
    // rows over all 256 G levels tiles a 256 x 256 colour plane, where 256 axis-aligned boxes of
    // 16 x 16 levels put median cut's mean error at exactly 4.0 by construction: no margin.)
    const int w = 256, h = 64;
    std::vector<std::uint8_t> f(static_cast<std::size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 3;
            f[i] = static_cast<std::uint8_t>(x);
            f[i + 1] = static_cast<std::uint8_t>(64 + y + x / 4);
            f[i + 2] = static_cast<std::uint8_t>(255 - x);
        }
    const GifFile g = decode_checked(encode_gif({f}, w, h, 4), w, h, 1);
    double err[3] = {0, 0, 0};
    for (std::size_t i = 0; i < f.size(); ++i) err[i % 3] += std::abs(int(g.frames[0].rgb[i]) - int(f[i]));
    const double n = static_cast<double>(w) * h;
    for (int c = 0; c < 3; ++c) {
        const double mean_err = err[c] / n;
        CAPTURE(c, mean_err);
        CHECK(mean_err <= 4.0);
    }
    // The palette is fully used: 256 distinct indices for a field this rich.
    std::set<int> used(g.frames[0].index.begin(), g.frames[0].index.end());
    CHECK(used.size() == 256);
}

TEST_CASE("gif_nearest_matches_full_search", "[gif]") {
    // Ruling R3: the 32^3 cell grid only prunes. Truecolour noise (> 256 colours, so all 256
    // palette entries are real) plus near-duplicate clusters (entries that tie): every decoded
    // index is the full search's nearest entry, the lowest index on ties.
    const int w = 160, h = 120;
    std::mt19937_64 rng(2026);
    std::vector<std::uint8_t> f(static_cast<std::size_t>(w) * h * 3);
    for (std::size_t i = 0; i < f.size(); i += 3) {
        const auto v = rng();
        if (v % 4 == 0) {  // a quarter of the pixels in tight clusters around a few colours
            const int base = static_cast<int>((v >> 8) % 6) * 40 + 20;
            for (int c = 0; c < 3; ++c) f[i + c] = static_cast<std::uint8_t>(base + static_cast<int>((v >> (16 + 4 * c)) % 3));
        } else {
            for (int c = 0; c < 3; ++c) f[i + c] = static_cast<std::uint8_t>(v >> (8 * c + 16));
        }
    }
    const GifFile g = decode_checked(encode_gif({f}, w, h, 1), w, h, 1);
    const auto& pal = g.global_table;
    std::size_t ties = 0, mismatches = 0;
    long first_pixel = -1;
    int first_got = -1, first_want = -1;
    for (std::size_t p = 0; p < f.size() / 3; ++p) {
        int best = INT_MAX, pick = -1, at_best = 0;
        for (int e = 0; e < 256; ++e) {
            int d = 0;
            for (int c = 0; c < 3; ++c) {
                const int dc = int(pal[static_cast<std::size_t>(e) * 3 + c]) - int(f[p * 3 + c]);
                d += dc * dc;
            }
            if (d < best) {
                best = d;
                pick = e;
                at_best = 1;
            } else if (d == best) {
                ++at_best;
            }
        }
        if (at_best > 1) ++ties;
        if (int(g.frames[0].index[p]) != pick && mismatches++ == 0) {
            first_pixel = static_cast<long>(p);
            first_got = g.frames[0].index[p];
            first_want = pick;
        }
    }
    CAPTURE(mismatches, first_pixel, first_got, first_want, ties);
    REQUIRE(mismatches == 0);
    REQUIRE(ties > 0);  // the lowest-index-on-ties rule was exercised (883 tied pixels at this seed)
}

TEST_CASE("gif_write_errors", "[gif]") {
    const auto pal = distinct_colours(4, 1);
    const Frames frames = {frame_of(9, 7, pal, [](int x, int y) { return x * y; })};
    const std::vector<std::uint8_t> expect = encode_gif(frames, 9, 7, 8);
    TempDir tmp("gdg_gif");

    SECTION("writes the bytes and leaves no temporary") {
        const std::string path = (tmp.path / "orbit.gif").string();
        write_gif(path, frames, 9, 7, 8);
        CHECK(gdg::test::read_file_bytes(path) == expect);
        CHECK(no_temps(path));
        struct stat st {};
        REQUIRE(stat(path.c_str(), &st) == 0);
        CHECK((st.st_mode & 0777) == 0644);
    }
    SECTION("an existing file is replaced") {
        const std::string path = (tmp.path / "orbit.gif").string();
        std::ofstream(path) << std::string(5000, 'x');  // longer than the GIF: no stale tail may survive
        write_gif(path, frames, 9, 7, 8);
        CHECK(gdg::test::read_file_bytes(path) == expect);
        CHECK(no_temps(path));
    }
    SECTION("unwritable directory") {
        const fs::path dir = tmp.path / "locked";
        fs::create_directory(dir);
        REQUIRE(chmod(dir.c_str(), 0500) == 0);
        const std::string path = (dir / "orbit.gif").string();
        CHECK(thrown_message([&] { write_gif(path, frames, 9, 7, 8); }) ==
              "cannot write " + path + ": " + std::strerror(EACCES));
        CHECK_FALSE(fs::exists(path));
        CHECK(no_temps(path));
    }
    SECTION("nonexistent directory") {
        const std::string path = (tmp.path / "missing" / "orbit.gif").string();
        CHECK(thrown_message([&] { write_gif(path, frames, 9, 7, 8); }) ==
              "cannot write " + path + ": " + std::strerror(ENOENT));
        CHECK(no_temps(path));
    }
    SECTION("the rename fails: the temporary is removed, the target untouched") {
        const fs::path target = tmp.path / "out.gif";
        fs::create_directory(target);
        std::ofstream(target / "keep") << "x";
        const std::string path = target.string();
        const std::string msg = thrown_message([&] { write_gif(path, frames, 9, 7, 8); });
        CHECK(msg.rfind("cannot write " + path + ": ", 0) == 0);
        CHECK(msg.size() > ("cannot write " + path + ": ").size());
        CHECK(no_temps(path));
        CHECK(fs::exists(target / "keep"));
    }
    SECTION("bad arguments are refused before the filesystem is touched") {
        const std::string path = (tmp.path / "bad.gif").string();
        auto refused = [&](const Frames& fr, int w, int h, int delay, int loops) {
            CHECK_THROWS_AS(write_gif(path, fr, w, h, delay, loops), std::invalid_argument);
            CHECK_THROWS_AS(encode_gif(fr, w, h, delay, loops), std::invalid_argument);
        };
        refused({}, 9, 7, 8, 0);                                                  // no frames
        refused({frames[0], std::vector<std::uint8_t>(10)}, 9, 7, 8, 0);          // wrong frame size
        refused(frames, 0, 7, 8, 0);                                              // width 0
        refused(frames, 9, -7, 8, 0);                                             // negative height
        refused({std::vector<std::uint8_t>(65536 * 3)}, 65536, 1, 8, 0);          // width > 65535
        refused(frames, 9, 7, -1, 0);                                             // delay < 0
        refused(frames, 9, 7, 65536, 0);                                          // delay > 65535
        refused(frames, 9, 7, 8, -1);                                             // loops < 0
        refused(frames, 9, 7, 8, 65536);                                          // loops > 65535
        CHECK_FALSE(fs::exists(path));
        CHECK(no_temps(path));
    }
}

TEST_CASE("gif_concurrent_calls_identical", "[gif]") {
    // Rich frames: > 256 colours (median cut + nearest search) and many LZW resets, two threads.
    const int w = 200, h = 120;
    Frames frames;
    for (int k = 0; k < 4; ++k) {
        std::vector<std::uint8_t> f(static_cast<std::size_t>(w) * h * 3);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 3;
                f[i] = static_cast<std::uint8_t>((x * 3 + k * 17) & 0xFF);
                f[i + 1] = static_cast<std::uint8_t>((y * 5 + x * k) & 0xFF);
                f[i + 2] = static_cast<std::uint8_t>((x * y + k) & 0xFF);
            }
        frames.push_back(std::move(f));
    }
    std::vector<std::uint8_t> a, b;
    std::thread t1([&] { a = encode_gif(frames, w, h, 8); });
    std::thread t2([&] { b = encode_gif(frames, w, h, 8); });
    t1.join();
    t2.join();
    REQUIRE_FALSE(a.empty());
    CHECK(a == b);
    CHECK(a == encode_gif(frames, w, h, 8));
    decode_checked(a, w, h, 4);
}
