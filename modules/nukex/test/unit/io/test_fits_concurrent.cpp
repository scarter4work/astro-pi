// FITSReader must be callable from several threads at once (the module's
// frame loaders and any embedding application, e.g. a Rust bridge, do this).
// CFITSIO is only safe for that when it is compiled with _REENTRANT (its
// USE_PTHREADS option): otherwise its global open-file table and error-message
// stack are unguarded and concurrent opens SIGSEGV inside ffopen.
//
// This file is built twice: in the NukeX tree (test_fits_concurrent) and in a
// standalone consumer of src/lib/io that carries none of the root
// CMakeLists.txt's global definitions (test_fits_concurrent_standalone, see
// test/standalone_io/). The second is the configuration that crashed: the
// root's PCL-motivated add_compile_definitions(_REENTRANT) used to leak into
// CFITSIO and hide the missing USE_PTHREADS in the module build.
#include "catch_amalgamated.hpp"
#include "nukex/io/fits_reader.hpp"

#include <fitsio.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

using namespace nukex;
namespace fs = std::filesystem;

namespace {

// Writes a w x h float32 mono image whose pixel i is (i % 251 + seed) / 512.
void write_fits(const std::string& path, int w, int h, int seed) {
    fs::remove(path);
    fitsfile* f = nullptr;
    int status = 0;
    fits_create_file(&f, path.c_str(), &status);
    long naxes[2] = {w, h};
    fits_create_img(f, FLOAT_IMG, 2, naxes, &status);
    std::vector<float> px(static_cast<std::size_t>(w) * h);
    for (std::size_t i = 0; i < px.size(); ++i)
        px[i] = static_cast<float>(i % 251 + seed) / 512.0f;
    long fpixel[2] = {1, 1};
    fits_write_pix(f, TFLOAT, fpixel, static_cast<LONGLONG>(px.size()), px.data(), &status);
    fits_close_file(f, &status);
    REQUIRE(status == 0);
}

fs::path scratch_dir() {
    const fs::path d = fs::temp_directory_path() /
        ("nukex_fits_concurrent_" + std::to_string(::getpid()));
    fs::create_directories(d);
    return d;
}

} // namespace

TEST_CASE("CFITSIO: vendored build is reentrant", "[fits][threads]") {
    // 1 only when CFITSIO was compiled with _REENTRANT (USE_PTHREADS=ON).
    REQUIRE(fits_is_reentrant() == 1);
}

TEST_CASE("FITSReader: concurrent reads from many threads", "[fits][threads]") {
    constexpr int kThreads = 16, kIters = 40, kW = 128, kH = 96;
    const fs::path dir = scratch_dir();

    // One file per thread plus one every thread shares, and a non-FITS file
    // whose failed open goes through CFITSIO's (global) error-message stack.
    std::vector<std::string> own;
    for (int t = 0; t < kThreads; ++t) {
        own.push_back((dir / ("own_" + std::to_string(t) + ".fits")).string());
        write_fits(own.back(), kW, kH, t);
    }
    const std::string shared = (dir / "shared.fits").string();
    write_fits(shared, kW, kH, 100);
    const std::string bad = (dir / "not_fits.fits").string();
    {
        std::ofstream o(bad);
        for (int i = 0; i < 400; ++i) o << "not a FITS file\n";
    }

    std::atomic<int> failures{0};
    auto check = [&](const FITSReadResult& r, int seed) {
        if (!r.success || r.image.width() != kW || r.image.height() != kH ||
            r.image.n_channels() != 1) { ++failures; return; }
        const float* d = r.image.data();
        const std::size_t probe[] = {0, 1, 250, 251, 5000,
                                     static_cast<std::size_t>(kW) * kH - 1};
        for (std::size_t i : probe)
            if (d[i] != static_cast<float>(i % 251 + seed) / 512.0f) { ++failures; return; }
    };

    std::vector<std::thread> pool;
    for (int t = 0; t < kThreads; ++t) {
        pool.emplace_back([&, t] {
            for (int k = 0; k < kIters; ++k) {
                check(FITSReader::read(own[t]), t);
                check(FITSReader::read(shared), 100);
                if (FITSReader::read(bad).success) ++failures;
                if (FITSReader::read_headers(own[t]).width != kW) ++failures;
            }
        });
    }
    for (auto& th : pool) th.join();

    std::error_code ec;
    fs::remove_all(dir, ec);
    REQUIRE(failures.load() == 0);
}
