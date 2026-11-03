// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief Scale of the soft bits that the GPU PUSCH demodulator hands to the host.
///
/// The host LDPC decoder, the UCI decoders and the HARQ soft-combining buffer take soft bits on the scale of the CPU
/// demodulation mapper. The GPU demodulator computes LLRs in natural units and converts them on the way to the host.
/// These tests demodulate the same noisy grid with both demodulators and check that the GPU soft bits are the CPU soft
/// bits, scale included, for every modulation.

#include "pxsch_bler_test_channel_emulator.h"
#include "pxsch_bler_test_factories.h"
#include "ocudu/phy/generic_functions/generic_functions_factories.h"
#include "ocudu/phy/generic_functions/transform_precoding/transform_precoding_factories.h"
#include "ocudu/phy/support/resource_grid.h"
#include "ocudu/phy/support/support_factories.h"
#include "ocudu/phy/upper/channel_modulation/channel_modulation_factories.h"
#include "ocudu/phy/upper/channel_processors/pusch/factories.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_codeword_buffer.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_demodulator.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_demodulator_notifier.h"
#include "ocudu/phy/upper/equalization/equalization_factories.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"
#include "ocudu/phy/upper/signal_processors/channel_estimator/factories.h"
#include "ocudu/phy/upper/signal_processors/pusch/factories.h"
#include "ocudu/ran/precoding/precoding_codebooks.h"
#include "ocudu/ran/pusch/pusch_mcs.h"
#include "ocudu/ran/sch/sch_dmrs_power.h"
#include "ocudu/ran/sch/tbs_calculator.h"
#include "ocudu/support/executors/inline_task_executor.h"
#include "ocudu/support/executors/task_worker_pool.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <gtest/gtest.h>
#include <thread>

using namespace ocudu;

namespace {

constexpr unsigned nof_prb   = 17;
constexpr unsigned nof_slots = 4;
constexpr unsigned nof_cdm   = 2;

struct softbit_scale_params {
  pusch_mcs_table table;
  unsigned        mcs;
  float           snr_dB;
};

std::ostream& operator<<(std::ostream& os, const softbit_scale_params& p)
{
  return os << "table" << static_cast<unsigned>(p.table) << "_mcs" << p.mcs << "_snr" << p.snr_dB;
}

class pdsch_completion : public pdsch_processor_notifier
{
public:
  void on_finish_processing() override { completed = true; }
  void wait()
  {
    while (!completed.load()) {
      std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
  }

private:
  std::atomic<bool> completed = {false};
};

class softbit_capture : public pusch_codeword_buffer
{
public:
  span<log_likelihood_ratio> get_next_block_view(unsigned block_size) override
  {
    block.resize(block_size);
    return block;
  }
  void on_new_block(span<const log_likelihood_ratio> data, const bit_buffer& /**/) override
  {
    softbits.insert(softbits.end(), data.begin(), data.end());
  }
  void on_end_codeword() override {}

