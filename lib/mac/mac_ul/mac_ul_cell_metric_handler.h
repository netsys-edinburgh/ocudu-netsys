// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/mac/mac_metrics.h"
#include <atomic>
#include <chrono>
#include <limits>

namespace ocudu {

/// \brief Accumulates MAC UL PDU decode latency for a single cell using lock-free atomics.
///
/// add_sample() is called from per-UE executors (concurrent); collect() is called from the
/// control executor when building the periodic metric report.
class mac_ul_cell_metric_handler
{
public:
  explicit mac_ul_cell_metric_handler(pci_t pci_) : pci(pci_) {}

  /// Record one PDU decode completion. Called concurrently from UE executors.
  void add_sample(std::chrono::nanoseconds elapsed)
  {
    const uint64_t ns = static_cast<uint64_t>(elapsed.count());
    count.fetch_add(1, std::memory_order_relaxed);
    sum.fetch_add(ns, std::memory_order_relaxed);

    // CAS loop for max
    uint64_t cur_max = max_ns.load(std::memory_order_relaxed);
    while (ns > cur_max && !max_ns.compare_exchange_weak(cur_max, ns, std::memory_order_relaxed)) {
    }

    // CAS loop for min
    uint64_t cur_min = min_ns.load(std::memory_order_relaxed);
    while (ns < cur_min && !min_ns.compare_exchange_weak(cur_min, ns, std::memory_order_relaxed)) {
    }
  }

  /// Atomically drain accumulators and build a report. Called from the control executor.
  mac_ul_cell_metric_report collect()
  {
    mac_ul_cell_metric_report rep;
    rep.pci      = pci;
    rep.nof_pdus = static_cast<unsigned>(count.exchange(0, std::memory_order_relaxed));

    const uint64_t drained_sum = sum.exchange(0, std::memory_order_relaxed);
    const uint64_t drained_max = max_ns.exchange(0, std::memory_order_relaxed);
    const uint64_t drained_min = min_ns.exchange(std::numeric_limits<uint64_t>::max(), std::memory_order_relaxed);

    if (rep.nof_pdus > 0) {
      rep.pdu_decode_latency.average = std::chrono::nanoseconds(drained_sum / rep.nof_pdus);
      rep.pdu_decode_latency.max     = std::chrono::nanoseconds(drained_max);
      rep.pdu_decode_latency.min =
          std::chrono::nanoseconds(drained_min == std::numeric_limits<uint64_t>::max() ? 0 : drained_min);
    }
    return rep;
  }

private:
  const pci_t           pci;
  std::atomic<uint64_t> count{0};
  std::atomic<uint64_t> sum{0};
  std::atomic<uint64_t> max_ns{0};
  std::atomic<uint64_t> min_ns{std::numeric_limits<uint64_t>::max()};

  static_assert(std::atomic<uint64_t>::is_always_lock_free);
};

} // namespace ocudu
