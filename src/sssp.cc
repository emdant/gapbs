// Copyright (c) 2015, The Regents of the University of California (Regents)
// See LICENSE.txt for license details

#include <cinttypes>
#include <iostream>
#include <limits>
#include <queue>
#include <vector>

#include "benchmark.h"
#include "builder.h"
#include "command_line.h"
#include "graph.h"
#include "omp.h"
#include "platform_atomics.h"
#include "pvector.h"
#include "timer.h"

/*
GAP Benchmark Suite
Kernel: Single-source Shortest Paths (SSSP)
Author: Scott Beamer, Yunming Zhang

Returns array of distances for all vertices from given source vertex

This SSSP implementation makes use of the ∆-stepping algorithm [1]. The type
used for weights and distances (WeightT) is typedefined in benchmark.h. The
delta parameter (-d) should be set for each input graph. This implementation
incorporates a new bucket fusion optimization [2] that significantly reduces
the number of iterations (& barriers) needed.

The bins of width delta are actually all thread-local and of type std::vector,
so they can grow but are otherwise capacity-proportional. Each iteration is
done in two phases separated by barriers. In the first phase, the current
shared bin is processed by all threads. As they find vertices whose distance
they are able to improve, they add them to their thread-local bins. During this
phase, each thread also votes on what the next bin should be (smallest
non-empty bin). In the next phase, each thread copies its selected
thread-local bin into the shared bin.

Once a vertex is added to a bin, it is not removed, even if its distance is
later updated and, it now appears in a lower bin. We find ignoring vertices if
their distance is less than the min distance for the current bin removes
enough redundant work to be faster than removing the vertex from older bins.

The bucket fusion optimization [2] executes the next thread-local bin in
the same iteration if the vertices in the next thread-local bin have the
same priority as those in the current shared bin. This optimization greatly
reduces the number of iterations needed without violating the priority-based
execution order, leading to significant speedup on large diameter road networks.

[1] Ulrich Meyer and Peter Sanders. "δ-stepping: a parallelizable shortest path
    algorithm." Journal of Algorithms, 49(1):114–152, 2003.

[2] Yunming Zhang, Ajay Brahmakshatriya, Xinyi Chen, Laxman Dhulipala,
    Shoaib Kamil, Saman Amarasinghe, and Julian Shun. "Optimizing ordered graph
    algorithms with GraphIt." The 18th International Symposium on Code
Generation and Optimization (CGO), pages 158-170, 2020.
*/

using namespace std;

const WeightT kDistInf = numeric_limits<WeightT>::max() / 2;
const size_t kMaxBin = numeric_limits<size_t>::max() / 2;
const size_t kBinSizeThreshold = 1000;

inline void RelaxEdges(const WGraph &g, NodeID u, WeightT delta,
                       pvector<WeightT> &dist,
                       vector<vector<NodeID>> &local_bins
#ifdef COUNT_RELAX
                       ,
                       size_t &visits
#endif
) {
  for (WNode wn : g.out_neigh(u)) {
#ifdef COUNT_RELAX
    visits++;
#endif
    WeightT old_dist = dist[wn.v];
    WeightT new_dist = dist[u] + wn.w;
    while (new_dist < old_dist) {
      if (compare_and_swap(dist[wn.v], old_dist, new_dist)) {
        size_t dest_bin = new_dist / delta;
        if (dest_bin >= local_bins.size())
          local_bins.resize(dest_bin + 1);
        local_bins[dest_bin].push_back(wn.v);
        break;
      }
      old_dist = dist[wn.v]; // swap failed, recheck dist update & retry
    }
  }
}