  std::vector<log_likelihood_ratio> softbits;

private:
  std::vector<log_likelihood_ratio> block;
};

class ignore_demod_stats : public pusch_demodulator_notifier
{
public:
  void on_provisional_stats(unsigned /**/, const demodulation_stats& /**/) override {}
  void on_end_stats(const demodulation_stats& /**/) override {}
};

class estimation_completion : public dmrs_pusch_estimator_notifier
{
public:
  explicit estimation_completion(std::function<void(const dmrs_pusch_estimator_results&)> callback_) :
    callback(std::move(callback_))
  {
  }
  void on_estimation_complete(const dmrs_pusch_estimator_results& results) override { callback(results); }

private:
  std::function<void(const dmrs_pusch_estimator_results&)> callback;
};

class PuschGpuSoftbitScaleTest : public ::testing::TestWithParam<softbit_scale_params>
{};

TEST_P(PuschGpuSoftbitScaleTest, GpuHostSoftbitsHaveTheCpuScale)
{
  if (!is_pusch_demodulator_acceleration_available()) {
    GTEST_SKIP() << "GPU PUSCH demodulator not available.";
  }
  const softbit_scale_params params = GetParam();

  task_worker_pool<concurrent_queue_policy::locking_mpmc>          pool("softbit", 2, 64);
  task_worker_pool_executor<concurrent_queue_policy::locking_mpmc> pool_exec(pool);
  inline_task_executor                                             inline_exec;

  std::shared_ptr<pdsch_processor_factory> pdsch_factory = create_sw_pdsch_processor_factory(pool_exec, 2, "", "auto");
  ASSERT_NE(pdsch_factory, nullptr);
  std::unique_ptr<pdsch_processor> transmitter = pdsch_factory->create();

  auto prg         = create_pseudo_random_generator_sw_factory();
  auto dft         = create_dft_processor_factory_fftw_slow();
  auto est_factory = create_dmrs_pusch_estimator_factory_sw(
      prg,
      create_low_papr_sequence_generator_sw_factory(),
      create_port_channel_estimator_factory_sw(create_time_alignment_estimator_dft_factory(dft)),
      inline_exec,
      pusch_constants::MAX_NOF_RX_PORTS,
      port_channel_estimator_fd_smoothing_strategy::filter,
      port_channel_estimator_td_interpolation_strategy::average,
      true);
  ASSERT_NE(est_factory, nullptr);
  std::unique_ptr<dmrs_pusch_estimator> estimator = est_factory->create();

  auto eq      = create_channel_equalizer_generic_factory(channel_equalizer_algorithm_type::mmse);
  auto demap   = create_demodulation_mapper_factory();
  auto evm     = create_evm_calculator_factory();
  auto precode = create_dft_transform_precoder_factory(dft, MAX_NOF_PRBS);
  std::unique_ptr<pusch_demodulator> cpu_demod =
      create_pusch_demodulator_factory_sw(eq, precode, demap, evm, prg, MAX_NOF_PRBS, true)->create();

  pusch_demodulator_factory_accelerated_configuration gpu_config;
  gpu_config.equalizer_factory                           = eq;
  gpu_config.precoder_factory                            = precode;
  gpu_config.demodulation_factory                        = demap;
  gpu_config.evm_calc_factory                            = evm;
  gpu_config.prg_factory                                 = prg;
  gpu_config.max_nof_prb                                 = MAX_NOF_PRBS;
  gpu_config.enable_post_eq_sinr                         = true;
  gpu_config.equalizer_algorithm                         = channel_equalizer_algorithm_type::mmse;
  gpu_config.compensate_cfo                              = true;
  gpu_config.td_interpolation_strategy                   = port_channel_estimator_td_interpolation_strategy::average;
  std::shared_ptr<pusch_demodulator_factory> gpu_factory = create_pusch_demodulator_factory_accelerated(gpu_config);
  ASSERT_NE(gpu_factory, nullptr);
  std::unique_ptr<pusch_demodulator> gpu_demod = gpu_factory->create();
  ASSERT_NE(gpu_demod, nullptr);

  std::shared_ptr<resource_grid_factory> grid_factory = create_resource_grid_factory();
  std::unique_ptr<resource_grid>         tx_grid =
      grid_factory->create(1, MAX_NSYMB_PER_SLOT, nof_prb * NOF_SUBCARRIERS_PER_RB);
  std::unique_ptr<resource_grid> rx_grid =
      grid_factory->create(1, MAX_NSYMB_PER_SLOT, nof_prb * NOF_SUBCARRIERS_PER_RB);

  symbol_slot_mask dmrs_mask(MAX_NSYMB_PER_SLOT);
  dmrs_mask.set(2);
  dmrs_mask.set(11);
  sch_mcs_description          mcs_descr  = pusch_mcs_get_config(params.table, sch_mcs_index(params.mcs), false, false);
  tbs_calculator_configuration tbs_config = {};
  tbs_config.mcs_descr                    = mcs_descr;
  tbs_config.n_prb                        = nof_prb;
  tbs_config.nof_layers                   = 1;
  tbs_config.nof_symb_sh                  = MAX_NSYMB_PER_SLOT;
  tbs_config.nof_dmrs_prb                 = get_nof_re_per_prb(dmrs_config_type::type1) * dmrs_mask.count() * nof_cdm;
  unsigned      tbs                       = tbs_calculator_calculate(tbs_config).to_bits().value();
  rb_allocation freq_alloc                = rb_allocation::make_type1(0, nof_prb, std::nullopt);

  // The transmitter is a PDSCH processor: with these parameters it maps the same symbols and DM-RS as a PUSCH.
  pdsch_processor::pdu_t tx_pdu;
  tx_pdu.context                     = std::nullopt;
  tx_pdu.rnti                        = to_rnti(0x1234);
  tx_pdu.bwp_size_rb                 = nof_prb;
  tx_pdu.bwp_start_rb                = 0;
  tx_pdu.cp                          = cyclic_prefix::NORMAL;
  tx_pdu.n_id                        = 0;
  tx_pdu.ref_point                   = pdsch_processor::pdu_t::PRB0;
  tx_pdu.dmrs_symbol_mask            = dmrs_mask;
  tx_pdu.dmrs                        = dmrs_config_type::type1;
  tx_pdu.scrambling_id               = 0;
  tx_pdu.n_scid                      = false;
  tx_pdu.nof_cdm_groups_without_data = nof_cdm;
  tx_pdu.freq_alloc                  = freq_alloc;
  tx_pdu.start_symbol_index          = 0;
  tx_pdu.nof_symbols                 = MAX_NSYMB_PER_SLOT;
  tx_pdu.tbs_lbrm                    = tbs_lbrm_default;
  tx_pdu.ratio_pdsch_data_to_sss_dB  = 0.0F;
  tx_pdu.ratio_pdsch_dmrs_to_sss_dB  = get_sch_to_dmrs_ratio_dB(nof_cdm);
  tx_pdu.precoding                   = precoding_configuration::make_wideband(make_identity(1));
  tx_pdu.codewords.emplace_back(pdsch_processor::codeword_description{
      mcs_descr.modulation, 0, get_ldpc_base_graph(mcs_descr.get_normalised_target_code_rate(), units::bits(tbs))});

  dmrs_pusch_estimator::configuration est_config;
  est_config.sequence_config = dmrs_pusch_estimator::pseudo_random_sequence_configuration{
      .type = dmrs_config_type::type1, .nof_tx_layers = 1, .scrambling_id = 0, .n_scid = false};
  est_config.scaling      = convert_dB_to_amplitude(-get_sch_to_dmrs_ratio_dB(nof_cdm));
  est_config.c_prefix     = cyclic_prefix::NORMAL;
  est_config.symbols_mask = dmrs_mask;
  est_config.rb_mask      = freq_alloc.get_crb_mask(0, nof_prb);
  est_config.first_symbol = 0;
  est_config.nof_symbols  = MAX_NSYMB_PER_SLOT;
  est_config.rx_ports     = {0};

  pusch_demodulator::configuration demod_config;
  demod_config.rnti                        = to_rnti(0x1234);
  demod_config.rb_mask                     = freq_alloc.get_crb_mask(0, nof_prb);
  demod_config.modulation                  = mcs_descr.modulation;
  demod_config.start_symbol_index          = 0;
  demod_config.nof_symbols                 = MAX_NSYMB_PER_SLOT;
  demod_config.dmrs_symb_pos               = dmrs_mask;
  demod_config.dmrs_type                   = dmrs_config_type::type1;
  demod_config.nof_cdm_groups_without_data = nof_cdm;
  demod_config.n_id                        = 0;
  demod_config.dmrs_scrambling_id          = 0;
  demod_config.n_scid                      = false;
  demod_config.nof_tx_layers               = 1;
  demod_config.dc_position                 = std::nullopt;
  demod_config.enable_transform_precoding  = false;
  demod_config.rx_ports                    = {0};

  std::mt19937         rgen(0x50f7);
  std::vector<uint8_t> payload(tbs / 8);
  double               cpu_gpu = 0;
  double               cpu_cpu = 0;
  double               gpu_gpu = 0;
  for (unsigned i_slot = 0; i_slot != nof_slots; ++i_slot) {
    slot_point slot(to_numerology_value(subcarrier_spacing::kHz30), i_slot);
    tx_pdu.slot       = slot;
    est_config.slot   = slot;
    demod_config.slot = slot;
    std::generate(payload.begin(), payload.end(), [&rgen]() { return static_cast<uint8_t>(rgen() & 0xff); });

    tx_grid->set_all_zero();
    pdsch_completion tx_done;
    transmitter->process(tx_grid->get_writer(), tx_done, {shared_transport_block(payload)}, tx_pdu);
    tx_done.wait();

    channel_emulator channel("single-tap",
                             "uniform-phase",
                             params.snr_dB,
                             0.0F,
                             0,
                             1,
                             1,
                             nof_prb * NOF_SUBCARRIERS_PER_RB,
                             MAX_NSYMB_PER_SLOT,
                             2,
                             subcarrier_spacing::kHz30,
                             pool_exec);
    channel.run(rx_grid->get_writer(), tx_grid->get_reader());

    softbit_capture       cpu_softbits;
    softbit_capture       gpu_softbits;
    estimation_completion est_done([&](const dmrs_pusch_estimator_results& results) {
      ignore_demod_stats cpu_stats;
      ignore_demod_stats gpu_stats;
      cpu_demod->demodulate(cpu_softbits, cpu_stats, rx_grid->get_reader(), results, demod_config);
      gpu_demod->demodulate(gpu_softbits, gpu_stats, rx_grid->get_reader(), results, demod_config);
    });
    estimator->estimate(est_done, rx_grid->get_reader(), est_config);

    ASSERT_EQ(cpu_softbits.softbits.size(), gpu_softbits.softbits.size());
    ASSERT_FALSE(cpu_softbits.softbits.empty());
    for (unsigned i = 0, n = cpu_softbits.softbits.size(); i != n; ++i) {
      double cpu = cpu_softbits.softbits[i].to_int();
      double gpu = gpu_softbits.softbits[i].to_int();
      cpu_gpu += cpu * gpu;
      cpu_cpu += cpu * cpu;
      gpu_gpu += gpu * gpu;
    }
  }
  pool.stop();

  // Least-squares slope of the GPU soft bits against the CPU soft bits, and their correlation.
  double slope       = cpu_gpu / cpu_cpu;
  double correlation = cpu_gpu / std::sqrt(cpu_cpu * gpu_gpu);
  EXPECT_GT(correlation, 0.99);
  EXPECT_NEAR(slope, 1.0, 0.05) << "the GPU host soft bits do not have the scale of the CPU demodulation mapper";
}

// SNRs low enough that the CPU soft bits are rarely clipped, which would bias the slope.
INSTANTIATE_TEST_SUITE_P(PuschGpuSoftbitScale,
                         PuschGpuSoftbitScaleTest,
                         ::testing::Values(softbit_scale_params{pusch_mcs_table::qam64, 4, 2.0F},
                                           softbit_scale_params{pusch_mcs_table::qam64, 10, 4.0F},
                                           softbit_scale_params{pusch_mcs_table::qam64, 20, 6.0F},
                                           softbit_scale_params{pusch_mcs_table::qam256, 24, 10.0F}));

} // namespace
