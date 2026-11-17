// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief [netsys] PUSCH bit error tracker implementation.

#include "pusch_bit_error_tracker.h"
#include "ocudu/ocuduvec/bit.h"
#include "ocudu/phy/upper/channel_coding/channel_coding_factories.h"
#include "ocudu/phy/upper/rx_buffer.h"
#include "ocudu/ran/sch/sch_segmentation.h"
#include "ocudu/support/math/bit_ops.h"
#include "ocudu/support/math/math_utils.h"
#include <algorithm>
#include <cstring>

using namespace ocudu;

/// Number of bits in one byte.
static constexpr unsigned BITS_PER_BYTE = 8;

/// \brief Gets the codeblock message length, that is, data, CRC and filler bits.
///
/// The message length is the codeblock length times the base graph rate, which is 1/3 for BG1 and 1/5 for BG2. It is
/// the same as the decoder message length.
static unsigned get_msg_length(const codeblock_metadata& metadata)
{
  unsigned inverse_rate = (metadata.tb_common.base_graph == ldpc_base_graph_type::BG1) ? 3 : 5;
  return metadata.cb_specific.full_length / inverse_rate;
}

/// Gets the number of decoded message bits of a codeblock, that is, data and CRC bits without filler bits.
static unsigned get_nof_msg_bits(const codeblock_metadata& metadata)
{
  return get_msg_length(metadata) - metadata.cb_specific.nof_filler_bits;
}

/// Gets the number of bits a sequence occupies in a snapshot, where it starts at a byte boundary.
static unsigned get_aligned_nof_bits(unsigned nof_bits)
{
  return divide_ceil(nof_bits, BITS_PER_BYTE) * BITS_PER_BYTE;
}

/// Gets the bytes of a bit buffer that hold \c nof_bits bits starting at the byte-aligned bit \c offset.
static span<const uint8_t> get_aligned_bytes(const bit_buffer& bits, unsigned offset, unsigned nof_bits)
{
  return bits.get_buffer().subspan(offset / BITS_PER_BYTE, divide_ceil(nof_bits, BITS_PER_BYTE));
}

/// Loads eight consecutive bytes of a packed bit sequence.
static uint64_t load_word(span<const uint8_t> bytes, unsigned i_byte)
{
  uint64_t word;
  std::memcpy(&word, bytes.data() + i_byte, sizeof(uint64_t));
  return word;
}

/// \brief Counts the bits that differ between two packed bit sequences.
///
/// The sequences start at a byte boundary and the bits are packed with the most significant bit first. Only the first
/// \c nof_bits bits are compared.
///
/// \param[in] a        First sequence.
/// \param[in] b        Second sequence.
/// \param[in] mask     Optional mask with the same layout as the sequences. If it is not empty, only the positions where
///                     the mask is one are compared.
/// \param[in] nof_bits Number of bits to compare.
/// \return The number of compared positions where the sequences differ.
static unsigned
count_bit_differences(span<const uint8_t> a, span<const uint8_t> b, span<const uint8_t> mask, unsigned nof_bits)
{
  unsigned nof_bytes = divide_ceil(nof_bits, BITS_PER_BYTE);
  ocudu_assert((a.size() >= nof_bytes) && (b.size() >= nof_bytes) && (mask.empty() || (mask.size() >= nof_bytes)),
               "The sequence sizes (i.e., {}, {} and {} bytes) are too small for {} bits.",
               a.size(),
               b.size(),
               mask.size(),
               nof_bits);

  bool     use_mask       = !mask.empty();
  unsigned nof_full_bytes = nof_bits / BITS_PER_BYTE;
  unsigned count          = 0;
  unsigned i_byte         = 0;

  // Compare eight bytes at a time.
  for (; i_byte + sizeof(uint64_t) <= nof_full_bytes; i_byte += sizeof(uint64_t)) {
    uint64_t diff = load_word(a, i_byte) ^ load_word(b, i_byte);
    if (use_mask) {
      diff &= load_word(mask, i_byte);
    }
    count += static_cast<unsigned>(count_ones(diff));
  }

  // Compare the remaining full bytes.
  for (; i_byte != nof_full_bytes; ++i_byte) {
    auto diff = static_cast<uint8_t>(a[i_byte] ^ b[i_byte]);
    if (use_mask) {
      diff &= mask[i_byte];
    }
    count += static_cast<unsigned>(count_ones(diff));
  }

  // Compare the remaining bits, which are the most significant bits of the last byte.
  unsigned nof_tail_bits = nof_bits % BITS_PER_BYTE;
  if (nof_tail_bits != 0) {
    auto diff = static_cast<uint8_t>((a[i_byte] ^ b[i_byte]) & (0xffU << (BITS_PER_BYTE - nof_tail_bits)));
    if (use_mask) {
      diff &= mask[i_byte];
    }
    count += static_cast<unsigned>(count_ones(diff));
  }

  return count;
}

