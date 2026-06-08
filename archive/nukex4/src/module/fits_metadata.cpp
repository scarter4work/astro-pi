// NukeX v4 — FITS metadata extraction via cfitsio (already vendored).
// Copyright (c) 2026 Scott Carter. MIT License.

#include "fits_metadata.hpp"

#include <fitsio.h>
#include <algorithm>
#include <cctype>
#include <cstring>

namespace nukex {

namespace {

std::string normalize(const char* raw) {
    if (!raw) return {};
    std::string s(raw);
    auto is_junk = [](char c) {
        return c == '\'' || c == '"' || std::isspace(static_cast<unsigned char>(c));
    };
    while (!s.empty() && is_junk(s.front())) s.erase(s.begin());
    while (!s.empty() && is_junk(s.back()))  s.pop_back();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

} // namespace

FITSMetadata read_fits_metadata(const std::string& path) {
    FITSMetadata meta;
    fitsfile* fptr = nullptr;
    int status = 0;

    if (fits_open_file(&fptr, path.c_str(), READONLY, &status) != 0) {
        return meta;  // read_ok stays false
    }

    char value[FLEN_VALUE]    = {0};
    char comment[FLEN_COMMENT] = {0};

    int local_status = 0;
    if (fits_read_key(fptr, TSTRING, "FILTER", value, comment, &local_status) == 0) {
        meta.filter = normalize(value);
    }

    local_status = 0;
    std::memset(value, 0, sizeof(value));
    if (fits_read_key(fptr, TSTRING, "BAYERPAT", value, comment, &local_status) == 0) {
        meta.bayer_pat = normalize(value);
    }

    long naxis3 = 1;
    local_status = 0;
    if (fits_read_key(fptr, TLONG, "NAXIS3", &naxis3, comment, &local_status) == 0) {
        meta.naxis3 = static_cast<int>(naxis3);
    } else {
        meta.naxis3 = 1;
    }

    fits_close_file(fptr, &status);
    meta.read_ok = true;
    return meta;
}

} // namespace nukex
