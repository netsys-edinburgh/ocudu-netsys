// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief [netsys] Streaming of the per-TB HARQ statistics over ZeroMQ, and its record format.
///
/// Each ZeroMQ message has two parts: a \ref stats_batch_header with \ref harq_tb_dump_magic and
/// \ref harq_tb_dump_version, followed by \c nof_records consecutive \ref harq_tb_record objects. All the fields are
/// little-endian. Any change to these types must be reflected in the consumer script, and any incompatible change must
/// also increase \ref harq_tb_dump_version.

#pragma once

#include "ocudu/support/netsys_stats/binary_stats_dumper.h"
#include <cstdint>
#include <string>
#include <type_traits>

namespace ocudu {

/// [netsys] Magic number of the HARQ TB stream ("HARQ" in ASCII).
inline constexpr uint32_t harq_tb_dump_magic = 0x48415251;
/// [netsys] Wire format version of \ref harq_tb_record.
inline constexpr uint16_t harq_tb_dump_version = 1;

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

#pragma pack(pop)

static_assert(sizeof(harq_tb_record) == 34, "Unexpected harq_tb_record size. Update the consumer script.");
static_assert(std::is_trivially_copyable_v<harq_tb_record>, "harq_tb_record must be sent as raw bytes.");

/// \brief [netsys] Streams the per-TB HARQ statistics to an external consumer.
///
/// The real-time scheduler threads only copy a record into a lock-free queue, see \ref binary_stats_dumper.
class harq_tb_dumper : public binary_stats_dumper<harq_tb_record>
{
public:
  /// \brief Creates the ZeroMQ socket of the stream and binds it to the given address.
  ///
  /// \param[in] address ZeroMQ address to bind, e.g. "tcp://127.0.0.1:5555".
  explicit harq_tb_dumper(const std::string& address) :
    binary_stats_dumper("harq_tb_dump", address, harq_tb_dump_magic, harq_tb_dump_version)
  {
  }

  /// \brief Gets the dumper in use. It can be called from any thread.
  ///
  /// \return A pointer to the dumper owned by the active \ref harq_tb_dump_session, or \c nullptr if the dump is
  /// disabled.
  static harq_tb_dumper* get() noexcept;
};

} // namespace ocudu
