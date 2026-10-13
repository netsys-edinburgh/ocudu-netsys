// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief Demodulation statistics reported by the accelerated PUSCH demodulator.
///
/// A statistic that was not measured is left empty. A substitute value would reach the scheduler as a measurement: a
/// made-up SINR feeds the PUSCH SINR average used for UL link adaptation, and a made-up EVM of zero is reported as an
/// error-free link.

#pragma once

#include "ocudu/phy/upper/channel_processors/pusch/pusch_demodulator_notifier.h"
#include "ocudu/support/math/math_utils.h"
#include <cmath>
#include <optional>

namespace ocudu {

/// \brief Gets the post-equalization SINR from the accumulated equalizer noise variance.
///
/// \param[in] noise_var_sum Sum of the post-equalization noise variances.
/// \param[in] nof_softbits  Number of softbits in the sum.
/// \return The SINR in dB, or nothing if no noise variance was measured.
inline std::optional<float> get_post_equalization_sinr_dB(float noise_var_sum, unsigned nof_softbits)
{
  if ((nof_softbits == 0) || !(noise_var_sum > 0.0F)) {
    return std::nullopt;
  }
  return -convert_power_to_dB(noise_var_sum / static_cast<float>(nof_softbits));
}

/// \brief Builds the final demodulation statistics from the accelerator readback.
///
/// Each value is reported only if it is finite: the accelerator returns a non-finite value for a statistic it could not
/// measure.
inline pusch_demodulator_notifier::demodulation_stats build_e2e_final_stats(float gpu_sinr_db,
                                                                            float gpu_dmrs_sinr_db,
                                                                            float gpu_epre_db,
                                                                            float gpu_rsrp_db,
                                                                            float gpu_ta_s,
                                                                            float gpu_cfo_hz,
                                                                            float gpu_evm)
{
  pusch_demodulator_notifier::demodulation_stats stats;

  // Report the post-equalization noise-variance SINR, as the CPU path and the deferred resident path both do. EVM is
  // a separate quantity and travels in stats.evm; substituting it here made this path disagree with the other three by
  // over 12 dB on the same link, while CRC stayed clean, so nothing downstream could detect it.
  if (std::isfinite(gpu_sinr_db)) {
    stats.sinr_dB.emplace(gpu_sinr_db);
  }
  if (std::isfinite(gpu_dmrs_sinr_db)) {
    stats.channel_estimator_sinr_dB.emplace(gpu_dmrs_sinr_db);
  }
  if (std::isfinite(gpu_evm)) {
    stats.evm.emplace(gpu_evm);
  }
  if (std::isfinite(gpu_epre_db)) {
    stats.epre_dB.emplace(gpu_epre_db);
  }
  if (std::isfinite(gpu_rsrp_db)) {
    stats.rsrp_dB.emplace(gpu_rsrp_db);
  }
  if (std::isfinite(gpu_ta_s)) {
    stats.time_alignment_s.emplace(gpu_ta_s);
  }
  if (std::isfinite(gpu_cfo_hz)) {
    stats.cfo_Hz.emplace(gpu_cfo_hz);
  }

  return stats;
}

} // namespace ocudu
