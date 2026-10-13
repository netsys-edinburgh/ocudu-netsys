// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/ocuduvec/copy.h"
#include "ocudu/phy/lower/modulation/modulation_factories.h"
#include "ocudu/support/error_handling.h"
#include <random>

namespace ocudu {

class ofdm_symbol_demodulator_spy : public ofdm_symbol_demodulator
{
public:
  struct demodulate_entry {
    std::vector<ci16_t>         input;
    const resource_grid_writer* grid;
    unsigned                    port_index;
    unsigned                    symbol_index;
  };

  ofdm_symbol_demodulator_spy(const ofdm_demodulator_configuration& config) :
    rgen(0), dist(-1, +1), configuration(config)
  {
    // Do nothing.
  }

  void set_center_frequency(double center_frequency_Hz) override {}

  unsigned get_symbol_size(unsigned symbol_index) const override
  {
    unsigned sampling_rate_Hz = (configuration.dft_size * 15000) << configuration.numerology;
    return configuration.cp.get_length(symbol_index, to_subcarrier_spacing(configuration.numerology))
               .to_samples(sampling_rate_Hz) +
           configuration.dft_size;
  }

  void
  demodulate(resource_grid_writer& grid, span<const ci16_t> input, unsigned port_index, unsigned symbol_index) override
  {
    report_fatal_error_if_not((input.size()) == (get_symbol_size(symbol_index)),
                              "input.size() != get_symbol_size(symbol_index)");

    demodulate_entries.emplace_back();
    demodulate_entry& entry = demodulate_entries.back();
    entry.grid              = &grid;
    entry.port_index        = port_index;
    entry.symbol_index      = symbol_index;

    entry.input.resize(input.size());
    ocuduvec::copy(entry.input, input);
  }

  /// Describes a call to demodulate_ci16_ports().
  struct demodulate_ports_entry {
    const resource_grid_writer*      grid;
    std::vector<std::vector<ci16_t>> inputs;
    float                            input_scale;
    std::vector<unsigned>            port_indices;
    unsigned                         symbol_index;
  };

  bool demodulate_ci16_ports(resource_grid_writer&     grid,
                             span<const ci16_t* const> inputs,
                             unsigned                  nof_samples,
                             float                     input_scale,
                             span<const unsigned>      port_indices,
                             unsigned                  symbol_index) override
  {
    demodulate_ports_entry& entry = demodulate_ports_entries.emplace_back();
    entry.grid                    = &grid;
    entry.input_scale             = input_scale;
    entry.port_indices.assign(port_indices.begin(), port_indices.end());
    entry.symbol_index = symbol_index;
    for (const ci16_t* input : inputs) {
      entry.inputs.emplace_back(input, input + nof_samples);
    }
    return accept_ports;
  }

  /// Sets whether demodulate_ci16_ports() demodulates the ports or declines them.
  void set_accept_ports(bool accept) { accept_ports = accept; }

  /// Describes a call to demodulate_ci16_symbol_batch_ports().
  struct demodulate_batch_entry {
    const resource_grid_writer* grid;
    unsigned                    first_symbol_index;
    unsigned                    nof_symbols;
    std::vector<unsigned>       port_indices;
    /// Samples of each port of each symbol, symbol-major.
    std::vector<std::vector<ci16_t>> inputs;
  };

  unsigned get_max_ci16_symbol_batch_size() const override { return max_symbol_batch_size; }

  bool demodulate_ci16_symbol_batch_ports(resource_grid_writer&     grid,
                                          span<const ci16_t* const> inputs,
                                          span<const unsigned>      symbol_sample_counts,
                                          float                     input_scale,
                                          span<const unsigned>      port_indices,
                                          unsigned                  first_symbol_index,
                                          unsigned                  nof_symbols) override
  {
    demodulate_batch_entry& entry = demodulate_batch_entries.emplace_back();
    entry.grid                    = &grid;
    entry.first_symbol_index      = first_symbol_index;
    entry.nof_symbols             = nof_symbols;
    entry.port_indices.assign(port_indices.begin(), port_indices.end());
    for (unsigned i_input = 0, nof_inputs = inputs.size(); i_input != nof_inputs; ++i_input) {
      unsigned nof_samples = symbol_sample_counts[i_input / port_indices.size()];
      entry.inputs.emplace_back(inputs[i_input], inputs[i_input] + nof_samples);
    }
    return accept_batches;
  }

  /// Sets the number of OFDM symbols get_max_ci16_symbol_batch_size() reports.
  void set_max_symbol_batch_size(unsigned size) { max_symbol_batch_size = size; }

  /// Sets whether demodulate_ci16_symbol_batch_ports() demodulates the batch or declines it.
  void set_accept_batches(bool accept) { accept_batches = accept; }

  const std::vector<demodulate_batch_entry>& get_demodulate_batch_entries() const { return demodulate_batch_entries; }

  const std::vector<demodulate_ports_entry>& get_demodulate_ports_entries() const { return demodulate_ports_entries; }

  void clear_demodulate_entries()
  {
    demodulate_entries.clear();
    demodulate_ports_entries.clear();
    demodulate_batch_entries.clear();
  }

  const ofdm_demodulator_configuration& get_configuration() const { return configuration; }

  const std::vector<demodulate_entry>& get_demodulate_entries() const { return demodulate_entries; }

private:
  std::mt19937                          rgen;
  std::uniform_real_distribution<float> dist;
  ofdm_demodulator_configuration        configuration;
  std::vector<demodulate_entry>         demodulate_entries;
  std::vector<demodulate_ports_entry>   demodulate_ports_entries;
  bool                                  accept_ports = false;
  std::vector<demodulate_batch_entry>   demodulate_batch_entries;
  unsigned                              max_symbol_batch_size = 1;
  bool                                  accept_batches        = false;
};

class ofdm_demodulator_factory_spy : public ofdm_demodulator_factory
{
public:
  std::unique_ptr<ofdm_symbol_demodulator>
  create_ofdm_symbol_demodulator(const ofdm_demodulator_configuration& config) override
  {
    std::unique_ptr<ofdm_symbol_demodulator_spy> ptr = std::make_unique<ofdm_symbol_demodulator_spy>(config);
    demodulators.push_back(ptr.get());
    return ptr;
  }
  std::unique_ptr<ofdm_slot_demodulator>
  create_ofdm_slot_demodulator(const ofdm_demodulator_configuration& config) override
  {
    return nullptr;
  }

  std::vector<ofdm_symbol_demodulator_spy*>& get_demodulators() { return demodulators; }

private:
  std::vector<ofdm_symbol_demodulator_spy*> demodulators;
};

} // namespace ocudu
