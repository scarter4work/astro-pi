// AutoContrast core (PCL-free): exact EMD by primal network simplex.
//
// Network. n supply nodes 0..n-1 (mass a_i), m demand nodes n..n+m-1 (mass b_j), and an artificial root R = n+m.
// Real arcs i -> n+j (index i*m + j, cost c_ij, uncapacitated). Each non-root node k also has one artificial
// arc (index n*m + k): k -> R with cost 0 for every supply node and every zero-mass demand node, and R -> k with
// cost C_art for a demand node with b_k > 0. C_art = 2 * (max(0, max c) - min(0, min c)) (1 when every cost is
// 0) exceeds anything a unit could save by passing through R, so an optimal solution moves no mass through the
// root and the real-arc flows solve the transportation problem. C_art is of the order of the costs, not a
// "big M": the node potentials stay at that scale and keep their precision.
//
// Basis. A spanning tree rooted at R, stored per node: parent, the tree arc to the parent (pred), its
// orientation (up: the arc points from the node to its parent), the flow on it, depth, and an intrusive
// doubly-linked child list. Nontree arcs carry no flow (no capacities). The tree is kept STRONGLY FEASIBLE
// (Cunningham 1976; Ahuja, Magnanti & Orlin, Network Flows, sec. 11.5): a positive amount of flow can be sent
// from every node to the root along its tree path, i.e. every tree arc with zero flow points toward the root.
// The initial tree of artificial arcs is strongly feasible: supply nodes and zero-mass demand nodes point up to
// R (any flow, zero included), and positive-mass demand nodes hang from R on arcs carrying b_k > 0.
//
// Pricing. Block search (as in LEMON / Grigoriadis): scan the real nontree arcs cyclically in blocks of
// ceil(sqrt(n*m)) and enter the most negative reduced cost c_ij + pi_i - pi_j of the first block holding one
// below -tol. Artificial arcs never re-enter (they would only route mass through R). tol = 16 eps C_range
// absorbs the rounding of the potentials (9.4e-13 on the 16 x 16 chroma grid).
//
// Leaving arc. The cycle is oriented along the entering arc p -> q; walking it from the apex down to p, across
// to q and back up to the apex, the blocking arcs are those traversed against their direction. Among the
// blocking arcs of minimal flow the LAST one met on that walk leaves (Cunningham's rule), which keeps the tree
// strongly feasible. Anti-cycling: on q's side the blocking arcs point down, and strong feasibility gives them
// positive flow, so a degenerate pivot (delta = 0) always cuts p's side; the subtree that moves then holds p,
// its potentials all rise by -rc > 0, and the sum of the potentials strictly increases. Nondegenerate pivots
// strictly decrease the cost. Cost non-increasing, potential sum strictly increasing at equal cost, and the
// potentials fixed by the basis: no basis can repeat, so the method is finite under any degeneracy, for any
// pricing rule. A pivot cap turns any floating-point surprise into a loud std::runtime_error, never a hang.
//
// Potentials are never updated incrementally: after each pivot the moved subtree is re-derived from its new
// parent edge by edge (pi_child = pi_parent -/+ c), so every tree arc's reduced cost is one rounding from zero
// and the final pricing scan -- which certifies optimality -- runs on exactly the potentials returned.
// Flows are re-derived from the tree at the end (each tree arc carries its subtree's net mass), so conservation
// holds to the rounding of those sums rather than to the drift of thousands of incremental updates.
#include "core/fingerprint/Emd.h"

#include "core/num/Sum.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace ac {

