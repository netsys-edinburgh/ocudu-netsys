// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ocudu/ran/precoding/precoding_and_beamforming_info.h"
#include <gtest/gtest.h>

using namespace ocudu;

TEST(precoding_and_beamforming_info_test, default_built_info_is_wideband_and_has_no_prg)
{
  const precoding_and_beamforming_info info;

  ASSERT_TRUE(info.is_wideband());
  ASSERT_TRUE(info.prgs.empty());
}

TEST(precoding_and_beamforming_info_test, info_with_a_prg_size_is_not_wideband)
{
  precoding_and_beamforming_info info;
  info.nof_rbs_per_prg = 4;

  ASSERT_FALSE(info.is_wideband());
}

TEST(precoding_and_beamforming_info_test, single_beam_precoding_maps_the_whole_allocation_onto_one_beam)
{
  const beam_identifier                beam_id = to_beam_id(5);
  const precoding_and_beamforming_info info    = make_single_beam_precoding(beam_id);

  ASSERT_TRUE(info.is_wideband());
  ASSERT_EQ(info.prgs.size(), 1);
  ASSERT_EQ(info.prgs[0].beams, precoding_beam_list{beam_id});
  // A single beam carries an unprecoded transmission.
  ASSERT_TRUE(std::holds_alternative<std::monostate>(info.prgs[0].pmi));
}

TEST(precoding_and_beamforming_info_test, default_precoding_selects_the_default_beams)
{
  const precoding_and_beamforming_info info = make_default_precoding();

  ASSERT_TRUE(info.is_wideband());
  ASSERT_EQ(info.prgs.size(), 1);
  // An empty beam list selects the beams that map directly onto the antenna ports.
  ASSERT_TRUE(info.prgs[0].beams.empty());
  ASSERT_TRUE(std::holds_alternative<std::monostate>(info.prgs[0].pmi));
}
