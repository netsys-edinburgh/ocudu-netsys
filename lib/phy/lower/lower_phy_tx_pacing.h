// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/gateways/baseband/baseband_gateway_timestamp.h"
#include "ocudu/phy/lower/sampling_rate.h"
#include "ocudu/support/math/math_utils.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>

namespace ocudu {

/// \brief Paces the downlink baseband processing against the latest receive timestamp.
///
/// The transmit timestamps may run ahead of the latest receive timestamp by the receive-to-transmit delay plus a guard.
/// The guard absorbs the jitter between the receive and transmit paths of the radio without making the transmission
/// wait every slot. Beyond it, the downlink processing waits: it sleeps in short steps while the transmission is well
/// ahead, so that it does not overshoot the radio timing margin, and yields otherwise.
class lower_phy_tx_pacing
{
public:
  /// Default guard.
  static constexpr std::chrono::microseconds default_guard{250};
  /// Maximum guard.
  static constexpr std::chrono::microseconds max_guard{5000};
  /// The downlink processing sleeps if the transmission is ahead by more than this.
  static constexpr std::chrono::microseconds sleep_threshold{100};
  /// Longest sleep step.
  static constexpr std::chrono::microseconds max_sleep_step{10};
  /// A sleep stops this long before the transmission is allowed, and the remaining wait yields.
  static constexpr std::chrono::microseconds yield_threshold{20};

  /// \brief Parses a guard given in microseconds, as the OCUDU_LOWER_PHY_TX_PACING_GUARD_US environment variable does.
  ///
  /// \return The guard, capped to \ref max_guard, or \ref default_guard if \c value is null, empty or not a number.
  static std::chrono::microseconds parse_guard(const char* value)
  {
    if ((value == nullptr) || (*value == '\0')) {
      return default_guard;
    }

    char*         end    = nullptr;
    unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value) {
      return default_guard;
    }
    return std::chrono::microseconds(
        static_cast<std::chrono::microseconds::rep>(std::min<unsigned long>(parsed, max_guard.count())));
  }

  /// What the downlink processing does before transmitting a timestamp.
  struct decision {
    enum class action { transmit, sleep, yield };
    /// Action to take.
    action what;
    /// Sleep duration, if \c what is \c sleep.
    std::chrono::microseconds sleep_for{0};
  };

  /// \brief Creates the pacing for a sampling rate.
  /// \param[in] srate_              Sampling rate of the timestamps.
  /// \param[in] rx_to_tx_max_delay_ Number of samples the transmission may run ahead of the reception.
  /// \param[in] guard_              Additional time the transmission may run ahead.
  lower_phy_tx_pacing(sampling_rate              srate_,
                      baseband_gateway_timestamp rx_to_tx_max_delay_,
                      std::chrono::microseconds  guard_) :
    srate(srate_), max_ahead_samples(rx_to_tx_max_delay_ + to_samples(guard_, srate_))
  {
  }

  /// \brief Decides what to do before transmitting a timestamp.
  ///
  /// \param[in] tx_timestamp      Timestamp to transmit.
  /// \param[in] last_rx_timestamp Latest receive timestamp.
  /// \param[in] time_to_timeout   Time left until the downlink processing stops waiting.
  decision decide(baseband_gateway_timestamp tx_timestamp,
                  baseband_gateway_timestamp last_rx_timestamp,
                  std::chrono::microseconds  time_to_timeout) const
  {
    baseband_gateway_timestamp latest_allowed = last_rx_timestamp + max_ahead_samples;
    if (tx_timestamp <= latest_allowed) {
      return {decision::action::transmit};
    }

    std::chrono::microseconds ahead = to_duration(tx_timestamp - latest_allowed, srate);
    if (ahead > sleep_threshold) {
      return {decision::action::sleep, std::min({time_to_timeout, max_sleep_step, ahead - yield_threshold})};
    }
    return {decision::action::yield};
  }

  /// Converts a duration to a number of samples, rounding up.
  static baseband_gateway_timestamp to_samples(std::chrono::microseconds duration, sampling_rate srate)
  {
    return divide_ceil(static_cast<uint64_t>(duration.count()) * srate.to_Hz<uint64_t>(), 1000000UL);
  }

  /// Converts a number of samples to a duration, rounding up.
  static std::chrono::microseconds to_duration(baseband_gateway_timestamp nof_samples, sampling_rate srate)
  {
    return std::chrono::microseconds(divide_ceil(nof_samples * 1000000UL, srate.to_Hz<uint64_t>()));
  }

private:
  sampling_rate              srate;
  baseband_gateway_timestamp max_ahead_samples;
};

} // namespace ocudu
