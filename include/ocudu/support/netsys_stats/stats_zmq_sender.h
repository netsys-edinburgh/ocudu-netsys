// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief [netsys] ZeroMQ sender of batches of fixed-size statistics records, and the batch header wire format.
///
/// Each ZeroMQ message has two parts: a \ref stats_batch_header followed by \c nof_records consecutive records of
/// \c record_size bytes. All the fields are little-endian. The \c magic and \c version fields identify the record type
/// and its layout, so any incompatible change to a record type must increase its version.

#pragma once

#include "ocudu/adt/span.h"
#include <chrono>
#include <cstdint>
#include <string>
#include <type_traits>

namespace ocudu {

#pragma pack(push, 1)

/// [netsys] Header of one batch of records, sent as the first part of each ZeroMQ message.
struct stats_batch_header {
  /// Magic number identifying the record type.
  uint32_t magic;
  /// Wire format version of the record type.
  uint16_t version;
  /// Size of each record, in bytes.
  uint16_t record_size;
  /// Number of records in this batch.
  uint32_t nof_records;
  /// Batch sequence number, starting at zero. A gap means that batches were lost after being sent.
  uint64_t seq;
  /// Total number of records dropped by the gNB since it started.
  uint64_t nof_dropped_total;
};

#pragma pack(pop)

static_assert(sizeof(stats_batch_header) == 28, "Unexpected stats_batch_header size. Update the consumer script.");
static_assert(std::is_trivially_copyable_v<stats_batch_header>, "stats_batch_header must be sent as raw bytes.");

/// \brief [netsys] Sends batches of records through a ZeroMQ PUSH socket, without ever blocking on the consumer.
///
/// The socket is not thread-safe: after construction, only one thread must call \ref send.
class stats_zmq_sender
{
public:
  /// \brief Creates the ZeroMQ PUSH socket and binds it to the given address.
  ///
  /// It must be called before the real-time threads are created, because the internal ZeroMQ threads inherit the
  /// scheduling priority and CPU affinity of the calling thread. A failure to bind is a fatal error.
  ///
  /// \param[in] name_        Name of the stream, used in the error messages.
  /// \param[in] address      ZeroMQ address to bind, e.g. "tcp://127.0.0.1:5555".
  /// \param[in] magic_       Magic number of the record type.
  /// \param[in] version_     Wire format version of the record type.
  /// \param[in] record_size_ Size of each record, in bytes.
  stats_zmq_sender(std::string        name_,
                   const std::string& address,
                   uint32_t           magic_,
                   uint16_t           version_,
                   uint16_t           record_size_);

  /// Closes the ZeroMQ socket, after trying to deliver the pending batches for a limited time.
  ~stats_zmq_sender();

  stats_zmq_sender(const stats_zmq_sender&)            = delete;
  stats_zmq_sender& operator=(const stats_zmq_sender&) = delete;

  /// \brief Sends one batch of records.
  ///
  /// \param[in] payload           Records of the batch, back to back.
  /// \param[in] nof_records       Number of records in \c payload.
  /// \param[in] nof_dropped_total Total number of records dropped so far, reported in the batch header.
  /// \return True if the batch was sent, false if there is no consumer or if its buffer is full. The sequence number
  /// only advances on success.
  bool send(span<const uint8_t> payload, uint32_t nof_records, uint64_t nof_dropped_total);

private:
  /// Maximum number of batches that ZeroMQ buffers for a slow consumer.
  static constexpr int send_hwm = 256;
  /// Maximum time that ZeroMQ keeps trying to deliver the pending batches after the socket is closed.
  static constexpr std::chrono::milliseconds linger_period{1000};

  /// Name of the stream.
  std::string name;
  /// Magic number, version and record size written in every batch header.
  uint32_t magic;
  uint16_t version;
  uint16_t record_size;
  /// Sequence number of the next batch.
  uint64_t next_seq = 0;
  /// ZeroMQ context.
  void* zmq_context = nullptr;
  /// ZeroMQ PUSH socket.
  void* zmq_push_socket = nullptr;
};

} // namespace ocudu
