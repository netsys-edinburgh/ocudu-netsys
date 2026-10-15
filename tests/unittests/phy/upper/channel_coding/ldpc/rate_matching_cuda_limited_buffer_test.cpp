// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief Limited-buffer rate dematching of the CUDA rate matcher against the software dematcher.
///
/// With limited-buffer rate matching, N_cb = min(N, N_ref) sets both the starting position of the redundancy version
/// and the length the bit selection wraps around (TS 38.212 Section 5.4.2.1). The software dematcher is the reference:
/// a GPU dematcher fed by the GPU matcher could not catch a wrap at the wrong length, since both would wrap alike.

#include "ocudu/phy/upper/channel_coding/channel_coding_factories.h"
#include "ocudu/phy/upper/channel_coding/ldpc/ldpc.h"
#include "ocudu/phy/upper/codeblock_metadata.h"
#include "ocudu/phy/upper/log_likelihood_ratio.h"
#include <cuda_runtime.h>
#include <gtest/gtest.h>
#include <random>
#include <rate_matching.h>
#include <vector>

using namespace ocudu;

namespace {

struct limited_buffer_params {
  ldpc_base_graph_type bg;
  unsigned             Z;
  unsigned             F;
  unsigned             N_ref;
  unsigned             E;
  unsigned             rv;
};

std::ostream& operator<<(std::ostream& os, const limited_buffer_params& p)
{
  return os << "BG" << ((p.bg == ldpc_base_graph_type::BG1) ? 1 : 2) << "_Z" << p.Z << "_F" << p.F << "_Nref" << p.N_ref
            << "_E" << p.E << "_rv" << p.rv;
}

class RateMatchingCudaLimitedBufferTest : public ::testing::TestWithParam<limited_buffer_params>
{};

} // namespace

