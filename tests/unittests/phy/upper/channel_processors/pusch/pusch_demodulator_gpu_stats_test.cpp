// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief Checks that the accelerated PUSCH demodulator reports unmeasured statistics as absent, not as values.

#include "pusch_demodulator_gpu_stats.h"
#include <gtest/gtest.h>
#include <limits>

using namespace ocudu;

namespace {

constexpr float unmeasured_inf = std::numeric_limits<float>::infinity();
constexpr float unmeasured_nan = std::numeric_limits<float>::quiet_NaN();

} // namespace

TEST(PuschDemodulatorGpuStats, PostEqualizationSinrFromNoiseVariance)
{
  // A mean noise variance of 0.01 is 20 dB.
  std::optional<float> sinr_dB = get_post_equalization_sinr_dB(0.01F * 100, 100);
  ASSERT_TRUE(sinr_dB.has_value());
  ASSERT_NEAR(*sinr_dB, 20.0F, 1e-4F);
}

TEST(PuschDemodulatorGpuStats, NoSoftbitsGiveNoPostEqualizationSinr)
{
  ASSERT_FALSE(get_post_equalization_sinr_dB(1.0F, 0).has_value());
}

TEST(PuschDemodulatorGpuStats, NoNoiseVarianceGivesNoPostEqualizationSinr)
{
  ASSERT_FALSE(get_post_equalization_sinr_dB(0.0F, 100).has_value());
  ASSERT_FALSE(get_post_equalization_sinr_dB(unmeasured_nan, 100).has_value());
}

TEST(PuschDemodulatorGpuStats, UnmeasuredReadbackLeavesEveryStatisticEmpty)
{
  // The accelerator reports a statistic it could not measure as a non-finite value.
  pusch_demodulator_notifier::demodulation_stats stats = build_e2e_final_stats(-unmeasured_inf,
                                                                               -unmeasured_inf,
                                                                               -unmeasured_inf,
                                                                               -unmeasured_inf,
                                                                               unmeasured_nan,
                                                                               unmeasured_nan,
                                                                               unmeasured_nan);
  ASSERT_FALSE(stats.sinr_dB.has_value());
  ASSERT_FALSE(stats.channel_estimator_sinr_dB.has_value());
  ASSERT_FALSE(stats.epre_dB.has_value());
  ASSERT_FALSE(stats.rsrp_dB.has_value());
  ASSERT_FALSE(stats.time_alignment_s.has_value());
  ASSERT_FALSE(stats.cfo_Hz.has_value());
  ASSERT_FALSE(stats.evm.has_value());
}

TEST(PuschDemodulatorGpuStats, MeasuredReadbackIsReportedUnchanged)
{
  pusch_demodulator_notifier::demodulation_stats stats =
      build_e2e_final_stats(18.0F, 21.0F, -3.0F, -6.0F, 1.0e-7F, 150.0F, 0.12F);
  ASSERT_EQ(stats.sinr_dB, 18.0F);
  ASSERT_EQ(stats.channel_estimator_sinr_dB, 21.0F);
  ASSERT_EQ(stats.epre_dB, -3.0F);
  ASSERT_EQ(stats.rsrp_dB, -6.0F);
  ASSERT_EQ(stats.time_alignment_s, 1.0e-7F);
  ASSERT_EQ(stats.cfo_Hz, 150.0F);
  ASSERT_EQ(stats.evm, 0.12F);
}