namespace {

std::vector<double> normalise(const std::vector<double>& w, const char* name)
{
   if (w.empty())
      throw std::invalid_argument(std::string("emd_exact: ") + name + " is empty");
   for (double x : w)
      if (!(x >= 0.0) || !std::isfinite(x))
         throw std::invalid_argument(std::string("emd_exact: ") + name +
                                     " weights must be finite and non-negative");
   const double s = pairwise_sum(w.data(), w.size());
   if (!(s > 0.0) || !std::isfinite(s))
      throw std::invalid_argument(std::string("emd_exact: ") + name + " weights must have a positive finite sum");
   std::vector<double> out(w.size());
   for (size_t i = 0; i < w.size(); ++i)
      out[i] = w[i] / s;
   return out;
}

class NetworkSimplex {
public:
   NetworkSimplex(const std::vector<double>& a, const std::vector<double>& b, const std::vector<double>& cost)
      : n_(int(a.size())), m_(int(b.size())), N_(n_ + m_ + 1), root_(n_ + m_), E_(size_t(n_) * size_t(m_)),
        a_(a), b_(b), c_(cost.data())
   {
      double cmax = 0.0, cmin = 0.0;
      for (size_t e = 0; e < E_; ++e) {
         cmax = std::max(cmax, c_[e]);
         cmin = std::min(cmin, c_[e]);
      }
      const double range = cmax - cmin;
      art_cost_hi_ = range > 0.0 ? 2.0 * range : 1.0;
      tol_ = 16.0 * DBL_EPSILON * range;
      block_ = std::max<size_t>(10, size_t(std::ceil(std::sqrt(double(E_)))));

      parent_.assign(N_, -1);
      pred_.assign(N_, -1);
      up_.assign(N_, 0);
      depth_.assign(N_, 0);
      pi_.assign(N_, 0.0);
      flow_.assign(N_, 0.0);
      first_child_.assign(N_, -1);
      next_sib_.assign(N_, -1);
      prev_sib_.assign(N_, -1);
      art_up_.assign(N_ - 1, 0);
      in_tree_.assign(E_, 0);
      stack_.reserve(N_);
      path_.reserve(N_);

      for (int k = 0; k < root_; ++k) {
         // k -> R (cost 0) for supply and zero-mass demand nodes; R -> k (cost C_art) carrying b_k > 0 otherwise
         art_up_[k] = (k < n_ || b_[k - n_] == 0.0) ? 1 : 0;
         parent_[k] = root_;
         pred_[k] = int(E_) + k;
         up_[k] = art_up_[k];
         depth_[k] = 1;
         flow_[k] = k < n_ ? a_[k] : b_[k - n_];
         pi_[k] = derive_pi(k);
         add_child(root_, k);
      }
   }

   long pivots = 0, degenerate = 0;

   void solve()
   {
      const long cap = 50L * long(E_ + size_t(N_)) + 1000L;
      for (;;) {
         const long e_in = find_entering();
         if (e_in < 0)
            return;
         if (++pivots > cap)
            throw std::runtime_error("emd_exact: network simplex exceeded its pivot cap (" + std::to_string(cap) +
                                     ") without reaching optimality");
         pivot(size_t(e_in));
      }
   }

   EmdResult result()
   {
      // Re-derive the tree flows from the masses: a tree arc carries the net mass of the subtree below it.
      std::vector<int> order;
      order.reserve(N_);
      stack_.clear();
      stack_.push_back(root_);
      while (!stack_.empty()) {
         const int w = stack_.back();
         stack_.pop_back();
         order.push_back(w);
         for (int ch = first_child_[w]; ch >= 0; ch = next_sib_[ch])
            stack_.push_back(ch);
      }
      std::vector<double> net(N_, 0.0);
      for (auto it = order.rbegin(); it != order.rend(); ++it) {
         const int w = *it;
         if (w == root_)
            break;
         net[w] += w < n_ ? a_[w] : -b_[w - n_];
         flow_[w] = up_[w] ? net[w] : -net[w];
         net[parent_[w]] += net[w];
      }

      EmdResult r;
      r.pivots = pivots;
      r.degenerate_pivots = degenerate;
      r.u.resize(n_);
      r.v.resize(m_);
      for (int i = 0; i < n_; ++i)
         r.u[i] = -pi_[i];
      for (int j = 0; j < m_; ++j)
         r.v[j] = pi_[n_ + j];
      double cost = 0.0;
      for (int w = 0; w < root_; ++w) {
         const size_t e = size_t(pred_[w]);
         if (e >= E_) {
            // an artificial arc left in the tree: it may hold only the rounding of sum(a) - sum(b)
            if (!(std::abs(flow_[w]) <= 1e-12)) {
               std::ostringstream msg;
               msg << "emd_exact: infeasible -- " << flow_[w] << " of the mass is routed through the artificial root";
               throw std::runtime_error(msg.str());
            }
            continue;
         }
         if (!(flow_[w] >= -1e-12))
            throw std::runtime_error("emd_exact: internal error, the optimal basis has a negative flow");
         const int i = int(e / size_t(m_)), j = int(e % size_t(m_));
         r.flows.emplace_back(i, j, flow_[w]);
         cost += c_[e] * flow_[w];
      }
      r.cost = cost;
      return r;
   }

private:
   int n_, m_, N_, root_;
   size_t E_;
   const std::vector<double>& a_;
   const std::vector<double>& b_;
   const double* c_;
   double art_cost_hi_ = 1.0, tol_ = 0.0;
   size_t block_ = 10, next_arc_ = 0;

