// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "pdxch_baseband_modulator_accelerator.h"
#include "resource_grid_cuda_visible_impl.h"
#include "ocudu/gateways/baseband/buffer/baseband_gateway_buffer_dynamic.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/phy/lower/modulation/modulation_factories.h"
#include "ocudu/phy/lower/sampling_rate.h"
#include "ocudu/phy/support/resource_grid.h"
#include "ocudu/phy/support/resource_grid_writer.h"
#include "ocudu/phy/support/support_factories.h"
#include "ocudu/ran/antenna_topology.h"
#include "ocudu/ran/beamforming/beam_weights_codebook_generator.h"
#include "ocudu/ran/cyclic_prefix.h"
#include "ocudu/ran/resource_block.h"
#include "ocudu/ran/subcarrier_spacing.h"
#include "ocudu/support/math/math_utils.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cuda_runtime.h>
#include <gtest/gtest.h>
#include <numeric>
#include <random>
#include <vector>

using namespace ocudu;

namespace {

/// Where the accelerator reads the resource grid from.
enum class grid_location { host, device };

std::ostream& operator<<(std::ostream& os, grid_location location)
{
  return os << ((location == grid_location::host) ? "host" : "device");
}

/// Resource elements written in one beam, i.e. one resource grid port.
struct beam_allocation {
  unsigned     port;
  unsigned     first_symbol;
  unsigned     nof_symbols;
  crb_interval crbs;
};

bool cuda_available()
{
  int nof_devices = 0;
  if ((cudaGetDeviceCount(&nof_devices) != cudaSuccess) || (nof_devices == 0)) {
    cudaGetLastError();
    return false;
  }
  return true;
}

/// Creates a codebook with random weights of unit magnitude.
beam_weights_codebook make_random_codebook(unsigned nof_beams, unsigned nof_antennas, std::mt19937& rgen)
{
  std::uniform_real_distribution<float> phase_dist(-M_PI, M_PI);
  std::vector<cf_t>                     coefficients(nof_beams * nof_antennas);
  for (cf_t& coefficient : coefficients) {
    coefficient = std::polar(1.0F, phase_dist(rgen));
  }
  return beam_weights_codebook(coefficients, nof_beams, nof_antennas);
}

/// \brief Compares the CUDA PDxCH baseband modulator with the software OFDM modulator.
///
/// The software modulator applies the beamforming codebook antenna by antenna, which makes it the reference for the
/// beam combining done on the GPU.
class PdxchBasebandModulatorCudaFixture : public ::testing::TestWithParam<grid_location>
{
protected:
  static constexpr unsigned bandwidth_rb   = 51;
  static constexpr unsigned rg_size        = bandwidth_rb * NOF_SUBCARRIERS_PER_RB;
  static constexpr unsigned slot_index_sf  = 1;
  static constexpr double   center_freq_Hz = 3.4101e9;
  /// The two implementations accumulate in a different order, so allow a couple of least significant bits.
  static constexpr int max_lsb_error = 2;

  void SetUp() override
  {
    if (!cuda_available()) {
      GTEST_SKIP() << "CUDA is not available.";
    }
    ocudulog::init();
  }

  /// Creates a zeroed resource grid at the location under test.
  std::unique_ptr<resource_grid> create_grid(unsigned nof_ports) const
  {
    std::unique_ptr<resource_grid> grid;
    if (GetParam() == grid_location::device) {
      resource_grid_cuda_visible_factory factory(resource_grid_cuda_visible_factory::direction::downlink);
      grid = factory.create(nof_ports, nof_symbols, rg_size);
      EXPECT_TRUE(grid->get_reader().supports_device_grid_reading());
    } else {
      grid = create_resource_grid_factory()->create(nof_ports, nof_symbols, rg_size);
    }
    grid->set_all_zero();
    return grid;
  }