/// \brief Writes a mask with one bit per LLR, set to one if the LLR is not zero.
///
/// \param[out] mask_bytes Destination of the mask, packed with the most significant bit first.
/// \param[in]  llrs       LLRs.
/// \return The number of LLRs equal to zero.
static unsigned write_nonzero_mask(span<uint8_t> mask_bytes, span<const log_likelihood_ratio> llrs)
{
  std::fill(mask_bytes.begin(), mask_bytes.end(), 0);

  unsigned nof_zeros = 0;
  for (unsigned i_llr = 0, nof_llrs = llrs.size(); i_llr != nof_llrs; ++i_llr) {
    if (llrs[i_llr] != 0) {
      mask_bytes[i_llr / BITS_PER_BYTE] |= static_cast<uint8_t>(0x80U >> (i_llr % BITS_PER_BYTE));
    } else {
      ++nof_zeros;
    }
  }
  return nof_zeros;
}

pusch_bit_error_tracker::pusch_bit_error_tracker(units::bits                max_codeword_size,
                                                 ldpc_encoder_factory&      encoder_factory,
                                                 ldpc_rate_matcher_factory& rate_matcher_factory,
                                                 unsigned                   nof_reencoders,
                                                 unsigned                   nof_entries) :
  logger(ocudulog::fetch_basic_logger("BITERR")), entries(nof_entries)
{
  logger.set_level(ocudulog::basic_levels::info);

  // BG2 has the shortest codeblocks. Therefore, a transport block that fits in the codeword never has more codeblocks
  // than a transport block of the codeword size segmented with BG2.
  max_nof_cbs = compute_nof_codeblocks(max_codeword_size, ldpc_base_graph_type::BG2);

  // The LLRs of a transmission do not exceed the codeword size. The code rate is lower than one, so the decoded
  // message bits do not exceed it either. Each codeblock also needs up to one byte for the alignment.
  unsigned capacity = max_codeword_size.value() + max_nof_cbs * BITS_PER_BYTE;

  // Allocate all the snapshot memory. The bit buffers are resized in place, so their views point to their own storage.
  for (entry& e : entries) {
    for (tx_snapshot& tx : e.stored_tx) {
      tx.codeblocks.reserve(max_nof_cbs);
      tx.msg_bits.resize(capacity);
      tx.llr_hard_bits.resize(capacity);
      tx.llr_nonzero.resize(capacity);
    }
  }

  // Create the re-encoders. The pool serves concurrent reports, and the spare re-encoder is used if the pool is empty.
  auto create_reencoder = [&encoder_factory, &rate_matcher_factory]() {
    auto re          = std::make_unique<reencoder>();
    re->encoder      = encoder_factory.create();
    re->rate_matcher = rate_matcher_factory.create();
    ocudu_assert(re->encoder && re->rate_matcher, "Invalid LDPC encoder or rate matcher.");
    return re;
  };

  ocudu_assert(nof_reencoders > 0, "The number of re-encoders must be greater than zero.");
  std::vector<std::unique_ptr<reencoder>> reencoders(nof_reencoders);
  for (std::unique_ptr<reencoder>& re : reencoders) {
    re = create_reencoder();
  }
  reencoder_pool  = std::make_unique<bounded_unique_object_pool<reencoder>>(reencoders);
  spare_reencoder = create_reencoder();
}

