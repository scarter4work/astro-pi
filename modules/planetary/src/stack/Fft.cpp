#include "Fft.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace planetary::stack {

bool IsFastSize(int n) {
    if (n < 1) return false;
    for (int p : {2, 3, 5})
        while (n % p == 0) n /= p;
    return n == 1;
}

int NextFastSize(int n) {
    n = std::max(n, 1);
    while (!IsFastSize(n)) ++n;
    return n;
}

int PrevFastSize(int n) {
    if (n < 1) throw std::invalid_argument("PrevFastSize: n = " + std::to_string(n));
    while (!IsFastSize(n)) --n;
    return n;
}

Fft::Fft(int n) : n_(n) {
    if (!IsFastSize(n)) throw std::invalid_argument("Fft: size " + std::to_string(n) + " is not 2-3-5-smooth");
    int m = n;
    while (m % 4 == 0) factors_.push_back(4), m /= 4;
    while (m % 2 == 0) factors_.push_back(2), m /= 2;
    while (m % 3 == 0) factors_.push_back(3), m /= 3;
    while (m % 5 == 0) factors_.push_back(5), m /= 5;
    if (factors_.empty()) factors_.push_back(1);  // n = 1: a single copy
    spans_.resize(factors_.size());
    int span = 1;
    for (size_t s = factors_.size(); s-- > 0;) spans_[s] = span *= factors_[s];
    tw_.resize(static_cast<size_t>(n));
    for (int j = 0; j < n; ++j) tw_[static_cast<size_t>(j)] = std::polar(1.0, -2 * M_PI * j / n);
}

void Fft::Forward(const Complex* in, Complex* out, std::ptrdiff_t stride) const { Run(in, stride, out, 0, false); }
void Fft::Inverse(const Complex* in, Complex* out, std::ptrdiff_t stride) const { Run(in, stride, out, 0, true); }

// DFT of length `span` = p m over in[0], in[stride], ...: the p sub-sequences in[r::p] (stride * p)
// are transformed into out[r m .. r m + m), then combined:
//   X[k1 + m k2] = sum_r W_p^{r k2} (W_span^{r k1} Y_r[k1]),  k1 < m, k2 < p.
// For one k1 the p inputs read and the p outputs written are the same p slots, so it is in place.
void Fft::Run(const Complex* in, std::ptrdiff_t stride, Complex* out, std::size_t stage, bool inverse) const {
    const int p = factors_[stage];
    const int m = spans_[stage] / p;
    if (m == 1) {
        for (int r = 0; r < p; ++r) out[r] = in[r * stride];
    } else {
        for (int r = 0; r < p; ++r) Run(in + r * stride, stride * p, out + r * m, stage + 1, inverse);
    }
    if (p == 1) return;
    const int step = n_ / spans_[stage];  // W_span^j = W_n^{j step}
    switch (p) {
        case 2:
            for (int k = 0; k < m; ++k) {
                const Complex t = out[k + m] * Twiddle(k * step, inverse);
                out[k + m] = out[k] - t;
                out[k] += t;
            }
            break;
        case 4:
            for (int k = 0; k < m; ++k) {
                const Complex t0 = out[k];
                const Complex t1 = out[k + m] * Twiddle(k * step, inverse);
                const Complex t2 = out[k + 2 * m] * Twiddle(2 * k * step, inverse);
                const Complex t3 = out[k + 3 * m] * Twiddle(3 * k * step, inverse);
                const Complex a = t0 + t2, b = t0 - t2, c = t1 + t3, d = t1 - t3;
                // W_4 = -i forward, +i inverse: X1 = b + W_4 d, X3 = b - W_4 d.
                const Complex wd = inverse ? Complex(-d.imag(), d.real()) : Complex(d.imag(), -d.real());
                out[k] = a + c;
                out[k + m] = b + wd;
                out[k + 2 * m] = a - c;
                out[k + 3 * m] = b - wd;
            }
            break;
        default: {  // 3 or 5
            Complex t[5], wp[5];
            for (int q = 0; q < p; ++q) wp[q] = Twiddle(q * (n_ / p), inverse);
            for (int k = 0; k < m; ++k) {
                for (int r = 0; r < p; ++r) t[r] = out[k + r * m] * Twiddle(r * k * step, inverse);
                for (int k2 = 0; k2 < p; ++k2) {
                    Complex s = t[0];
                    for (int r = 1; r < p; ++r) s += t[r] * wp[(r * k2) % p];
                    out[k + k2 * m] = s;
                }
            }
        }
    }
}

Fft2D::Fft2D(int width, int height) : rows_(width), cols_(height) {}

void Fft2D::Run(std::vector<Complex>& data, bool inverse) const {
    const int w = rows_.Size(), h = cols_.Size();
    if (data.size() != static_cast<size_t>(w) * h)
        throw std::invalid_argument("Fft2D: " + std::to_string(data.size()) + " values for " + std::to_string(w) +
                                    "x" + std::to_string(h));
    std::vector<Complex> tmp(static_cast<size_t>(std::max(w, h)));
    for (int y = 0; y < h; ++y) {
        Complex* row = data.data() + static_cast<size_t>(y) * w;
        inverse ? rows_.Inverse(row, tmp.data()) : rows_.Forward(row, tmp.data());
        std::copy(tmp.begin(), tmp.begin() + w, row);
    }
    for (int x = 0; x < w; ++x) {
        Complex* col = data.data() + x;
        inverse ? cols_.Inverse(col, tmp.data(), w) : cols_.Forward(col, tmp.data(), w);
        for (int y = 0; y < h; ++y) col[static_cast<size_t>(y) * w] = tmp[static_cast<size_t>(y)];
    }
}

}  // namespace planetary::stack
