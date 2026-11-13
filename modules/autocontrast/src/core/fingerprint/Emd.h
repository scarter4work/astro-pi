// AutoContrast core (PCL-free): exact optimal-transport EMD between two discrete distributions — the
// transportation LP that scipy.stats.wasserstein_distance_nd hands to HiGHS, solved here by a primal network
// simplex (strongly feasible spanning-tree bases, block-search pricing).
#pragma once

#include <tuple>
#include <vector>

namespace ac {

struct EmdResult {
   double cost = 0.0;                                 // sum c_ij x_ij: the optimal transport cost
   std::vector<double> u, v;                          // duals: c_ij - u_i - v_j >= 0 on every arc, = 0 on basic arcs
   std::vector<std::tuple<int, int, double>> flows;   // every basic arc (i, j, x_ij), degenerate (x = 0) ones too
   long pivots = 0;                                   // simplex pivots, and how many of them moved no flow
   long degenerate_pivots = 0;
};

// emd_exact(supply, demand, cost): min sum c_ij x_ij subject to sum_j x_ij = a_i, sum_i x_ij = b_j, x >= 0, with
// a = supply / sum(supply) and b = demand / sum(demand) (scipy's normalisation; sum is numpy's pairwise sum) and
// `cost` the n x m ground-cost matrix, row-major. Zero-mass bins stay in the graph as nodes.
// Throws std::invalid_argument on empty or mis-sized input, a negative or non-finite weight, a weight vector
// summing to zero (scipy: ValueError) or a non-finite cost; std::runtime_error if the solve itself fails
// (unbounded or infeasible -- impossible with equal normalised totals, but checked, never returned as a number).
// Reentrant: all state is local to the call.
EmdResult emd_exact(const std::vector<double>& supply, const std::vector<double>& demand,
                    const std::vector<double>& cost);

} // namespace ac
