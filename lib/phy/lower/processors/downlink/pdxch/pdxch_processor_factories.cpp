// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ocudu/phy/lower/processors/downlink/pdxch/pdxch_processor_factories.h"
#include "pdxch_baseband_modulator_accelerator.h"
#include "pdxch_processor_factories_cuda.h"
#include "pdxch_processor_impl.h"
#include "ocudu/ran/beamforming/beam_weights_codebook_generator.h"
#include "ocudu/ran/resource_block.h"
#include "ocudu/support/math/math_utils.h"
#ifdef ENABLE_CUDA
#include <cuda_runtime.h>
#endif

using namespace ocudu;

namespace {

/// Modulator gain that normalizes the signal to unitary power according to the number of subcarriers, and applies the
/// back-off to account for the signal PAPR.
float get_modulator_gain_dB(const pdxch_processor_configuration& config, float gain_backoff_dB)
{
  return -convert_power_to_dB(static_cast<float>(config.bandwidth_rb * NOF_SUBCARRIERS_PER_RB)) - gain_backoff_dB;
}

class pdxch_processor_factory_sw : public pdxch_processor_factory
{
public:
  pdxch_processor_factory_sw(std::shared_ptr<ofdm_modulator_factory> ofdm_mod_factory_, float gain_backoff_dB_) :
    ofdm_mod_factory(std::move(ofdm_mod_factory_)), gain_backoff_dB(gain_backoff_dB_)
  {
    ocudu_assert(ofdm_mod_factory, "Invalid OFDM modulator factory.");
  }

  std::unique_ptr<pdxch_processor> create(const pdxch_processor_configuration& config,
                                          task_executor&                       modulation_executor) override
  {
    float modulator_gain_dB = get_modulator_gain_dB(config, gain_backoff_dB);

    ofdm_modulator_configuration mod_config = {.numerology     = to_numerology_value(config.scs),
                                               .bw_rb          = config.bandwidth_rb,
                                               .dft_size       = config.srate.get_dft_size(config.scs),
                                               .cp             = config.cp,
                                               .scale          = convert_dB_to_amplitude(modulator_gain_dB),
                                               .center_freq_Hz = config.center_freq_Hz};

    pdxch_processor_impl::configuration pdxch_config = {
        .scs = config.scs, .cp = config.cp, .srate = config.srate, .tx_ant_topology = config.tx_ant_topology};

    return std::make_unique<pdxch_processor_impl>(
        ofdm_mod_factory->create_ofdm_symbol_modulator(mod_config), modulation_executor, pdxch_config);
  }

private:
  std::shared_ptr<ofdm_modulator_factory> ofdm_mod_factory;
  float                                   gain_backoff_dB;
};

#ifdef ENABLE_CUDA
/// \brief PDxCH processor factory with a CUDA full-slot baseband modulation accelerator.
///
/// The processors keep the software OFDM modulator for the slots the accelerator does not take.
class pdxch_processor_factory_cuda : public pdxch_processor_factory
{
public:
  pdxch_processor_factory_cuda(std::shared_ptr<ofdm_modulator_factory> ofdm_mod_factory_, float gain_backoff_dB_) :
    ofdm_mod_factory(std::move(ofdm_mod_factory_)), gain_backoff_dB(gain_backoff_dB_)
  {
    ocudu_assert(ofdm_mod_factory, "Invalid OFDM modulator factory.");
  }

  std::unique_ptr<pdxch_processor> create(const pdxch_processor_configuration& config,
                                          task_executor&                       modulation_executor) override
  {
    float modulator_gain_dB = get_modulator_gain_dB(config, gain_backoff_dB);

    ofdm_modulator_configuration mod_config = {.numerology     = to_numerology_value(config.scs),
                                               .bw_rb          = config.bandwidth_rb,
                                               .dft_size       = config.srate.get_dft_size(config.scs),
                                               .cp             = config.cp,
                                               .scale          = convert_dB_to_amplitude(modulator_gain_dB),
                                               .center_freq_Hz = config.center_freq_Hz};

    pdxch_processor_impl::configuration pdxch_config = {
        .scs = config.scs, .cp = config.cp, .srate = config.srate, .tx_ant_topology = config.tx_ant_topology};

    unsigned nof_antennas = generate_beam_weights_codebook(config.tx_ant_topology).get_nof_antennas();

    auto accelerator_factory = [config, nof_antennas, modulator_gain_dB]() {
      return create_pdxch_baseband_modulator_accelerator_cuda(config.scs,
                                                              config.cp,
                                                              config.srate,
                                                              config.bandwidth_rb,
                                                              config.center_freq_Hz,
                                                              nof_antennas,
                                                              modulator_gain_dB);
    };

    return std::make_unique<pdxch_processor_impl>(ofdm_mod_factory->create_ofdm_symbol_modulator(mod_config),
                                                  modulation_executor,
                                                  pdxch_config,
                                                  std::move(accelerator_factory));
  }

private:
  std::shared_ptr<ofdm_modulator_factory> ofdm_mod_factory;
  float                                   gain_backoff_dB;
};
#endif

} // namespace

std::shared_ptr<pdxch_processor_factory>
ocudu::create_pdxch_processor_factory_sw(std::shared_ptr<ofdm_modulator_factory> ofdm_mod_factory,
                                         float                                   gain_backoff_dB)
{
  return std::make_shared<pdxch_processor_factory_sw>(std::move(ofdm_mod_factory), gain_backoff_dB);
}

bool ocudu::is_pdxch_processor_factory_cuda_available()
{
#ifdef ENABLE_CUDA
  int device_count = 0;
  if ((cudaGetDeviceCount(&device_count) != cudaSuccess) || (device_count == 0)) {
    cudaGetLastError();
    return false;
  }
  return true;
#else
  return false;
#endif
}

std::shared_ptr<pdxch_processor_factory>
ocudu::create_pdxch_processor_factory_cuda(std::shared_ptr<ofdm_modulator_factory> ofdm_mod_factory,
                                           float                                   gain_backoff_dB)
{
#ifdef ENABLE_CUDA
  return std::make_shared<pdxch_processor_factory_cuda>(std::move(ofdm_mod_factory), gain_backoff_dB);
#else
  return create_pdxch_processor_factory_sw(std::move(ofdm_mod_factory), gain_backoff_dB);
#endif
}
