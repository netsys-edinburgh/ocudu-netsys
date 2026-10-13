// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ocudu/phy/generic_functions/generic_functions_factories.h"
#include "ocudu/phy/support/resource_grid.h"
#include "ocudu/phy/support/resource_grid_writer.h"
#include "ocudu/phy/support/support_factories.h"
#include "ocudu/phy/support/time_alignment_estimator/time_alignment_estimator_factories.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"
#include "ocudu/phy/upper/signal_processors/srs/doa_estimator_configuration.h"
#include "ocudu/phy/upper/signal_processors/srs/doa_estimator_factory.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_configuration.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_factory.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_result.h"
#include "ocudu/ran/cyclic_prefix.h"
#include "ocudu/ran/resource_block.h"
#include "ocudu/ran/srs/srs_information.h"
#include "ocudu/ran/srs/srs_resource_configuration.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <gtest/gtest.h>
#include <vector>

using namespace ocudu;

namespace {

constexpr unsigned nof_antennas = 4;
constexpr unsigned max_nof_prb  = 52;

/// Creates an SRS estimator with direction-of-arrival estimation for a uniform linear array.
std::unique_ptr<srs_estimator> create_estimator()
{
  std::shared_ptr<dft_processor_factory> dft_factory = create_dft_processor_factory_generic();
  doa_estimator_configuration            doa_config  = {
                  .nof_antennas = nof_antennas, .antenna_distance_over_wavelength = 0.5F, .cross_polarized = false};
  std::shared_ptr<srs_estimator_factory> factory =
      create_srs_estimator_generic_factory(create_low_papr_sequence_generator_sw_factory(),
                                           create_time_alignment_estimator_dft_factory(dft_factory),
                                           create_doa_estimator_factory(doa_config),
                                           max_nof_prb,
                                           "disabled");
  return factory->create();
}

/// Creates the configuration of a one-port SRS received by the given grid ports.
srs_estimator_configuration make_config(const std::array<unsigned, nof_antennas>& rx_ports)
{
  srs_resource_configuration resource;
  resource.nof_antenna_ports   = srs_resource_configuration::one_two_four_enum::one;
  resource.nof_symbols         = srs_nof_symbols::n2;
  resource.start_symbol        = 12;
  resource.configuration_index = 6;
  resource.sequence_id         = 5;
  resource.bandwidth_index     = 0;
  resource.comb_size           = tx_comb_size::n2;
  resource.comb_offset         = 0;
  resource.cyclic_shift        = 0;
  resource.freq_position       = 0;
  resource.freq_shift          = 0;
  resource.freq_hopping        = 0;
  resource.hopping             = srs_group_or_sequence_hopping::neither;
  resource.periodicity         = srs_resource_configuration::periodicity_and_offset{0, 0};

  srs_estimator_configuration config;
  config.slot     = slot_point(to_numerology_value(subcarrier_spacing::kHz30), 0);
  config.resource = resource;
  config.ports.assign(rx_ports.begin(), rx_ports.end());
  return config;
}

/// \brief Writes the SRS as a plane wave received by a uniform linear array.
///
/// The \f$i\f$-th requested port is the \f$i\f$-th antenna of the array, whichever grid port it is.
void write_plane_wave(resource_grid& grid, const srs_estimator_configuration& config, float angle_degrees)
{
  srs_information   info = get_srs_information(config.resource, 0);
  std::vector<cf_t> sequence(info.sequence_length);
  create_low_papr_sequence_generator_sw_factory()->create()->generate(
      sequence, info.sequence_group, info.sequence_number, info.n_cs, info.n_cs_max);

  float                phase_step = M_PI * std::sin(angle_degrees * static_cast<float>(M_PI) / 180.0F);
  std::vector<cbf16_t> received(info.sequence_length);
  grid.set_all_zero();
  for (unsigned i_antenna = 0; i_antenna != nof_antennas; ++i_antenna) {
    // A wave arriving from a positive broadside angle reaches antenna i later than antenna 0.
    cf_t steering = std::polar(1.0F, -phase_step * static_cast<float>(i_antenna));
    std::transform(sequence.begin(), sequence.end(), received.begin(), [steering](cf_t sample) {
      return to_cbf16(steering * sample);
    });
    for (unsigned i_symbol     = config.resource.start_symbol.value(),
                  i_symbol_end = i_symbol + static_cast<unsigned>(config.resource.nof_symbols);
         i_symbol != i_symbol_end;
         ++i_symbol) {
      grid.get_writer().put(
          config.ports[i_antenna], i_symbol, info.mapping_initial_subcarrier, info.comb_size, received);
    }
  }
}

/// Estimates the direction of arrival of a plane wave received on the given grid ports.
doa_estimator_result estimate_direction(const std::array<unsigned, nof_antennas>& rx_ports, float angle_degrees)
{
  srs_estimator_configuration    config = make_config(rx_ports);
  std::unique_ptr<resource_grid> grid   = create_resource_grid_factory()->create(
      2 * nof_antennas, get_nsymb_per_slot(cyclic_prefix::NORMAL), max_nof_prb * NOF_SUBCARRIERS_PER_RB);
  write_plane_wave(*grid, config, angle_degrees);

  std::optional<doa_estimator_result> result = create_estimator()->estimate(grid->get_reader(), config).doa_result;
  EXPECT_TRUE(result.has_value());
  return result.value_or(doa_estimator_result(0));
}

} // namespace

// The direction of arrival depends on which antenna receives each sample, that is, on the position of the port in the
// request, not on the grid port that holds it. Requests that differ only in the grid ports, here out of order and
// beyond the number of requested ports, must estimate the same direction.
TEST(srs_estimator_doa_test, direction_follows_the_order_of_the_requested_ports)
{
  static constexpr float angle_degrees = 20.0F;

  doa_estimator_result reference = estimate_direction({0, 1, 2, 3}, angle_degrees);
  ASSERT_FALSE(reference.doa_components.empty());
  EXPECT_NEAR(reference.doa_components.front().broadside_angle_degrees, angle_degrees, 1.0F);

  for (const std::array<unsigned, nof_antennas>& rx_ports :
       {std::array<unsigned, nof_antennas>{3, 1, 0, 2}, std::array<unsigned, nof_antennas>{4, 5, 6, 7}}) {
    doa_estimator_result result = estimate_direction(rx_ports, angle_degrees);
    ASSERT_EQ(result.doa_components.size(), reference.doa_components.size());
    for (unsigned i_component = 0; i_component != result.doa_components.size(); ++i_component) {
      EXPECT_FLOAT_EQ(result.doa_components[i_component].broadside_angle_degrees,
                      reference.doa_components[i_component].broadside_angle_degrees);
      EXPECT_FLOAT_EQ(result.doa_components[i_component].spectrum_strength,
                      reference.doa_components[i_component].spectrum_strength);
    }
  }
}