   std::vector<int> parent_, pred_, depth_, first_child_, next_sib_, prev_sib_;
   std::vector<uint8_t> up_, art_up_, in_tree_;
   std::vector<double> pi_, flow_;
   std::vector<int> stack_, path_;

   int tail(size_t e) const
   {
      if (e < E_)
         return int(e / size_t(m_));
      const int k = int(e - E_);
      return art_up_[k] ? k : root_;
   }
   int head(size_t e) const
   {
      if (e < E_)
         return n_ + int(e % size_t(m_));
      const int k = int(e - E_);
      return art_up_[k] ? root_ : k;
   }
   double arc_cost(size_t e) const
   {
      if (e < E_)
         return c_[e];
      return art_up_[e - E_] ? 0.0 : art_cost_hi_;
   }

   // The potential that makes w's tree arc reduced-cost zero, from its parent's: c + pi_tail - pi_head = 0.
   double derive_pi(int w) const
   {
      const double c = arc_cost(size_t(pred_[w]));
      return up_[w] ? pi_[parent_[w]] - c : pi_[parent_[w]] + c;
   }

   void add_child(int par, int ch)
   {
      prev_sib_[ch] = -1;
      next_sib_[ch] = first_child_[par];
      if (first_child_[par] >= 0)
         prev_sib_[first_child_[par]] = ch;
      first_child_[par] = ch;
   }
   void remove_child(int par, int ch)
   {
      if (prev_sib_[ch] >= 0)
         next_sib_[prev_sib_[ch]] = next_sib_[ch];
      else
         first_child_[par] = next_sib_[ch];
      if (next_sib_[ch] >= 0)
         prev_sib_[next_sib_[ch]] = prev_sib_[ch];
      prev_sib_[ch] = next_sib_[ch] = -1;
   }

   // Block search over the real arcs, cyclically from next_arc_. -1 when no nontree arc prices below -tol: that
   // full scan is the optimality certificate.
   long find_entering()
   {
      const size_t m = size_t(m_);
      const double* pd = pi_.data() + n_;   // demand potentials
      double best = -tol_;
      long best_e = -1;
      size_t in_block = 0, scanned = 0;
      size_t e = next_arc_;
      size_t i = e / m, j = e % m;
      while (scanned < E_) {
         // the rest of row i (or of the block, whichever ends first)
         const size_t run = std::min({m - j, E_ - scanned, block_ - in_block});
         const double pi_i = pi_[i];
         const double* ce = c_ + e;
         const uint8_t* te = in_tree_.data() + e;
         for (size_t k = 0; k < run; ++k) {
            const double rc = ce[k] + pi_i - pd[j + k];
            if (rc < best && !te[k]) {
               best = rc;
               best_e = long(e + k);
            }
         }
         e += run;
         j += run;
         scanned += run;
         in_block += run;
         if (j == m) {
            j = 0;
            if (++i == size_t(n_)) {
               i = 0;
               e = 0;
            }
         }
         if (in_block == block_) {
            if (best_e >= 0) {
               next_arc_ = e;
               return best_e;
            }
            in_block = 0;
         }
      }
      next_arc_ = e;
      return best_e;
   }