  /// \brief Writes random QPSK symbols in the given allocations.
  ///
  /// A device grid is written the way a GPU producer writes it, through the device grid mapping of the writer, so the
  /// accelerator reads it from device memory.
  void fill_grid(resource_grid& grid, span<const beam_allocation> allocations)
  {
    std::uniform_int_distribution<int> bit_dist(0, 1);
    auto                               random_qpsk = [&]() {
      return cf_t(bit_dist(rgen) ? M_SQRT1_2 : -M_SQRT1_2, bit_dist(rgen) ? M_SQRT1_2 : -M_SQRT1_2);
    };

    resource_grid_writer& writer = grid.get_writer();
    if (GetParam() == grid_location::host) {
      for (const beam_allocation& allocation : allocations) {
        std::vector<cf_t> symbols(allocation.crbs.length() * NOF_SUBCARRIERS_PER_RB);
        for (unsigned i_symbol = allocation.first_symbol; i_symbol != allocation.first_symbol + allocation.nof_symbols;
             ++i_symbol) {
          std::generate(symbols.begin(), symbols.end(), random_qpsk);
          writer.put(allocation.port, i_symbol, allocation.crbs.start() * NOF_SUBCARRIERS_PER_RB, symbols);
        }
      }
      return;
    }

    // Device grid layout: port, then symbol, then subcarrier.
    std::vector<cbf16_t> host_grid(static_cast<size_t>(writer.get_nof_ports()) * nof_symbols * rg_size);
    for (const beam_allocation& allocation : allocations) {
      for (unsigned i_symbol = allocation.first_symbol; i_symbol != allocation.first_symbol + allocation.nof_symbols;
           ++i_symbol) {
        auto first = host_grid.begin() + (allocation.port * nof_symbols + i_symbol) * rg_size +
                     allocation.crbs.start() * NOF_SUBCARRIERS_PER_RB;
        std::generate(first, first + allocation.crbs.length() * NOF_SUBCARRIERS_PER_RB, [&]() {
          return to_cbf16(random_qpsk());
        });
      }
    }

    cudaStream_t stream = nullptr;
    ASSERT_EQ(cudaStreamCreate(&stream), cudaSuccess);
    ASSERT_TRUE(writer.prepare_device_grid_mapping(stream));
    ASSERT_EQ(cudaMemcpyAsync(writer.get_device_grid_bf16(),
                              host_grid.data(),
                              host_grid.size() * sizeof(cbf16_t),
                              cudaMemcpyHostToDevice,
                              stream),
              cudaSuccess);
    for (const beam_allocation& allocation : allocations) {
      std::array<unsigned, 1> ports = {allocation.port};
      ASSERT_TRUE(writer.on_device_grid_mapping_enqueued(
          stream,
          {.ports   = ports,
           .symbols = {allocation.first_symbol, allocation.first_symbol + allocation.nof_symbols},
           .crbs    = allocation.crbs}));
    }
    ASSERT_EQ(cudaStreamSynchronize(stream), cudaSuccess);
    ASSERT_EQ(cudaStreamDestroy(stream), cudaSuccess);
  }

  /// Creates the accelerator for the given number of antenna ports, with the gain the lower PHY uses.
  std::unique_ptr<pdxch_baseband_modulator_accelerator> create_accelerator(unsigned nof_antennas) const
  {
    return create_pdxch_baseband_modulator_accelerator_cuda(
        scs, cp, srate, bandwidth_rb, center_freq_Hz, nof_antennas, modulator_gain_dB);
  }

  /// Modulates one slot with the accelerator. Returns false if the accelerator declines it.
  bool modulate_gpu(pdxch_baseband_modulator_accelerator& accelerator,
                    const resource_grid&                  grid,
                    baseband_gateway_buffer_dynamic&      output)
  {
    if (!accelerator.enqueue(output, grid.get_reader(), slot_index_sf * nof_symbols, symbol_sizes)) {
      return false;
    }
    EXPECT_TRUE(accelerator.wait());
    return true;
  }

