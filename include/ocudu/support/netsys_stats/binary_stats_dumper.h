// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief [netsys] Streaming of fixed-size statistics records to an external consumer over ZeroMQ.

#pragma once

#include "ocudu/adt/mpmc_queue.h"
#include "ocudu/adt/span.h"
#include "ocudu/support/executors/unique_thread.h"
#include "ocudu/support/netsys_stats/stats_zmq_sender.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <type_traits>

namespace ocudu {

/// \brief [netsys] Streams fixed-size statistics records to an external consumer.
///
/// The real-time threads only copy a record into a lock-free queue. A background thread groups the queued records
/// into batches and sends them with a \ref stats_zmq_sender.
///
/// \tparam Record Record type. It is sent as raw bytes, so it must be trivially copyable and have a fixed layout.
template <typename Record>
class binary_stats_dumper
{
  static_assert(std::is_trivially_copyable_v<Record>, "Records are sent as raw bytes.");
  static_assert(sizeof(Record) <= UINT16_MAX, "The record size must fit in the batch header.");

public:
  /// \brief Creates the ZeroMQ socket of the stream. See \ref stats_zmq_sender for the threading constraints.
  ///
  /// \param[in] name_   Name of the stream and of its background thread (at most 15 characters).
  /// \param[in] address ZeroMQ address to bind.
  /// \param[in] magic   Magic number of the record type.
  /// \param[in] version Wire format version of the record type.
  binary_stats_dumper(std::string name_, const std::string& address, uint32_t magic, uint16_t version) :
    name(std::move(name_)),
    sender(name, address, magic, version, static_cast<uint16_t>(sizeof(Record))),
    queue(queue_capacity)
  {
  }

  /// Stops the background thread. The socket is closed afterwards, when \c sender is destroyed.
  ~binary_stats_dumper() { stop(); }

  /// \brief Starts the background thread.
  ///
  /// It must be called before the real-time threads are created, because the new thread inherits the scheduling
  /// priority and CPU affinity of the calling thread.
  void start()
  {
    // Do nothing if the background thread is already running.
    if (running.exchange(true)) {
      return;
    }
    worker = unique_thread(name, [this]() { run(); });
  }

  /// Stops the background thread, after sending the records that are still in the queue.
  void stop()
  {
    // Do nothing if the background thread is not running.
    if (not running.exchange(false)) {
      return;
    }
    worker.join();
  }

  /// \brief Queues one record for transmission.
  ///
  /// It can be called concurrently from several threads. It never blocks and never allocates memory. If the queue is
  /// full, the record is dropped and counted.
  void push(const Record& record) noexcept
  {
    if (not queue.try_push(record)) {
      nof_dropped.fetch_add(1, std::memory_order_relaxed);
    }
  }

private:
  /// Maximum number of records waiting to be sent.
  static constexpr size_t queue_capacity = 16384;
  /// Maximum number of records in one batch.
  static constexpr size_t max_batch_size = 512;
  /// Maximum time that a record waits in an incomplete batch before being sent.
  static constexpr std::chrono::milliseconds flush_period{100};
  /// Sleep time of the background thread while the current batch is not ready to be sent.
  static constexpr std::chrono::milliseconds poll_period{10};

  using record_queue =
      concurrent_queue<Record, concurrent_queue_policy::lockfree_mpmc, concurrent_queue_wait_policy::non_blocking>;

  /// Body of the background thread.
  void run()
  {
    span<Record> batch_view(batch);
    size_t       nof_records = 0;
    auto         batch_start = std::chrono::steady_clock::now();

    while (running.load()) {
      const auto now = std::chrono::steady_clock::now();

      // The waiting time of a batch starts when its first record can arrive.
      if (nof_records == 0) {
        batch_start = now;
      }

      // Move the queued records to the free part of the batch.
      nof_records += queue.try_pop_bulk(batch_view.subspan(nof_records, batch_view.size() - nof_records));

      const bool is_full = nof_records == batch_view.size();
      const bool is_due  = (nof_records != 0) and (now - batch_start >= flush_period);
      if (is_full or is_due) {
        send_batch(batch_view.first(nof_records));
        nof_records = 0;
      } else {
        std::this_thread::sleep_for(poll_period);
      }
    }

    // Send the records that are still in the batch or in the queue.
    while (true) {
      nof_records += queue.try_pop_bulk(batch_view.subspan(nof_records, batch_view.size() - nof_records));
      if (nof_records == 0) {
        break;
      }
      send_batch(batch_view.first(nof_records));
      nof_records = 0;
    }
  }

  /// Sends one batch of records. Only called from the background thread.
  void send_batch(span<const Record> records)
  {
    span<const uint8_t> payload(reinterpret_cast<const uint8_t*>(records.data()), records.size() * sizeof(Record));
    if (not sender.send(
            payload, static_cast<uint32_t>(records.size()), nof_dropped.load(std::memory_order_relaxed))) {
      nof_dropped.fetch_add(records.size(), std::memory_order_relaxed);
    }
  }

  /// Name of the stream and of the background thread.
  std::string name;
  /// ZeroMQ sender. While the background thread is running, only that thread uses it.
  stats_zmq_sender sender;
  /// Records waiting to be sent.
  record_queue queue;
  /// Total number of records dropped since the start.
  std::atomic<uint64_t> nof_dropped{0};
  /// Batch being assembled. Only accessed from the background thread.
  std::array<Record, max_batch_size> batch{};
  /// True while the background thread must keep running.
  std::atomic<bool> running{false};
  /// Background thread.
  unique_thread worker;
};

} // namespace ocudu
