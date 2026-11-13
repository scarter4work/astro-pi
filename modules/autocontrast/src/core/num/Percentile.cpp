#include "core/num/Percentile.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace ac {

namespace {

bool has_nan(const std::vector<double>& v)
{
   for (double x : v)
      if (std::isnan(x))
         return true;
   return false;
}

// numpy/lib/_function_base_impl.py::_lerp (4608-4611):
//   diff_b_a = b - a;  result = a + diff_b_a * t;  where t >= 0.5: result = b - diff_b_a * (1 - t)
double lerp(double a, double b, double t)
{
   const double diff = b - a;
   return t >= 0.5 ? b - diff * (1 - t) : a + diff * t;
}

struct Pos {
   size_t prev, next;   // order-statistic indices
   double gamma;
};

// _quantile "linear" (4769-4821): virtual_index = (n - 1) * (q / 100) [percentile() divides q by 100 first,
// line 4243], previous = floor(v), next = previous + 1; v >= n - 1 -> both are the last element;
// gamma = v - previous.
Pos locate(size_t n, double q)
{
   if (!(q >= 0.0 && q <= 100.0))
      throw std::invalid_argument("percentile: q must be in [0, 100]");
   const double virt = static_cast<double>(n - 1) * (q / 100.0);
   if (virt >= static_cast<double>(n - 1))
      return {n - 1, n - 1, 0.0};
   const double fl = std::floor(virt);
   return {static_cast<size_t>(fl), static_cast<size_t>(fl) + 1, virt - fl};
}

} // namespace

std::vector<double> percentiles(std::vector<double> v, const std::vector<double>& qs)
{
   if (v.empty())
      throw std::invalid_argument("percentile: empty input");
   std::vector<Pos> pos;
   pos.reserve(qs.size());
   for (double q : qs)
      pos.push_back(locate(v.size(), q));
   if (has_nan(v))
      return std::vector<double>(qs.size(), std::numeric_limits<double>::quiet_NaN());

   // Select every needed order statistic, ascending, each nth_element confined to what is still unplaced.
   std::vector<size_t> need;
   need.reserve(pos.size() * 2);
   for (const Pos& p : pos) {
      need.push_back(p.prev);
      need.push_back(p.next);
   }
   std::sort(need.begin(), need.end());
   need.erase(std::unique(need.begin(), need.end()), need.end());
   size_t lo = 0;
   for (size_t idx : need) {
      std::nth_element(v.begin() + static_cast<std::ptrdiff_t>(lo), v.begin() + static_cast<std::ptrdiff_t>(idx),
                       v.end());
      lo = idx + 1;
      if (lo >= v.size())
         break;
   }
   std::vector<double> out;
   out.reserve(qs.size());
   for (const Pos& p : pos)
      out.push_back(lerp(v[p.prev], v[p.next], p.gamma));
   return out;
}

double percentile(std::vector<double> v, double q)
{
   return percentiles(std::move(v), std::vector<double>{q})[0];
}

double median(std::vector<double> v)
{
   if (v.empty())
      throw std::invalid_argument("median: empty input");
   if (has_nan(v))
      return std::numeric_limits<double>::quiet_NaN();
   const size_t n = v.size(), h = n / 2;
   std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(h), v.end());
   const double hi = v[h];
   if (n % 2 == 1)
      return hi;
   // np.median (4040-4051): mean of part[h-1:h+1] = (a + b) / 2
   const double lo = *std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(h));
   return (lo + hi) / 2.0;
}

} // namespace ac
