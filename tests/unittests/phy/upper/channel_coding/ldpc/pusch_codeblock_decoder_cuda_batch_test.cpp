// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief HARQ soft-buffer export of the CUDA batch PUSCH codeblock decoder.
///
/// A GPU-resident first transmission keeps its rate-dematched LLRs on the device. A retransmission of the same
/// transport block is decoded on the host path and soft-combines with the host rate-matching buffer, so the resident
/// decode must be able to seed that buffer. These tests check that export_resident_softbits() reproduces what the host
/// rate dematcher would have written (layout, scale, saturation) so that the subsequent combination is exact.

#include "cuda_host_llr_scale.h"
#include "pusch_codeblock_decoder_cuda_batch.h"
#include "ocudu/adt/bit_buffer.h"
#include "ocudu/phy/upper/channel_coding/channel_coding_factories.h"
#include "ocudu/phy/upper/channel_coding/ldpc/ldpc.h"
#include "ocudu/phy/upper/codeblock_metadata.h"
#include "ocudu/phy/upper/log_likelihood_ratio.h"
#include <chrono>
#include <cuda.h>
#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <gtest/gtest.h>
#include <random>
#include <thread>
#include <vector>

using namespace ocudu;

namespace {

struct export_test_params {
  ldpc_base_graph_type bg;
  unsigned             Z;
  modulation_scheme    mod;
  /// Rate-matched length as a fraction (percent) of the short codeword length.
  unsigned E_percent;
};

std::ostream& operator<<(std::ostream& os, const export_test_params& p)
{
  return os << "BG" << ((p.bg == ldpc_base_graph_type::BG1) ? 1 : 2) << "_Z" << p.Z << "_Qm"
            << get_bits_per_symbol(p.mod) << "_E" << p.E_percent;
}

class PuschCodeblockDecoderCudaBatchExportTest : public ::testing::TestWithParam<export_test_params>
{
protected:
  static std::unique_ptr<pusch_codeblock_decoder_cuda_batch>
  make_decoder(std::shared_ptr<cuda_gpu_wait_quarantine> quarantine = nullptr, unsigned max_prb = 51)
  {
    auto dematcher_factory = create_ldpc_rate_dematcher_factory_sw("auto");
    auto crc_factory       = create_crc_calculator_factory_sw("auto");
    if (!dematcher_factory || !crc_factory) {
      return nullptr;
    }
    pusch_codeblock_decoder_cuda_batch::sch_crc crcs;
    crcs.crc16  = crc_factory->create(crc_generator_poly::CRC16);
    crcs.crc24A = crc_factory->create(crc_generator_poly::CRC24A);
    crcs.crc24B = crc_factory->create(crc_generator_poly::CRC24B);
    // Every existing export case fits a 51-PRB SISO carrier; preserve all BG/Z/Qm/E cases.
    return std::make_unique<pusch_codeblock_decoder_cuda_batch>(
        dematcher_factory->create(), std::move(crcs), 0, max_prb, 1, std::move(quarantine));
  }

