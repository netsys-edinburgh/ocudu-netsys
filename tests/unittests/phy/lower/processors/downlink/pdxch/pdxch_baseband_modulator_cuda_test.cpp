// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "../../../../support/resource_grid_test_doubles.h"
#include "pdxch_baseband_modulator_accelerator.h"
#include "ocudu/gateways/baseband/buffer/baseband_gateway_buffer_dynamic.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/phy/lower/modulation/modulation_factories.h"
#include "ocudu/phy/lower/sampling_rate.h"
#include "ocudu/phy/support/support_factories.h"
#include "ocudu/ran/cyclic_prefix.h"
#include "ocudu/ran/resource_block.h"
#include "ocudu/ran/subcarrier_spacing.h"
#include "ocudu/support/math/math_utils.h"
#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <numeric>
#include <vector>

using namespace ocudu;

namespace {

std::shared_ptr<dft_processor_factory> make_dft_factory()
{
  std::shared_ptr<dft_processor_factory> dft_factory = create_dft_processor_factory_fftw_fast();
  if (dft_factory == nullptr) {
    dft_factory = create_dft_processor_factory_generic();
  }
  return dft_factory;
}

std::vector<unsigned> make_symbol_sizes(sampling_rate srate, subcarrier_spacing scs, cyclic_prefix cp)
{
  const unsigned        nof_symbols = get_nsymb_per_slot(cp);
  std::vector<unsigned> symbol_sizes(nof_symbols);
  for (unsigned symbol = 0; symbol != nof_symbols; ++symbol) {
    symbol_sizes[symbol] = srate.get_dft_size(scs) + cp.get_length(symbol, scs).to_samples(srate.to_Hz());
  }
  return symbol_sizes;
}

void fill_deterministic_grid(resource_grid_reader_spy& grid_reader,
                             unsigned                  nof_ports,
                             unsigned                  nof_symbols,
                             unsigned                  rg_size)
{
  for (unsigned port = 0; port != nof_ports; ++port) {
    for (unsigned symbol = 0; symbol != nof_symbols; ++symbol) {
      for (unsigned subcarrier = 0; subcarrier != rg_size; ++subcarrier) {
        resource_grid_reader_spy::expected_entry_t entry = {};
        entry.port                                       = port;
        entry.symbol                                     = symbol;
        entry.subcarrier                                 = subcarrier;

        if ((subcarrier % 7) == ((port + symbol) % 7)) {
          const int re_seed = static_cast<int>((port + 3) * (symbol + 5) * (subcarrier + 11) % 29) - 14;
          const int im_seed = static_cast<int>((port + 7) * (symbol + 2) * (subcarrier + 17) % 31) - 15;
          entry.value       = cf_t(static_cast<float>(re_seed) * 4e-4F, static_cast<float>(im_seed) * 4e-4F);
        } else {
          entry.value = cf_t();
        }
        grid_reader.write(entry);
      }
    }
  }
}

/// Runs the CUDA modulator and the software modulator on the same four-port grid and compares their 16-bit outputs.
void run_four_port_output_matches_cpu_modulator(float modulator_gain_dB)
{
  ocudulog::init();

  static constexpr unsigned nof_ports      = 4;
  static constexpr unsigned bandwidth_rb   = 51;
  static constexpr unsigned slot_index_sf  = 1;
  static constexpr double   center_freq_Hz = 3.4101e9;

  const sampling_rate      srate             = sampling_rate::from_MHz(23.04);
  const subcarrier_spacing scs               = subcarrier_spacing::kHz30;
  const cyclic_prefix      cp                = cyclic_prefix::NORMAL;
  const unsigned           nof_symbols       = get_nsymb_per_slot(cp);
  const unsigned           rg_size           = bandwidth_rb * NOF_SUBCARRIERS_PER_RB;
  const unsigned           dft_size          = srate.get_dft_size(scs);
  const unsigned           i_symbol_sf_begin = slot_index_sf * nof_symbols;

  auto accelerator = create_pdxch_baseband_modulator_accelerator_cuda(
      scs, cp, srate, bandwidth_rb, center_freq_Hz, nof_ports, modulator_gain_dB);
  ASSERT_NE(accelerator, nullptr);

  resource_grid_reader_spy grid_reader(nof_ports, nof_symbols, bandwidth_rb);
  fill_deterministic_grid(grid_reader, nof_ports, nof_symbols, rg_size);

  std::vector<unsigned> symbol_sizes = make_symbol_sizes(srate, scs, cp);
  const unsigned        nof_samples  = std::accumulate(symbol_sizes.begin(), symbol_sizes.end(), 0U);

  baseband_gateway_buffer_dynamic gpu_output(nof_ports, nof_samples);
  gpu_output.resize(nof_samples);

  ASSERT_TRUE(accelerator->enqueue(gpu_output, grid_reader, i_symbol_sf_begin, symbol_sizes));
  ASSERT_TRUE(accelerator->wait());

  ofdm_factory_generic_configuration ofdm_config = {.dft_factory = make_dft_factory()};
  ASSERT_NE(ofdm_config.dft_factory, nullptr);
  std::shared_ptr<ofdm_modulator_factory> ofdm_factory = create_ofdm_modulator_factory_generic(ofdm_config);
  ASSERT_NE(ofdm_factory, nullptr);

  std::unique_ptr<ofdm_symbol_modulator> cpu_modulator =
      ofdm_factory->create_ofdm_symbol_modulator({.numerology     = to_numerology_value(scs),
                                                  .bw_rb          = bandwidth_rb,
                                                  .dft_size       = dft_size,
                                                  .cp             = cp,
                                                  .scale          = convert_dB_to_amplitude(modulator_gain_dB),
                                                  .center_freq_Hz = center_freq_Hz});
  ASSERT_NE(cpu_modulator, nullptr);

  // The two implementations accumulate in a different order, so allow a couple of LSBs.
  static constexpr int max_lsb_error = 2;
  unsigned             sample_offset = 0;
  for (unsigned symbol = 0; symbol != nof_symbols; ++symbol) {
    std::vector<ci16_t> cpu_symbol(symbol_sizes[symbol]);

    for (unsigned port = 0; port != nof_ports; ++port) {
      // Identity beamforming: antenna port takes only the grid port with the same index.
      std::vector<cf_t> port_weights(nof_ports, cf_t());
      port_weights[port] = cf_t(1.0F, 0.0F);
      cpu_modulator->modulate(cpu_symbol, grid_reader, port_weights, i_symbol_sf_begin + symbol);

      span<const ci16_t> gpu_symbol = gpu_output[port].subspan(sample_offset, symbol_sizes[symbol]);
      ASSERT_EQ(gpu_symbol.size(), cpu_symbol.size());
      for (unsigned sample = 0; sample != cpu_symbol.size(); ++sample) {
        ASSERT_LE(std::abs(cpu_symbol[sample].real() - gpu_symbol[sample].real()), max_lsb_error)
            << "port " << port << ", symbol " << symbol << ", sample " << sample;
        ASSERT_LE(std::abs(cpu_symbol[sample].imag() - gpu_symbol[sample].imag()), max_lsb_error)
            << "port " << port << ", symbol " << symbol << ", sample " << sample;
      }
    }
    sample_offset += symbol_sizes[symbol];
  }
}

} // namespace

TEST(pdxch_baseband_modulator_cuda, live_20mhz_four_port_output_matches_cpu_modulator)
{
  run_four_port_output_matches_cpu_modulator(0.0F);
}

// The gain the lower PHY derives from the bandwidth and the back-off must be applied as the software modulator scale.
TEST(pdxch_baseband_modulator_cuda, attenuated_output_matches_cpu_modulator)
{
  run_four_port_output_matches_cpu_modulator(-6.0F);
}
