// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cuda_runtime.h>
#include <limits>
#include <thread>

/// \brief Gets the longest a host thread waits for GPU work before treating it as failed.
///
/// A healthy wait lasts at most a few slots; reaching this bound means the GPU work is stuck, and the wait reports a
/// failure instead of blocking the thread for ever. Set it with OCUDU_CUDA_GPU_WAIT_TIMEOUT_US, in microseconds. The
/// upper PHY reads the same variable, with the same default, through phy_acceleration_gpu_wait_timeout().
inline std::chrono::microseconds ocudu_cuda_gpu_wait_timeout()
{
  static const std::chrono::microseconds timeout = []() {
    static constexpr unsigned long default_timeout_us = 100000;
    const char*                    value              = std::getenv("OCUDU_CUDA_GPU_WAIT_TIMEOUT_US");
    if ((value == nullptr) || (value[0] == '\0')) {
      return std::chrono::microseconds(default_timeout_us);
    }
    // Same rules as phy_acceleration_env_unsigned() in the upper PHY: a negative, malformed, out-of-range or zero value
    // selects the default.
    if (value[0] == '-') {
      return std::chrono::microseconds(default_timeout_us);
    }
    char* end                  = nullptr;
    errno                      = 0;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if ((end == value) || (*end != '\0') || (errno == ERANGE) || (parsed > std::numeric_limits<unsigned>::max()) ||
        (parsed == 0)) {
      return std::chrono::microseconds(default_timeout_us);
    }
    return std::chrono::microseconds(parsed);
  }();
  return timeout;
}

/// Gets the steady-clock instant at which a GPU wait that starts now gives up.
inline std::chrono::steady_clock::time_point ocudu_cuda_gpu_wait_deadline()
{
  return std::chrono::steady_clock::now() + ocudu_cuda_gpu_wait_timeout();
}

/// \brief Waits for all the work queued on \c stream, for at most the GPU wait timeout.
/// \return The stream status, or cudaErrorTimeout if the work did not finish in time.
inline cudaError_t ocudu_cuda_stream_wait_bounded(cudaStream_t stream)
{
  const auto  deadline = ocudu_cuda_gpu_wait_deadline();
  cudaError_t status;
  while ((status = cudaStreamQuery(stream)) == cudaErrorNotReady) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return cudaErrorTimeout;
    }
    std::this_thread::yield();
  }
  return status;
}

/// \brief Waits for \c event, for at most the GPU wait timeout.
/// \return The event status, or cudaErrorTimeout if the event did not complete in time.
inline cudaError_t ocudu_cuda_event_wait_bounded(cudaEvent_t event)
{
  const auto  deadline = ocudu_cuda_gpu_wait_deadline();
  cudaError_t status;
  while ((status = cudaEventQuery(event)) == cudaErrorNotReady) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return cudaErrorTimeout;
    }
    std::this_thread::yield();
  }
  return status;
}