  /// Uploads host int8 LLRs as device fp16 LLRs on the demodulator scale (host = scale x device).
  static void* upload_as_half(const std::vector<log_likelihood_ratio>& llrs, modulation_scheme mod)
  {
    const float         scale = get_cuda_host_llr_scale(get_bits_per_symbol(mod));
    std::vector<__half> half(llrs.size());
    for (size_t i = 0; i != llrs.size(); ++i) {
      half[i] = __float2half(static_cast<float>(llrs[i].to_int()) / scale);
    }
    void* device = nullptr;
    if (cudaMalloc(&device, half.size() * sizeof(__half)) != cudaSuccess) {
      return nullptr;
    }
    if (cudaMemcpy(device, half.data(), half.size() * sizeof(__half), cudaMemcpyHostToDevice) != cudaSuccess) {
      cudaFree(device);
      return nullptr;
    }
    return device;
  }
};

std::unique_ptr<pusch_codeblock_decoder_cuda_batch>
      PuschCodeblockDecoderCudaBatchExportTest_make_decoder(std::shared_ptr<cuda_gpu_wait_quarantine> quarantine);
void* PuschCodeblockDecoderCudaBatchExportTest_upload_as_half(const std::vector<log_likelihood_ratio>& llrs,
                                                              modulation_scheme                        mod);

/// Gives the timeout test access to the fixture helpers.
class PuschCodeblockDecoderCudaBatchHelpers : public PuschCodeblockDecoderCudaBatchExportTest
{
public:
  using PuschCodeblockDecoderCudaBatchExportTest::make_decoder;
  using PuschCodeblockDecoderCudaBatchExportTest::upload_as_half;
};

std::unique_ptr<pusch_codeblock_decoder_cuda_batch>
PuschCodeblockDecoderCudaBatchExportTest_make_decoder(std::shared_ptr<cuda_gpu_wait_quarantine> quarantine)
{
  return PuschCodeblockDecoderCudaBatchHelpers::make_decoder(std::move(quarantine));
}

void* PuschCodeblockDecoderCudaBatchExportTest_upload_as_half(const std::vector<log_likelihood_ratio>& llrs,
                                                              modulation_scheme                        mod)
{
  return PuschCodeblockDecoderCudaBatchHelpers::upload_as_half(llrs, mod);
}

} // namespace

