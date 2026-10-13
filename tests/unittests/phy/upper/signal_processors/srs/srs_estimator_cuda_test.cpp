// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "phy_acceleration_resource_grid_factory.h"
#include "signal_processors/srs/srs_estimator_cuda_impl.h"
#include "ocudu/phy/generic_functions/generic_functions_factories.h"
#include "ocudu/phy/support/resource_grid.h"
#include "ocudu/phy/support/resource_grid_reader.h"
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
#include "ocudu/ran/srs/srs_channel_matrix.h"
#include "ocudu/ran/srs/srs_information.h"
#include "ocudu/ran/srs/srs_resource_configuration.h"
#include "ocudu/support/error_handling.h"
#include "ocudu/support/math/math_utils.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <gtest/gtest.h>
#include <numeric>
#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace ocudu;

namespace {

enum class grid_mode { host, cuda_visible };

/// \brief Sets an environment variable for the current scope and restores its previous state on destruction.
class scoped_env_var
{
public:
  explicit scoped_env_var(const char* name_) : name(name_)
  {
    const char* value = std::getenv(name.c_str());
    if (value != nullptr) {
      original_value = value;
    }
  }

  ~scoped_env_var()
  {
    if (original_value.has_value()) {
      (void)setenv(name.c_str(), original_value->c_str(), 1);
    } else {
      (void)unsetenv(name.c_str());
    }
  }

  scoped_env_var(const scoped_env_var&)            = delete;
  scoped_env_var& operator=(const scoped_env_var&) = delete;

