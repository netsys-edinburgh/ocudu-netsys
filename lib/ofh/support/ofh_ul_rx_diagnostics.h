// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/ocudulog/logger.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace ocudu {
namespace ofh {

/// \brief Diagnostics of the Open Fronthaul uplink receive path.
///
/// Follows a received U-Plane frame from the kernel to the resource grid and reports, once a second, one line in the
/// Open Fronthaul log (info level):
///   - Ethernet receiver (ru_txrx thread): frames, receive wake-ups and the largest burst drained in one, frames the
///     kernel dropped because the socket queue was full, the longest gap between two receive polls, and the time from
///     the kernel receive timestamp to the read.
///   - Message receiver (uplink thread): the time from the read to the processing of a frame, and the earliest and
///     latest arrival of the frames relative to their OTA symbol, measured with the kernel receive timestamp ("wire")
///     and at processing time (as the reception window metrics did before the kernel timestamp was used).
///
/// The counters are process-wide and lock free (relaxed atomics); with several sectors they add up. The line is
/// written by whichever thread calls \ref report_if_due() first after a second has elapsed. Set
/// OCUDU_OFH_UL_RX_DIAG=0 to disable the collection and the report.
class ul_rx_diagnostics
{
public:
  /// Returns the process-wide instance.
  static ul_rx_diagnostics& get()
  {
    static ul_rx_diagnostics instance;
    return instance;
  }

  /// Returns true if the diagnostics are collected.
  bool enabled() const { return is_enabled; }

  /// Records a receive wake-up of the Ethernet receiver that drained \c nof_frames frames, \c poll_gap_ns after the
  /// previous one.
  void on_rx_wakeup(unsigned nof_frames, uint64_t poll_gap_ns)
  {
    if (!is_enabled) {
      return;
    }
    if (nof_frames != 0) {
      rx_wakeups.fetch_add(1, std::memory_order_relaxed);
      update_max(rx_max_burst, nof_frames);
    }
    update_max(rx_max_poll_gap_ns, poll_gap_ns);
  }

  /// Records a frame read from the socket \c kernel_to_read_ns after the kernel received it (negative or zero when the
  /// kernel timestamp is not available).
  void on_rx_frame(int64_t kernel_to_read_ns)
  {
    if (!is_enabled) {
      return;
    }
    rx_frames.fetch_add(1, std::memory_order_relaxed);
    if (kernel_to_read_ns > 0) {
      accumulate(kernel_to_read, static_cast<uint64_t>(kernel_to_read_ns));
    }
  }

  /// Records frames dropped by the kernel because the socket receive queue was full.
  void on_socket_drops(uint64_t nof_drops)
  {
    if (is_enabled && (nof_drops != 0)) {
      socket_drops.fetch_add(nof_drops, std::memory_order_relaxed);
    }
  }

  /// \brief Records the processing of a received frame by the message receiver.
  ///
  /// \param[in] read_to_process_ns       Time from the socket read to the processing (negative if unknown).
  /// \param[in] wire_symbol_offset       Arrival relative to the frame's OTA symbol at the kernel receive time, in
  ///                                     symbols.
  /// \param[in] processing_symbol_offset Arrival relative to the frame's OTA symbol at processing time, in symbols.
  void on_frame_processed(int64_t read_to_process_ns, int wire_symbol_offset, int processing_symbol_offset)
  {
    if (!is_enabled) {
      return;
    }
    if (read_to_process_ns > 0) {
      accumulate(read_to_process, static_cast<uint64_t>(read_to_process_ns));
    }
    update_max(wire_latest_symbol, wire_symbol_offset);
    update_min(wire_earliest_symbol, wire_symbol_offset);
    update_max(processing_latest_symbol, processing_symbol_offset);
  }

  /// Sets the OFDM symbol duration used to print the arrival offsets in microseconds.
  void set_symbol_duration(std::chrono::nanoseconds symbol_duration)
  {
    symbol_duration_ns.store(static_cast<uint64_t>(symbol_duration.count()), std::memory_order_relaxed);
  }