TEST_P(PuschCodeblockDecoderCudaBatchExportTest, ExportMatchesHostDematcherAndCombines)
{
  const export_test_params& params = GetParam();

  auto decoder = make_decoder();
  ASSERT_NE(decoder, nullptr);
  auto host_dematcher = create_ldpc_rate_dematcher_factory_sw("auto")->create();
  ASSERT_NE(host_dematcher, nullptr);

  // Single codeblock, no filler bits, RV 0, unlimited buffer.
  const unsigned bg_K        = (params.bg == ldpc_base_graph_type::BG1) ? 22 : 10;
  const unsigned K           = bg_K * params.Z;
  const unsigned N_full      = ldpc::compute_full_codeblock_size(params.bg, units::bits(K)).value();
  const unsigned N_short     = N_full - 2 * params.Z; // Codeword without the punctured systematic bits.
  const unsigned Qm          = get_bits_per_symbol(params.mod);
  unsigned       E           = (N_short * params.E_percent) / 100;
  E                          = (E / Qm) * Qm;
  const unsigned tb_crc_bits = (K - 24 > 3824) ? 24 : 16;
  const unsigned tbs_bits    = K - tb_crc_bits;
  ASSERT_EQ(tbs_bits % 8, 0U);

  codeblock_metadata cfg          = {};
  cfg.tb_common.base_graph        = params.bg;
  cfg.tb_common.lifting_size      = static_cast<ldpc::lifting_size_t>(params.Z);
  cfg.tb_common.rv                = 0;
  cfg.tb_common.mod               = params.mod;
  cfg.tb_common.Nref              = 0;
  cfg.tb_common.cw_length         = E;
  cfg.cb_specific.full_length     = N_full;
  cfg.cb_specific.rm_length       = E;
  cfg.cb_specific.nof_filler_bits = 0;
  cfg.cb_specific.cw_offset       = 0;
  cfg.cb_specific.nof_crc_bits    = tb_crc_bits;

  // Random soft bits: the transport block is noise, so the resident decode fails and the export path is exercised.
  std::mt19937                       rng(0x5eed + params.Z);
  std::uniform_int_distribution<int> dist(-40, 40);
  std::vector<log_likelihood_ratio>  first(E);
  std::vector<log_likelihood_ratio>  second(E);
  for (unsigned i = 0; i != E; ++i) {
    first[i]  = log_likelihood_ratio(static_cast<int8_t>(dist(rng)));
    second[i] = log_likelihood_ratio(static_cast<int8_t>(dist(rng)));
  }

  // Host reference: what the CPU dematcher writes for a new transmission.
  std::vector<log_likelihood_ratio> host_buffer(N_full, log_likelihood_ratio(0));
  host_dematcher->rate_dematch(host_buffer, first, true, cfg);

  // Device: resident decode of the first transmission, then export into a buffer pre-filled with garbage.
  void* d_llrs = upload_as_half(first, params.mod);
  ASSERT_NE(d_llrs, nullptr);
  std::vector<uint8_t>                                  tb(tbs_bits / 8);
  std::vector<codeblock_metadata::cb_specific_metadata> cb_specific = {cfg.cb_specific};
  auto                                                  result      = decoder->decode_resident_softbits(
      d_llrs, E, nullptr, cfg.tb_common, cb_specific, tb, crc_generator_poly::CRC24A, 6, nullptr, -1);
  EXPECT_FALSE(result.tb_crc_ok) << "random soft bits are not expected to decode";

  // Out-of-envelope input must not expose a previous decode's resident softbits,
  // touch caller output, or submit work using the deliberately null device pointer.
  const auto           capacity = *make_pusch_decoder_capacity(51, 1);
  std::vector<uint8_t> rejected_output(tb.size(), 0xA5);
  auto                 rejected = decoder->decode_resident_softbits(nullptr,
                                                    capacity.codeword_llrs + 1,
                                                    nullptr,
                                                    cfg.tb_common,
                                                    cb_specific,
                                                    rejected_output,
                                                    crc_generator_poly::CRC24A,
                                                    6,
                                                    nullptr,
                                                    -1);
  EXPECT_FALSE(rejected.tb_crc_ok);
  EXPECT_FALSE(rejected.all_cb_crcs_ok);
  EXPECT_EQ(rejected.nof_iterations, 0U);
  EXPECT_EQ(rejected_output, std::vector<uint8_t>(tb.size(), 0xA5));
  std::vector<log_likelihood_ratio>       rejected_export(N_full, log_likelihood_ratio(77));
  std::vector<span<log_likelihood_ratio>> rejected_buffers = {rejected_export};
  EXPECT_FALSE(decoder->export_resident_softbits(rejected_buffers));
  EXPECT_EQ(rejected_export, std::vector<log_likelihood_ratio>(N_full, log_likelihood_ratio(77)));

  const auto expect_rejected =
      [&](size_t count, const std::vector<codeblock_metadata::cb_specific_metadata>& metadata, size_t bytes) {
        std::vector<uint8_t> output(bytes, 0xA5);
        auto                 invalid = decoder->decode_resident_softbits(
            nullptr, count, nullptr, cfg.tb_common, metadata, output, crc_generator_poly::CRC24A, 6, nullptr, -1);
        EXPECT_FALSE(invalid.tb_crc_ok);
        EXPECT_FALSE(invalid.all_cb_crcs_ok);
        EXPECT_EQ(invalid.nof_iterations, 0U);
        EXPECT_EQ(output, std::vector<uint8_t>(bytes, 0xA5));
        EXPECT_FALSE(decoder->export_resident_softbits(rejected_buffers));
      };
  expect_rejected(E, {}, tb.size());
  expect_rejected(E - 1, cb_specific, tb.size());
  expect_rejected(E, cb_specific, capacity.tb_bytes);
  auto invalid_metadata         = cb_specific;
  invalid_metadata[0].rm_length = 0;
  expect_rejected(E, invalid_metadata, tb.size());
  invalid_metadata[0].rm_length = capacity.codeword_llrs + 1;
  expect_rejected(capacity.codeword_llrs + 1, invalid_metadata, tb.size());
  invalid_metadata[0].rm_length = 8;
  invalid_metadata.resize(capacity.codeblocks + 1, invalid_metadata[0]);
  expect_rejected(invalid_metadata.size() * 8, invalid_metadata, tb.size());

  // Host-input admission also rejects before copying or dematching, while
  // retaining one failed result per requested CB (the caller indexes each).
  std::vector<described_rx_codeblock> described            = {{first, cfg}};
  std::vector<bit_buffer>             data_buffers         = {bit_buffer::from_bytes(rejected_output)};
  const auto                          expect_host_rejected = [&]() {
    auto failures =
        decoder->decode_batch(described, data_buffers, rejected_buffers, true, crc_generator_poly::CRC24A, true, 6);
    ASSERT_EQ(failures.size(), described.size());
    for (const auto& failure : failures) {
      EXPECT_FALSE(failure.crc_ok);
      EXPECT_EQ(failure.nof_iterations, 0U);
    }
    EXPECT_EQ(rejected_output, std::vector<uint8_t>(tb.size(), 0xA5));
    EXPECT_EQ(rejected_export, std::vector<log_likelihood_ratio>(N_full, log_likelihood_ratio(77)));
  };
  described[0].second.cb_specific.rm_length = E + 1;
  expect_host_rejected();
  std::vector<log_likelihood_ratio> oversized(capacity.codeword_llrs + 1, log_likelihood_ratio(1));
  described[0].first                        = oversized;
  described[0].second.cb_specific.rm_length = oversized.size();
  expect_host_rejected();
  described[0].first                        = span<const log_likelihood_ratio>(first).first(8);
  described[0].second.cb_specific.rm_length = 8;
  described.resize(capacity.codeblocks + 1, described[0]);
  data_buffers.resize(described.size(), data_buffers[0]);
  rejected_buffers.resize(described.size(), rejected_buffers[0]);
  expect_host_rejected();

  // Recover on the same instance. The usual numerical export/HARQ checks below
  // now also prove a rejected grant did not corrupt the subsequent valid decode.
  result = decoder->decode_resident_softbits(
      d_llrs, E, nullptr, cfg.tb_common, cb_specific, tb, crc_generator_poly::CRC24A, 6, nullptr, -1);
  EXPECT_FALSE(result.tb_crc_ok);

  std::vector<log_likelihood_ratio>       exported(N_full, log_likelihood_ratio(77));
  std::vector<span<log_likelihood_ratio>> buffers = {exported};
  ASSERT_TRUE(decoder->export_resident_softbits(buffers));
  // A second export must refuse: the decode buffers rotate.
  EXPECT_FALSE(decoder->export_resident_softbits(buffers));

  unsigned mismatches = 0;
  int      max_diff   = 0;
  for (unsigned i = 0; i != N_full; ++i) {
    const int diff = std::abs(exported[i].to_int() - host_buffer[i].to_int());
    max_diff       = std::max(max_diff, diff);
    if (diff > 1) {
      ++mismatches;
    }
  }
  EXPECT_EQ(mismatches, 0U) << "exported soft bits differ from the host dematcher by more than one LSB (max "
                            << max_diff << ")";

  // Retransmission: combining on the exported buffer must equal combining on the host buffer.
  host_dematcher->rate_dematch(host_buffer, second, false, cfg);
  host_dematcher->rate_dematch(exported, second, false, cfg);
  unsigned combine_mismatches = 0;
  for (unsigned i = 0; i != N_full; ++i) {
    if (std::abs(exported[i].to_int() - host_buffer[i].to_int()) > 1) {
      ++combine_mismatches;
    }
  }
  EXPECT_EQ(combine_mismatches, 0U);

  cudaFree(d_llrs);
}

