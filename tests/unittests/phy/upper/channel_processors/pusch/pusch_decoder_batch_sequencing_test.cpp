// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief Sequencing of the batched GPU decoding of the PUSCH decoder across the repetitions of a TTI bundle.
///
/// The receive buffer clears the codeblocks of a transmission for decoding once the earlier repetitions using the same
/// buffer are decoded. The batched decoder decodes all the codeblocks at once, so it must wait for every codeblock to
/// be cleared, notify the result once, and keep the buffer locked for the next repetition until the last one.

#include "../../rx_buffer_test_doubles.h"
#include "pusch_decoder_impl.h"
#include "ocudu/phy/upper/channel_coding/channel_coding_factories.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_decoder_notifier.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_decoder_result.h"
#include "ocudu/ran/resource_block.h"
#include <gtest/gtest.h>
#include <vector>

using namespace ocudu;

namespace {

/// Batch decoder that decodes every codeblock successfully into a known pattern.
class resident_decoder_spy : public pusch_resident_codeblock_decoder
{
public:
  std::vector<codeblock_result> decode_batch(span<const described_rx_codeblock> codeblock_llrs,
                                             span<bit_buffer>                   cb_data_buffers,
                                             span<span<log_likelihood_ratio>>   rm_buffers,
                                             bool                               new_data,
                                             crc_generator_poly                 crc_poly,
                                             bool                               use_early_stop,
                                             unsigned                           nof_ldpc_iterations) override
  {
    ++nof_decode_batch_calls;
    for (bit_buffer& data : cb_data_buffers) {
      for (unsigned i_bit = 0; i_bit != data.size(); ++i_bit) {
        data.insert(pattern_bit(i_bit), i_bit, 1);
      }
    }
    return std::vector<codeblock_result>(codeblock_llrs.size(), {.crc_ok = true, .nof_iterations = 1});
  }

  void rate_dematch_only(span<const log_likelihood_ratio>,
                         span<log_likelihood_ratio>,
                         bool,
                         const codeblock_metadata&) override
  {
  }

  bool export_resident_softbits(span<span<log_likelihood_ratio>>) override { return false; }

  tb_decode_result decode_resident_softbits(void*,
                                            size_t,
                                            const scrambling_config*,
                                            const codeblock_metadata::tb_common_metadata&,
                                            span<const codeblock_metadata::cb_specific_metadata>,
                                            span<uint8_t>,
                                            crc_generator_poly,
                                            unsigned,
                                            void*,
                                            int) override
  {
    return {};
  }

  const pusch_resident_codeblock_decoder_timing_stats& get_last_timing_stats() const override { return timing; }

  void set_demod_timing(float, float) override {}

  void set_timing_enabled(bool) override {}

  void set_ldpc_decoder_algorithm(const std::string&) override {}

  /// Bit written at the given position of every decoded codeblock.
  static uint8_t pattern_bit(unsigned i_bit) { return (i_bit % 3) == 0 ? 1 : 0; }

  unsigned nof_decode_batch_calls = 0;

private:
  pusch_resident_codeblock_decoder_timing_stats timing;
};

class decoder_notifier_spy : public pusch_decoder_notifier
{
public:
  void on_sch_data(const pusch_decoder_result& result_) override
  {
    ++nof_notifications;
    result = result_;
  }

  unsigned             nof_notifications = 0;
  pusch_decoder_result result;
};

/// Packs the decoder pattern into bytes, most significant bit first.
std::vector<uint8_t> make_expected_transport_block(unsigned nof_bytes)
{
  std::vector<uint8_t> bytes(nof_bytes);
  for (unsigned i_bit = 0; i_bit != nof_bytes * 8; ++i_bit) {
    bytes[i_bit / 8] |= resident_decoder_spy::pattern_bit(i_bit) << (7 - (i_bit % 8));
  }
  return bytes;
}

class PuschDecoderBatchSequencingFixture : public ::testing::Test
{
protected:
  /// Creates a decoder with the batch decoder spy and no executor, so that the decoding runs when it is started.
  void create_decoder()
  {
    auto decoder_spy = std::make_unique<resident_decoder_spy>();
    batch_decoder    = decoder_spy.get();

    std::shared_ptr<crc_calculator_factory> crc_factory = create_crc_calculator_factory_sw("auto");
    pusch_decoder_impl::sch_crc             crc_set     = {crc_factory->create(crc_generator_poly::CRC16),
                                                           crc_factory->create(crc_generator_poly::CRC24A),
                                                           crc_factory->create(crc_generator_poly::CRC24B)};
    decoder = std::make_unique<pusch_decoder_impl>(create_ldpc_segmenter_rx_factory_sw()->create(),
                                                   nullptr,
                                                   std::move(decoder_spy),
                                                   std::move(crc_set),
                                                   nullptr,
                                                   MAX_NOF_PRBS,
                                                   1);
  }

