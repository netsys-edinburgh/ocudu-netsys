// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/support/netsys_stats/stats_zmq_sender.h"
#include "ocudu/support/error_handling.h"
#include <zmq.h>

using namespace ocudu;

stats_zmq_sender::stats_zmq_sender(std::string        name_,
                                   const std::string& address,
                                   uint32_t           magic_,
                                   uint16_t           version_,
                                   uint16_t           record_size_) :
  name(std::move(name_)), magic(magic_), version(version_), record_size(record_size_)
{
  zmq_context = ::zmq_ctx_new();
  report_fatal_error_if_not(
      zmq_context != nullptr, "{}: failed to create ZMQ context: {}", name, ::zmq_strerror(::zmq_errno()));

  zmq_push_socket = ::zmq_socket(zmq_context, ZMQ_PUSH);
  report_fatal_error_if_not(
      zmq_push_socket != nullptr, "{}: failed to create ZMQ socket: {}", name, ::zmq_strerror(::zmq_errno()));

  // The socket options must be set before binding.
  const int hwm = send_hwm;
  report_fatal_error_if_not(::zmq_setsockopt(zmq_push_socket, ZMQ_SNDHWM, &hwm, sizeof(hwm)) == 0,
                            "{}: failed to set ZMQ_SNDHWM: {}",
                            name,
                            ::zmq_strerror(::zmq_errno()));

  const int linger_ms = static_cast<int>(linger_period.count());
  report_fatal_error_if_not(::zmq_setsockopt(zmq_push_socket, ZMQ_LINGER, &linger_ms, sizeof(linger_ms)) == 0,
                            "{}: failed to set ZMQ_LINGER: {}",
                            name,
                            ::zmq_strerror(::zmq_errno()));

  report_fatal_error_if_not(::zmq_bind(zmq_push_socket, address.c_str()) == 0,
                            "{}: failed to bind ZMQ socket to '{}': {}",
                            name,
                            address,
                            ::zmq_strerror(::zmq_errno()));
}

stats_zmq_sender::~stats_zmq_sender()
{
  ::zmq_close(zmq_push_socket);
  ::zmq_ctx_term(zmq_context);
}

bool stats_zmq_sender::send(span<const uint8_t> payload, uint32_t nof_records, uint64_t nof_dropped_total)
{
  stats_batch_header header;
  header.magic             = magic;
  header.version           = version;
  header.record_size       = record_size;
  header.nof_records       = nof_records;
  header.seq               = next_seq;
  header.nof_dropped_total = nof_dropped_total;

  // The header and the records form one two-part message, which ZeroMQ delivers completely or not at all. With
  // ZMQ_DONTWAIT, a send fails immediately if there is no consumer or if its buffer is full.
  const bool sent = (::zmq_send(zmq_push_socket, &header, sizeof(header), ZMQ_DONTWAIT | ZMQ_SNDMORE) >= 0) and
                    (::zmq_send(zmq_push_socket, payload.data(), payload.size(), ZMQ_DONTWAIT) >= 0);

  // The sequence number only advances on success, so a gap at the consumer means a batch was lost after being sent.
  if (sent) {
    ++next_seq;
  }
  return sent;
}