// A codeblock made only of erasures, as when nothing was received, carries no information. The decoder converges to the
// all-zero codeword, whose CRC is zero, so the CRC alone would report it as decoded.
TEST_P(PuschCodeblockDecoderCudaBatchExportTest, AllErasureCodeblockIsNotDecoded)
{
  const export_test_params& params = GetParam();

  auto decoder = make_decoder();
  ASSERT_NE(decoder, nullptr);

  const unsigned bg_K        = (params.bg == ldpc_base_graph_type::BG1) ? 22 : 10;
  const unsigned K           = bg_K * params.Z;
  const unsigned N_full      = ldpc::compute_full_codeblock_size(params.bg, units::bits(K)).value();
  const unsigned N_short     = N_full - 2 * params.Z;
  const unsigned Qm          = get_bits_per_symbol(params.mod);
  unsigned       E           = (N_short * params.E_percent) / 100;
  E                          = (E / Qm) * Qm;
  const unsigned tb_crc_bits = (K - 24 > 3824) ? 24 : 16;
  const unsigned tbs_bits    = K - tb_crc_bits;
  ASSERT_EQ(tbs_bits % 8, 0U);

  codeblock_metadata cfg            = {};
  cfg.tb_common.base_graph          = params.bg;
  cfg.tb_common.lifting_size        = static_cast<ldpc::lifting_size_t>(params.Z);
  cfg.tb_common.rv                  = 0;
  cfg.tb_common.mod                 = params.mod;
  cfg.tb_common.Nref                = 0;
  cfg.tb_common.cw_length           = E;
  cfg.cb_specific.full_length       = N_full;
  cfg.cb_specific.rm_length         = E;
  cfg.cb_specific.nof_filler_bits   = 0;
  cfg.cb_specific.cw_offset         = 0;
  cfg.cb_specific.nof_crc_bits      = tb_crc_bits;
  const crc_generator_poly crc_poly = (tb_crc_bits == 24) ? crc_generator_poly::CRC24A : crc_generator_poly::CRC16;

  std::vector<log_likelihood_ratio> erasures(E, log_likelihood_ratio(0));

  // Resident decode of a first transmission.
  void* d_llrs = upload_as_half(erasures, params.mod);
  ASSERT_NE(d_llrs, nullptr);
  std::vector<uint8_t>                                  tb(tbs_bits / 8);
  std::vector<codeblock_metadata::cb_specific_metadata> cb_specific = {cfg.cb_specific};
  auto                                                  resident =
      decoder->decode_resident_softbits(d_llrs, E, nullptr, cfg.tb_common, cb_specific, tb, crc_poly, 6, nullptr, -1);
  cudaFree(d_llrs);
  EXPECT_FALSE(resident.tb_crc_ok) << "resident decode reported an all-erasure codeblock as decoded";

  // Host-visible batch decode, of a new transmission and of a retransmission onto an empty soft buffer.
  for (bool new_data : {true, false}) {
    SCOPED_TRACE(new_data ? "new transmission" : "retransmission");
    std::vector<log_likelihood_ratio>       soft_buffer(N_full, log_likelihood_ratio(0));
    std::vector<span<log_likelihood_ratio>> rm_buffers = {soft_buffer};
    dynamic_bit_buffer                      cb_data(K);
    std::vector<bit_buffer>                 cb_data_buffers = {cb_data};
    std::vector<described_rx_codeblock>     codeblocks      = {{erasures, cfg}};
    auto results = decoder->decode_batch(codeblocks, cb_data_buffers, rm_buffers, new_data, crc_poly, true, 6);
    ASSERT_EQ(results.size(), 1U);
    EXPECT_FALSE(results.front().crc_ok) << "batch decode reported an all-erasure codeblock as decoded";
  }
}

