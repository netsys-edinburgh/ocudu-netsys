// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "lower_phy_tx_pacing.h"
#include <gtest/gtest.h>

using namespace ocudu;
using namespace std::chrono_literals;

namespace {

using action = lower_phy_tx_pacing::decision::action;

const sampling_rate srate = sampling_rate::from_MHz(30.72);

/// Timestamp the given time after timestamp zero.
baseband_gateway_timestamp at(std::chrono::microseconds time)
{
  return lower_phy_tx_pacing::to_samples(time, srate);
}

} // namespace

TEST(lower_phy_tx_pacing_test, parses_the_guard)
{
  EXPECT_EQ(lower_phy_tx_pacing::parse_guard(nullptr), lower_phy_tx_pacing::default_guard);
  EXPECT_EQ(lower_phy_tx_pacing::parse_guard(""), lower_phy_tx_pacing::default_guard);
  EXPECT_EQ(lower_phy_tx_pacing::parse_guard("not a number"), lower_phy_tx_pacing::default_guard);
  EXPECT_EQ(lower_phy_tx_pacing::parse_guard("0"), 0us);
  EXPECT_EQ(lower_phy_tx_pacing::parse_guard("400"), 400us);
  EXPECT_EQ(lower_phy_tx_pacing::parse_guard("100000"), lower_phy_tx_pacing::max_guard);
}

TEST(lower_phy_tx_pacing_test, converts_between_samples_and_time_rounding_up)
{
  EXPECT_EQ(lower_phy_tx_pacing::to_samples(250us, srate), 7680U);
  EXPECT_EQ(lower_phy_tx_pacing::to_duration(7680, srate), 250us);
  EXPECT_EQ(lower_phy_tx_pacing::to_duration(7681, srate), 251us);
  EXPECT_EQ(lower_phy_tx_pacing::to_samples(0us, srate), 0U);
}

// The transmission runs ahead of the reception by up to the receive-to-transmit delay plus the guard without waiting.
TEST(lower_phy_tx_pacing_test, transmits_within_the_delay_and_the_guard)
{
  static constexpr baseband_gateway_timestamp last_rx = 1000000;
  static constexpr baseband_gateway_timestamp delay   = 30720;
  lower_phy_tx_pacing                         pacing(srate, delay, 250us);

  baseband_gateway_timestamp latest_allowed = last_rx + delay + lower_phy_tx_pacing::to_samples(250us, srate);
  EXPECT_EQ(pacing.decide(last_rx, last_rx, 1ms).what, action::transmit);
  EXPECT_EQ(pacing.decide(latest_allowed, last_rx, 1ms).what, action::transmit);
  EXPECT_NE(pacing.decide(latest_allowed + 1, last_rx, 1ms).what, action::transmit);

  // Without a guard, only the delay is allowed.
  lower_phy_tx_pacing no_guard(srate, delay, 0us);
  EXPECT_EQ(no_guard.decide(last_rx + delay, last_rx, 1ms).what, action::transmit);
  EXPECT_NE(no_guard.decide(last_rx + delay + 1, last_rx, 1ms).what, action::transmit);
}

// Well ahead, the processing sleeps in short steps that stop before the transmission is allowed; close to it, it
// yields.
TEST(lower_phy_tx_pacing_test, sleeps_when_well_ahead_and_yields_when_close)
{
  static constexpr baseband_gateway_timestamp last_rx = 0;
  lower_phy_tx_pacing                         pacing(srate, 0, 0us);

  lower_phy_tx_pacing::decision far = pacing.decide(at(500us), last_rx, 1ms);
  EXPECT_EQ(far.what, action::sleep);
  EXPECT_EQ(far.sleep_for, lower_phy_tx_pacing::max_sleep_step);

  // A sleep never exceeds the time left before the timeout.
  lower_phy_tx_pacing::decision before_timeout = pacing.decide(at(500us), last_rx, 3us);
  EXPECT_EQ(before_timeout.what, action::sleep);
  EXPECT_EQ(before_timeout.sleep_for, 3us);

  // Just above the sleep threshold, the sleep leaves the last part of the wait to yields.
  lower_phy_tx_pacing::decision just_above = pacing.decide(at(105us), last_rx, 1ms);
  EXPECT_EQ(just_above.what, action::sleep);
  EXPECT_LE(just_above.sleep_for, 105us - lower_phy_tx_pacing::yield_threshold);

  EXPECT_EQ(pacing.decide(at(100us), last_rx, 1ms).what, action::yield);
  EXPECT_EQ(pacing.decide(1, last_rx, 1ms).what, action::yield);
}
