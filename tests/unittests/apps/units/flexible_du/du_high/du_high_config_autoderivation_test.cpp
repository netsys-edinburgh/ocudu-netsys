// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "apps/units/flexible_o_du/o_du_high/du_high/du_high_config.h"
#include "apps/units/flexible_o_du/o_du_high/du_high/du_high_config_cli11_schema.h"
#include <gtest/gtest.h>

using namespace ocudu;

namespace {

/// Auto-derives the parameters of a single cell with the given number of DL antennas and SSB beams.
std::vector<du_high_unit_ssb_beam_config> derive_ssb_beams(unsigned                                  nof_antennas_dl,
                                                           std::vector<du_high_unit_ssb_beam_config> beams)
{
  du_high_unit_config cfg;
  cfg.cells_cfg.front().cell.nof_antennas_dl = nof_antennas_dl;
  cfg.cells_cfg.front().cell.ssb_cfg.beams   = std::move(beams);

  autoderive_du_high_parameters_after_parsing(cfg);

  return cfg.cells_cfg.front().cell.ssb_cfg.beams;
}

/// Asserts that the SSB candidate holds the given beam coordinates.
void assert_beam(const du_high_unit_ssb_beam_config& beam,
                 unsigned                            ssb_index,
                 unsigned                            i_pol,
                 unsigned                            i_beam_dim1,
                 unsigned                            i_beam_dim2)
{
  ASSERT_EQ(beam.ssb_index, ssb_index);
  ASSERT_TRUE(beam.beam_coordinates.has_value());
  ASSERT_EQ(beam.beam_coordinates->i_pol, i_pol);
  ASSERT_EQ(beam.beam_coordinates->i_beam_dim1, i_beam_dim1);
  ASSERT_EQ(beam.beam_coordinates->i_beam_dim2, i_beam_dim2);
}

} // namespace

TEST(du_high_ssb_beam_autoderivation_test, single_ssb_candidate_uses_the_first_beam)
{
  const auto beams = derive_ssb_beams(1, {{.ssb_index = 0}});

  ASSERT_EQ(beams.size(), 1);
  assert_beam(beams[0], 0, 0, 0, 0);
}

TEST(du_high_ssb_beam_autoderivation_test, derived_beams_sweep_the_polarization_before_the_first_dimension)
{
  // A four antenna cell uses the 2x1 single-panel topology: two polarizations and eight beams in the first dimension.
  const auto beams = derive_ssb_beams(4, {{.ssb_index = 0}, {.ssb_index = 2}, {.ssb_index = 4}, {.ssb_index = 5}});

  ASSERT_EQ(beams.size(), 4);
  assert_beam(beams[0], 0, 0, 0, 0);
  assert_beam(beams[1], 2, 1, 0, 0);
  assert_beam(beams[2], 4, 0, 1, 0);
  assert_beam(beams[3], 5, 1, 1, 0);
}

TEST(du_high_ssb_beam_autoderivation_test, the_sweep_follows_the_ssb_candidate_order_not_the_configuration_order)
{
  const auto beams = derive_ssb_beams(4, {{.ssb_index = 5}, {.ssb_index = 0}});

  ASSERT_EQ(beams.size(), 2);
  assert_beam(beams[0], 5, 1, 0, 0);
  assert_beam(beams[1], 0, 0, 0, 0);
}

TEST(du_high_ssb_beam_autoderivation_test, configured_beams_are_not_derived)
{
  const auto beams = derive_ssb_beams(
      4,
      {{.ssb_index = 0, .beam_coordinates = du_high_unit_ssb_beam_coordinates_config{.i_pol = 1, .i_beam_dim1 = 7}},
       {.ssb_index = 1}});

  ASSERT_EQ(beams.size(), 2);
  assert_beam(beams[0], 0, 1, 7, 0);
  // The configured candidate still takes a position in the sweep.
  assert_beam(beams[1], 1, 1, 0, 0);
}

TEST(du_high_ssb_beam_autoderivation_test, the_second_dimension_overflows_when_the_grid_runs_out_of_beams)
{
  // A single antenna cell defines a single beam, so the second candidate falls outside the grid and the configuration
  // validator rejects it.
  const auto beams = derive_ssb_beams(1, {{.ssb_index = 0}, {.ssb_index = 1}});

  ASSERT_EQ(beams.size(), 2);
  assert_beam(beams[0], 0, 0, 0, 0);
  assert_beam(beams[1], 1, 0, 0, 1);
}

TEST(du_high_ssb_beam_autoderivation_test, beams_are_not_derived_when_the_nof_dl_antennas_has_no_topology)
{
  const auto beams = derive_ssb_beams(3, {{.ssb_index = 0}});

  ASSERT_EQ(beams.size(), 1);
  ASSERT_FALSE(beams[0].beam_coordinates.has_value());
}
