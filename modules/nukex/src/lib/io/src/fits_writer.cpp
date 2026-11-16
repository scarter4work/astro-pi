#include "nukex/io/fits_writer.hpp"

#include <fitsio.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <thread>
#include <functional>

namespace nukex {

namespace {
std::string fits_error(int status) {
    char msg[FLEN_ERRMSG] = {};
    fits_get_errstatus(status, msg);
    return msg;
}
} // namespace

bool FITSWriter::write_float(const std::string& path, const Image& image,
                             const std::vector<FITSCard>& cards,
                             const std::vector<std::string>& history,
                             std::string& error) {
    namespace fs = std::filesystem;
    if (image.empty()) { error = "FITSWriter: empty image for " + path; return false; }

    // A unique temporary beside the destination: same filesystem, so the
    // final rename is atomic.
    static std::atomic<unsigned> counter{0};
    const std::string tmp = path + ".tmp-"
        + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xffffff)
        + "-" + std::to_string(counter.fetch_add(1));
    std::error_code ec;
    fs::remove(tmp, ec);

    fitsfile* fp = nullptr;
    int status = 0;
    fits_create_diskfile(&fp, tmp.c_str(), &status);
    if (status != 0) { error = "FITSWriter: cannot create " + tmp + ": " + fits_error(status); return false; }

    const int naxis = image.n_channels() > 1 ? 3 : 2;
    long naxes[3] = {image.width(), image.height(), image.n_channels()};
    fits_create_img(fp, FLOAT_IMG, naxis, naxes, &status);
    fits_write_img(fp, TFLOAT, 1, static_cast<LONGLONG>(image.data_size()),
                   const_cast<float*>(image.data()), &status);
    for (const auto& c : cards) {
        if (status != 0) break;
        char* comment = c.comment.empty() ? nullptr : const_cast<char*>(c.comment.c_str());
        switch (c.type) {
            case FITSCard::Type::STRING:
                fits_update_key_str(fp, c.key.c_str(), c.text.c_str(), comment, &status); break;
            case FITSCard::Type::REAL:
                fits_update_key_dbl(fp, c.key.c_str(), c.real, -15, comment, &status); break;
            case FITSCard::Type::INTEGER:
                fits_update_key_lng(fp, c.key.c_str(), static_cast<LONGLONG>(c.integer), comment, &status); break;
            case FITSCard::Type::LOGICAL:
                fits_update_key_log(fp, c.key.c_str(), c.logical ? 1 : 0, comment, &status); break;
        }
    }
    for (const auto& h : history) {
        if (status != 0) break;
        fits_write_history(fp, h.c_str(), &status);
    }
    int close_status = 0;
    fits_close_file(fp, &close_status);
    if (status == 0) status = close_status;
    if (status != 0) {
        error = "FITSWriter: writing " + path + ": " + fits_error(status);
        fs::remove(tmp, ec);
        return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        error = "FITSWriter: cannot move " + tmp + " to " + path + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace nukex
