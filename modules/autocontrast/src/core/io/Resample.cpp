// AutoContrast core: Pillow-exact resizing. Ported from Pillow 12.3.0 (MIT-CMU / HPND licence, see
// THIRD_PARTY_NOTICES.txt): src/libImaging/Resample.c (precompute_coeffs, normalize_coeffs_8bpc, clip8, the
// 8 bpc and 32 bpc horizontal/vertical passes, ImagingResampleInner), src/PIL/Image.py Image.resize (the
// tall-image vertical-first branch) and src/libImaging/Geometry.c ImagingScaleAffine (NEAREST, modes 1/P).
// Expression order follows the C source; ac_core builds with -ffp-contract=off so nothing is fused.
#include "core/io/Resample.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace ac {

namespace {

constexpr double kBicubicSupport = 2.0;
constexpr int kPrecisionBits = 32 - 8 - 2;   // PRECISION_BITS

double bicubic_filter(double x)
{
   constexpr double a = -0.5;
   if (x < 0.0)
      x = -x;
   if (x < 1.0)
      return ((a + 2.0) * x - (a + 3.0)) * x * x + 1;
   if (x < 2.0)
      return (((x - 5) * x + 8) * x - 4) * a;
   return 0.0;
}

struct Coeffs {
   int ksize = 0;
   std::vector<int> bounds;   // (xmin, xmax-count) per output sample
   std::vector<double> kk;    // out_size x ksize
};

// precompute_coeffs(inSize, in0, in1, outSize, BICUBIC). in0/in1 are Pillow's float box edges.
Coeffs precompute_coeffs(int in_size, float in0, float in1, int out_size)
{
   Coeffs c;
   const double scale = static_cast<double>(in1 - in0) / out_size;
   double filterscale = scale;
   if (filterscale < 1.0)
      filterscale = 1.0;
   const double support = kBicubicSupport * filterscale;
   c.ksize = static_cast<int>(std::ceil(support)) * 2 + 1;
   c.kk.assign(static_cast<size_t>(out_size) * c.ksize, 0.0);
   c.bounds.assign(static_cast<size_t>(out_size) * 2, 0);
   const double inv_filterscale = 1.0 / filterscale;
   for (int xx = 0; xx < out_size; ++xx) {
      const double center = in0 + (xx + 0.5) * scale;
      double ww = 0.0;
      int xmin = static_cast<int>(center - support + 0.5);
      if (xmin < 0)
         xmin = 0;
      int xmax = static_cast<int>(center + support + 0.5);
      if (xmax > in_size)
         xmax = in_size;
      xmax -= xmin;
      double* k = &c.kk[static_cast<size_t>(xx) * c.ksize];
      int x = 0;
      for (; x < xmax; ++x) {
         const double w = bicubic_filter((x + xmin - center + 0.5) * inv_filterscale);
         k[x] = w;
         ww += w;
      }
      if (ww != 0.0)
         for (x = 0; x < xmax; ++x)
            k[x] /= ww;
      for (; x < c.ksize; ++x)
         k[x] = 0;
      c.bounds[static_cast<size_t>(xx) * 2 + 0] = xmin;
      c.bounds[static_cast<size_t>(xx) * 2 + 1] = xmax;
   }
   return c;
}

std::vector<int32_t> normalize_coeffs_8bpc(const Coeffs& c)
{
   std::vector<int32_t> kk(c.kk.size());
   for (size_t x = 0; x < c.kk.size(); ++x) {
      if (c.kk[x] < 0)
         kk[x] = static_cast<int>(-0.5 + c.kk[x] * (1 << kPrecisionBits));
      else
         kk[x] = static_cast<int>(0.5 + c.kk[x] * (1 << kPrecisionBits));
   }
   return kk;
}

// clip8: clip8_lookups[in >> PRECISION_BITS] -- an arithmetic shift, then the table's clamp to [0, 255].
inline uint8_t clip8(int in)
{
   const int v = in >> kPrecisionBits;
   return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// One row of the 8 bpc horizontal pass: in (w x c) -> out (nw x c).
void horizontal_row_8bpc(const uint8_t* in, uint8_t* out, int c, int nw, int ksize, const std::vector<int>& bounds,
                         const std::vector<int32_t>& kk)
{
   for (int xx = 0; xx < nw; ++xx) {
      const int xmin = bounds[static_cast<size_t>(xx) * 2 + 0];
      const int xmax = bounds[static_cast<size_t>(xx) * 2 + 1];
      const int32_t* k = &kk[static_cast<size_t>(xx) * ksize];
      for (int ch = 0; ch < c; ++ch) {
         int ss = 1 << (kPrecisionBits - 1);
         for (int x = 0; x < xmax; ++x)
            ss += in[static_cast<size_t>(x + xmin) * c + ch] * k[x];
         out[static_cast<size_t>(xx) * c + ch] = clip8(ss);
      }
   }
}

// The 8 bpc vertical pass over `rows` (row r at in + r * row_bytes, rows indexed from the shifted bounds).
void vertical_8bpc(const uint8_t* in, int width, int c, uint8_t* out, int nh, int ksize, const std::vector<int>& bounds,
                   const std::vector<int32_t>& kk, int bound_shift)
{
   const size_t row_bytes = static_cast<size_t>(width) * c;
   for (int yy = 0; yy < nh; ++yy) {
      const int32_t* k = &kk[static_cast<size_t>(yy) * ksize];
      const int ymin = bounds[static_cast<size_t>(yy) * 2 + 0] - bound_shift;
      const int ymax = bounds[static_cast<size_t>(yy) * 2 + 1];
      uint8_t* line = out + static_cast<size_t>(yy) * row_bytes;
      for (size_t i = 0; i < row_bytes; ++i) {
         int ss = 1 << (kPrecisionBits - 1);
         for (int y = 0; y < ymax; ++y)
            ss += in[static_cast<size_t>(y + ymin) * row_bytes + i] * k[y];
         line[i] = clip8(ss);
      }
   }
}

void check_args(int w, int h, int c, int nw, int nh, const char* who)
{
   if (w < 1 || h < 1 || nw < 1 || nh < 1)
      throw std::invalid_argument(std::string(who) + ": height and width must be > 0");
   if (c < 1 || c > 4)
      throw std::invalid_argument(std::string(who) + ": 1 to 4 channels, got " + std::to_string(c));
}

// Image.resize's tall branch: self.size[1] > self.size[0] * 100 and size[1] < self.size[1].
bool tall_branch(int w, int h, int nh) { return static_cast<long long>(h) > static_cast<long long>(w) * 100 && nh < h; }

// ImagingResampleInner for 8 bpc over a full image (no tall logic): horizontal pass first, then vertical.
std::vector<uint8_t> resample_inner_u8(const uint8_t* src, int w, int h, int c, int nw, int nh)
{
   const bool need_h = nw != w;
   const bool need_v = nh != h;
   const Coeffs cv = precompute_coeffs(h, 0.0f, static_cast<float>(h), nh);
   const int ybox_first = cv.bounds[0];
   const int ybox_last = cv.bounds[static_cast<size_t>(nh) * 2 - 2] + cv.bounds[static_cast<size_t>(nh) * 2 - 1];
   std::vector<uint8_t> temp;
   const uint8_t* vin = src;
   int vwidth = w, shift = 0;
   if (need_h) {
      const Coeffs ch = precompute_coeffs(w, 0.0f, static_cast<float>(w), nw);
      const auto kk = normalize_coeffs_8bpc(ch);
      temp.resize(static_cast<size_t>(nw) * (ybox_last - ybox_first) * c);
      for (int y = ybox_first; y < ybox_last; ++y)
         horizontal_row_8bpc(src + static_cast<size_t>(y) * w * c, temp.data() + static_cast<size_t>(y - ybox_first) * nw * c,
                             c, nw, ch.ksize, ch.bounds, kk);
      vin = temp.data();
      vwidth = nw;
      shift = ybox_first;
   }
   if (need_v) {
      std::vector<uint8_t> out(static_cast<size_t>(vwidth) * nh * c);
      vertical_8bpc(vin, vwidth, c, out.data(), nh, cv.ksize, cv.bounds, normalize_coeffs_8bpc(cv), shift);
      return out;
   }
   if (need_h)
      return temp;
   return std::vector<uint8_t>(src, src + static_cast<size_t>(w) * h * c);
}

// The 32 bpc (FLOAT32) passes: double accumulation, one float rounding per pass.
std::vector<float> resample_inner_f32(const float* src, int w, int h, int nw, int nh)
{
   const bool need_h = nw != w;
   const bool need_v = nh != h;
   const Coeffs cv = precompute_coeffs(h, 0.0f, static_cast<float>(h), nh);
   const int ybox_first = cv.bounds[0];
   const int ybox_last = cv.bounds[static_cast<size_t>(nh) * 2 - 2] + cv.bounds[static_cast<size_t>(nh) * 2 - 1];
   std::vector<float> temp;
   const float* vin = src;
   int vwidth = w, shift = 0;
   if (need_h) {
      const Coeffs ch = precompute_coeffs(w, 0.0f, static_cast<float>(w), nw);
      temp.resize(static_cast<size_t>(nw) * (ybox_last - ybox_first));
      for (int y = ybox_first; y < ybox_last; ++y) {
         const float* line_in = src + static_cast<size_t>(y) * w;
         float* line_out = temp.data() + static_cast<size_t>(y - ybox_first) * nw;
         for (int xx = 0; xx < nw; ++xx) {
            const int xmin = ch.bounds[static_cast<size_t>(xx) * 2 + 0];
            const int xmax = ch.bounds[static_cast<size_t>(xx) * 2 + 1];
            const double* k = &ch.kk[static_cast<size_t>(xx) * ch.ksize];
            double ss = 0.0;
            for (int x = 0; x < xmax; ++x)
               ss += line_in[x + xmin] * k[x];
            line_out[xx] = static_cast<float>(ss);
         }
      }
      vin = temp.data();
      vwidth = nw;
      shift = ybox_first;
   }
   if (need_v) {
      std::vector<float> out(static_cast<size_t>(vwidth) * nh);
      for (int yy = 0; yy < nh; ++yy) {
         const int ymin = cv.bounds[static_cast<size_t>(yy) * 2 + 0] - shift;
         const int ymax = cv.bounds[static_cast<size_t>(yy) * 2 + 1];
         const double* k = &cv.kk[static_cast<size_t>(yy) * cv.ksize];
         for (int xx = 0; xx < vwidth; ++xx) {
            double ss = 0.0;
            for (int y = 0; y < ymax; ++y)
               ss += vin[static_cast<size_t>(y + ymin) * vwidth + xx] * k[y];
            out[static_cast<size_t>(yy) * vwidth + xx] = static_cast<float>(ss);
         }
      }
      return out;
   }
   if (need_h)
      return temp;
   return std::vector<float>(src, src + static_cast<size_t>(w) * h);
}

// _resize with NEAREST: ImagingScaleAffine with a[0] = (box[2] - box[0]) / xsize, a[4] = (box[3] - box[1]) / ysize,
// a[2] = box[0], a[5] = box[1] (box = the whole float image), fill = 1.
std::vector<uint8_t> scale_affine_nearest(const uint8_t* src, int w, int h, int c, int nw, int nh)
{
   if (nw == w && nh == h)   // integer box whose size is the requested size: ImagingCrop of everything
      return std::vector<uint8_t>(src, src + static_cast<size_t>(w) * h * c);
   const float box2 = static_cast<float>(w), box3 = static_cast<float>(h);
   const double a0 = static_cast<double>(box2 - 0.0f) / nw;
   const double a4 = static_cast<double>(box3 - 0.0f) / nh;
   const double a2 = 0.0, a5 = 0.0;
   auto coord = [](double v) { return v < 0.0 ? -1 : static_cast<int>(v); };   // COORD
   std::vector<int> xintab(static_cast<size_t>(nw), 0);
   double xo = a2 + a0 * 0.5;
   double yo = a5 + a4 * 0.5;
   int xmin = nw, xmax = 0;
   for (int x = 0; x < nw; ++x) {
      const int xin = coord(xo);
      if (xin >= 0 && xin < w) {
         xmax = x + 1;
         if (x < xmin)
            xmin = x;
         xintab[static_cast<size_t>(x)] = xin;
      }
      xo += a0;
   }
   std::vector<uint8_t> out(static_cast<size_t>(nw) * nh * c, 0);   // fill
   for (int y = 0; y < nh; ++y) {
      const int yi = coord(yo);
      if (yi >= 0 && yi < h) {
         const uint8_t* in = src + static_cast<size_t>(yi) * w * c;
         uint8_t* line = out.data() + static_cast<size_t>(y) * nw * c;
         for (int x = xmin; x < xmax; ++x)
            for (int ch = 0; ch < c; ++ch)
               line[static_cast<size_t>(x) * c + ch] = in[static_cast<size_t>(xintab[static_cast<size_t>(x)]) * c + ch];
      }
      yo += a4;
   }
   return out;
}

} // namespace

std::vector<uint8_t> resize_u8(const uint8_t* src, int w, int h, int c, int nw, int nh)
{
   check_args(w, h, c, nw, nh, "resize_u8");
   if (nw == w && nh == h)
      return std::vector<uint8_t>(src, src + static_cast<size_t>(w) * h * c);
   if (tall_branch(w, h, nh)) {
      const auto tmp = resample_inner_u8(src, w, h, c, w, nh);
      return resample_inner_u8(tmp.data(), w, nh, c, nw, nh);
   }
   return resample_inner_u8(src, w, h, c, nw, nh);
}

std::vector<float> resize_f32(const float* src, int w, int h, int nw, int nh)
{
   check_args(w, h, 1, nw, nh, "resize_f32");
   if (nw == w && nh == h)
      return std::vector<float>(src, src + static_cast<size_t>(w) * h);
   if (tall_branch(w, h, nh)) {
      const auto tmp = resample_inner_f32(src, w, h, w, nh);
      return resample_inner_f32(tmp.data(), w, nh, nw, nh);
   }
   return resample_inner_f32(src, w, h, nw, nh);
}

std::vector<uint8_t> resize_nearest_u8(const uint8_t* src, int w, int h, int c, int nw, int nh)
{
   check_args(w, h, c, nw, nh, "resize_nearest_u8");
   if (nw == w && nh == h)
      return std::vector<uint8_t>(src, src + static_cast<size_t>(w) * h * c);
   if (tall_branch(w, h, nh)) {
      const auto tmp = scale_affine_nearest(src, w, h, c, w, nh);
      return scale_affine_nearest(tmp.data(), w, nh, c, nw, nh);
   }
   return scale_affine_nearest(src, w, h, c, nw, nh);
}

// ---- streaming ------------------------------------------------------------------------------------------------

ResizeU8Stream::ResizeU8Stream(int w, int h, int c, int nw, int nh) : w_(w), h_(h), c_(c), nw_(nw), nh_(nh)
{
   check_args(w, h, c, nw, nh, "ResizeU8Stream");
   if (nw == w && nh == h) {
      // Image.resize returns a copy: keep every row as is
      ybox_first_ = 0;
      ybox_last_ = h;
      held_width_ = w;
      held_.resize(static_cast<size_t>(w) * h * c);
      return;
   }
   tall_ = tall_branch(w, h, nh);
   const Coeffs cv = precompute_coeffs(h, 0.0f, static_cast<float>(h), nh);
   ybox_first_ = cv.bounds[0];
   ybox_last_ = cv.bounds[static_cast<size_t>(nh) * 2 - 2] + cv.bounds[static_cast<size_t>(nh) * 2 - 1];
   ksize_v_ = cv.ksize;
   bounds_v_ = cv.bounds;
   kk_v_ = normalize_coeffs_8bpc(cv);
   need_v_ = nh != h;
   need_h_ = nw != w;
   if (need_h_) {
      const Coeffs ch = precompute_coeffs(w, 0.0f, static_cast<float>(w), nw);
      ksize_h_ = ch.ksize;
      bounds_h_ = ch.bounds;
      kk_h_ = normalize_coeffs_8bpc(ch);
   }
   // Tall images run the vertical pass first: hold the source rows the vertical pass reads, unreduced.
   held_width_ = (tall_ || !need_h_) ? w : nw;
   held_.resize(static_cast<size_t>(held_width_) * (ybox_last_ - ybox_first_) * c);
}

void ResizeU8Stream::push_row(const uint8_t* row)
{
   if (next_ >= h_)
      throw std::logic_error("ResizeU8Stream: more rows pushed than the image has");
   const int y = next_++;
   if (y < ybox_first_ || y >= ybox_last_)
      return;   // no output sample reads this row
   uint8_t* dst = held_.data() + static_cast<size_t>(y - ybox_first_) * held_width_ * c_;
   if (held_width_ == w_)
      std::copy(row, row + static_cast<size_t>(w_) * c_, dst);
   else
      horizontal_row_8bpc(row, dst, c_, nw_, ksize_h_, bounds_h_, kk_h_);
}

std::vector<uint8_t> ResizeU8Stream::finish()
{
   if (next_ != h_)
      throw std::logic_error("ResizeU8Stream: " + std::to_string(next_) + " of " + std::to_string(h_) + " rows pushed");
   if (nw_ == w_ && nh_ == h_)
      return std::move(held_);
   if (tall_) {
      // vertical over the held source rows (w x nh), then horizontal (ImagingResample twice, as Image.resize does)
      std::vector<uint8_t> v(static_cast<size_t>(w_) * nh_ * c_);
      vertical_8bpc(held_.data(), w_, c_, v.data(), nh_, ksize_v_, bounds_v_, kk_v_, ybox_first_);
      held_.clear();
      held_.shrink_to_fit();
      if (!need_h_)
         return v;
      std::vector<uint8_t> out(static_cast<size_t>(nw_) * nh_ * c_);
      for (int y = 0; y < nh_; ++y)
         horizontal_row_8bpc(v.data() + static_cast<size_t>(y) * w_ * c_, out.data() + static_cast<size_t>(y) * nw_ * c_,
                             c_, nw_, ksize_h_, bounds_h_, kk_h_);
      return out;
   }
   if (!need_v_)
      return std::move(held_);
   std::vector<uint8_t> out(static_cast<size_t>(held_width_) * nh_ * c_);
   vertical_8bpc(held_.data(), held_width_, c_, out.data(), nh_, ksize_v_, bounds_v_, kk_v_, ybox_first_);
   held_.clear();
   held_.shrink_to_fit();
   return out;
}

} // namespace ac