pvector<WeightT> DeltaStep(const WGraph &g, NodeID source, WeightT delta,
                           bool logging_enabled = false) {
  Timer t;
#ifdef COUNT_RELAX
  size_t total_visits = 0;
#endif
#ifdef COUNT_TIME
  double total_current_bucket_time = 0;
  double total_bucket_fusion_time = 0;
  double total_copy_time = 0;
  double total_barriers_time = 0;
#endif
  pvector<WeightT> dist(g.num_nodes(), kDistInf);
  dist[source] = 0;
  pvector<NodeID> frontier(g.num_edges_directed());
  // two element arrays for double buffering curr=iter&1, next=(iter+1)&1
  size_t shared_indexes[2] = {0, kMaxBin};
  size_t frontier_tails[2] = {1, 0};
  frontier[0] = source;
  t.Start();
#pragma omp parallel
  {
#ifdef COUNT_RELAX
    size_t visits = 0;
#endif
#ifdef COUNT_TIME
    CumulativeTimer cb_t, bf_t, cp_t, bs_t;
#endif
    vector<vector<NodeID>> local_bins(0);
    size_t iter = 0;
    while (shared_indexes[iter & 1] != kMaxBin) {
      size_t &curr_bin_index = shared_indexes[iter & 1];
      size_t &next_bin_index = shared_indexes[(iter + 1) & 1];
      size_t &curr_frontier_tail = frontier_tails[iter & 1];
      size_t &next_frontier_tail = frontier_tails[(iter + 1) & 1];
#ifdef COUNT_TIME
      cb_t.Start();
#endif
#pragma omp for nowait schedule(dynamic, 64)
      for (size_t i = 0; i < curr_frontier_tail; i++) {
        NodeID u = frontier[i];
        if (dist[u] >= delta * static_cast<WeightT>(curr_bin_index))
          RelaxEdges(g, u, delta, dist, local_bins
#ifdef COUNT_RELAX
                     ,
                     visits
#endif
          );
      }
#ifdef COUNT_TIME
      cb_t.Stop();
      bf_t.Start();
#endif

      while (curr_bin_index < local_bins.size() &&
             !local_bins[curr_bin_index].empty() &&
             local_bins[curr_bin_index].size() < kBinSizeThreshold) {
        vector<NodeID> curr_bin_copy = local_bins[curr_bin_index];
        local_bins[curr_bin_index].resize(0);
        for (NodeID u : curr_bin_copy)
          RelaxEdges(g, u, delta, dist, local_bins
#ifdef COUNT_RELAX
                     ,
                     visits
#endif
          );
      }
#ifdef COUNT_TIME
      bf_t.Stop();
      bs_t.Start();
#endif

      for (size_t i = curr_bin_index; i < local_bins.size(); i++) {
        if (!local_bins[i].empty()) {
#pragma omp critical
          next_bin_index = min(next_bin_index, i);
          break;
        }
      }
#pragma omp barrier
#ifdef COUNT_TIME
      bs_t.Stop();
      cp_t.Start();
#endif
#pragma omp single nowait
      {
        t.Stop();
        if (logging_enabled)
          PrintStep(curr_bin_index, t.Millisecs(), curr_frontier_tail);
        t.Start();
        curr_bin_index = kMaxBin;
        curr_frontier_tail = 0;
      }
      if (next_bin_index < local_bins.size()) {
        size_t copy_start = fetch_and_add(next_frontier_tail,
                                          local_bins[next_bin_index].size());
        copy(local_bins[next_bin_index].begin(),
             local_bins[next_bin_index].end(), frontier.data() + copy_start);
        local_bins[next_bin_index].resize(0);
      }
      iter++;
#ifdef COUNT_TIME
      cp_t.Stop();
      bs_t.Start();
#endif

#pragma omp barrier

#ifdef COUNT_TIME
      bs_t.Stop();
#endif
    } //////////////////// end while : sssp finished
#ifdef COUNT_RELAX
#pragma omp atomic
    total_visits += visits;
#endif
#ifdef COUNT_TIME
    double current_bucket_time = cb_t.Seconds();
    double bucket_fusion_time = bf_t.Seconds();
    double copy_time = cp_t.Seconds();
    double barriers_time = bs_t.Seconds();
#pragma omp critical
    {
      total_current_bucket_time += current_bucket_time;
      total_bucket_fusion_time += bucket_fusion_time;
      total_copy_time += copy_time;
      total_barriers_time += barriers_time;
    }

#endif
#pragma omp single
    if (logging_enabled)
      cout << "took " << iter << " iterations" << endl;
  }
#ifdef COUNT_RELAX
  cout << "Number of relaxations: " << total_visits << endl;
#endif
#ifdef COUNT_TIME
  total_current_bucket_time = total_current_bucket_time / omp_get_max_threads();
  total_bucket_fusion_time = total_bucket_fusion_time / omp_get_max_threads();
  total_copy_time = total_copy_time / omp_get_max_threads();
  total_barriers_time = total_barriers_time / omp_get_max_threads();

  cout << "current_bucket time: " << total_current_bucket_time << " seconds"
       << endl;
  cout << "bucket_fusion time: " << total_bucket_fusion_time << " seconds"
       << endl;
  cout << "copy_buckets time: " << total_copy_time << " seconds" << endl;
  cout << "barriers time: " << total_barriers_time << " seconds" << endl;
#endif
  return dist;
}

void PrintSSSPStats(const WGraph &g, const pvector<WeightT> &dist) {
  auto NotInf = [](WeightT d) { return d != kDistInf; };
  int64_t num_reached = count_if(dist.begin(), dist.end(), NotInf);
  cout << "SSSP Tree reaches " << num_reached << " nodes" << endl;
}

// Compares against simple serial implementation
bool SSSPVerifier(const WGraph &g, NodeID source,
                  const pvector<WeightT> &dist_to_test) {
  // Serial Dijkstra implementation to get oracle distances
  pvector<WeightT> oracle_dist(g.num_nodes(), kDistInf);
  oracle_dist[source] = 0;
  typedef pair<WeightT, NodeID> WN;
  priority_queue<WN, vector<WN>, greater<WN>> mq;
  mq.push(make_pair(0, source));
  while (!mq.empty()) {
    WeightT td = mq.top().first;
    NodeID u = mq.top().second;
    mq.pop();
    if (td == oracle_dist[u]) {
      for (WNode wn : g.out_neigh(u)) {
        if (td + wn.w < oracle_dist[wn.v]) {
          oracle_dist[wn.v] = td + wn.w;
          mq.push(make_pair(td + wn.w, wn.v));
        }
      }
    }
  }
  // Report any mismatches
  bool all_ok = true;
  for (NodeID n : g.vertices()) {
    if (dist_to_test[n] != oracle_dist[n]) {
      cout << n << ": " << dist_to_test[n] << " != " << oracle_dist[n] << endl;
      all_ok = false;
    }
  }
  return all_ok;
}

int main(int argc, char *argv[]) {
  CLDelta<WeightT> cli(argc, argv, "single-source shortest-path");
  if (!cli.ParseArgs())
    return -1;
  WeightedBuilder b(cli);
  WGraph g = b.MakeGraph();
  g.PrintStats();

  SourcePicker<WGraph> sp(g, cli.start_vertex());
  for (auto i = 0; i < cli.num_sources(); i++) {
    auto source = sp.PickNext();
    std::cout << "Source: " << source << std::endl;

    auto SSSPBound = [&sp, &cli, source](const WGraph &g) {
      return DeltaStep(g, source, cli.delta(), cli.logging_en());
    };

    auto VerifierBound = [source](const WGraph &g,
                                  const pvector<WeightT> &dist) {
      return SSSPVerifier(g, source, dist);
    };

    BenchmarkKernel(cli, g, SSSPBound, PrintSSSPStats, VerifierBound);
  }

  return 0;
}