INSTANTIATE_TEST_SUITE_P(
    PuschCodeblockDecoderCudaBatch,
    PuschCodeblockDecoderCudaBatchExportTest,
    ::testing::Values(export_test_params{ldpc_base_graph_type::BG1, 384, modulation_scheme::QPSK, 100},
                      export_test_params{ldpc_base_graph_type::BG1, 384, modulation_scheme::QAM64, 60},
                      export_test_params{ldpc_base_graph_type::BG1, 208, modulation_scheme::QAM16, 80},
                      export_test_params{ldpc_base_graph_type::BG2, 384, modulation_scheme::QPSK, 100},
                      export_test_params{ldpc_base_graph_type::BG2, 160, modulation_scheme::QAM256, 50}),
    ::testing::PrintToStringParamName());

namespace {

/// \brief Holds the work queued on a stream behind a gate until the test releases it, as a stuck GPU would.
///
/// The gate word lives in mapped host memory and the host releases it with a plain store: a release written by another
/// stream could share a hardware queue with a gated stream and never run.
class stream_gate
{
public:
  explicit stream_gate(cudaStream_t stream_) : stream(stream_)
  {
    void* host_word   = nullptr;
    void* device_word = nullptr;
    supported         = (cudaHostAlloc(&host_word, sizeof(uint32_t), cudaHostAllocMapped) == cudaSuccess) &&
                (cudaHostGetDevicePointer(&device_word, host_word, 0) == cudaSuccess);
    if (supported) {
      gate_word  = static_cast<volatile uint32_t*>(host_word);
      *gate_word = 0;
      supported  = cuStreamWaitValue32(reinterpret_cast<CUstream>(stream),
                                      reinterpret_cast<CUdeviceptr>(device_word),
                                      1U,
                                      CU_STREAM_WAIT_VALUE_EQ) == CUDA_SUCCESS;
    }
  }

