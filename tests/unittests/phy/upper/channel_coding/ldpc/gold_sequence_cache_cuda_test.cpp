// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include <atomic>
#include <chrono>
#include <cuda.h>
#include <cuda_runtime.h>
#include <gtest/gtest.h>
#include <scrambling.h>
#include <thread>
#include <vector>

namespace {

bool is_cuda_device_available()
{
  int nof_devices = 0;
  if ((cudaGetDeviceCount(&nof_devices) != cudaSuccess) || (nof_devices == 0)) {
    (void)cudaGetLastError();
    return false;
  }
  return true;
}

/// Copies the current sequence of \c scrambler to the host.
std::vector<uint32_t> read_sequence(scrambler_handle_t scrambler, unsigned nof_words, cudaStream_t stream)
{
  std::vector<uint32_t> words(nof_words);
  const unsigned int*   d_sequence = scrambler_get_sequence_ptr(scrambler);
  if ((d_sequence == nullptr) ||
      (cudaMemcpyAsync(words.data(), d_sequence, nof_words * sizeof(uint32_t), cudaMemcpyDeviceToHost, stream) !=
       cudaSuccess) ||
      (cudaStreamSynchronize(stream) != cudaSuccess)) {
    words.clear();
  }
  return words;
}

} // namespace

// The process-wide Gold-sequence cache evicts sequences when more keys are live than it holds, and frees the evicted
// buffers once the device is idle. A stuck GPU must not hold the scramblers, which every PUSCH and PDSCH worker shares,
// while that happens: churning the cache with another stream held behind a gate does not wait for the GPU.
TEST(gold_sequence_cache_cuda_test, eviction_does_not_wait_for_a_stuck_gpu)
{
  if (!is_cuda_device_available()) {
    GTEST_SKIP() << "No CUDA device.";
  }

  cudaStream_t stuck_stream = nullptr;
  cudaStream_t stream       = nullptr;
  ASSERT_EQ(cudaStreamCreateWithFlags(&stuck_stream, cudaStreamNonBlocking), cudaSuccess);
  ASSERT_EQ(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), cudaSuccess);

  scrambler_handle_t scrambler = nullptr;
  ASSERT_EQ(scrambler_create(&scrambler), NR_LDPC_SUCCESS);

  static constexpr int      nof_bits  = 4096;
  static constexpr unsigned nof_words = nof_bits / 32;
  static constexpr uint32_t first_key = 0x1234567;

  ASSERT_EQ(scrambler_configure_c_init(scrambler, first_key), NR_LDPC_SUCCESS);
  ASSERT_EQ(scrambler_generate_sequence(scrambler, nof_bits, stream), NR_LDPC_SUCCESS);
  const std::vector<uint32_t> first_sequence = read_sequence(scrambler, nof_words, stream);
  ASSERT_EQ(first_sequence.size(), nof_words);

  // Close the gate only now: the first use of the scrambling kernels loads their module, which synchronizes the device.
  // The gate word lives in mapped host memory and the host releases it with a plain store.
  void* host_word   = nullptr;
  void* device_word = nullptr;
  ASSERT_EQ(cudaHostAlloc(&host_word, sizeof(uint32_t), cudaHostAllocMapped), cudaSuccess);
  ASSERT_EQ(cudaHostGetDevicePointer(&device_word, host_word, 0), cudaSuccess);
  auto* gate_word = static_cast<volatile uint32_t*>(host_word);
  *gate_word      = 0;
  if (cuStreamWaitValue32(reinterpret_cast<CUstream>(stuck_stream),
                          reinterpret_cast<CUdeviceptr>(device_word),
                          1U,
                          CU_STREAM_WAIT_VALUE_EQ) != CUDA_SUCCESS) {
    *gate_word = 1;
    GTEST_SKIP() << "CUDA stream memory operations on mapped host memory are not supported by this device.";
  }

  // Enough distinct keys to evict every cached sequence and to reach the reclamation threshold more than once. The
  // churn runs on a worker so that the test can still release the gate if it waits for the GPU.
  const unsigned    initially_reclaimed = scrambler_get_nof_reclaimed_gold_sequence_buffers();
  std::atomic<bool> churn_done{false};
  std::atomic<bool> churn_ok{true};
  std::thread       churn([&]() {
    for (uint32_t i = 1; i != 1 + 128 + 3 * 64; ++i) {
      if ((scrambler_configure_c_init(scrambler, first_key + i) != NR_LDPC_SUCCESS) ||
          (scrambler_generate_sequence(scrambler, nof_bits, stream) != NR_LDPC_SUCCESS)) {
        churn_ok = false;
      }
    }
    churn_done = true;
  });

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!churn_done && (std::chrono::steady_clock::now() < deadline)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const bool done_while_stuck = churn_done;
  EXPECT_TRUE(done_while_stuck) << "the cache churn waited for the stuck GPU";

  // Nothing parked is freed while the GPU is stuck.
  EXPECT_EQ(scrambler_get_nof_reclaimed_gold_sequence_buffers(), initially_reclaimed);

  *gate_word = 1;
  churn.join();
  EXPECT_TRUE(churn_ok);
  ASSERT_EQ(cudaStreamSynchronize(stuck_stream), cudaSuccess);

  // An evicted key regenerates the same sequence.
  ASSERT_EQ(scrambler_configure_c_init(scrambler, first_key), NR_LDPC_SUCCESS);
  ASSERT_EQ(scrambler_generate_sequence(scrambler, nof_bits, stream), NR_LDPC_SUCCESS);
  EXPECT_EQ(read_sequence(scrambler, nof_words, stream), first_sequence);

  // Once the GPU is idle, the parked buffers are freed.
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while ((scrambler_get_nof_reclaimed_gold_sequence_buffers() == initially_reclaimed) &&
         (std::chrono::steady_clock::now() < deadline)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_GT(scrambler_get_nof_reclaimed_gold_sequence_buffers(), initially_reclaimed);

  scrambler_destroy(scrambler);
  EXPECT_EQ(cudaStreamDestroy(stream), cudaSuccess);
  EXPECT_EQ(cudaStreamDestroy(stuck_stream), cudaSuccess);
  EXPECT_EQ(cudaFreeHost(host_word), cudaSuccess);
}
