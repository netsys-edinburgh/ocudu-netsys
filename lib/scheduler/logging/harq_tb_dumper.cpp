// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "harq_tb_dumper.h"
#include "ocudu/scheduler/harq_tb_dump_session.h"
#include "ocudu/support/error_handling.h"
#include "fmt/format.h"
#include <thread>
#include <zmq.h>

using namespace ocudu;

/// Dumper in use, or \c nullptr if the dump is disabled.
static std::atomic<harq_tb_dumper*> active_dumper{nullptr};

harq_tb_dump_session::harq_tb_dump_session(const std::string& address)
{
  if (address.empty()) {
    return;
  }

  dumper = std::make_unique<harq_tb_dumper>(address);
  dumper->start();
  active_dumper.store(dumper.get(), std::memory_order_release);
  fmt::println("HARQ TB dump: streaming to {}", address);
}

harq_tb_dump_session::~harq_tb_dump_session()
{
  // The member dumper is destroyed after this line, which sends the remaining records and closes the socket.
  active_dumper.store(nullptr, std::memory_order_release);
}

harq_tb_dumper* harq_tb_dumper::get() noexcept
{
  return active_dumper.load(std::memory_order_acquire);
}

harq_tb_dumper::harq_tb_dumper(const std::string& address) : queue(queue_capacity)
{
  zmq_context = ::zmq_ctx_new();
  report_fatal_error_if_not(
      zmq_context != nullptr, "HARQ dump: failed to create ZMQ context: {}", ::zmq_strerror(::zmq_errno()));

  zmq_push_socket = ::zmq_socket(zmq_context, ZMQ_PUSH);
  report_fatal_error_if_not(
      zmq_push_socket != nullptr, "HARQ dump: failed to create ZMQ socket: {}", ::zmq_strerror(::zmq_errno()));

  // The socket options must be set before binding.
  const int send_hwm = zmq_send_hwm;
  report_fatal_error_if_not(::zmq_setsockopt(zmq_push_socket, ZMQ_SNDHWM, &send_hwm, sizeof(send_hwm)) == 0,
                            "HARQ dump: failed to set ZMQ_SNDHWM: {}",
                            ::zmq_strerror(::zmq_errno()));

  const int linger_ms = static_cast<int>(zmq_linger_period.count());
  report_fatal_error_if_not(::zmq_setsockopt(zmq_push_socket, ZMQ_LINGER, &linger_ms, sizeof(linger_ms)) == 0,
                            "HARQ dump: failed to set ZMQ_LINGER: {}",
                            ::zmq_strerror(::zmq_errno()));

  report_fatal_error_if_not(::zmq_bind(zmq_push_socket, address.c_str()) == 0,
                            "HARQ dump: failed to bind ZMQ socket to '{}': {}",
                            address,
                            ::zmq_strerror(::zmq_errno()));
}

harq_tb_dumper::~harq_tb_dumper()
{
  // The background thread must finish first, because it is the one using the socket.
  stop();
  ::zmq_close(zmq_push_socket);
  ::zmq_ctx_term(zmq_context);
}

void harq_tb_dumper::start()
{
  // Do nothing if the background thread is already running.
  if (running.exchange(true)) {
    return;
  }
  worker = unique_thread("harq_tb_dump", [this]() { run(); });
}

void harq_tb_dumper::stop()
{
  // Do nothing if the background thread is not running.
  if (not running.exchange(false)) {
    return;
  }
  worker.join();
}

void harq_tb_dumper::push(const harq_tb_record& record) noexcept
{
  if (not queue.try_push(record)) {
    nof_dropped.fetch_add(1, std::memory_order_relaxed);
  }
}

void harq_tb_dumper::run()
{
  span<harq_tb_record> batch_view(batch);
  size_t               nof_records = 0;
  auto                 batch_start = std::chrono::steady_clock::now();

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

void harq_tb_dumper::send_batch(span<const harq_tb_record> records)
{
  harq_tb_batch_header header;
  header.nof_records       = static_cast<uint32_t>(records.size());
  header.seq               = next_seq;
  header.nof_dropped_total = nof_dropped.load(std::memory_order_relaxed);

  // The header and the records form one two-part message, which ZeroMQ delivers completely or not at all. With
  // ZMQ_DONTWAIT, a send fails immediately if there is no consumer or if its buffer is full.
  const bool sent =
      (::zmq_send(zmq_push_socket, &header, sizeof(header), ZMQ_DONTWAIT | ZMQ_SNDMORE) >= 0) and
      (::zmq_send(zmq_push_socket, records.data(), records.size() * sizeof(harq_tb_record), ZMQ_DONTWAIT) >= 0);

  // The sequence number only advances on success, so a gap at the consumer means a batch was lost after being sent.
  if (sent) {
    ++next_seq;
  } else {
    nof_dropped.fetch_add(records.size(), std::memory_order_relaxed);
  }
}
