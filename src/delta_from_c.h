// See LICENSE.txt for license details.

#ifndef DELTA_FROM_C_H_
#define DELTA_FROM_C_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <type_traits>

#include "omp.h"

/*
Derive delta from a graph-independent constant C:

    delta = C * mean_edge_weight / average_degree

`average_degree` is the number of arcs stored per vertex, num_edges_directed() /
num_nodes(). An undirected graph stores every edge in both directions, so this
is 2m/n, which is what the graph statistics tool reports as "Average degree".
`mean_edge_weight` is taken over the same stored arcs; an undirected edge
contributes twice with the same weight, so the mean is unaffected.

TIMING
------
DeltaSelector::Get() is called from inside the timed region, once per trial of
every source, immediately before the data structures are initialised. That is
the whole point: the cost of choosing delta is charged to the algorithm.

To take it back out of the timer, pass -O. Warmup() then computes the value
once before the timer starts and Get() returns the cached value. The call site
never moves, so switching between the two costs nothing.
*/

// Mean weight over the arcs stored in the CSR.
template <typename GraphT_> double MeanEdgeWeight(const GraphT_ &g) {
  double total = 0.0;
  const int64_t num_nodes = g.num_nodes();

#pragma omp parallel for reduction(+ : total) schedule(dynamic, 1024)
  for (int64_t u = 0; u < num_nodes; u++)
    for (auto wn : g.out_neigh(u))
      total += static_cast<double>(wn.w);

  return total / static_cast<double>(g.num_edges_directed());
}

template <typename WeightT_> class DeltaSelector {
public:
  DeltaSelector(WeightT_ fixed_delta, double c, bool use_c, bool outside_timer)
      : fixed_delta_(fixed_delta), c_(c), use_c_(use_c),
        outside_timer_(outside_timer), cached_(), have_cached_(false), last_() {}

  // Call before the timer starts. Only does work under -O.
  template <typename GraphT_> void Warmup(const GraphT_ &g) {
    if (use_c_ && outside_timer_) {
      cached_ = Compute(g);
      have_cached_ = true;
    }
  }

  // Call inside the timed region, just before the data structures are built.
  template <typename GraphT_> WeightT_ Get(const GraphT_ &g) const {
    if (!use_c_)
      last_ = fixed_delta_;
    else if (have_cached_)
      last_ = cached_;
    else
      last_ = Compute(g);
    return last_;
  }

  // Report the delta the last Get() handed out. Call outside the timer.
  void PrintLast() const {
    if (use_c_)
      std::cout << "Derived delta: " << last_ << " (C = " << c_ << ")"
                << std::endl;
  }

  bool use_c() const { return use_c_; }

private:
  template <typename GraphT_> WeightT_ Compute(const GraphT_ &g) const {
    const double average_degree = static_cast<double>(g.num_edges_directed()) /
                                  static_cast<double>(g.num_nodes());
    double delta = c_ * MeanEdgeWeight(g) / average_degree;

    // An integer-weighted graph has no use for a delta below 1.
    if (std::is_integral<WeightT_>::value)
      delta = std::max(1.0, std::round(delta));

    return static_cast<WeightT_>(delta);
  }

  WeightT_ fixed_delta_;
  double c_;
  bool use_c_;
  bool outside_timer_;
  WeightT_ cached_;
  bool have_cached_;
  mutable WeightT_ last_;
};

#endif // DELTA_FROM_C_H_
