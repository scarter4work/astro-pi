#pragma once

#include <xtensor/containers/xtensor.hpp>
#include <xtensor/views/xview.hpp>
#include <xtensor/views/xslice.hpp>
#include <xtensor/io/xio.hpp>
#include <xtensor/core/xlayout.hpp>

#include <vector>
#include <string>
#include <stdexcept>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <new>

namespace nukex {

struct SubMetadata {
    double fwhm = 0;
    double eccentricity = 0;
    double skyBackground = 0;
    double hfr = 0;
    double altitude = 0;
    double exposure = 0;
    double gain = 0;
    double ccdTemp = 0;
    std::string object;
    std::string filter;
};

class SubCube {
public:
    /// Constructor — allocates (nSubs, height, width) tensor, zero-initialized.
    /// Uses column-major layout so that fixing (y, x) yields contiguous Z-columns.
    /// Throws std::bad_alloc if allocation fails, with diagnostic message to stderr.
    SubCube(size_t nSubs, size_t height, size_t width)
        : m_nSubs(nSubs), m_height(height), m_width(width)
    {
        try {
            // Column-major: dimension 0 (nSubs) is contiguous in memory,
            // so fixing y and x gives a contiguous run of nSubs floats.
            m_cube = xt::xtensor<float, 3, xt::layout_type::column_major>::from_shape({nSubs, height, width});
            m_cube.fill(0.0f);

            m_provenance = xt::xtensor<uint32_t, 2>::from_shape({height, width});
            m_provenance.fill(0);

            m_distType = xt::xtensor<uint8_t, 2>::from_shape({height, width});
            m_distType.fill(0);

            m_metadata.resize(nSubs);
        } catch (const std::bad_alloc& e) {
            std::cerr << "SubCube: failed to allocate tensor ("
                      << nSubs << " x " << height << " x " << width
                      << ") — " << e.what() << std::endl;
            throw;
        }
    }

    // Dimensions
    size_t numSubs() const { return m_nSubs; }
    size_t height()  const { return m_height; }
    size_t width()   const { return m_width; }

    /// Z-column access — returns an xtensor view of shape (nSubs,) at pixel (y, x).
    /// Elements are contiguous in memory because m_cube uses column-major layout
    /// with nSubs as dimension 0 (the fastest-varying dimension).
    auto zColumn(size_t y, size_t x) const {
        return xt::view(m_cube, xt::all(), y, x);
    }

    /// Raw pointer to the start of the Z-column at (y, x).
    /// The nSubs elements are contiguous (stride-1) due to column-major layout.
    const float* zColumnPtr(size_t y, size_t x) const {
        return &m_cube(0, y, x);
    }

    float* zColumnPtr(size_t y, size_t x) {
        return &m_cube(0, y, x);
    }

    // Single pixel access
    float pixel(size_t z, size_t y, size_t x) const {
        return m_cube(z, y, x);
    }

    void setPixel(size_t z, size_t y, size_t x, float val) {
        m_cube(z, y, x) = val;
    }

    /// Write an entire sub slice (one frame, height * width pixels in row-major order).
    void setSub(size_t z, const float* data, size_t count) {
        if (count != m_height * m_width) {
            throw std::invalid_argument(
                "SubCube::setSub: count mismatch (expected "
                + std::to_string(m_height * m_width) + ", got "
                + std::to_string(count) + ")");
        }
        // data is row-major (y then x), copy element-by-element since our
        // internal layout is column-major.
        for (size_t y = 0; y < m_height; ++y) {
            for (size_t x = 0; x < m_width; ++x) {
                m_cube(z, y, x) = data[y * m_width + x];
            }
        }
    }

    // Metadata per sub
    const SubMetadata& metadata(size_t z) const { return m_metadata.at(z); }
    void setMetadata(size_t z, const SubMetadata& meta) { m_metadata.at(z) = meta; }

    // Provenance map — which Z index was selected per pixel
    uint32_t provenance(size_t y, size_t x) const { return m_provenance(y, x); }
    void setProvenance(size_t y, size_t x, uint32_t z) { m_provenance(y, x) = z; }

    // Distribution type map
    uint8_t distType(size_t y, size_t x) const { return m_distType(y, x); }
    void setDistType(size_t y, size_t x, uint8_t t) { m_distType(y, x) = t; }

    // Per-frame pixel masks (trail detection, bad pixel maps, etc.)
    // mask(z, y, x) = 1 means pixel (y,x) in frame z is masked (should be rejected).
    bool hasMasks() const { return m_hasMasks; }
    void allocateMasks() {
        m_masks = xt::xtensor<uint8_t, 3, xt::layout_type::column_major>::from_shape({m_nSubs, m_height, m_width});
        m_masks.fill(0);
        m_hasMasks = true;
    }
    uint8_t mask(size_t z, size_t y, size_t x) const {
        return m_hasMasks ? m_masks(z, y, x) : 0;
    }
    void setMask(size_t z, size_t y, size_t x, uint8_t v) {
        if (m_hasMasks) m_masks(z, y, x) = v;
    }
    const uint8_t* maskColumnPtr(size_t y, size_t x) const {
        return m_hasMasks ? &m_masks(0, y, x) : nullptr;
    }

    // Direct tensor access
    xt::xtensor<float, 3, xt::layout_type::column_major>& cube() { return m_cube; }
    const xt::xtensor<float, 3, xt::layout_type::column_major>& cube() const { return m_cube; }
    xt::xtensor<uint32_t, 2>& provenanceMap() { return m_provenance; }
    xt::xtensor<uint8_t, 2>& distTypeMap() { return m_distType; }

private:
    xt::xtensor<float, 3, xt::layout_type::column_major> m_cube;  // shape: (nSubs, height, width) — column-major for contiguous Z
    xt::xtensor<uint32_t, 2> m_provenance;                        // shape: (height, width)
    xt::xtensor<uint8_t, 2>  m_distType;                          // shape: (height, width)
    xt::xtensor<uint8_t, 3, xt::layout_type::column_major> m_masks; // shape: (nSubs, height, width) — optional per-frame masks
    bool m_hasMasks = false;
    std::vector<SubMetadata>  m_metadata;
    size_t m_nSubs, m_height, m_width;
};

} // namespace nukex