void pusch_bit_error_tracker::on_crc_ko(const tx_context&                  ctx,
                                        span<const described_rx_codeblock> codeblocks,
                                        span<const bool>                   hard_bits_valid,
                                        rx_buffer&                         buffer)
{
  ocudu_assert(hard_bits_valid.size() == codeblocks.size(),
               "The number of hard bit flags (i.e., {}) does not match the number of codeblocks (i.e., {}).",
               hard_bits_valid.size(),
               codeblocks.size());

  // A new transport block allocates an entry. A retransmission is only tracked if the first transmission was stored.
  trx_buffer_identifier id(ctx.rnti, ctx.harq_id);
  entry*                e = acquire(id, ctx.new_data);
  if (e == nullptr) {
    return;
  }

  if (ctx.new_data) {
    // An entry with failed transmissions belongs to a previous transport block that was never recovered.
    if (e->nof_failed_tx != 0) {
      report_lost(*e, "new_data");
    }
    start_tb(*e, ctx, codeblocks.size());
  } else if (!is_same_tb(*e, ctx, codeblocks.size())) {
    report_lost(*e, "tb_mismatch");
    release(*e, true);
    return;
  }

  store_tx(*e, ctx, codeblocks, hard_bits_valid, buffer);
  release(*e, false);
}

void pusch_bit_error_tracker::on_crc_ok(const tx_context&                  ctx,
                                        span<const described_rx_codeblock> codeblocks,
                                        rx_buffer&                         buffer)
{
  // Only transport blocks with stored failed transmissions have an entry.
  trx_buffer_identifier id(ctx.rnti, ctx.harq_id);
  entry*                e = acquire(id, false);
  if (e == nullptr) {
    return;
  }

  if (ctx.new_data) {
    // The previous transport block of this HARQ process was never recovered.
    report_lost(*e, "new_data");
  } else if (!is_same_tb(*e, ctx, codeblocks.size())) {
    report_lost(*e, "tb_mismatch");
  } else {
    report_recovered(*e, ctx, codeblocks, buffer);
  }

  release(*e, true);
}

pusch_bit_error_tracker::entry* pusch_bit_error_tracker::acquire(trx_buffer_identifier id, bool allocate)
{
  std::lock_guard<std::mutex> lock(mutex);

  // Look for the entry of the HARQ process.
  auto it = std::find_if(entries.begin(), entries.end(), [id](const entry& e) { return e.id == id; });
  if (it != entries.end()) {
    // Calls for the same HARQ process are serialized by the receive buffer, so the entry should not be in use.
    if (it->in_use) {
      logger.warning("{}: bit error tracker entry is already in use.", id);
      return nullptr;
    }
    it->in_use = true;
    return &*it;
  }

  if (!allocate) {
    return nullptr;
  }

  // Look for a free entry.
  it = std::find_if(
      entries.begin(), entries.end(), [](const entry& e) { return e.id == trx_buffer_identifier::invalid(); });

  // If there is no free entry, evict the least recently used entry that is not in use.
  if (it == entries.end()) {
    for (auto candidate = entries.begin(); candidate != entries.end(); ++candidate) {
      if (!candidate->in_use && ((it == entries.end()) || (candidate->last_use < it->last_use))) {
        it = candidate;
      }
    }
    if (it == entries.end()) {
      return nullptr;
    }
    report_lost(*it, "evicted");
  }

  it->id            = id;
  it->in_use        = true;
  it->nof_failed_tx = 0;
  it->nof_stored_tx = 0;
  return &*it;
}

void pusch_bit_error_tracker::release(entry& e, bool free_entry)
{
  std::lock_guard<std::mutex> lock(mutex);
  e.in_use   = false;
  e.last_use = ++use_counter;
  if (free_entry) {
    e.id = trx_buffer_identifier::invalid();
  }
}

void pusch_bit_error_tracker::start_tb(entry& e, const tx_context& ctx, unsigned nof_cbs)
{
  e.rnti          = ctx.rnti;
  e.harq_id       = ctx.harq_id;
  e.tbs           = ctx.tbs;
  e.nof_cbs       = nof_cbs;
  e.nof_failed_tx = 0;
  e.first_slot    = ctx.slot;
  e.last_slot     = ctx.slot;
  e.nof_stored_tx = 0;
}

bool pusch_bit_error_tracker::is_same_tb(const entry& e, const tx_context& ctx, unsigned nof_cbs)
{
  return (e.tbs == ctx.tbs) && (e.nof_cbs == nof_cbs);
}