TEST_P(RateMatchingCudaLimitedBufferTest, DematchingMatchesTheSoftwareDematcher)
{
  int nof_devices = 0;
  if ((cudaGetDeviceCount(&nof_devices) != cudaSuccess) || (nof_devices == 0)) {
    (void)cudaGetLastError();
    GTEST_SKIP() << "No CUDA device.";
  }
  const limited_buffer_params& params = GetParam();

  const bool     bg1    = params.bg == ldpc_base_graph_type::BG1;
  const unsigned Kb     = bg1 ? 22 : 10;
  const unsigned N_cols = bg1 ? 68 : 52;
  const unsigned K      = Kb * params.Z;
  const unsigned N      = (N_cols - 2) * params.Z;
  const unsigned N_cb   = std::min(params.N_ref, N);
  const unsigned N_full = N_cols * params.Z;
  const unsigned punct  = 2 * params.Z;
  const unsigned Kd_cb  = K - params.F - punct;
  ASSERT_EQ(ldpc::compute_full_codeblock_size(params.bg, units::bits(K)).value(), N);
  // No position is selected twice, so both dematchers copy every soft bit without combining.
  ASSERT_LE(params.E, N_cb - params.F);

  // Software reference, on the host soft-buffer layout (no punctured columns).
  codeblock_metadata cfg          = {};
  cfg.tb_common.base_graph        = params.bg;
  cfg.tb_common.lifting_size      = static_cast<ldpc::lifting_size_t>(params.Z);
  cfg.tb_common.rv                = params.rv;
  cfg.tb_common.mod               = modulation_scheme::BPSK;
  cfg.tb_common.Nref              = params.N_ref;
  cfg.tb_common.cw_length         = params.E;
  cfg.cb_specific.full_length     = N;
  cfg.cb_specific.rm_length       = params.E;
  cfg.cb_specific.nof_filler_bits = params.F;
  cfg.cb_specific.cw_offset       = 0;
  cfg.cb_specific.nof_crc_bits    = 24;

  std::mt19937                       rng(0x5eed + params.rv + params.Z);
  std::uniform_int_distribution<int> dist(1, 60);
  std::uniform_int_distribution<int> sign(0, 1);
  std::vector<log_likelihood_ratio>  received(params.E);
  for (log_likelihood_ratio& llr : received) {
    int value = dist(rng);
    llr       = log_likelihood_ratio(static_cast<int8_t>(sign(rng) ? value : -value));
  }

  auto dematcher = create_ldpc_rate_dematcher_factory_sw("auto")->create();
  ASSERT_NE(dematcher, nullptr);
  std::vector<log_likelihood_ratio> reference(N, log_likelihood_ratio(0));
  dematcher->rate_dematch(reference, received, true, cfg);

  // GPU dematcher, configured as the PUSCH decoder does.
  nr_ldpc_config_t ldpc_cfg   = {};
  ldpc_cfg.base_graph         = bg1 ? 1 : 2;
  ldpc_cfg.lifting_size       = static_cast<int>(params.Z);
  ldpc_cfg.lifting_set_index  = nr_ldpc_get_lifting_set_index(ldpc_cfg.lifting_size);
  ldpc_cfg.redundancy_version = static_cast<int>(params.rv);
  ldpc_cfg.num_info_bits      = static_cast<int>(K - params.F);
  ldpc_cfg.num_parity_bits    = static_cast<int>((N_cols - Kb) * params.Z);
  ldpc_cfg.max_parity_nodes   = static_cast<int>(N_cols - Kb);
  ldpc_cfg.num_codeword_bits  = static_cast<int>(N_full);
  ldpc_cfg.num_filler_bits    = static_cast<int>(params.F);
  ldpc_cfg.puncture           = true;

  nr_rate_match_config_t rm_cfg = {};
  rm_cfg.E                      = static_cast<int>(params.E);
  rm_cfg.Q_m                    = 1;
  rm_cfg.rv                     = static_cast<int>(params.rv);
  rm_cfg.N_cb                   = static_cast<int>(N_cb);
  rm_cfg.k0             = rate_matcher_compute_k0(ldpc_cfg.base_graph, ldpc_cfg.lifting_size, rm_cfg.rv, rm_cfg.N_cb);
  rm_cfg.limited_buffer = true;

  rate_matcher_handle_t matcher = nullptr;
  ASSERT_EQ(rate_matcher_create(&matcher), NR_LDPC_SUCCESS);
  ASSERT_EQ(rate_matcher_configure_rx(matcher, &ldpc_cfg, &rm_cfg), NR_LDPC_SUCCESS);

  std::vector<float> received_float(params.E);
  for (unsigned i = 0; i != params.E; ++i) {
    received_float[i] = static_cast<float>(received[i].to_int());
  }
  float* d_received = nullptr;
  float* d_output   = nullptr;
  ASSERT_EQ(cudaMalloc(&d_received, params.E * sizeof(float)), cudaSuccess);
  ASSERT_EQ(cudaMalloc(&d_output, N_full * sizeof(float)), cudaSuccess);
  ASSERT_EQ(cudaMemcpy(d_received, received_float.data(), params.E * sizeof(float), cudaMemcpyHostToDevice),
            cudaSuccess);
  ASSERT_EQ(rate_matcher_dematch_batch(matcher, d_received, d_output, 1, nullptr), NR_LDPC_SUCCESS);
  std::vector<float> output(N_full);
  ASSERT_EQ(cudaMemcpy(output.data(), d_output, N_full * sizeof(float), cudaMemcpyDeviceToHost), cudaSuccess);
  cudaFree(d_received);
  cudaFree(d_output);
  rate_matcher_destroy(matcher);

  // Every circular-buffer position outside the filler bits must hold the same soft bit.
  unsigned mismatches = 0;
  for (unsigned i = 0; i != N; ++i) {
    if ((i >= Kd_cb) && (i < Kd_cb + params.F)) {
      continue;
    }
    if (output[punct + i] != static_cast<float>(reference[i].to_int())) {
      ++mismatches;
    }
  }
  EXPECT_EQ(mismatches, 0U) << "of " << N - params.F << " circular-buffer positions";
}

// N_ref as the gNB derives it from the default TBS_LBRM: 19169 for C = 100 and 12611 for C = 152 (base graph 1,
// Z = 384), and a base graph 2 case with filler bits. Every redundancy version, with E shorter than N_ref.
INSTANTIATE_TEST_SUITE_P(RateMatchingCuda,
                         RateMatchingCudaLimitedBufferTest,
                         ::testing::Values(limited_buffer_params{ldpc_base_graph_type::BG1, 384, 0, 19169, 11264, 0},
                                           limited_buffer_params{ldpc_base_graph_type::BG1, 384, 0, 19169, 11264, 1},
                                           limited_buffer_params{ldpc_base_graph_type::BG1, 384, 0, 19169, 11264, 2},
                                           limited_buffer_params{ldpc_base_graph_type::BG1, 384, 0, 19169, 11264, 3},
                                           limited_buffer_params{ldpc_base_graph_type::BG1, 384, 0, 12611, 9182, 2},
                                           limited_buffer_params{ldpc_base_graph_type::BG1, 384, 0, 12611, 9182, 3},
                                           limited_buffer_params{ldpc_base_graph_type::BG2, 160, 40, 5000, 3600, 0},
                                           limited_buffer_params{ldpc_base_graph_type::BG2, 160, 40, 5000, 3600, 1},
                                           limited_buffer_params{ldpc_base_graph_type::BG2, 160, 40, 5000, 3600, 2},
                                           limited_buffer_params{ldpc_base_graph_type::BG2, 160, 40, 5000, 3600, 3}),
                         ::testing::PrintToStringParamName());