  /// \brief Writes the report line if at least one second elapsed since the previous one, and restarts the counters.
  ///
  /// \param[in] logger Destination logger (written at info level).
  void report_if_due(ocudulog::basic_logger& logger)
  {
    if (!is_enabled) {
      return;
    }
    const int64_t now =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    int64_t last = last_report.load(std::memory_order_relaxed);
    if (last == 0) {
      last_report.compare_exchange_strong(last, now, std::memory_order_relaxed);
      return;
    }
    if ((now - last < report_period.count()) ||
        !last_report.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
      return;
    }

    const double symbol_us   = to_us(symbol_duration_ns.load(std::memory_order_relaxed));
    const int    wire_min    = wire_earliest_symbol.exchange(no_min_symbol, std::memory_order_relaxed);
    const int    wire_max    = wire_latest_symbol.exchange(no_max_symbol, std::memory_order_relaxed);
    const int    process_max = processing_latest_symbol.exchange(no_max_symbol, std::memory_order_relaxed);
    const bool   any_frame   = (wire_max != no_max_symbol);

    logger.info("UL rx diagnostics: frames={} wakeups={} max_burst={} socket_drops={} max_poll_gap={:.1f}us "
                "kernel_to_read=[avg={:.1f}us max={:.1f}us] read_to_process=[avg={:.1f}us max={:.1f}us] "
                "wire_arrival=[{:.1f}us, {:.1f}us] processing_arrival_max={:.1f}us",
                rx_frames.exchange(0, std::memory_order_relaxed),
                rx_wakeups.exchange(0, std::memory_order_relaxed),
                rx_max_burst.exchange(0, std::memory_order_relaxed),
                socket_drops.exchange(0, std::memory_order_relaxed),
                to_us(rx_max_poll_gap_ns.exchange(0, std::memory_order_relaxed)),
                avg_us(kernel_to_read),
                max_us(kernel_to_read),
                avg_us(read_to_process),
                max_us(read_to_process),
                any_frame ? wire_min * symbol_us : 0.0,
                any_frame ? wire_max * symbol_us : 0.0,
                any_frame ? process_max * symbol_us : 0.0);
    reset(kernel_to_read);
    reset(read_to_process);
  }

private:
  /// Sum, count and maximum of a duration, in nanoseconds.
  struct duration_stats {
    std::atomic<uint64_t> sum_ns{0};
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> max_ns{0};
  };

  static constexpr std::chrono::nanoseconds report_period = std::chrono::seconds(1);
  static constexpr int                      no_min_symbol = std::numeric_limits<int>::max();
  static constexpr int                      no_max_symbol = std::numeric_limits<int>::min();

  ul_rx_diagnostics() : is_enabled(read_enabled()) {}

  static bool read_enabled()
  {
    const char* value = std::getenv("OCUDU_OFH_UL_RX_DIAG");
    return (value == nullptr) || (value[0] == '\0') || (std::strcmp(value, "0") != 0);
  }

  template <typename T>
  static void update_max(std::atomic<T>& max, T value)
  {
    T current = max.load(std::memory_order_relaxed);
    while ((value > current) && !max.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
  }

  template <typename T>
  static void update_min(std::atomic<T>& min, T value)
  {
    T current = min.load(std::memory_order_relaxed);
    while ((value < current) && !min.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
  }

  static void accumulate(duration_stats& stats, uint64_t value_ns)
  {
    stats.sum_ns.fetch_add(value_ns, std::memory_order_relaxed);
    stats.count.fetch_add(1, std::memory_order_relaxed);
    update_max(stats.max_ns, value_ns);
  }

  static void reset(duration_stats& stats)
  {
    stats.sum_ns.store(0, std::memory_order_relaxed);
    stats.count.store(0, std::memory_order_relaxed);
    stats.max_ns.store(0, std::memory_order_relaxed);
  }

  static double to_us(uint64_t value_ns) { return static_cast<double>(value_ns) / 1000.0; }

  static double avg_us(const duration_stats& stats)
  {
    const uint64_t count = stats.count.load(std::memory_order_relaxed);
    return (count != 0) ? to_us(stats.sum_ns.load(std::memory_order_relaxed)) / static_cast<double>(count) : 0.0;
  }

  static double max_us(const duration_stats& stats) { return to_us(stats.max_ns.load(std::memory_order_relaxed)); }

  const bool            is_enabled;
  std::atomic<int64_t>  last_report{0};
  std::atomic<uint64_t> symbol_duration_ns{0};
  std::atomic<uint64_t> rx_frames{0};
  std::atomic<uint64_t> rx_wakeups{0};
  std::atomic<unsigned> rx_max_burst{0};
  std::atomic<uint64_t> rx_max_poll_gap_ns{0};
  std::atomic<uint64_t> socket_drops{0};
  duration_stats        kernel_to_read;
  duration_stats        read_to_process;
  std::atomic<int>      wire_earliest_symbol{no_min_symbol};
  std::atomic<int>      wire_latest_symbol{no_max_symbol};
  std::atomic<int>      processing_latest_symbol{no_max_symbol};
};

} // namespace ofh
} // namespace ocudu