  bool set(const char* value) { return setenv(name.c_str(), value, 1) == 0; }

private:
  std::string                name;
  std::optional<std::string> original_value;
};

struct srs_test_profile {
  unsigned                nof_rx_ports        = 1;
  unsigned                nof_tx_ports        = 1;
  std::array<unsigned, 4> rx_ports            = {0, 1, 2, 3};
  subcarrier_spacing      scs                 = subcarrier_spacing::kHz30;
  unsigned                nof_symbols         = 1;
  unsigned                start_symbol        = 13;
  tx_comb_size            comb_size           = tx_comb_size::n4;
  unsigned                comb_offset         = 0;
  unsigned                cyclic_shift        = 0;
  unsigned                configuration_index = 63;
  unsigned                bandwidth_index     = 0;
  unsigned                freq_position       = 0;
  unsigned                freq_shift          = 0;
  unsigned                sequence_id         = 0;
};

srs_resource_configuration::one_two_four_enum to_srs_ports(unsigned nof_tx_ports)
{
  switch (nof_tx_ports) {
    case 1:
      return srs_resource_configuration::one_two_four_enum::one;
    case 2:
      return srs_resource_configuration::one_two_four_enum::two;
    case 4:
      return srs_resource_configuration::one_two_four_enum::four;
    default:
      report_fatal_error("Unsupported SRS port count {}.", nof_tx_ports);
  }
}

srs_estimator_configuration make_srs_config(const srs_test_profile& profile)
{
  srs_resource_configuration resource;
  resource.nof_antenna_ports   = to_srs_ports(profile.nof_tx_ports);
  resource.nof_symbols         = static_cast<srs_nof_symbols>(profile.nof_symbols);
  resource.start_symbol        = profile.start_symbol;
  resource.configuration_index = profile.configuration_index;
  resource.sequence_id         = profile.sequence_id;
  resource.bandwidth_index     = profile.bandwidth_index;
  resource.comb_size           = profile.comb_size;
  resource.comb_offset         = profile.comb_offset;
  resource.cyclic_shift        = profile.cyclic_shift;
  resource.freq_position       = profile.freq_position;
  resource.freq_shift          = profile.freq_shift;
  resource.freq_hopping        = profile.bandwidth_index;
  resource.hopping             = srs_group_or_sequence_hopping::neither;
  resource.periodicity         = srs_resource_configuration::periodicity_and_offset{0, 0};

  srs_estimator_configuration config;
  config.slot     = slot_point(to_numerology_value(profile.scs), 0);
  config.resource = resource;
  config.ports.resize(profile.nof_rx_ports);
  for (unsigned i_rx_port = 0; i_rx_port != profile.nof_rx_ports; ++i_rx_port) {
    config.ports[i_rx_port] = profile.rx_ports[i_rx_port];
  }
  return config;
}

std::shared_ptr<srs_estimator_factory>
create_test_srs_factory(const std::string&                     acceleration_mode,
                        std::shared_ptr<doa_estimator_factory> doa_factory = nullptr)
{
  std::shared_ptr<low_papr_sequence_generator_factory> sequence_factory =
      create_low_papr_sequence_generator_sw_factory();
  EXPECT_NE(sequence_factory, nullptr);

  std::shared_ptr<dft_processor_factory> dft_factory = create_dft_processor_factory_fftw_slow();
  if (!dft_factory) {
    dft_factory = create_dft_processor_factory_generic();
  }
  EXPECT_NE(dft_factory, nullptr);

  std::shared_ptr<time_alignment_estimator_factory> ta_factory =
      create_time_alignment_estimator_dft_factory(dft_factory);
  EXPECT_NE(ta_factory, nullptr);

  return create_srs_estimator_generic_factory(
      sequence_factory, ta_factory, std::move(doa_factory), MAX_NOF_PRBS, acceleration_mode);
}

unsigned get_required_grid_ports(const srs_estimator_configuration& config)
{
  unsigned nof_ports = 0;
  for (unsigned port : config.ports) {
    nof_ports = std::max(nof_ports, port + 1U);
  }
  return nof_ports;
}

std::unique_ptr<resource_grid> create_test_grid(const srs_estimator_configuration& config, grid_mode mode)
{
  std::shared_ptr<resource_grid_factory> fallback_factory = create_resource_grid_factory();
  EXPECT_NE(fallback_factory, nullptr);

  if (mode == grid_mode::cuda_visible) {
    // Managed CUDA-visible grids are only auto-selected on integrated GPUs, where device and host memory are physically
    // shared. Request them explicitly so the CUDA-visible path is also covered on discrete GPUs, where the auto policy
    // prefers pinned host memory instead.
    scoped_env_var grid_mode_var("OCUDU_UL_CUDA_VISIBLE_GRID");
    EXPECT_TRUE(grid_mode_var.set("managed"));

    std::shared_ptr<resource_grid_factory> accelerated_factory = create_phy_acceleration_resource_grid_factory(
        fallback_factory, phy_acceleration_resource_grid_direction::uplink, true);
    EXPECT_NE(accelerated_factory, nullptr);
    return accelerated_factory->create(
        get_required_grid_ports(config), get_nsymb_per_slot(cyclic_prefix::NORMAL), MAX_NOF_SUBCARRIERS);
  }

  return fallback_factory->create(
      get_required_grid_ports(config), get_nsymb_per_slot(cyclic_prefix::NORMAL), MAX_NOF_SUBCARRIERS);
}

cf_t make_channel_coefficient(unsigned i_rx_port, unsigned i_tx_port)
{
  float amplitude = 0.45F + 0.07F * static_cast<float>(i_rx_port + 1) + 0.05F * static_cast<float>(i_tx_port + 1);
  float phase     = 0.37F * static_cast<float>(i_rx_port + 1) - 0.29F * static_cast<float>(i_tx_port + 1);
  return std::polar(amplitude, phase);
}

srs_channel_matrix make_expected_channel_matrix(unsigned nof_rx_ports, unsigned nof_tx_ports, float gain = 1.0F)
{
  srs_channel_matrix matrix(nof_rx_ports, nof_tx_ports);
  for (unsigned i_rx_port = 0; i_rx_port != nof_rx_ports; ++i_rx_port) {
    for (unsigned i_tx_port = 0; i_tx_port != nof_tx_ports; ++i_tx_port) {
      matrix.set_coefficient(gain * make_channel_coefficient(i_rx_port, i_tx_port), i_rx_port, i_tx_port);
    }
  }
  return matrix;
}

void populate_srs_grid(resource_grid&                     grid,
                       const srs_estimator_configuration& config,
                       float                              snr_db,
                       unsigned                           seed,
                       float                              gain = 1.0F)
{
  grid.set_all_zero();

  unsigned nof_rx_ports     = config.ports.size();
  unsigned nof_tx_ports     = static_cast<unsigned>(config.resource.nof_antenna_ports);
  unsigned nof_symbols      = static_cast<unsigned>(config.resource.nof_symbols);
  unsigned start_symbol     = config.resource.start_symbol.value();
  unsigned grid_nof_symbols = grid.get_reader().get_nof_symbols();
  unsigned grid_nof_subc    = grid.get_reader().get_nof_subc();
  unsigned sequence_length  = get_srs_information(config.resource, 0).sequence_length;

  std::unique_ptr<low_papr_sequence_generator> sequence_generator =
      create_low_papr_sequence_generator_sw_factory()->create();
  ASSERT_NE(sequence_generator, nullptr);

  std::vector<std::vector<cf_t>> sequences(nof_tx_ports, std::vector<cf_t>(sequence_length));
  std::vector<srs_information>   info_per_tx;
  info_per_tx.reserve(nof_tx_ports);

  for (unsigned i_tx_port = 0; i_tx_port != nof_tx_ports; ++i_tx_port) {
    srs_information info = get_srs_information(config.resource, i_tx_port);
    ASSERT_LE(info.mapping_initial_subcarrier + (info.sequence_length - 1U) * info.comb_size, grid_nof_subc - 1U);
    info_per_tx.push_back(info);
    sequence_generator->generate(
        span<cf_t>(sequences[i_tx_port]), info.sequence_group, info.sequence_number, info.n_cs, info.n_cs_max);
  }

  std::vector<std::vector<cf_t>>    symbols(nof_rx_ports * grid_nof_symbols, std::vector<cf_t>(grid_nof_subc, cf_t()));
  std::vector<std::vector<uint8_t>> active(nof_rx_ports * grid_nof_symbols, std::vector<uint8_t>(grid_nof_subc, 0));

  double   signal_power_sum = 0.0;
  unsigned active_count     = 0;
  for (unsigned i_rx_port = 0; i_rx_port != nof_rx_ports; ++i_rx_port) {
    for (unsigned i_symbol = start_symbol; i_symbol != start_symbol + nof_symbols; ++i_symbol) {
      std::vector<cf_t>&    symbol = symbols[i_rx_port * grid_nof_symbols + i_symbol];
      std::vector<uint8_t>& mask   = active[i_rx_port * grid_nof_symbols + i_symbol];

      for (unsigned i_tx_port = 0; i_tx_port != nof_tx_ports; ++i_tx_port) {
        const srs_information& info    = info_per_tx[i_tx_port];
        cf_t                   channel = gain * make_channel_coefficient(i_rx_port, i_tx_port);
        for (unsigned i_re = 0; i_re != sequence_length; ++i_re) {
          unsigned k = info.mapping_initial_subcarrier + i_re * info.comb_size;
          symbol[k] += channel * sequences[i_tx_port][i_re];
          mask[k] = 1;
        }
      }

      for (unsigned k = 0; k != grid_nof_subc; ++k) {
        if (mask[k] != 0) {
          signal_power_sum += std::norm(symbol[k]);
          ++active_count;
        }
      }
    }
  }

  float signal_power = (active_count != 0) ? static_cast<float>(signal_power_sum / active_count) : 1.0F;
  float noise_var    = signal_power / convert_dB_to_power(snr_db);
  float noise_sigma  = std::sqrt(noise_var / 2.0F);

  std::mt19937                    rgen(seed);
  std::normal_distribution<float> normal_dist(0.0F, noise_sigma);
  resource_grid_writer&           writer = grid.get_writer();
  for (unsigned i_rx_port = 0; i_rx_port != nof_rx_ports; ++i_rx_port) {
    unsigned physical_rx_port = config.ports[i_rx_port];
    for (unsigned i_symbol = 0; i_symbol != grid_nof_symbols; ++i_symbol) {
      span<cbf16_t>               view   = writer.get_view(physical_rx_port, i_symbol);
      const std::vector<cf_t>&    symbol = symbols[i_rx_port * grid_nof_symbols + i_symbol];
      const std::vector<uint8_t>& mask   = active[i_rx_port * grid_nof_symbols + i_symbol];
      for (unsigned k = 0; k != grid_nof_subc; ++k) {
        cf_t value = symbol[k];
        if (mask[k] != 0) {
          value += cf_t(normal_dist(rgen), normal_dist(rgen));
        }
        view[k] = to_cbf16(value);
      }
    }
  }
}

float channel_matrix_relative_error(const srs_channel_matrix& reference, const srs_channel_matrix& candidate)
{
  EXPECT_EQ(reference.get_nof_rx_ports(), candidate.get_nof_rx_ports());
  EXPECT_EQ(reference.get_nof_tx_ports(), candidate.get_nof_tx_ports());

  double error_power = 0.0;
  double ref_power   = 0.0;
  for (unsigned i_rx_port = 0; i_rx_port != reference.get_nof_rx_ports(); ++i_rx_port) {
    for (unsigned i_tx_port = 0; i_tx_port != reference.get_nof_tx_ports(); ++i_tx_port) {
      cf_t ref  = reference.get_coefficient(i_rx_port, i_tx_port);
      cf_t cand = candidate.get_coefficient(i_rx_port, i_tx_port);
      error_power += std::norm(ref - cand);
      ref_power += std::norm(ref);
    }
  }

  return (ref_power > 0.0) ? static_cast<float>(std::sqrt(error_power / ref_power)) : 0.0F;
}

float relative_delta(float reference, float candidate)
{
  float denominator = std::max(std::abs(reference), 1e-12F);
  return std::abs(candidate - reference) / denominator;
}

void assert_srs_results_match(const srs_estimator_result& cpu_result,
                              const srs_estimator_result& gpu_result,
                              const srs_channel_matrix&   expected_channel)
{
  EXPECT_LT(channel_matrix_relative_error(cpu_result.channel_matrix, gpu_result.channel_matrix), 6.0e-2F);
  EXPECT_TRUE(cpu_result.channel_matrix.is_near(expected_channel, 8.0e-2F));
  EXPECT_TRUE(gpu_result.channel_matrix.is_near(expected_channel, 8.0e-2F));

  ASSERT_TRUE(cpu_result.epre_dB.has_value());
  ASSERT_TRUE(gpu_result.epre_dB.has_value());
  EXPECT_NEAR(cpu_result.epre_dB.value(), gpu_result.epre_dB.value(), 2.0e-2F);

  ASSERT_TRUE(cpu_result.rsrp_dB.has_value());
  ASSERT_TRUE(gpu_result.rsrp_dB.has_value());
  EXPECT_NEAR(cpu_result.rsrp_dB.value(), gpu_result.rsrp_dB.value(), 2.0e-2F);

  ASSERT_TRUE(cpu_result.noise_variance.has_value());
  ASSERT_TRUE(gpu_result.noise_variance.has_value());
  EXPECT_LT(relative_delta(cpu_result.noise_variance.value(), gpu_result.noise_variance.value()), 1.5e-1F);

  EXPECT_NEAR(cpu_result.time_alignment.time_alignment, gpu_result.time_alignment.time_alignment, 5.0e-9);
}

void run_srs_gpu_correctness_case(const srs_test_profile& profile, grid_mode mode, float snr_db = 45.0F)
{
  if (!is_srs_estimator_acceleration_available()) {
    GTEST_SKIP() << "CUDA SRS estimator is not available.";
  }

  srs_estimator_configuration    config = make_srs_config(profile);
  std::unique_ptr<resource_grid> grid   = create_test_grid(config, mode);
  ASSERT_NE(grid, nullptr);
  if (mode == grid_mode::cuda_visible) {
    ASSERT_TRUE(grid->get_reader().supports_device_grid_reading());
  }

  populate_srs_grid(*grid, config, snr_db, 0x5eed1234U + profile.nof_rx_ports * 17U + profile.nof_tx_ports * 101U);

  std::shared_ptr<srs_estimator_factory> cpu_factory = create_test_srs_factory("disabled");
  std::shared_ptr<srs_estimator_factory> gpu_factory = create_test_srs_factory("enabled");
  ASSERT_NE(cpu_factory, nullptr);
  ASSERT_NE(gpu_factory, nullptr);

  std::unique_ptr<srs_estimator> cpu_estimator = cpu_factory->create();
  std::unique_ptr<srs_estimator> gpu_estimator = gpu_factory->create();
  ASSERT_NE(cpu_estimator, nullptr);
  ASSERT_NE(gpu_estimator, nullptr);

  srs_estimator_result cpu_result = cpu_estimator->estimate(grid->get_reader(), config);
  srs_estimator_result gpu_result = gpu_estimator->estimate(grid->get_reader(), config);
  assert_srs_results_match(
      cpu_result, gpu_result, make_expected_channel_matrix(profile.nof_rx_ports, profile.nof_tx_ports));
}

/// \brief Alternates two host grids with different channel gains through a single GPU estimator.
///
/// A pooled estimator serves every uplink processor, so consecutive calls arrive with different grids. Each result must
/// reflect the grid it was given, not the one staged by the previous call. Both estimators report a noise-normalized
/// channel matrix, so the 6 dB gain difference shows up in EPRE and RSRP rather than in the channel coefficients.
void run_srs_gpu_alternating_grids_case(const srs_test_profile& profile)
{
  if (!is_srs_estimator_acceleration_available()) {
    GTEST_SKIP() << "CUDA SRS estimator is not available.";
  }

  srs_estimator_configuration    config = make_srs_config(profile);
  std::unique_ptr<resource_grid> grid_a = create_test_grid(config, grid_mode::host);
  std::unique_ptr<resource_grid> grid_b = create_test_grid(config, grid_mode::host);
  ASSERT_NE(grid_a, nullptr);
  ASSERT_NE(grid_b, nullptr);

  static constexpr float gain_a = 1.0F;
  static constexpr float gain_b = 0.5F;
  populate_srs_grid(*grid_a, config, 45.0F, 0x5eed1234U, gain_a);
  populate_srs_grid(*grid_b, config, 45.0F, 0x5eed5678U, gain_b);

  std::shared_ptr<srs_estimator_factory> cpu_factory = create_test_srs_factory("disabled");
  std::shared_ptr<srs_estimator_factory> gpu_factory = create_test_srs_factory("enabled");
  ASSERT_NE(cpu_factory, nullptr);
  ASSERT_NE(gpu_factory, nullptr);

  std::unique_ptr<srs_estimator> cpu_estimator = cpu_factory->create();
  std::unique_ptr<srs_estimator> gpu_estimator = gpu_factory->create();
  ASSERT_NE(cpu_estimator, nullptr);
  ASSERT_NE(gpu_estimator, nullptr);

  srs_estimator_result cpu_result_a = cpu_estimator->estimate(grid_a->get_reader(), config);
  srs_estimator_result cpu_result_b = cpu_estimator->estimate(grid_b->get_reader(), config);
  srs_channel_matrix   expected_a   = make_expected_channel_matrix(profile.nof_rx_ports, profile.nof_tx_ports, gain_a);
  srs_channel_matrix   expected_b   = make_expected_channel_matrix(profile.nof_rx_ports, profile.nof_tx_ports, gain_b);

  // A, B, A: the second call switches grids and the third switches back.
  for (unsigned i_call = 0; i_call != 3; ++i_call) {
    bool                        use_a  = (i_call % 2) == 0;
    const resource_grid&        grid   = use_a ? *grid_a : *grid_b;
    const srs_estimator_result& cpu    = use_a ? cpu_result_a : cpu_result_b;
    const srs_channel_matrix&   expect = use_a ? expected_a : expected_b;
    SCOPED_TRACE(fmt::format("call {} on grid {}", i_call, use_a ? 'A' : 'B'));

    srs_estimator_result gpu_result = gpu_estimator->estimate(grid.get_reader(), config);
    assert_srs_results_match(cpu, gpu_result, expect);
  }
}

srs_test_profile make_multi_port_profile()
{
  srs_test_profile profile;
  profile.nof_rx_ports        = 2;
  profile.nof_tx_ports        = 2;
  profile.rx_ports            = {0, 1, 2, 3};
  profile.scs                 = subcarrier_spacing::kHz30;
  profile.nof_symbols         = 4;
  profile.start_symbol        = 10;
  profile.comb_size           = tx_comb_size::n2;
  profile.comb_offset         = 1;
  profile.cyclic_shift        = 3;
  profile.configuration_index = 0;
  profile.bandwidth_index     = 0;
  profile.freq_position       = 0;
  profile.freq_shift          = 0;
  profile.sequence_id         = 0;
  return profile;
}

/// Gets a single-port, single-symbol profile whose pilot count is far below the GPU workload threshold.
srs_test_profile make_small_profile()
{
  srs_test_profile profile;
  profile.nof_rx_ports        = 1;
  profile.nof_tx_ports        = 1;
  profile.rx_ports            = {3, 0, 0, 0};
  profile.scs                 = subcarrier_spacing::kHz15;
  profile.nof_symbols         = 1;
  profile.start_symbol        = 13;
  profile.comb_size           = tx_comb_size::n2;
  profile.comb_offset         = 1;
  profile.cyclic_shift        = 3;
  profile.configuration_index = 0;
  profile.bandwidth_index     = 0;
  profile.freq_position       = 0;
  profile.freq_shift          = 0;
  profile.sequence_id         = 11;
  return profile;
}

/// Creates a direction-of-arrival estimator factory for a uniform linear array of four antennas.
std::shared_ptr<doa_estimator_factory> create_test_doa_factory()
{
  doa_estimator_configuration config = {
      .nof_antennas = 4, .antenna_distance_over_wavelength = 0.5F, .cross_polarized = false};
  std::shared_ptr<doa_estimator_factory> factory = create_doa_estimator_factory(config);
  EXPECT_NE(factory, nullptr);
  return factory;
}

/// Profile of an SRS received by four antennas, as direction-of-arrival estimation needs.
srs_test_profile make_doa_profile(const std::array<unsigned, 4>& rx_ports)
{
  srs_test_profile profile;
  profile.nof_rx_ports        = 4;
  profile.nof_tx_ports        = 1;
  profile.rx_ports            = rx_ports;
  profile.scs                 = subcarrier_spacing::kHz30;
  profile.nof_symbols         = 2;
  profile.start_symbol        = 12;
  profile.comb_size           = tx_comb_size::n2;
  profile.comb_offset         = 0;
  profile.cyclic_shift        = 0;
  profile.configuration_index = 6;
  profile.bandwidth_index     = 0;
  profile.freq_position       = 0;
  profile.freq_shift          = 0;
  profile.sequence_id         = 5;
  return profile;
}

/// Estimates the SRS of \c profile with direction of arrival, on a new grid of the given type.
srs_estimator_result estimate_with_doa(const srs_test_profile& profile, grid_mode mode, const std::string& acceleration)
{
  srs_estimator_configuration    config = make_srs_config(profile);
  std::unique_ptr<resource_grid> grid   = create_test_grid(config, mode);
  EXPECT_NE(grid, nullptr);
  // The channel only depends on the position of the port in the request, so equivalent requests on other grid ports
  // receive the same signal.
  populate_srs_grid(*grid, config, 30.0F, 0xd0a5eedU);

  std::unique_ptr<srs_estimator> estimator = create_test_srs_factory(acceleration, create_test_doa_factory())->create();
  EXPECT_NE(estimator, nullptr);
  return estimator->estimate(grid->get_reader(), config);
}

void assert_doa_results_match(const srs_estimator_result& expected, const srs_estimator_result& actual)
{
  ASSERT_TRUE(expected.doa_result.has_value());
  ASSERT_TRUE(actual.doa_result.has_value()) << "The direction of arrival was not estimated.";
  const auto& expected_components = expected.doa_result->doa_components;
  const auto& actual_components   = actual.doa_result->doa_components;
  ASSERT_FALSE(expected_components.empty());
  ASSERT_EQ(expected_components.size(), actual_components.size());
  for (unsigned i_component = 0; i_component != expected_components.size(); ++i_component) {
    EXPECT_NEAR(expected_components[i_component].broadside_angle_degrees,
                actual_components[i_component].broadside_angle_degrees,
                1e-3F);
    EXPECT_NEAR(expected_components[i_component].spectrum_strength,
                actual_components[i_component].spectrum_strength,
                1e-3F * std::abs(expected_components[i_component].spectrum_strength));
  }
}

/// Creates the CUDA SRS estimator directly, so a test can see which path produced each estimate.
std::unique_ptr<srs_estimator_cuda_impl> create_cuda_estimator(bool force_gpu_path)
{
  std::unique_ptr<srs_estimator> software_estimator = create_test_srs_factory("disabled")->create();
  EXPECT_NE(software_estimator, nullptr);
  return std::make_unique<srs_estimator_cuda_impl>(create_low_papr_sequence_generator_sw_factory()->create(),
                                                   std::move(software_estimator),
                                                   nullptr,
                                                   MAX_NOF_PRBS,
                                                   force_gpu_path);
}

/// \brief Estimates a configuration below the GPU workload threshold in \c auto mode.
///
/// The estimate must be complete and match the software estimator, whichever path produces it. \c expect_gpu tells
/// whether the GPU or the software estimator is expected to produce it.
void run_srs_auto_below_threshold_case(grid_mode mode, bool expect_gpu)
{
  if (!is_srs_estimator_acceleration_available()) {
    GTEST_SKIP() << "CUDA SRS estimator is not available.";
  }

  // Pin the threshold at its default and make sure nothing forces the GPU, whatever the environment says.
  scoped_env_var min_pilot_re_var("OCUDU_SRS_ACCELERATION_MIN_PILOT_RE");
  scoped_env_var force_var("OCUDU_SRS_ACCELERATION_FORCE");
  ASSERT_TRUE(min_pilot_re_var.set("4096"));
  ASSERT_TRUE(force_var.set("0"));

  srs_test_profile               profile = make_small_profile();
  srs_estimator_configuration    config  = make_srs_config(profile);
  std::unique_ptr<resource_grid> grid    = create_test_grid(config, mode);
  ASSERT_NE(grid, nullptr);
  ASSERT_EQ(grid->get_reader().supports_device_grid_reading(), mode == grid_mode::cuda_visible);
  populate_srs_grid(*grid, config, 45.0F, 0x5eed4321U);

  std::unique_ptr<srs_estimator>           cpu_estimator = create_test_srs_factory("disabled")->create();
  std::unique_ptr<srs_estimator_cuda_impl> gpu_estimator = create_cuda_estimator(/*force_gpu_path=*/false);
  ASSERT_NE(cpu_estimator, nullptr);
  ASSERT_TRUE(gpu_estimator->is_gpu_available());

  srs_estimator_result cpu_result = cpu_estimator->estimate(grid->get_reader(), config);
  srs_estimator_result gpu_result = gpu_estimator->estimate(grid->get_reader(), config);

  ASSERT_EQ(gpu_result.channel_matrix.get_nof_rx_ports(), profile.nof_rx_ports);
  ASSERT_EQ(gpu_result.channel_matrix.get_nof_tx_ports(), profile.nof_tx_ports);
  assert_srs_results_match(
      cpu_result, gpu_result, make_expected_channel_matrix(profile.nof_rx_ports, profile.nof_tx_ports));
  EXPECT_EQ(gpu_estimator->get_nof_host_fallbacks(), expect_gpu ? 0U : 1U);
  EXPECT_EQ(gpu_estimator->get_nof_dropped_estimates(), 0U);
}

/// Gets a configuration with more receive ports than the GPU estimator supports.
srs_estimator_configuration make_too_many_rx_ports_config()
{
  srs_estimator_configuration config = make_srs_config(make_small_profile());
  config.ports.resize(SRS_ESTIMATOR_MAX_RX_PORTS + 1);
  std::iota(config.ports.begin(), config.ports.end(), 0U);
  return config;
}
} // namespace

