// Test-only: the probes' FITS writer (stack-probe, stack-region-probe). Never shipped.
// 32-bit IEEE float, NAXIS3 = 3 planes for colour, first row = top of the image (the bench's reader).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "SerFile.h"

namespace planetary::probe {

inline void WriteFits(const std::string& path, const ser::FloatImage& im) {
    std::vector<std::string> cards;
    auto card = [&](const std::string& key, const std::string& value) {
        std::string c = key;
        c.resize(8, ' ');
        c += "= ";
        std::string v = value;
        if (v.size() < 20) v = std::string(20 - v.size(), ' ') + v;
        c += v;
        c.resize(80, ' ');
        cards.push_back(c);
    };
    card("SIMPLE", "T");
    card("BITPIX", "-32");
    card("NAXIS", im.channels == 1 ? "2" : "3");
    card("NAXIS1", std::to_string(im.width));
    card("NAXIS2", std::to_string(im.height));
    if (im.channels != 1) card("NAXIS3", std::to_string(im.channels));
    std::string end = "END";
    end.resize(80, ' ');
    cards.push_back(end);
    std::string header;
    for (const auto& c : cards) header += c;
    header.resize((header.size() + 2879) / 2880 * 2880, ' ');
    std::vector<uint8_t> data;
    data.reserve(im.data.size() * 4);
    for (int c = 0; c < im.channels; ++c)
        for (int y = 0; y < im.height; ++y)
            for (int x = 0; x < im.width; ++x) {
                const float v = im.data[(static_cast<size_t>(y) * im.width + x) * im.channels + c];
                uint32_t u;
                std::memcpy(&u, &v, 4);
                for (int k = 3; k >= 0; --k) data.push_back(static_cast<uint8_t>(u >> (8 * k)));
            }
    data.resize((data.size() + 2879) / 2880 * 2880, 0);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot write " + path);
    const bool written = std::fwrite(header.data(), 1, header.size(), f) == header.size() &&
                         std::fwrite(data.data(), 1, data.size(), f) == data.size();
    if (std::fclose(f) != 0 || !written) throw std::runtime_error("cannot write " + path + " (disk full?)");
}

}  // namespace planetary::probe
