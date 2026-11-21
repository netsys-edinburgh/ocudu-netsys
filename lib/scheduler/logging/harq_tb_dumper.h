// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief [netsys] Streaming of the per-TB HARQ statistics over ZeroMQ, and its wire format.
///
/// Each ZeroMQ message has two parts: a \ref harq_tb_batch_header followed by \c nof_records consecutive
/// \ref harq_tb_record objects. All the fields are little-endian. Any change to these types must be reflected in the
/// consumer script, and any incompatible change must also increase \ref harq_tb_batch_header::version.

#pragma once

#include "ocudu/adt/mpmc_queue.h"
#include "ocudu/adt/span.h"
#include "ocudu/support/executors/unique_thread.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

namespace ocudu {

/// [netsys] Direction of a transport block. The values are part of the wire format.
enum class harq_tb_direction : uint8_t { dl = 0, ul = 1 };

/// [netsys] Final outcome of a transport block. The values are part of the wire format.
enum class harq_tb_result : uint8_t {
  /// ACK or CRC OK received after at least one reTx.
  ack = 0,
  /// NACK received after the last allowed reTx.
  max_retx = 1,
  /// ReTxs cancelled by the scheduler.
  cancelled = 2,
  /// The scheduler did not schedule the pending reTx in time.
  retx_timeout = 3,
  /// The HARQ-ACK or CRC indication never arrived.
  feedback_timeout = 4,
  /// HARQ process reset after at least one reTx, e.g. because the reTx grant could not be allocated.
  reset = 5
};

#pragma pack(push, 1)

/// [netsys] Final state of one transport block that needed at least one reTx or was dropped.
struct harq_tb_record {
  /// UNIX time at which the outcome was determined, in nanoseconds.
  uint64_t wall_time_ns;
  /// Slot of the first transmission, as given by \c slot_point::count().
  uint32_t first_tx_count;
  /// Slot of the last HARQ-ACK or CRC, as given by \c slot_point::count().
  uint32_t ack_count;
  /// Transport block size, in bytes.
  uint32_t tbs_bytes;
  /// Time from the first transmission to the slot of the last HARQ-ACK or CRC, in microseconds.
  uint32_t t_ack_us;
  /// RNTI of the UE.
  uint16_t rnti;
  /// DU UE index.
  uint16_t ue_index;
  /// HARQ process identifier.
  uint8_t h_id;
  /// Direction of the transport block.
  harq_tb_direction dir;
  /// Final outcome of the transport block.
  harq_tb_result result;
  /// Number of reTxs, excluding the first transmission.
  uint8_t nof_retxs;
  /// Maximum number of reTxs allowed for this transport block.
  uint8_t max_retxs;
  /// Numerology of the slots, used to interpret \c first_tx_count and \c ack_count.
  uint8_t numerology;
};

/// [netsys] Header of one batch of records, sent as the first part of each ZeroMQ message.
struct harq_tb_batch_header {
  /// Magic number identifying this message type ("HARQ" in ASCII).
  uint32_t magic = 0x48415251;
  /// Wire format version.
  uint16_t version = 1;
  /// Size of each record, in bytes.
  uint16_t record_size = sizeof(harq_tb_record);
  /// Number of records in this batch.
  uint32_t nof_records = 0;
  /// Batch sequence number, starting at zero. A gap means that batches were lost.
  uint64_t seq = 0;
  /// Total number of records dropped by the gNB since it started.
  uint64_t nof_dropped_total = 0;
};

#pragma pack(pop)

static_assert(sizeof(harq_tb_record) == 34, "Unexpected harq_tb_record size. Update the consumer script.");
static_assert(sizeof(harq_tb_batch_header) == 28, "Unexpected harq_tb_batch_header size. Update the consumer script.");
static_assert(std::is_trivially_copyable_v<harq_tb_record>, "harq_tb_record must be sent as raw bytes.");
static_assert(std::is_trivially_copyable_v<harq_tb_batch_header>, "harq_tb_batch_header must be sent as raw bytes.");

/// \brief [netsys] Streams the per-TB HARQ statistics to an external consumer.
///
/// The real-time scheduler threads only copy a fixed-size record into a lock-free queue. A background thread groups the
/// queued records into batches and sends them through a ZeroMQ PUSH socket without ever blocking on the consumer.
class harq_tb_dumper
{
public:
  /// \brief Creates the ZeroMQ PUSH socket and binds it to the given address.
  ///
  /// It must be called before the real-time threads are created, because the internal ZeroMQ threads inherit the
  /// scheduling priority and CPU affinity of the calling thread. A failure to bind is a fatal error.
  ///
  /// \param[in] address ZeroMQ address to bind, e.g. "tcp://127.0.0.1:5557".
  explicit harq_tb_dumper(const std::string& address);

  /// Stops the background thread and closes the ZeroMQ socket.
  ~harq_tb_dumper();

  /// \brief Gets the dumper in use. It can be called from any thread.
  ///
  /// \return A pointer to the dumper owned by the active \ref harq_tb_dump_session, or \c nullptr if the dump is
  /// disabled.
  static harq_tb_dumper* get() noexcept;

  /// \brief Starts the background thread.
  ///
  /// It must be called before the real-time threads are created, because the new thread inherits the scheduling
  /// priority and CPU affinity of the calling thread.
  void start();

  /// Stops the background thread, after sending the records that are still in the queue.
  void stop();

  /// \brief Queues one record for transmission.
  ///
  /// It can be called concurrently from several threads. It never blocks and never allocates memory. If the queue is
  /// full, the record is dropped and counted in \ref nof_dropped.
  ///
  /// \param[in] record Record to queue.
  void push(const harq_tb_record& record) noexcept;

private:
  /// Maximum number of records waiting to be sent.
  static constexpr size_t queue_capacity = 16384;
  /// Maximum number of records in one batch.
  static constexpr size_t max_batch_size = 512;
  /// Maximum time that a record waits in an incomplete batch before being sent.
  static constexpr std::chrono::milliseconds flush_period{100};
  /// Sleep time of the background thread while the current batch is not ready to be sent.
  static constexpr std::chrono::milliseconds poll_period{10};
  /// Maximum number of batches that ZeroMQ buffers for a slow consumer.
  static constexpr int zmq_send_hwm = 256;
  /// Maximum time that ZeroMQ keeps trying to deliver the pending batches after the socket is closed.
  static constexpr std::chrono::milliseconds zmq_linger_period{1000};

  using record_queue = concurrent_queue<harq_tb_record,
                                        concurrent_queue_policy::lockfree_mpmc,
                                        concurrent_queue_wait_policy::non_blocking>;

  /// Body of the background thread.
  void run();

  /// Sends one batch of records. Only called from the background thread.
  void send_batch(span<const harq_tb_record> records);

  /// Records waiting to be sent.
  record_queue queue;
  /// Total number of records dropped since the start.
  std::atomic<uint64_t> nof_dropped{0};
  /// Batch being assembled. Only accessed from the background thread.
  std::array<harq_tb_record, max_batch_size> batch{};
  /// Sequence number of the next batch. Only accessed from the background thread.
  uint64_t next_seq = 0;
  /// ZeroMQ context.
  void* zmq_context = nullptr;
  /// ZeroMQ PUSH socket. While the background thread is running, only that thread uses it.
  void* zmq_push_socket = nullptr;
  /// True while the background thread must keep running.
  std::atomic<bool> running{false};
  /// Background thread.
  unique_thread worker;
};

} // namespace ocudu