void pusch_bit_error_tracker::store_tx(entry&                             e,
                                       const tx_context&                  ctx,
                                       span<const described_rx_codeblock> codeblocks,
                                       span<const bool>                   hard_bits_valid,
                                       rx_buffer&                         buffer)
{
  unsigned tx_idx = e.nof_failed_tx;
  ++e.nof_failed_tx;
  e.last_slot = ctx.slot;

  // Skip the snapshot if all the snapshot slots are used. The transmission still counts as failed.
  if (e.nof_stored_tx == max_nof_stored_tx) {
    return;
  }

  tx_snapshot& tx      = e.stored_tx[e.nof_stored_tx];
  unsigned     nof_cbs = codeblocks.size();

  // Check that the transmission fits in the snapshot memory.
  unsigned nof_aligned_msg_bits = 0;
  unsigned nof_aligned_llrs     = 0;
  for (const described_rx_codeblock& cb : codeblocks) {
    nof_aligned_msg_bits += get_aligned_nof_bits(get_nof_msg_bits(cb.second));
    nof_aligned_llrs += get_aligned_nof_bits(cb.first.size());
  }
  if ((nof_cbs > tx.codeblocks.capacity()) || (nof_aligned_msg_bits > tx.msg_bits.size()) ||
      (nof_aligned_llrs > tx.llr_hard_bits.size())) {
    logger.warning("rnti={} h_id={}: bit error snapshot skipped, {} codeblocks, {} message bits and {} LLRs exceed "
                   "the capacity of {} codeblocks and {} bits.",
                   e.rnti,
                   static_cast<unsigned>(e.harq_id),
                   nof_cbs,
                   nof_aligned_msg_bits,
                   nof_aligned_llrs,
                   tx.codeblocks.capacity(),
                   tx.msg_bits.size());
    return;
  }

  tx.tx_idx = tx_idx;
  tx.slot   = ctx.slot;
  tx.rv     = ctx.rv;
  tx.codeblocks.resize(nof_cbs);

  span<uint8_t> msg_bytes     = tx.msg_bits.get_buffer();
  span<uint8_t> nonzero_bytes = tx.llr_nonzero.get_buffer();
  unsigned      msg_offset    = 0;
  unsigned      llr_offset    = 0;
  for (unsigned i_cb = 0; i_cb != nof_cbs; ++i_cb) {
    span<const log_likelihood_ratio> llrs     = codeblocks[i_cb].first;
    const codeblock_metadata&        metadata = codeblocks[i_cb].second;
    codeblock_snapshot&              cb       = tx.codeblocks[i_cb];

    cb.metadata     = metadata;
    cb.nof_msg_bits = get_nof_msg_bits(metadata);
    cb.msg_offset   = msg_offset;
    cb.msg_valid    = hard_bits_valid[i_cb];
    cb.nof_llrs     = llrs.size();
    cb.llr_offset   = llr_offset;

    // After decoding: copy the decoded message bits, starting at a byte boundary.
    if (cb.msg_valid) {
      bit_buffer          msg = buffer.get_codeblock_data_bits(i_cb, cb.nof_msg_bits);
      span<const uint8_t> src = msg.get_buffer();
      std::copy(src.begin(), src.end(), msg_bytes.begin() + msg_offset / BITS_PER_BYTE);
    }

    // Before decoding: store the hard decisions of the LLRs and which LLRs are not zero.
    hard_decision(tx.llr_hard_bits, llrs, llr_offset);
    cb.nof_zero_llrs = write_nonzero_mask(
        nonzero_bytes.subspan(llr_offset / BITS_PER_BYTE, divide_ceil(cb.nof_llrs, BITS_PER_BYTE)), llrs);

    msg_offset += get_aligned_nof_bits(cb.nof_msg_bits);
    llr_offset += get_aligned_nof_bits(cb.nof_llrs);
  }

  ++e.nof_stored_tx;
}

void pusch_bit_error_tracker::report_recovered(const entry&                       e,
                                               const tx_context&                  ctx,
                                               span<const described_rx_codeblock> codeblocks,
                                               rx_buffer&                         buffer)
{
  // Use a re-encoder from the pool or, if the pool is empty, the spare one.
  bounded_unique_object_pool<reencoder>::ptr pooled = reencoder_pool->get();
  if (pooled) {
    count_and_report(e, ctx, codeblocks, buffer, *pooled);
    return;
  }

  std::lock_guard<std::mutex> lock(spare_reencoder_mutex);
  count_and_report(e, ctx, codeblocks, buffer, *spare_reencoder);
}