TEST(srs_estimator_cuda_test, forced_gpu_host_grid_matches_cpu_and_known_channel)
{
  srs_test_profile profile;
  profile.nof_rx_ports        = 1;
  profile.nof_tx_ports        = 1;
  profile.rx_ports            = {0, 1, 2, 3};
  profile.scs                 = subcarrier_spacing::kHz30;
  profile.nof_symbols         = 4;
  profile.start_symbol        = 10;
  profile.comb_size           = tx_comb_size::n2;
  profile.comb_offset         = 1;
  profile.cyclic_shift        = 3;
  profile.configuration_index = 0;
  profile.bandwidth_index     = 0;
  profile.freq_position       = 0;
  profile.freq_shift          = 0;
  profile.sequence_id         = 0;

  run_srs_gpu_correctness_case(profile, grid_mode::host);
}

TEST(srs_estimator_cuda_test, forced_gpu_cuda_visible_grid_matches_cpu_for_sparse_rx_ports)
{
  srs_test_profile profile;
  profile.nof_rx_ports        = 1;
  profile.nof_tx_ports        = 1;
  profile.rx_ports            = {3, 0, 0, 0};
  profile.scs                 = subcarrier_spacing::kHz15;
  profile.nof_symbols         = 1;
  profile.start_symbol        = 13;
  profile.comb_size           = tx_comb_size::n2;
  profile.comb_offset         = 1;
  profile.cyclic_shift        = 3;
  profile.configuration_index = 0;
  profile.bandwidth_index     = 0;
  profile.freq_position       = 0;
  profile.freq_shift          = 0;
  profile.sequence_id         = 11;

  run_srs_gpu_correctness_case(profile, grid_mode::cuda_visible);
}

