// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "cuda_rt_utils.h"
#include <chrono>
#include <cuda.h>
#include <gtest/gtest.h>
#include <thread>

using namespace ocudu;

namespace {

/// \brief Holds the work of a stream behind a gate until the test releases it.
///
/// The gate is a device word the stream waits on. It stands for a stuck GPU: the stream does not finish until
/// release() writes the word from another stream.
class gated_stream
{
public:
  gated_stream()
  {
    supported = (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) == cudaSuccess) &&
                (cudaStreamCreateWithFlags(&release_stream, cudaStreamNonBlocking) == cudaSuccess) &&
                (cudaMalloc(&gate_word, sizeof(*gate_word)) == cudaSuccess) &&
                (cudaMemset(gate_word, 0, sizeof(*gate_word)) == cudaSuccess) &&
                (cuStreamWaitValue32(reinterpret_cast<CUstream>(stream),
                                     reinterpret_cast<CUdeviceptr>(gate_word),
                                     1U,
                                     CU_STREAM_WAIT_VALUE_EQ) == CUDA_SUCCESS);
  }

  ~gated_stream()
  {
    release();
    if (stream != nullptr) {
      (void)cudaStreamSynchronize(stream);
      (void)cudaStreamDestroy(stream);
    }
    if (release_stream != nullptr) {
      (void)cudaStreamDestroy(release_stream);
    }
    if (gate_word != nullptr) {
      (void)cudaFree(gate_word);
    }
  }

  gated_stream(const gated_stream&)            = delete;
  gated_stream& operator=(const gated_stream&) = delete;

  /// Lets the work held behind the gate run.
  void release()
  {
    if (!released && (gate_word != nullptr) && (release_stream != nullptr)) {
      (void)cuStreamWriteValue32(reinterpret_cast<CUstream>(release_stream),
                                 reinterpret_cast<CUdeviceptr>(gate_word),
                                 1U,
                                 CU_STREAM_WRITE_VALUE_DEFAULT);
      (void)cudaStreamSynchronize(release_stream);
      released = true;
    }
  }

  bool         supported      = false;
  cudaStream_t stream         = nullptr;
  cudaStream_t release_stream = nullptr;

private:
  uint32_t* gate_word = nullptr;
  bool      released  = false;
};

bool is_cuda_device_available()
{
  int nof_devices = 0;
  if ((cudaGetDeviceCount(&nof_devices) != cudaSuccess) || (nof_devices == 0)) {
    (void)cudaGetLastError();
    return false;
  }
  return true;
}

} // namespace

// A stuck stream must not hold the waiting thread for ever: the bounded waits give up after the GPU wait timeout.
TEST(cuda_gpu_wait_test, bounded_waits_give_up_on_a_stuck_stream)
{
  if (!is_cuda_device_available()) {
    GTEST_SKIP() << "No CUDA device.";
  }
  gated_stream gate;
  if (!gate.supported) {
    GTEST_SKIP() << "CUDA stream memory operations are not supported by this device.";
  }
  cudaEvent_t event = nullptr;
  ASSERT_EQ(cudaEventCreateWithFlags(&event, cudaEventDisableTiming), cudaSuccess);
  ASSERT_EQ(cudaEventRecord(event, gate.stream), cudaSuccess);

  auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(cuda_stream_wait_bounded(gate.stream), cudaErrorTimeout);
  EXPECT_EQ(cuda_event_wait_bounded(event), cudaErrorTimeout);
  auto elapsed = std::chrono::steady_clock::now() - start;

  // Each wait lasts the timeout, and the test environment sets a short one.
  EXPECT_GE(elapsed, 2 * phy_acceleration_gpu_wait_timeout());
  EXPECT_LT(elapsed, std::chrono::seconds(5));

  gate.release();
  EXPECT_EQ(cuda_stream_wait_bounded(gate.stream), cudaSuccess);
  EXPECT_EQ(cuda_event_wait_bounded(event), cudaSuccess);
  (void)cudaEventDestroy(event);
}

// After a timeout, the component stays off the GPU until the work that timed out has finished, then returns to it once.
TEST(cuda_gpu_wait_test, quarantine_lasts_until_the_stuck_work_finishes)
{
  if (!is_cuda_device_available()) {
    GTEST_SKIP() << "No CUDA device.";
  }
  gated_stream gate;
  if (!gate.supported) {
    GTEST_SKIP() << "CUDA stream memory operations are not supported by this device.";
  }

  cuda_gpu_wait_quarantine quarantine;
  EXPECT_EQ(quarantine.poll(), cuda_gpu_wait_quarantine::state::on_gpu);

  ASSERT_EQ(cuda_stream_wait_bounded(gate.stream), cudaErrorTimeout);
  EXPECT_EQ(quarantine.on_wait_timeout(gate.stream), 1U);
  EXPECT_EQ(quarantine.on_wait_timeout(gate.stream), 2U);
  EXPECT_EQ(quarantine.poll(), cuda_gpu_wait_quarantine::state::off_gpu);
  EXPECT_EQ(quarantine.poll(), cuda_gpu_wait_quarantine::state::off_gpu);

  gate.release();
  auto                            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  cuda_gpu_wait_quarantine::state state    = quarantine.poll();
  while ((state == cuda_gpu_wait_quarantine::state::off_gpu) && (std::chrono::steady_clock::now() < deadline)) {
    std::this_thread::yield();
    state = quarantine.poll();
  }
  EXPECT_EQ(state, cuda_gpu_wait_quarantine::state::back_on_gpu);
  EXPECT_EQ(quarantine.poll(), cuda_gpu_wait_quarantine::state::on_gpu);
  EXPECT_EQ(quarantine.get_nof_timeouts(), 2U);
}

// Pooled components share one quarantine: it lasts until the stuck work of every stream has finished.
TEST(cuda_gpu_wait_test, quarantine_lasts_until_every_stuck_stream_finishes)
{
  if (!is_cuda_device_available()) {
    GTEST_SKIP() << "No CUDA device.";
  }
  gated_stream first;
  gated_stream second;
  if (!first.supported || !second.supported) {
    GTEST_SKIP() << "CUDA stream memory operations are not supported by this device.";
  }

  cuda_gpu_wait_quarantine quarantine;
  quarantine.on_wait_timeout(first.stream);
  quarantine.on_wait_timeout(second.stream);

  first.release();
  ASSERT_EQ(cuda_stream_wait_bounded(first.stream), cudaSuccess);
  EXPECT_EQ(quarantine.poll(), cuda_gpu_wait_quarantine::state::off_gpu);

  second.release();
  ASSERT_EQ(cuda_stream_wait_bounded(second.stream), cudaSuccess);
  EXPECT_EQ(quarantine.poll(), cuda_gpu_wait_quarantine::state::back_on_gpu);
  EXPECT_EQ(quarantine.poll(), cuda_gpu_wait_quarantine::state::on_gpu);
}

TEST(cuda_gpu_wait_test, failure_counts_are_logged_on_the_first_and_every_doubling)
{
  EXPECT_FALSE(is_gpu_failure_count_logged(0));
  EXPECT_TRUE(is_gpu_failure_count_logged(1));
  EXPECT_TRUE(is_gpu_failure_count_logged(2));
  EXPECT_FALSE(is_gpu_failure_count_logged(3));
  EXPECT_TRUE(is_gpu_failure_count_logged(4));
  EXPECT_FALSE(is_gpu_failure_count_logged(6));
  EXPECT_TRUE(is_gpu_failure_count_logged(1024));
}
