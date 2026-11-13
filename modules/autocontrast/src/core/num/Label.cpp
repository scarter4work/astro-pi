#include "core/num/Label.h"

#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace ac {

namespace {

int find_root(std::vector<int>& parent, int a)
{
   while (parent[static_cast<size_t>(a)] != a) {
      parent[static_cast<size_t>(a)] = parent[static_cast<size_t>(parent[static_cast<size_t>(a)])];
      a = parent[static_cast<size_t>(a)];
   }
   return a;
}

} // namespace

Labels label4(const std::vector<uint8_t>& mask, int w, int h)
{
   if (w < 0 || h < 0 || mask.size() != static_cast<size_t>(w) * static_cast<size_t>(h))
      throw std::invalid_argument("label4: mask size does not match w*h");
   Labels out;
   out.label.assign(mask.size(), 0);

   // Pass 1: provisional labels from the up and left neighbours, union-find on conflicts.
   std::vector<int> parent{0};
   for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
         const size_t i = static_cast<size_t>(y) * w + x;
         if (!mask[i])
            continue;
         const int up = y > 0 ? out.label[i - w] : 0;
         const int left = x > 0 ? out.label[i - 1] : 0;
         if (!up && !left) {
            out.label[i] = static_cast<int>(parent.size());
            parent.push_back(static_cast<int>(parent.size()));
         } else if (up && left) {
            const int ru = find_root(parent, up), rl = find_root(parent, left);
            const int r = std::min(ru, rl);
            parent[static_cast<size_t>(std::max(ru, rl))] = r;
            out.label[i] = r;
         } else {
            out.label[i] = up ? up : left;
         }
      }

   // Pass 2: consecutive final labels in raster order of first appearance (scipy's numbering), and boxes.
   std::vector<int> final(parent.size(), 0);
   for (int& l : out.label) {
      if (!l)
         continue;
      l = find_root(parent, l);
   }
   int next = 0;
   for (size_t i = 0; i < out.label.size(); ++i) {
      int& l = out.label[i];
      if (!l)
         continue;
      int& f = final[static_cast<size_t>(l)];
      if (!f) {
         f = ++next;
         out.boxes.push_back({h, 0, w, 0});
      }
      l = f;
      Box& b = out.boxes[static_cast<size_t>(f - 1)];
      const int y = static_cast<int>(i / static_cast<size_t>(w)), x = static_cast<int>(i % static_cast<size_t>(w));
      b.y0 = std::min(b.y0, y);
      b.y1 = std::max(b.y1, y + 1);
      b.x0 = std::min(b.x0, x);
      b.x1 = std::max(b.x1, x + 1);
   }
   out.count = next;
   return out;
}

} // namespace ac