TEST(srs_estimator_cuda_test, forced_gpu_alternating_host_grids_match_cpu)
{
  srs_test_profile profile;
  profile.nof_rx_ports        = 1;
  profile.nof_tx_ports        = 1;
  profile.rx_ports            = {0, 1, 2, 3};
  profile.scs                 = subcarrier_spacing::kHz30;
  profile.nof_symbols         = 4;
  profile.start_symbol        = 10;
  profile.comb_size           = tx_comb_size::n2;
  profile.comb_offset         = 1;
  profile.cyclic_shift        = 3;
  profile.configuration_index = 0;
  profile.bandwidth_index     = 0;
  profile.freq_position       = 0;
  profile.freq_shift          = 0;
  profile.sequence_id         = 0;

  run_srs_gpu_alternating_grids_case(profile);
}

// Above 40 dB the channel matrix is normalized by the SNR floor, which is derived from the RSRP summed over all port
// pairs. With a single port pair the sum and the average coincide, so only a multi-port case can tell them apart.
TEST(srs_estimator_cuda_test, forced_gpu_multi_port_matches_cpu_above_snr_floor)
{
  run_srs_gpu_correctness_case(make_multi_port_profile(), grid_mode::host, 45.0F);
}

TEST(srs_estimator_cuda_test, forced_gpu_multi_port_matches_cpu_below_snr_floor)
{
  run_srs_gpu_correctness_case(make_multi_port_profile(), grid_mode::host, 30.0F);
}

