// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief [netsys] PUSCH bit error tracker declaration.

#pragma once

#include "ocudu/adt/bit_buffer.h"
#include "ocudu/adt/span.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/phy/upper/channel_coding/ldpc/ldpc_encoder.h"
#include "ocudu/phy/upper/channel_coding/ldpc/ldpc_rate_matcher.h"
#include "ocudu/phy/upper/codeblock_metadata.h"
#include "ocudu/phy/upper/trx_buffer_identifier.h"
#include "ocudu/ran/harq_id.h"
#include "ocudu/ran/rnti.h"
#include "ocudu/ran/slot_point.h"
#include "ocudu/support/memory_pool/bounded_object_pool.h"
#include "ocudu/support/units.h"
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace ocudu {

class ldpc_encoder_factory;
class ldpc_rate_matcher_factory;
class rx_buffer;

/// \brief [netsys] Counts the bit errors of PUSCH transmissions that failed the transport block CRC.
///
/// When a transport block fails the CRC, the tracker stores a snapshot of the transmission. When a retransmission of
/// the same transport block, identified by its RNTI and HARQ process, passes the CRC, the decoded codeblocks become the
/// ground truth. The tracker then counts, for every stored failed transmission:
/// - after decoding: the wrong and right bits of the decoded message of each codeblock (data and CRC bits, without
///   filler bits); and
/// - before decoding: the wrong and right hard decisions of the LLRs delivered to the decoder. The ground truth is LDPC
///   encoded and rate matched again with the parameters of the failed transmission (redundancy version, rate-matched
///   length and modulation) to obtain the transmitted bits. LLRs equal to zero carry no information and are counted
///   separately.
///
/// The counts are logged in one line per failed transmission. Transport blocks that are never recovered are logged in
/// a separate line, without bit counts. A transport block is only tracked if its first transmission was stored. All
/// memory is allocated at construction.
///
/// A single instance is shared by all the PUSCH decoders created by the same factory. Calls for different HARQ
/// processes can be concurrent. Calls for the same HARQ process are serialized by the receive buffer.
class pusch_bit_error_tracker
{
public:
  /// Maximum number of failed transmissions stored for each transport block.
  static constexpr unsigned max_nof_stored_tx = 4;
  /// Default number of HARQ processes that can be tracked at the same time.
  static constexpr unsigned default_nof_entries = 64;

  /// Parameters of the PUSCH transmission being reported.
  struct tx_context {
    /// RNTI of the transmission.
    rnti_t rnti;
    /// HARQ process identifier.
    harq_id_t harq_id;
    /// Slot of the transmission.
    slot_point slot;
    /// Redundancy version.
    unsigned rv;
    /// Set to true for the first transmission of a transport block.
    bool new_data;
    /// Transport block size.
    units::bytes tbs;
  };

  /// \brief Creates a tracker and allocates all its memory.
  /// \param[in] max_codeword_size    Maximum number of UL-SCH codeword bits of a PUSCH transmission.
  /// \param[in] encoder_factory      LDPC encoder factory, used to regenerate the transmitted codeblocks.
  /// \param[in] rate_matcher_factory LDPC rate matcher factory, used to regenerate the transmitted codeblocks.
  /// \param[in] nof_reencoders       Number of encoder and rate matcher pairs that can be used concurrently.
  /// \param[in] nof_entries          Number of HARQ processes that can be tracked at the same time.
  pusch_bit_error_tracker(units::bits                max_codeword_size,
                          ldpc_encoder_factory&      encoder_factory,
                          ldpc_rate_matcher_factory& rate_matcher_factory,
                          unsigned                   nof_reencoders,
                          unsigned                   nof_entries = default_nof_entries);

  /// \brief Stores a snapshot of a transmission whose transport block failed the CRC.
  ///
  /// \param[in] ctx             Transmission context.
  /// \param[in] codeblocks      Rate-matched LLRs and metadata of each codeblock of this transmission.
  /// \param[in] hard_bits_valid Indicates, for each codeblock, whether the data bits in the receive buffer were written by
  ///                            the decoder for this transport block.
  /// \param[in] buffer          Receive buffer holding the decoded data bits of each codeblock.
  void on_crc_ko(const tx_context&                  ctx,
                 span<const described_rx_codeblock> codeblocks,
                 span<const bool>                   hard_bits_valid,
                 rx_buffer&                         buffer);

  /// \brief Counts and logs the bit errors of the stored failed transmissions of a transport block that passed the CRC.
  ///
  /// \param[in] ctx        Transmission context.
  /// \param[in] codeblocks Rate-matched LLRs and metadata of each codeblock of this transmission.
  /// \param[in] buffer     Receive buffer holding the correctly decoded data bits of each codeblock.
  void on_crc_ok(const tx_context& ctx, span<const described_rx_codeblock> codeblocks, rx_buffer& buffer);

private:
  /// Snapshot of one codeblock of a failed transmission.
  struct codeblock_snapshot {
    /// Codeblock metadata of the failed transmission.
    codeblock_metadata metadata;
    /// Number of decoded message bits, that is, data and CRC bits without filler bits.
    unsigned nof_msg_bits = 0;
    /// Offset of the decoded message bits within \ref tx_snapshot::msg_bits. It is a multiple of 8.
    unsigned msg_offset = 0;
    /// True if the decoded message bits were written by the decoder for this transport block.
    bool msg_valid = false;
    /// Number of LLRs of the codeblock in this transmission, that is, its rate-matched length.
    unsigned nof_llrs = 0;
    /// Offset of the LLR bits within \ref tx_snapshot::llr_hard_bits and \ref tx_snapshot::llr_nonzero. It is a
    /// multiple of 8.
    unsigned llr_offset = 0;
    /// Number of LLRs equal to zero.
    unsigned nof_zero_llrs = 0;
  };

  /// Snapshot of one failed transmission.
  struct tx_snapshot {
    /// Index of the transmission within the transport block, starting from zero for the first transmission.
    unsigned tx_idx = 0;
    /// Slot of the transmission.
    slot_point slot;
    /// Redundancy version of the transmission.
    unsigned rv = 0;
    /// Codeblock snapshots. Their memory is reserved at construction.
    std::vector<codeblock_snapshot> codeblocks;
    /// Decoded message bits of all codeblocks. Each codeblock starts at a byte boundary.
    dynamic_bit_buffer msg_bits;
    /// Hard decisions of the LLRs of all codeblocks, one for LLRs lower than or equal to zero. Each codeblock starts at
    /// a byte boundary.
    dynamic_bit_buffer llr_hard_bits;
    /// One bit per LLR, set to one if the LLR is not zero. It has the same layout as \ref llr_hard_bits.
    dynamic_bit_buffer llr_nonzero;
  };

  /// Tracking state of one HARQ process.
  struct entry {
    /// HARQ process identifier. Invalid if the entry is free. Protected by the mutex.
    trx_buffer_identifier id = trx_buffer_identifier::invalid();
    /// True while a caller owns the entry. Protected by the mutex.
    bool in_use = false;
    /// Value of the use counter the last time the entry was released. Protected by the mutex.
    uint64_t last_use = 0;

    // The following fields are only accessed by the caller that owns the entry.

    /// RNTI of the transport block.
    rnti_t rnti = rnti_t::INVALID_RNTI;
    /// HARQ process identifier of the transport block.
    harq_id_t harq_id = INVALID_HARQ_ID;
    /// Transport block size.
    units::bytes tbs;
    /// Number of codeblocks of the transport block.
    unsigned nof_cbs = 0;
    /// Number of failed transmissions, including the ones that were not stored.
    unsigned nof_failed_tx = 0;
    /// Slot of the first transmission.
    slot_point first_slot;
    /// Slot of the last failed transmission.
    slot_point last_slot;
    /// Number of valid transmissions in \ref stored_tx.
    unsigned nof_stored_tx = 0;
    /// Stored failed transmissions.
    std::array<tx_snapshot, max_nof_stored_tx> stored_tx;
  };

  /// LDPC encoder and rate matcher that regenerate the transmitted codeblocks from the ground truth.
  struct reencoder {
    /// LDPC encoder.
    std::unique_ptr<ldpc_encoder> encoder;
    /// LDPC rate matcher.
    std::unique_ptr<ldpc_rate_matcher> rate_matcher;
  };

  /// Bit counts of one failed transmission.
  struct bit_counts {
    /// Number of decoded message bits compared.
    unsigned post_bits = 0;
    /// Number of wrong decoded message bits.
    unsigned post_err = 0;
    /// Number of codeblocks excluded from the decoded message counts.
    unsigned post_skipped_cbs = 0;
    /// Number of LLRs.
    unsigned pre_bits = 0;
    /// Number of LLRs with a wrong hard decision.
    unsigned pre_err = 0;
    /// Number of LLRs equal to zero.
    unsigned pre_zero = 0;
  };

  /// \brief Gets exclusive access to the entry of a HARQ process.
  ///
  /// \param[in] id       HARQ process identifier.
  /// \param[in] allocate Set to true to allocate a new entry if none exists for \c id. If the tracker is full, the least
  ///                     recently used entry is evicted.
  /// \return A pointer to the entry, or \c nullptr if there is no entry available.
  entry* acquire(trx_buffer_identifier id, bool allocate);

  /// \brief Gives back the exclusive access to an entry.
  /// \param[in] e          Entry obtained with \ref acquire.
  /// \param[in] free_entry Set to true to free the entry, so it can be used by another HARQ process.
  void release(entry& e, bool free_entry);

  /// Initializes an entry for a new transport block.
  static void start_tb(entry& e, const tx_context& ctx, unsigned nof_cbs);

  /// Returns true if the transmission belongs to the transport block tracked by the entry.
  static bool is_same_tb(const entry& e, const tx_context& ctx, unsigned nof_cbs);

  /// Stores the snapshot of a failed transmission in an entry.
  void store_tx(entry&                             e,
                const tx_context&                  ctx,
                span<const described_rx_codeblock> codeblocks,
                span<const bool>                   hard_bits_valid,
                rx_buffer&                         buffer);

  /// Selects a re-encoder and reports the bit errors of all the stored failed transmissions of an entry.
  void report_recovered(const entry&                       e,
                        const tx_context&                  ctx,
                        span<const described_rx_codeblock> codeblocks,
                        rx_buffer&                         buffer);

  /// Counts the bit errors of all the stored failed transmissions of an entry and logs them.
  void count_and_report(const entry&                       e,
                        const tx_context&                  ctx,
                        span<const described_rx_codeblock> codeblocks,
                        rx_buffer&                         buffer,
                        reencoder&                         re);

  /// Logs a transport block that was never recovered.
  void report_lost(const entry& e, const char* cause);

  /// Logger for the bit error reports.
  ocudulog::basic_logger& logger;
  /// Maximum number of codeblocks that can be stored for each transmission.
  unsigned max_nof_cbs;
  /// Protects the entry identifiers, the ownership flags and the use counter.
  std::mutex mutex;
  /// Counter used to find the least recently used entry.
  uint64_t use_counter = 0;
  /// Tracking entries. The vector is never resized after construction.
  std::vector<entry> entries;
  /// Re-encoders for concurrent reports.
  std::unique_ptr<bounded_unique_object_pool<reencoder>> reencoder_pool;
  /// Protects the spare re-encoder.
  std::mutex spare_reencoder_mutex;
  /// Re-encoder used when the pool is empty.
  std::unique_ptr<reencoder> spare_reencoder;
};

} // namespace ocudu