  ~stream_gate()
  {
    release();
    (void)cudaStreamSynchronize(stream);
    if (gate_word != nullptr) {
      (void)cudaFreeHost(const_cast<uint32_t*>(gate_word));
    }
  }

  stream_gate(const stream_gate&)            = delete;
  stream_gate& operator=(const stream_gate&) = delete;

  void release()
  {
    if (gate_word != nullptr) {
      *gate_word = 1;
    }
  }

  bool supported = false;

private:
  cudaStream_t       stream    = nullptr;
  volatile uint32_t* gate_word = nullptr;
};

} // namespace

// A stuck GPU must not hold the PUSCH worker for ever. A decode whose wait times out fails its transport block and
// leaves nothing to export into the HARQ soft buffers; every decoder sharing the quarantine stays off the GPU until the
// stuck work has finished.
TEST(PuschCodeblockDecoderCudaBatchTimeoutTest, GpuWaitTimeoutFailsTheTransportBlockAndLeavesTheGpuUntilItDrains)
{
  auto quarantine = std::make_shared<cuda_gpu_wait_quarantine>();
  auto decoder    = PuschCodeblockDecoderCudaBatchExportTest_make_decoder(quarantine);
  auto sibling    = PuschCodeblockDecoderCudaBatchExportTest_make_decoder(quarantine);
  ASSERT_NE(decoder, nullptr);
  ASSERT_NE(sibling, nullptr);

  // Single BG2 codeblock of random soft bits.
  const unsigned     Z           = 160;
  const unsigned     K           = 10 * Z;
  const unsigned     N_full      = ldpc::compute_full_codeblock_size(ldpc_base_graph_type::BG2, units::bits(K)).value();
  const unsigned     E           = N_full - 2 * Z;
  const unsigned     tb_crc_bits = 16;
  codeblock_metadata cfg         = {};
  cfg.tb_common.base_graph       = ldpc_base_graph_type::BG2;
  cfg.tb_common.lifting_size     = static_cast<ldpc::lifting_size_t>(Z);
  cfg.tb_common.rv               = 0;
  cfg.tb_common.mod              = modulation_scheme::QPSK;
  cfg.tb_common.Nref             = 0;
  cfg.tb_common.cw_length        = E;
  cfg.cb_specific.full_length    = N_full;
  cfg.cb_specific.rm_length      = E;
  cfg.cb_specific.nof_filler_bits = 0;
  cfg.cb_specific.cw_offset       = 0;
  cfg.cb_specific.nof_crc_bits    = tb_crc_bits;

  std::mt19937                       rng(0x5eed);
  std::uniform_int_distribution<int> dist(-40, 40);
  std::vector<log_likelihood_ratio>  llrs(E);
  for (log_likelihood_ratio& llr : llrs) {
    llr = log_likelihood_ratio(static_cast<int8_t>(dist(rng)));
  }
  void* d_llrs = PuschCodeblockDecoderCudaBatchExportTest_upload_as_half(llrs, cfg.tb_common.mod);
  ASSERT_NE(d_llrs, nullptr);
  std::vector<uint8_t>                                  tb((K - tb_crc_bits) / 8);
  std::vector<codeblock_metadata::cb_specific_metadata> cb_specific = {cfg.cb_specific};

  // A healthy resident decode leaves soft bits to export.
  auto healthy = decoder->decode_resident_softbits(
      d_llrs, E, nullptr, cfg.tb_common, cb_specific, tb, crc_generator_poly::CRC16, 6, nullptr, -1);
  EXPECT_FALSE(healthy.tb_crc_ok);
  ASSERT_TRUE(decoder->is_accelerator_usable());

  // The resident decode runs on the caller's stream; hold it behind a gate.
  cudaStream_t caller_stream = nullptr;
  ASSERT_EQ(cudaStreamCreateWithFlags(&caller_stream, cudaStreamNonBlocking), cudaSuccess);
  {
    stream_gate caller_gate(caller_stream);
    if (!caller_gate.supported) {
      cudaFree(d_llrs);
      GTEST_SKIP() << "CUDA stream memory operations are not supported by this device.";
    }
    auto start = std::chrono::steady_clock::now();
    auto stuck = decoder->decode_resident_softbits(
        d_llrs, E, nullptr, cfg.tb_common, cb_specific, tb, crc_generator_poly::CRC16, 6, caller_stream, -1);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
    EXPECT_FALSE(stuck.tb_crc_ok);
    EXPECT_FALSE(stuck.all_cb_crcs_ok);

    // Nothing of the failed decode can be exported, nor anything of the healthy one before it.
    std::vector<log_likelihood_ratio>       exported(N_full);
    std::vector<span<log_likelihood_ratio>> buffers = {exported};
    EXPECT_FALSE(decoder->export_resident_softbits(buffers));

    // Every decoder sharing the quarantine is off the GPU.
    EXPECT_FALSE(decoder->is_accelerator_usable());
    EXPECT_FALSE(sibling->is_accelerator_usable());
    EXPECT_EQ(sibling->get_nof_gpu_wait_timeouts(), 1U);

    // A host-visible batch decode whose wait times out fails every codeblock.
    stream_gate                             decoder_gate(decoder->get_cuda_stream());
    std::vector<log_likelihood_ratio>       soft_buffer(N_full, log_likelihood_ratio(0));
    std::vector<span<log_likelihood_ratio>> rm_buffers = {soft_buffer};
    dynamic_bit_buffer                      cb_data(K);
    std::vector<bit_buffer>                 cb_data_buffers = {cb_data};
    std::vector<described_rx_codeblock>     codeblocks      = {{llrs, cfg}};
    auto                                    results =
        decoder->decode_batch(codeblocks, cb_data_buffers, rm_buffers, true, crc_generator_poly::CRC16, true, 6);
    ASSERT_EQ(results.size(), 1U);
    EXPECT_FALSE(results.front().crc_ok);
    EXPECT_EQ(results.front().nof_iterations, 6U);
    EXPECT_EQ(decoder->get_nof_gpu_wait_timeouts(), 2U);

    // Releasing one stream is not enough.
    caller_gate.release();
    ASSERT_EQ(cudaStreamSynchronize(caller_stream), cudaSuccess);
    EXPECT_FALSE(sibling->is_accelerator_usable());
  }

  // Once every stuck stream has drained, the decoders use the GPU again.
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!sibling->is_accelerator_usable() && (std::chrono::steady_clock::now() < deadline)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(sibling->is_accelerator_usable());
  EXPECT_TRUE(decoder->is_accelerator_usable());

  EXPECT_EQ(cudaStreamDestroy(caller_stream), cudaSuccess);
  cudaFree(d_llrs);
}
