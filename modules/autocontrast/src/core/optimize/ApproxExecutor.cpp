#include "core/optimize/ApproxExecutor.h"

#include "core/fingerprint/Starlet.h"
#include "core/num/Gaussian.h"
#include "core/num/Percentile.h"
#include "core/num/Sum.h"
#include "core/optimize/Guardrails.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace ac {

namespace {

// Python's max(a, b): the first argument unless the second is strictly greater (so a NaN first argument wins).
double py_max(double a, double b) { return b > a ? b : a; }

// np.clip(x, lo, hi) for float64 (numpy/_core/src/umath/clip.cpp: _NPY_MIN(_NPY_MAX(x, lo), hi), NaN-propagating).
double np_clip(double x, double lo, double hi)
{
   const double t = std::isnan(x) ? x : (x > lo ? x : lo);
   return std::isnan(t) ? t : (t < hi ? t : hi);
}

// repr(str) for the error message: single quotes unless the text holds a single quote and no double quote.
std::string py_repr(const std::string& s)
{
   const bool dq = s.find('\'') != std::string::npos && s.find('"') == std::string::npos;
   const char q = dq ? '"' : '\'';
   std::string out(1, q);
   for (unsigned char c : s) {
      if (c == '\\') out += "\\\\";
      else if (c == static_cast<unsigned char>(q)) { out += '\\'; out += static_cast<char>(c); }
      else if (c == '\n') out += "\\n";
      else if (c == '\r') out += "\\r";
      else if (c == '\t') out += "\\t";
      else if (c < 0x20 || c == 0x7f) {
         char buf[8];
         std::snprintf(buf, sizeof buf, "\\x%02x", c);
         out += buf;
      } else out += static_cast<char>(c);
   }
   out += q;
   return out;
}

Plane channel(const Image& im, int c)
{
   Plane p(im.width, im.height);
   const size_t n = p.data.size(), ch = static_cast<size_t>(im.channels);
   for (size_t i = 0; i < n; ++i)
      p.data[i] = im.data[i * ch + static_cast<size_t>(c)];
   return p;
}

void set_channel(Image& im, int c, const Plane& p)
{
   const size_t n = p.data.size(), ch = static_cast<size_t>(im.channels);
   for (size_t i = 0; i < n; ++i)
      im.data[i * ch + static_cast<size_t>(c)] = p.data[i];
}

} // namespace

double approx_sigma_for_scale(double scale_arcsec, double pixel_scale_arcsec)
{
   // numpy_exec.py:22-23 (2.355 = FWHM / sigma, 1e-9 and 0.5 floors)
   return py_max(scale_arcsec / py_max(pixel_scale_arcsec, 1e-9) / 2.355, 0.5);
}

int max_supportable_layer(int height, int width)
{
   // numpy_exec.py:26-37: int(math.floor(math.log2(min(height, width))))
   return static_cast<int>(std::floor(std::log2(static_cast<double>(std::min(height, width)))));
}

Image ApproxExecutor::apply(const Image& parent, const Action& action, double pixel_scale_arcsec)
{
   Image out = parent;   // np.array(rgb, dtype=np.float64, copy=True)
   const double s = action.strength();
   const size_t n = out.data.size();
   const int nch = out.channels;

   if (action.kind == "local_contrast") {
      // int(action.params.get("layer", 3))
      int layer = 3;
      if (action.params.contains("layer")) {
         const Json& l = action.params.at("layer");
         layer = l.is_number_integer() ? l.get<int>() : static_cast<int>(l.get<double>());
      }
      const int max_layer = max_supportable_layer(out.height, out.width);
      if (layer > max_layer)
         throw std::invalid_argument("local_contrast layer " + std::to_string(layer) + " exceeds what a " +
                                     std::to_string(out.height) + "x" + std::to_string(out.width) +
                                     " image supports (max layer " + std::to_string(max_layer) +
                                     "); rejecting rather than hanging on an a trous kernel far larger than the image");
      for (int c = 0; c < nch; ++c) {
         Starlet st = starlet_transform(channel(out, c), layer + 1, threads_);
         for (double& v : st.planes[static_cast<size_t>(layer)].data)
            v *= 1.0 + s;
         // planes.sum(axis=0) + residual: add.reduce over the outer axis starts at add's identity +0.0 and adds
         // the planes in order, elementwise.
         Plane sum(out.width, out.height);
         for (size_t i = 0; i < sum.data.size(); ++i) {
            double acc = 0.0;
            for (const Plane& p : st.planes)
               acc += p.data[i];
            sum.data[i] = acc + st.residual.data[i];
         }
         set_channel(out, c, sum);
      }
   } else if (action.kind == "local_equalize") {
      // `action.scale_arcsec or 8.0`: None and 0.0 both fall back (NaN is truthy).
      const double scale =
         (action.scale_arcsec && *action.scale_arcsec != 0.0) ? *action.scale_arcsec : 8.0;
      const double sigma = approx_sigma_for_scale(scale, pixel_scale_arcsec);
      for (int c = 0; c < nch; ++c) {
         Plane x = channel(out, c);
         const Plane m = gaussian_filter(x, sigma, threads_);
         for (size_t i = 0; i < x.data.size(); ++i)
            x.data[i] = x.data[i] + s * (x.data[i] - m.data[i]);
         set_channel(out, c, x);
      }
   } else if (action.kind == "core_hdr") {
      // Compress the bright end, which is what HDRMT does to cores.
      const double k = 1.0 + 8.0 * s;
      const double den = std::log1p(k);
      for (size_t i = 0; i < n; ++i)
         out.data[i] = std::log1p(out.data[i] * k) / den;
   } else if (action.kind == "tonal_reshape") {
      // Monotone S-curve about the midpoint.
      for (size_t i = 0; i < n; ++i)
         out.data[i] = np_clip(0.5 + (out.data[i] - 0.5) * (1.0 + s), 0.0, 1.0);
   } else if (action.kind == "black_point") {
      // Clip-limited: never move the black point past the 1st percentile.
      const double floor_ = percentile(out.data, 1.0) * s;
      const double den = py_max(1.0 - floor_, 1e-9);
      for (size_t i = 0; i < n; ++i)
         out.data[i] = np_clip((out.data[i] - floor_) / den, 0.0, 1.0);
   } else if (action.kind == "chroma") {
      const Plane gray = gray_of(out, threads_);   // out.mean(axis=-1, keepdims=True)
      const size_t ch = static_cast<size_t>(nch);
      for (size_t i = 0; i < gray.data.size(); ++i)
         for (size_t c = 0; c < ch; ++c) {
            double& v = out.data[i * ch + c];
            v = gray.data[i] + (v - gray.data[i]) * (1.0 + s);
         }
   } else if (action.kind == "background_neutralize") {
      std::vector<double> medians(static_cast<size_t>(nch));
      for (int c = 0; c < nch; ++c)
         medians[static_cast<size_t>(c)] = median(channel(out, c).data);
      // medians.mean(): np.add.reduce (numpy's pairwise order) / count
      const double mean = pairwise_sum(medians.data(), medians.size()) / static_cast<double>(nch);
      const size_t ch = static_cast<size_t>(nch);
      for (size_t i = 0; i < n; ++i)
         out.data[i] = out.data[i] - medians[i % ch] + mean;
   } else if (action.kind == "star_split") {
      // A mode change, not a pixel change; the loop handles layer routing.
   } else {
      throw std::invalid_argument("unknown action kind " + py_repr(action.kind));
   }

   for (double& v : out.data)
      v = np_clip(v, 0.0, 1.0);
   return out;
}

} // namespace ac