// Below the workload threshold, a host grid is estimated by the software estimator. The GPU estimator used to return
// an empty estimate here instead.
TEST(srs_estimator_cuda_test, auto_host_grid_below_threshold_falls_back_to_software)
{
  run_srs_auto_below_threshold_case(grid_mode::host, /*expect_gpu=*/false);
}

// A CUDA-visible grid stays on the GPU below the threshold: its data is already on the device.
TEST(srs_estimator_cuda_test, auto_cuda_visible_grid_below_threshold_stays_on_gpu)
{
  run_srs_auto_below_threshold_case(grid_mode::cuda_visible, /*expect_gpu=*/true);
}

TEST(srs_estimator_cuda_test, auto_unsupported_configuration_falls_back_to_software)
{
  if (!is_srs_estimator_acceleration_available()) {
    GTEST_SKIP() << "CUDA SRS estimator is not available.";
  }

  srs_estimator_configuration    config = make_too_many_rx_ports_config();
  std::unique_ptr<resource_grid> grid   = create_test_grid(config, grid_mode::host);
  ASSERT_NE(grid, nullptr);
  populate_srs_grid(*grid, config, 45.0F, 0x5eed8765U);

  std::unique_ptr<srs_estimator>           cpu_estimator = create_test_srs_factory("disabled")->create();
  std::unique_ptr<srs_estimator_cuda_impl> gpu_estimator = create_cuda_estimator(/*force_gpu_path=*/false);
  ASSERT_NE(cpu_estimator, nullptr);

  srs_estimator_result cpu_result = cpu_estimator->estimate(grid->get_reader(), config);
  srs_estimator_result gpu_result = gpu_estimator->estimate(grid->get_reader(), config);

  ASSERT_EQ(gpu_result.channel_matrix.get_nof_rx_ports(), config.ports.size());
  assert_srs_results_match(cpu_result, gpu_result, make_expected_channel_matrix(config.ports.size(), 1));
  EXPECT_EQ(gpu_estimator->get_nof_host_fallbacks(), 1U);
  EXPECT_EQ(gpu_estimator->get_nof_dropped_estimates(), 0U);
}