void pusch_bit_error_tracker::count_and_report(const entry&                       e,
                                               const tx_context&                  ctx,
                                               span<const described_rx_codeblock> codeblocks,
                                               rx_buffer&                         buffer,
                                               reencoder&                         re)
{
  std::array<bit_counts, max_nof_stored_tx>      counts = {};
  static_bit_buffer<ldpc::MAX_MESSAGE_SIZE>      message;
  static_bit_buffer<ldpc::MAX_CODEBLOCK_RM_SIZE> codeword;

  for (unsigned i_cb = 0; i_cb != e.nof_cbs; ++i_cb) {
    const codeblock_metadata& metadata     = codeblocks[i_cb].second;
    unsigned                  msg_length   = get_msg_length(metadata);
    unsigned                  nof_msg_bits = get_nof_msg_bits(metadata);

    // Ground truth: the correctly decoded message. The filler bits are set to zero, as in the transmitter.
    bit_buffer truth = buffer.get_codeblock_data_bits(i_cb, nof_msg_bits);
    message.resize(msg_length);
    message.zero();
    ocuduvec::copy_offset(message, 0, truth, 0, nof_msg_bits);

    // Encode the ground truth. The encoded codeblock does not depend on the transmission.
    ldpc_encoder::configuration encoder_config = {.base_graph   = metadata.tb_common.base_graph,
                                                  .lifting_size = metadata.tb_common.lifting_size,
                                                  .Nref         = metadata.tb_common.Nref};
    const ldpc_encoder_buffer&  encoded        = re.encoder->encode(message, encoder_config);

    for (unsigned i_tx = 0; i_tx != e.nof_stored_tx; ++i_tx) {
      const tx_snapshot&        tx = e.stored_tx[i_tx];
      const codeblock_snapshot& cb = tx.codeblocks[i_cb];
      bit_counts&               c  = counts[i_tx];

      // After decoding: compare the decoded message bits with the ground truth.
      if (cb.msg_valid) {
        c.post_err += count_bit_differences(
            truth.get_buffer(), get_aligned_bytes(tx.msg_bits, cb.msg_offset, cb.nof_msg_bits), {}, cb.nof_msg_bits);
        c.post_bits += cb.nof_msg_bits;
      } else {
        ++c.post_skipped_cbs;
      }

      // Before decoding: rate match the ground truth with the parameters of the failed transmission to obtain the
      // transmitted bits, and compare them with the hard decisions of the LLRs that are not zero.
      codeword.resize(cb.nof_llrs);
      re.rate_matcher->rate_match(codeword, encoded, cb.metadata);
      c.pre_err += count_bit_differences(codeword.get_buffer(),
                                         get_aligned_bytes(tx.llr_hard_bits, cb.llr_offset, cb.nof_llrs),
                                         get_aligned_bytes(tx.llr_nonzero, cb.llr_offset, cb.nof_llrs),
                                         cb.nof_llrs);
      c.pre_zero += cb.nof_zero_llrs;
      c.pre_bits += cb.nof_llrs;
    }
  }

  for (unsigned i_tx = 0; i_tx != e.nof_stored_tx; ++i_tx) {
    const tx_snapshot& tx = e.stored_tx[i_tx];
    const bit_counts&  c  = counts[i_tx];
    logger.info("UL_BITERR rnti={} h_id={} tbs={} nof_cbs={} tx_idx={} slot={} rv={} nof_failed_tx={} ok_slot={} "
                "ok_rv={} post_bits={} post_err={} post_ok={} post_skipped_cbs={} pre_bits={} pre_err={} pre_ok={} "
                "pre_zero={}",
                e.rnti,
                static_cast<unsigned>(e.harq_id),
                e.tbs.value(),
                e.nof_cbs,
                tx.tx_idx,
                tx.slot,
                tx.rv,
                e.nof_failed_tx,
                ctx.slot,
                ctx.rv,
                c.post_bits,
                c.post_err,
                c.post_bits - c.post_err,
                c.post_skipped_cbs,
                c.pre_bits,
                c.pre_err,
                c.pre_bits - c.pre_err - c.pre_zero,
                c.pre_zero);
  }
}

void pusch_bit_error_tracker::report_lost(const entry& e, const char* cause)
{
  logger.info("UL_BITERR_LOST rnti={} h_id={} tbs={} nof_cbs={} nof_failed_tx={} nof_stored_tx={} first_slot={} "
              "last_slot={} cause={}",
              e.rnti,
              static_cast<unsigned>(e.harq_id),
              e.tbs.value(),
              e.nof_cbs,
              e.nof_failed_tx,
              e.nof_stored_tx,
              e.first_slot,
              e.last_slot,
              cause);
}
