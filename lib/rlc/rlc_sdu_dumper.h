// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief [netsys] Streaming of the per-SDU RLC AM reTx statistics over ZeroMQ, and its record format.
///
/// Each ZeroMQ message has two parts: a \ref stats_batch_header with \ref rlc_sdu_dump_magic and
/// \ref rlc_sdu_dump_version, followed by \c nof_records consecutive \ref rlc_sdu_record objects. All the fields are
/// little-endian. Any change to these types must be reflected in the consumer script, and any incompatible change must
/// also increase \ref rlc_sdu_dump_version.

#pragma once

#include "ocudu/ran/du_types.h"
#include "ocudu/ran/rb_id.h"
#include "ocudu/support/netsys_stats/binary_stats_dumper.h"
#include <chrono>
#include <cstdint>
#include <string>
#include <type_traits>

namespace ocudu {

/// [netsys] Magic number of the RLC SDU stream ("RLCS" in ASCII).
inline constexpr uint32_t rlc_sdu_dump_magic = 0x524c4353;
/// [netsys] Wire format version of \ref rlc_sdu_record.
inline constexpr uint16_t rlc_sdu_dump_version = 1;

/// [netsys] Value of \ref rlc_sdu_record::nof_retxs and \ref rlc_sdu_record::max_retx_thresh when they are unknown.
inline constexpr uint8_t rlc_sdu_unknown = 0xff;

/// [netsys] Direction of an RLC SDU. The values are part of the wire format.
enum class rlc_sdu_direction : uint8_t {
  /// DL: the gNB is the RLC transmitter.
  dl = 0,
  /// UL: the gNB is the RLC receiver.
  ul = 1
};

/// [netsys] Outcome of an RLC SDU. The values are part of the wire format.
enum class rlc_sdu_result : uint8_t {
  /// DL: the SDU was ACKed after at least one RLC reTx.
  ack = 0,
  /// DL: the SDU reached the maximum number of RLC reTxs.
  max_retx = 1,
  /// UL: the SDU was fully received after the gNB NACKed it.
  recovered = 2
};

/// [netsys] Type of radio bearer. The values are part of the wire format.
enum class rlc_sdu_rb_type : uint8_t { srb = 0, drb = 1 };

#pragma pack(push, 1)

/// [netsys] Outcome of one RLC AM SDU that needed a reTx.
struct rlc_sdu_record {
  /// UNIX time at which the outcome was determined, in nanoseconds.
  uint64_t wall_time_ns;
  /// \brief Measured time, in microseconds.
  ///
  /// DL: from the first transmission of the SDU to the STATUS report that ACKs it, or to the max reTx event.
  /// UL: from the generation of the first STATUS report that NACKs the SN to the full reception of the SDU.
  uint32_t t_us;
  /// RLC sequence number.
  uint32_t sn;
  /// SDU length, in bytes.
  uint32_t sdu_len;
  /// DU UE index. It matches \c harq_tb_record::ue_index.
  uint16_t ue_index;
  /// Type of radio bearer.
  rlc_sdu_rb_type rb_type;
  /// SRB or DRB identifier, e.g. 1 for DRB1.
  uint8_t rb_id;
  /// Direction of the SDU.
  rlc_sdu_direction dir;
  /// Outcome of the SDU.
  rlc_sdu_result result;
  /// DL: number of reTxs, excluding the first transmission. UL: \ref rlc_sdu_unknown.
  uint8_t nof_retxs;
  /// DL: maximum reTx threshold of the bearer. UL: \ref rlc_sdu_unknown.
  uint8_t max_retx_thresh;
};

#pragma pack(pop)

static_assert(sizeof(rlc_sdu_record) == 28, "Unexpected rlc_sdu_record size. Update the consumer script.");
static_assert(std::is_trivially_copyable_v<rlc_sdu_record>, "rlc_sdu_record must be sent as raw bytes.");

/// \brief [netsys] Creates a record with the fields that are common to all the SDUs of a bearer and direction.
///
/// It also sets the wall time to now, and \c nof_retxs and \c max_retx_thresh to \ref rlc_sdu_unknown.
inline rlc_sdu_record make_rlc_sdu_record(du_ue_index_t ue_index, rb_id_t rb_id, rlc_sdu_direction dir)
{
  rlc_sdu_record record{};
  record.wall_time_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count());
  record.ue_index = static_cast<uint16_t>(ue_index);
  if (rb_id.is_srb()) {
    record.rb_type = rlc_sdu_rb_type::srb;
    record.rb_id   = srb_id_to_uint(rb_id.get_srb_id());
  } else {
    record.rb_type = rlc_sdu_rb_type::drb;
    record.rb_id   = static_cast<uint8_t>(rb_id.get_drb_id());
  }
  record.dir             = dir;
  record.nof_retxs       = rlc_sdu_unknown;
  record.max_retx_thresh = rlc_sdu_unknown;
  return record;
}

/// \brief [netsys] Streams the per-SDU RLC AM reTx statistics to an external consumer.
///
/// The RLC entities only copy a record into a lock-free queue, see \ref binary_stats_dumper.
class rlc_sdu_dumper : public binary_stats_dumper<rlc_sdu_record>
{
public:
  /// \brief Creates the ZeroMQ socket of the stream and binds it to the given address.
  ///
  /// \param[in] address ZeroMQ address to bind, e.g. "tcp://127.0.0.1:5556".
  explicit rlc_sdu_dumper(const std::string& address) :
    binary_stats_dumper("rlc_sdu_dump", address, rlc_sdu_dump_magic, rlc_sdu_dump_version)
  {
  }

  /// \brief Gets the dumper in use. It can be called from any thread.
  ///
  /// \return A pointer to the dumper owned by the active RLC SDU dump session, or \c nullptr if the dump is disabled.
  static rlc_sdu_dumper* get() noexcept;
};

} // namespace ocudu