// The forced GPU path drops an estimate it cannot produce instead of handing it to the software estimator.
TEST(srs_estimator_cuda_test, forced_gpu_unsupported_configuration_is_dropped)
{
  if (!is_srs_estimator_acceleration_available()) {
    GTEST_SKIP() << "CUDA SRS estimator is not available.";
  }

  srs_estimator_configuration    config = make_too_many_rx_ports_config();
  std::unique_ptr<resource_grid> grid   = create_test_grid(config, grid_mode::host);
  ASSERT_NE(grid, nullptr);
  populate_srs_grid(*grid, config, 45.0F, 0x5eed8765U);

  std::unique_ptr<srs_estimator_cuda_impl> gpu_estimator = create_cuda_estimator(/*force_gpu_path=*/true);

  srs_estimator_result result = gpu_estimator->estimate(grid->get_reader(), config);

  EXPECT_EQ(result.channel_matrix.get_nof_rx_ports(), 0U);
  EXPECT_FALSE(result.rsrp_dB.has_value());
  EXPECT_EQ(gpu_estimator->get_nof_dropped_estimates(), 1U);
  EXPECT_EQ(gpu_estimator->get_nof_host_fallbacks(), 0U);
}

// The GPU gathers the received SRS of the first transmit port for the direction-of-arrival estimator, which must then
// estimate the same direction as from the samples the software estimator reads. The forced GPU path drops an estimate
// it cannot produce, so the result comes from the GPU.
TEST(srs_estimator_cuda_test, forced_gpu_estimates_the_direction_of_arrival_like_cpu)
{
  if (!is_srs_estimator_acceleration_available()) {
    GTEST_SKIP() << "CUDA SRS estimator is not available.";
  }

  srs_test_profile profile = make_doa_profile({0, 1, 2, 3});
  for (grid_mode mode : {grid_mode::host, grid_mode::cuda_visible}) {
    SCOPED_TRACE((mode == grid_mode::host) ? "host grid" : "CUDA-visible grid");
    assert_doa_results_match(estimate_with_doa(profile, mode, "disabled"), estimate_with_doa(profile, mode, "enabled"));
  }
}

// The samples of each receive port come from the grid port the request names, in request order.
TEST(srs_estimator_cuda_test, forced_gpu_direction_of_arrival_follows_the_requested_ports)
{
  if (!is_srs_estimator_acceleration_available()) {
    GTEST_SKIP() << "CUDA SRS estimator is not available.";
  }

  srs_estimator_result reference = estimate_with_doa(make_doa_profile({0, 1, 2, 3}), grid_mode::host, "disabled");
  assert_doa_results_match(reference, estimate_with_doa(make_doa_profile({3, 1, 0, 2}), grid_mode::host, "enabled"));
}