  /// Feeds the softbits of one transmission of a transport block of \c tbs_bytes on \c nof_softbits softbits.
  void transmit(unsigned tbs_bytes, unsigned nof_softbits, bool last_repetition)
  {
    transport_block.assign(tbs_bytes, 0);
    pusch_decoder::configuration config;
    config.base_graph      = ldpc_base_graph_type::BG1;
    config.mod             = modulation_scheme::QPSK;
    config.nof_layers      = 1;
    config.new_data        = true;
    config.last_repetition = last_repetition;

    pusch_decoder_buffer& softbits_buffer =
        decoder->new_data(transport_block, unique_rx_buffer(rx_buffer), notifier, config);
    decoder->set_nof_softbits(units::bits(nof_softbits));
    std::vector<log_likelihood_ratio> softbits(nof_softbits, log_likelihood_ratio(1));
    softbits_buffer.on_new_softbits(softbits);
    softbits_buffer.on_end_softbits();
  }

  static constexpr unsigned max_codeblock_size = 3 * 8448;

  rx_buffer_spy                       rx_buffer{max_codeblock_size, 2};
  decoder_notifier_spy                notifier;
  resident_decoder_spy*               batch_decoder = nullptr;
  std::unique_ptr<pusch_decoder_impl> decoder;
  std::vector<uint8_t>                transport_block;
};

} // namespace

// The batched decode starts only once the receive buffer has cleared every codeblock of the transmission.
TEST_F(PuschDecoderBatchSequencingFixture, decodes_once_every_codeblock_is_cleared)
{
  create_decoder();
  rx_buffer.set_defer_decode(true);
  ASSERT_TRUE(rx_buffer.try_lock());

  // A transport block of 1250 bytes is segmented in two codeblocks.
  transmit(1250, 30000, true);
  ASSERT_EQ(rx_buffer.get_nof_pending_decodes(), 2);

  rx_buffer.clear_pending_decode();
  EXPECT_EQ(batch_decoder->nof_decode_batch_calls, 0);
  EXPECT_EQ(notifier.nof_notifications, 0);

  rx_buffer.clear_pending_decode();
  EXPECT_EQ(batch_decoder->nof_decode_batch_calls, 1);
  EXPECT_EQ(notifier.nof_notifications, 1);
  EXPECT_EQ(notifier.result.nof_codeblocks_total, 2);
}

// A transmission that is not the last repetition of a bundle keeps the buffer for the next one, even when it decodes.
// The last repetition releases it.
TEST_F(PuschDecoderBatchSequencingFixture, keeps_the_buffer_until_the_last_repetition)
{
  create_decoder();

  // A short transport block is one codeblock, whose data is the transport block.
  ASSERT_TRUE(rx_buffer.try_lock());
  transmit(8, 600, false);
  ASSERT_EQ(notifier.nof_notifications, 1);
  ASSERT_TRUE(notifier.result.tb_crc_ok);
  EXPECT_EQ(transport_block, make_expected_transport_block(8));
  EXPECT_EQ(rx_buffer.get_nof_releases(), 0);
  EXPECT_EQ(rx_buffer.get_nof_unlocks(), 1);

  ASSERT_TRUE(rx_buffer.try_lock());
  transmit(8, 600, true);
  ASSERT_EQ(notifier.nof_notifications, 2);
  ASSERT_TRUE(notifier.result.tb_crc_ok);
  EXPECT_EQ(rx_buffer.get_nof_releases(), 1);
  EXPECT_EQ(rx_buffer.get_nof_unlocks(), 1);
}
