#pragma once
#include <complex>
#include <cstddef>
#include <vector>

namespace planetary::stack {

using Complex = std::complex<double>;

// True when n >= 1 has no prime factor other than 2, 3 and 5 (the sizes Fft takes).
bool IsFastSize(int n);
// Smallest fast size >= max(n, 1).
int NextFastSize(int n);
// Largest fast size <= n; throws std::invalid_argument for n < 1.
int PrevFastSize(int n);

// In-tree complex DFT of one fixed length n, any 2-3-5-smooth n (written here: no FFT library is
// vendored in this repository and the plan allows no new dependency). Mixed-radix decimation in time
// (radix 4, 2, 3, 5; recursive, out of place), twiddles from one table of n roots of unity computed
// with std::polar in double precision -- round-off error grows as O(log n) eps. Unnormalised both
// ways: Inverse(Forward(x)) = n x.
// Immutable after construction: one Fft may be used from many threads at once.
class Fft {
public:
    // Throws std::invalid_argument when n is not a fast size.
    explicit Fft(int n);
    int Size() const { return n_; }
    // out[k] = sum_j in[j * stride] e^{-2 pi i j k / n}. `in` and `out` must not overlap.
    void Forward(const Complex* in, Complex* out, std::ptrdiff_t stride = 1) const;
    // out[k] = sum_j in[j * stride] e^{+2 pi i j k / n}. `in` and `out` must not overlap.
    void Inverse(const Complex* in, Complex* out, std::ptrdiff_t stride = 1) const;

private:
    void Run(const Complex* in, std::ptrdiff_t stride, Complex* out, std::size_t stage, bool inverse) const;
    Complex Twiddle(int index, bool inverse) const { return inverse ? std::conj(tw_[index]) : tw_[index]; }

    int n_;
    std::vector<int> factors_;  // radices, outermost first
    std::vector<int> spans_;    // spans_[s] = product of factors_[s..]
    std::vector<Complex> tw_;   // e^{-2 pi i j / n}, j = 0 .. n-1
};

// 2-D DFT of a width x height row-major array (rows, then columns), in place; both sides fast sizes.
// Unnormalised: Inverse(Forward(x)) = width * height * x. Thread-safe (scratch is per call).
class Fft2D {
public:
    Fft2D(int width, int height);
    int Width() const { return rows_.Size(); }
    int Height() const { return cols_.Size(); }
    // Throws std::invalid_argument when data.size() != width * height.
    void Forward(std::vector<Complex>& data) const { Run(data, false); }
    void Inverse(std::vector<Complex>& data) const { Run(data, true); }

private:
    void Run(std::vector<Complex>& data, bool inverse) const;
    Fft rows_, cols_;
};

}  // namespace planetary::stack
