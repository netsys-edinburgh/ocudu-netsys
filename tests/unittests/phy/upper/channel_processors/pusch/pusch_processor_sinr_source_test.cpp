// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief Checks that the PUSCH processor notifier adaptor reports each SINR type from its own source.
///
/// The DM-RS channel estimator and the demodulator measure different SINRs. The channel-estimator SINR must come only
/// from the channel estimator, or from a demodulator that measured it explicitly, and never from the
/// post-equalization SINR.

#include "pusch_processor_notifier_adaptor.h"
#include "pusch_processor_result_test_doubles.h"
#include <gtest/gtest.h>

using namespace ocudu;

namespace {

constexpr float    estimator_sinr_dB         = 12.0F;
constexpr float    demodulator_dmrs_sinr_dB  = 14.0F;
constexpr float    post_equalization_sinr_dB = 30.0F;
constexpr unsigned nof_provisional_symbols   = 3;

class csi_part1_feedback_dummy : public pusch_processor_csi_part1_feedback
{
public:
  void on_csi_part1(const uci_payload_type& /* part1 */) override {}
};

/// \brief Runs one transmission through the adaptor and returns the SINR reported with the SCH data.
///
/// \param[in] sinr_calc_method SINR type the result reports.
/// \param[in] estimator_sinr   Channel-estimator SINR in the CSI before demodulation, if the channel estimator ran.
/// \param[in] demod_stats      Statistics reported by the demodulator, provisional and final.
std::optional<float> get_reported_sinr(channel_state_information::sinr_type                  sinr_calc_method,
                                       std::optional<float>                                  estimator_sinr,
                                       const pusch_demodulator_notifier::demodulation_stats& demod_stats)
{
  channel_state_information csi(sinr_calc_method);
  if (estimator_sinr.has_value()) {
    csi.set_sinr_dB(channel_state_information::sinr_type::channel_estimator, *estimator_sinr);
  }

  pusch_processor_result_notifier_spy result_notifier;
  csi_part1_feedback_dummy            csi_part1_feedback;
  pusch_processor_notifier_adaptor    adaptor;
  adaptor.new_transmission(result_notifier, csi_part1_feedback, csi);

  pusch_demodulator_notifier& demod_notifier = adaptor.get_demodulator_notifier();
  for (unsigned i_symbol = 0; i_symbol != nof_provisional_symbols; ++i_symbol) {
    demod_notifier.on_provisional_stats(i_symbol, demod_stats);
  }
  demod_notifier.on_end_stats(demod_stats);

  pusch_decoder_result decoder_result = {};
  decoder_result.tb_crc_ok            = true;
  adaptor.get_sch_data_notifier().on_sch_data(decoder_result);

  const auto& sch_entries = result_notifier.get_sch_entries();
  EXPECT_EQ(sch_entries.size(), 1);
  if (sch_entries.empty()) {
    return std::nullopt;
  }
  return sch_entries.front().csi.get_sinr_dB();
}

pusch_demodulator_notifier::demodulation_stats make_post_equalization_stats()
{
  pusch_demodulator_notifier::demodulation_stats stats;
  stats.sinr_dB = post_equalization_sinr_dB;
  return stats;
}

} // namespace

// Host demodulator: the channel estimator ran and the demodulator reports only its post-equalization SINR.
TEST(PuschProcessorSinrSource, EstimatorSinrSurvivesPostEqualizationStats)
{
  std::optional<float> sinr_dB = get_reported_sinr(
      channel_state_information::sinr_type::channel_estimator, estimator_sinr_dB, make_post_equalization_stats());
  ASSERT_TRUE(sinr_dB.has_value());
  ASSERT_FLOAT_EQ(*sinr_dB, estimator_sinr_dB);
}

TEST(PuschProcessorSinrSource, PostEqualizationSinrComesFromDemodulator)
{
  std::optional<float> sinr_dB = get_reported_sinr(
      channel_state_information::sinr_type::post_equalization, estimator_sinr_dB, make_post_equalization_stats());
  ASSERT_TRUE(sinr_dB.has_value());
  ASSERT_FLOAT_EQ(*sinr_dB, post_equalization_sinr_dB);
}

// Resident demodulator: no channel estimator ran, and the demodulator measured the DM-RS SINR itself.
TEST(PuschProcessorSinrSource, DemodulatorDmrsSinrFillsMissingEstimatorSinr)
{
  pusch_demodulator_notifier::demodulation_stats stats = make_post_equalization_stats();
  stats.channel_estimator_sinr_dB                      = demodulator_dmrs_sinr_dB;

  std::optional<float> sinr_dB =
      get_reported_sinr(channel_state_information::sinr_type::channel_estimator, std::nullopt, stats);
  ASSERT_TRUE(sinr_dB.has_value());
  ASSERT_FLOAT_EQ(*sinr_dB, demodulator_dmrs_sinr_dB);
}

// No channel estimator ran and no DM-RS SINR was measured: report none rather than the post-equalization SINR.
TEST(PuschProcessorSinrSource, MissingEstimatorSinrIsNotReplacedByPostEqualization)
{
  std::optional<float> sinr_dB = get_reported_sinr(
      channel_state_information::sinr_type::channel_estimator, std::nullopt, make_post_equalization_stats());
  ASSERT_FALSE(sinr_dB.has_value());
}