   void pivot(size_t e_in)
   {
      const int p = tail(e_in), q = head(e_in);
      // apex of the cycle
      int x = p, y = q;
      while (x != y) {
         if (depth_[x] >= depth_[y])
            x = parent_[x];
         else
            y = parent_[y];
      }
      const int join = x;

      // Leaving arc: last blocking arc of minimal flow walking apex -> p -> q -> apex. On p's side the walk goes
      // down (blocking: arcs pointing up), on q's side up (blocking: arcs pointing down); ties on p's side go to
      // the node nearest p (strict <), on q's side to the node nearest the apex (<=), and q's side wins ties.
      double delta = std::numeric_limits<double>::infinity();
      int u_out = -1;
      bool out_on_p_side = false;
      for (int u = p; u != join; u = parent_[u])
         if (up_[u] && flow_[u] < delta) {
            delta = flow_[u];
            u_out = u;
            out_on_p_side = true;
         }
      for (int u = q; u != join; u = parent_[u])
         if (!up_[u] && flow_[u] <= delta) {
            delta = flow_[u];
            u_out = u;
            out_on_p_side = false;
         }
      if (u_out < 0)
         throw std::runtime_error("emd_exact: unbounded -- a negative-cost cycle with no blocking arc");

      if (delta > 0.0) {
         for (int u = p; u != join; u = parent_[u])
            flow_[u] += up_[u] ? -delta : delta;
         for (int u = q; u != join; u = parent_[u])
            flow_[u] += up_[u] ? delta : -delta;
      } else {
         ++degenerate;
      }

      // Re-hang the subtree cut off below u_out from the entering arc's endpoint inside it.
      const int v_in = out_on_p_side ? p : q, u_in = out_on_p_side ? q : p;
      const size_t e_out = size_t(pred_[u_out]);
      if (e_out < E_)
         in_tree_[e_out] = 0;
      in_tree_[e_in] = 1;

      path_.clear();
      for (int w = v_in; w != u_out; w = parent_[w])
         path_.push_back(w);
      path_.push_back(u_out);
      remove_child(parent_[u_out], u_out);
      for (size_t t = path_.size() - 1; t-- > 0;) {
         const int w = path_[t], above = path_[t + 1];   // w is currently a child of `above`
         remove_child(above, w);
         parent_[above] = w;
         pred_[above] = pred_[w];
         up_[above] = up_[w] ? 0 : 1;
         flow_[above] = flow_[w];
         add_child(w, above);
      }
      parent_[v_in] = u_in;
      pred_[v_in] = int(e_in);
      up_[v_in] = v_in == p ? 1 : 0;
      flow_[v_in] = delta;
      add_child(u_in, v_in);

      // Re-derive depth and potentials of the moved subtree, parents before children.
      stack_.clear();
      stack_.push_back(v_in);
      while (!stack_.empty()) {
         const int w = stack_.back();
         stack_.pop_back();
         depth_[w] = depth_[parent_[w]] + 1;
         pi_[w] = derive_pi(w);
         for (int ch = first_child_[w]; ch >= 0; ch = next_sib_[ch])
            stack_.push_back(ch);
      }
   }
};

} // namespace

EmdResult emd_exact(const std::vector<double>& supply, const std::vector<double>& demand,
                    const std::vector<double>& cost)
{
   const auto a = normalise(supply, "supply");
   const auto b = normalise(demand, "demand");
   if (cost.size() != a.size() * b.size())
      throw std::invalid_argument("emd_exact: cost must be supply.size() x demand.size() (" +
                                  std::to_string(a.size()) + " x " + std::to_string(b.size()) + "), got " +
                                  std::to_string(cost.size()) + " entries");
   for (double c : cost)
      if (!std::isfinite(c))
         throw std::invalid_argument("emd_exact: every cost must be finite");
   NetworkSimplex ns(a, b, cost);
   ns.solve();
   return ns.result();
}

} // namespace ac