  /// Asserts that every antenna port of \c gpu_output matches the software modulator applying \c codebook.
  void assert_matches_cpu(const beam_weights_codebook&     codebook,
                          const resource_grid&             grid,
                          baseband_gateway_buffer_dynamic& gpu_output)
  {
    ofdm_factory_generic_configuration     ofdm_config = {.dft_factory = create_dft_processor_factory_generic()};
    std::unique_ptr<ofdm_symbol_modulator> cpu_modulator =
        create_ofdm_modulator_factory_generic(ofdm_config)
            ->create_ofdm_symbol_modulator({.numerology     = to_numerology_value(scs),
                                            .bw_rb          = bandwidth_rb,
                                            .dft_size       = srate.get_dft_size(scs),
                                            .cp             = cp,
                                            .scale          = convert_dB_to_amplitude(modulator_gain_dB),
                                            .center_freq_Hz = center_freq_Hz});
    ASSERT_NE(cpu_modulator, nullptr);

    for (unsigned i_antenna = 0, nof_antennas = codebook.get_nof_antennas(); i_antenna != nof_antennas; ++i_antenna) {
      span<const ci16_t> gpu_antenna   = gpu_output[i_antenna];
      unsigned           sample_offset = 0;
      for (unsigned i_symbol = 0; i_symbol != nof_symbols; ++i_symbol) {
        std::vector<ci16_t> cpu_symbol(symbol_sizes[i_symbol]);
        cpu_modulator->modulate(cpu_symbol,
                                grid.get_reader(),
                                codebook.get_antenna_coefficients(i_antenna),
                                slot_index_sf * nof_symbols + i_symbol);

        span<const ci16_t> gpu_symbol = gpu_antenna.subspan(sample_offset, symbol_sizes[i_symbol]);
        for (unsigned i_sample = 0; i_sample != cpu_symbol.size(); ++i_sample) {
          ASSERT_LE(std::abs(cpu_symbol[i_sample].real() - gpu_symbol[i_sample].real()), max_lsb_error)
              << "antenna " << i_antenna << ", symbol " << i_symbol << ", sample " << i_sample;
          ASSERT_LE(std::abs(cpu_symbol[i_sample].imag() - gpu_symbol[i_sample].imag()), max_lsb_error)
              << "antenna " << i_antenna << ", symbol " << i_symbol << ", sample " << i_sample;
        }
        sample_offset += symbol_sizes[i_symbol];
      }
    }
  }

  /// Fills a grid with one port per beam of \c codebook, modulates it on the GPU and compares with the software.
  void run_codebook_case(const beam_weights_codebook& codebook, span<const beam_allocation> allocations)
  {
    std::unique_ptr<resource_grid> grid = create_grid(codebook.get_nof_beams());
    ASSERT_NE(grid, nullptr);
    fill_grid(*grid, allocations);

    std::unique_ptr<pdxch_baseband_modulator_accelerator> accelerator = create_accelerator(codebook.get_nof_antennas());
    ASSERT_NE(accelerator, nullptr);
    ASSERT_TRUE(accelerator->set_beam_weights(codebook));

    baseband_gateway_buffer_dynamic output(codebook.get_nof_antennas(), nof_samples);
    output.resize(nof_samples);
    ASSERT_TRUE(modulate_gpu(*accelerator, *grid, output));
    assert_matches_cpu(codebook, *grid, output);
  }

  const subcarrier_spacing    scs               = subcarrier_spacing::kHz30;
  const cyclic_prefix         cp                = cyclic_prefix::NORMAL;
  const sampling_rate         srate             = sampling_rate::from_MHz(23.04);
  const unsigned              nof_symbols       = get_nsymb_per_slot(cp);
  const float                 modulator_gain_dB = -convert_power_to_dB(static_cast<float>(rg_size)) - 6.0F;
  const std::vector<unsigned> symbol_sizes      = [this]() {
    std::vector<unsigned> sizes(nof_symbols);
    for (unsigned i_symbol = 0; i_symbol != nof_symbols; ++i_symbol) {
      unsigned i_symbol_sf = slot_index_sf * nof_symbols + i_symbol;
      sizes[i_symbol]      = srate.get_dft_size(scs) + cp.get_length(i_symbol_sf, scs).to_samples(srate.to_Hz());
    }
    return sizes;
  }();
  const unsigned nof_samples = std::accumulate(symbol_sizes.begin(), symbol_sizes.end(), 0U);
  std::mt19937   rgen{1234};
};

// The identity codebook maps each beam onto the antenna port with the same index.
TEST_P(PdxchBasebandModulatorCudaFixture, identity_codebook_matches_cpu_modulator)
{
  beam_weights_codebook codebook = generate_beam_weights_codebook(antenna_topology::four_ports);
  run_codebook_case(codebook,
                    std::vector<beam_allocation>{{0, 0, 14, {0, bandwidth_rb}},
                                                 {1, 0, 14, {0, bandwidth_rb}},
                                                 {2, 2, 10, {5, 30}},
                                                 {3, 0, 14, {0, bandwidth_rb}}});
}

// A codebook with fewer beams than the GPU combines is mapped whole.
TEST_P(PdxchBasebandModulatorCudaFixture, small_codebook_combines_every_beam)
{
  beam_weights_codebook codebook = make_random_codebook(6, 4, rgen);
  run_codebook_case(codebook,
                    std::vector<beam_allocation>{
                        {0, 0, 14, {0, bandwidth_rb}}, {2, 1, 4, {3, 20}}, {3, 5, 9, {30, 51}}, {5, 0, 14, {10, 40}}});
}

// A codebook with more beams than the GPU combines is reduced to the beams that carry data in the slot. The beams
// below have allocations in the lower half, the upper half and across the center of the band.
TEST_P(PdxchBasebandModulatorCudaFixture, large_codebook_combines_the_beams_used_in_the_slot)
{
  beam_weights_codebook codebook = generate_beam_weights_codebook(antenna_topology::single_panel_two_one);
  ASSERT_GT(codebook.get_nof_beams(), pdxch_cuda_max_nof_beams_per_slot);
  run_codebook_case(
      codebook,
      std::vector<beam_allocation>{
          {1, 0, 14, {0, bandwidth_rb}}, {7, 2, 3, {4, 12}}, {12, 4, 10, {30, 45}}, {19, 0, 1, {20, 32}}});
}

// A slot that uses more beams than the GPU combines is left to the software modulator.
TEST_P(PdxchBasebandModulatorCudaFixture, slot_with_too_many_beams_is_declined)
{
  beam_weights_codebook        codebook = generate_beam_weights_codebook(antenna_topology::single_panel_two_one);
  std::vector<beam_allocation> allocations;
  for (unsigned i_beam = 0; i_beam != pdxch_cuda_max_nof_beams_per_slot + 1; ++i_beam) {
    allocations.push_back({i_beam, i_beam % nof_symbols, 1, {i_beam, i_beam + 1}});
  }

  std::unique_ptr<resource_grid> grid = create_grid(codebook.get_nof_beams());
  ASSERT_NE(grid, nullptr);
  fill_grid(*grid, allocations);

  std::unique_ptr<pdxch_baseband_modulator_accelerator> accelerator = create_accelerator(codebook.get_nof_antennas());
  ASSERT_NE(accelerator, nullptr);
  ASSERT_TRUE(accelerator->set_beam_weights(codebook));

  baseband_gateway_buffer_dynamic output(codebook.get_nof_antennas(), nof_samples);
  output.resize(nof_samples);
  ASSERT_FALSE(modulate_gpu(*accelerator, *grid, output));

  // The accelerator stays usable for the next slot.
  allocations.pop_back();
  grid->set_all_zero();
  fill_grid(*grid, allocations);
  ASSERT_TRUE(modulate_gpu(*accelerator, *grid, output));
  assert_matches_cpu(codebook, *grid, output);
}

INSTANTIATE_TEST_SUITE_P(PdxchBasebandModulatorCuda,
                         PdxchBasebandModulatorCudaFixture,
                         ::testing::Values(grid_location::host, grid_location::device));

// The accelerator only takes codebooks for its number of antenna ports.
TEST(PdxchBasebandModulatorCuda, rejects_a_codebook_for_other_antenna_ports)
{
  if (!cuda_available()) {
    GTEST_SKIP() << "CUDA is not available.";
  }

  auto accelerator = create_pdxch_baseband_modulator_accelerator_cuda(
      subcarrier_spacing::kHz30, cyclic_prefix::NORMAL, sampling_rate::from_MHz(23.04), 51, 3.5e9, 4, -30.0F);
  ASSERT_NE(accelerator, nullptr);
  EXPECT_FALSE(accelerator->set_beam_weights(generate_beam_weights_codebook(antenna_topology::two_port)));
  EXPECT_TRUE(accelerator->set_beam_weights(generate_beam_weights_codebook(antenna_topology::four_ports)));
}

} // namespace
