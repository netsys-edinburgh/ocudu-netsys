// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "../../../phy_acceleration_runtime_options.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cuda_runtime.h>
#include <mutex>
#include <sched.h>
#include <vector>

namespace ocudu {

enum class upper_phy_cuda_stream_priority_mode { normal, high, low };

inline upper_phy_cuda_stream_priority_mode parse_upper_phy_cuda_stream_priority(const char* value)
{
  if ((value == nullptr) || (*value == '\0')) {
    return upper_phy_cuda_stream_priority_mode::normal;
  }

  if ((std::strcmp(value, "1") == 0) || (std::strcmp(value, "true") == 0) || (std::strcmp(value, "on") == 0) ||
      (std::strcmp(value, "yes") == 0) || (std::strcmp(value, "high") == 0) || (std::strcmp(value, "realtime") == 0)) {
    return upper_phy_cuda_stream_priority_mode::high;
  }

  if ((std::strcmp(value, "low") == 0) || (std::strcmp(value, "lowest") == 0) ||
      (std::strcmp(value, "background") == 0)) {
    return upper_phy_cuda_stream_priority_mode::low;
  }

  return upper_phy_cuda_stream_priority_mode::normal;
}

inline upper_phy_cuda_stream_priority_mode get_upper_phy_cuda_stream_priority(
    const char*                         role_env_name = nullptr,
    upper_phy_cuda_stream_priority_mode default_mode  = upper_phy_cuda_stream_priority_mode::normal)
{
  const char* value = (role_env_name != nullptr) ? std::getenv(role_env_name) : nullptr;
  if ((value != nullptr) && (*value != '\0')) {
    return parse_upper_phy_cuda_stream_priority(value);
  }

  value = std::getenv("OCUDU_UPPER_PHY_CUDA_STREAM_PRIORITY");
  if ((value != nullptr) && (*value != '\0')) {
    return parse_upper_phy_cuda_stream_priority(value);
  }

  value = std::getenv("OCUDU_UPPER_PHY_CUDA_HIGH_PRIORITY");
  if ((value == nullptr) || (*value == '\0')) {
    return default_mode;
  }

  return parse_upper_phy_cuda_stream_priority(value);
}

inline cudaError_t
cudaStreamCreateUpperPhy(cudaStream_t*                       stream,
                         const char*                         role_env_name = nullptr,
                         upper_phy_cuda_stream_priority_mode default_mode = upper_phy_cuda_stream_priority_mode::normal)
{
  const upper_phy_cuda_stream_priority_mode priority_mode =
      get_upper_phy_cuda_stream_priority(role_env_name, default_mode);
  if (priority_mode == upper_phy_cuda_stream_priority_mode::normal) {
    return cudaStreamCreateWithFlags(stream, cudaStreamNonBlocking);
  }

  int         least_priority    = 0;
  int         greatest_priority = 0;
  cudaError_t status            = cudaDeviceGetStreamPriorityRange(&least_priority, &greatest_priority);
  if (status == cudaSuccess) {
    const int requested_priority =
        (priority_mode == upper_phy_cuda_stream_priority_mode::high) ? greatest_priority : least_priority;
    status = cudaStreamCreateWithPriority(stream, cudaStreamNonBlocking, requested_priority);
    if (status == cudaSuccess) {
      return cudaSuccess;
    }
    (void)cudaGetLastError();
  }

  return cudaStreamCreateWithFlags(stream, cudaStreamNonBlocking);
}

/// \brief RT-friendly CUDA stream synchronization.
///
/// This function synchronizes on a CUDA stream while yielding the CPU to other threads.
/// When the calling thread has real-time (SCHED_FIFO/SCHED_RR) priority, a blocking
/// cudaStreamSynchronize() can cause priority inversion by preventing GPU driver threads
/// (which run at normal priority) from making progress.
///
/// This function uses cudaStreamQuery() in a loop with sched_yield() to allow other
/// threads to run while waiting for the GPU to complete.
///
/// \param stream The CUDA stream to synchronize on.
/// \return cudaSuccess on success, or an error code if the stream had an error.
/// Experiment knob: OCUDU_CUDA_SYNC_BLOCKING=1 switches the yielding query loops to
/// plain blocking cudaStreamSynchronize/cudaEventSynchronize. On integrated GPUs the
/// ~1 MHz cudaEventQuery polling can starve driver-side dispatch of queued stream work.
inline bool cuda_sync_blocking_enabled()
{
  static const bool enabled = []() {
    const char* v = std::getenv("OCUDU_CUDA_SYNC_BLOCKING");
    return (v != nullptr) && (v[0] != '0');
  }();
  return enabled;
}

/// Experiment knob: OCUDU_CUDA_SYNC_SLEEP_US=N throttles the query loops to one
/// cudaStreamQuery/cudaEventQuery every N microseconds (0 = poll continuously with
/// sched_yield only). Throttling keeps host wake-up latency in the low microseconds
/// while freeing the driver to dispatch queued stream work between polls.
inline unsigned cuda_sync_sleep_us()
{
  static const unsigned sleep_us = []() -> unsigned {
    const char* v = std::getenv("OCUDU_CUDA_SYNC_SLEEP_US");
    if (v == nullptr || *v == '\0') {
      return 0;
    }
    return static_cast<unsigned>(std::strtoul(v, nullptr, 10));
  }();
  return sleep_us;
}

/// Experiment knob: OCUDU_CUDA_SYNC_SPIN_US=N busy-spins ~N microseconds between
/// queries (sched_yield only, no CUDA calls, no timers). Unlike nanosleep this is not
/// subject to timer slack (~50us for non-RT threads), so the wake latency stays ~N us
/// while the driver is left alone to dispatch queued stream work between polls.
inline unsigned cuda_sync_spin_us()
{
  static const unsigned spin_us = []() -> unsigned {
    const char* v = std::getenv("OCUDU_CUDA_SYNC_SPIN_US");
    if (v == nullptr || *v == '\0') {
      return 0;
    }
    return static_cast<unsigned>(std::strtoul(v, nullptr, 10));
  }();
  return spin_us;
}

inline void cuda_sync_poll_pause()
{
  const unsigned spin_us = cuda_sync_spin_us();
  if (spin_us != 0) {
    timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    const long budget_ns = static_cast<long>(spin_us) * 1000L;
    for (;;) {
      sched_yield();
      timespec t1;
      clock_gettime(CLOCK_MONOTONIC, &t1);
      long elapsed_ns = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
      if (elapsed_ns >= budget_ns) {
        return;
      }
    }
  }
  const unsigned sleep_us = cuda_sync_sleep_us();
  if (sleep_us == 0) {
    sched_yield();
    return;
  }
  timespec ts = {0, static_cast<long>(sleep_us) * 1000L};
  nanosleep(&ts, nullptr);
}

inline cudaError_t cudaStreamSynchronizeYielding(cudaStream_t stream)
{
  if (cuda_sync_blocking_enabled()) {
    return cudaStreamSynchronize(stream);
  }
  cudaError_t err;
  while ((err = cudaStreamQuery(stream)) == cudaErrorNotReady) {
    cuda_sync_poll_pause();
  }
  return err;
}

/// \brief RT-friendly CUDA event synchronization.
///
/// This function synchronizes on a CUDA event while yielding the CPU to other threads.
/// Similar to cudaStreamSynchronizeYielding, this avoids priority inversion by polling
/// the event status and yielding between checks.
///
/// \param event The CUDA event to synchronize on.
/// \return cudaSuccess on success, or an error code if the event had an error.
inline cudaError_t cudaEventSynchronizeYielding(cudaEvent_t event)
{
  if (cuda_sync_blocking_enabled()) {
    return cudaEventSynchronize(event);
  }
  cudaError_t err;
  while ((err = cudaEventQuery(event)) == cudaErrorNotReady) {
    cuda_sync_poll_pause();
  }
  return err;
}

/// \brief Waits for all the work queued on \c stream, for at most the GPU wait timeout, while yielding the CPU.
///
/// Polls like cudaStreamSynchronizeYielding(), but gives up once phy_acceleration_gpu_wait_timeout() has elapsed: a
/// healthy wait lasts at most a few slots, so reaching the bound means the GPU work is stuck.
/// \return The stream status, or cudaErrorTimeout if the work did not finish in time.
/// \note The experiment knob OCUDU_CUDA_SYNC_BLOCKING=1 waits in cudaStreamSynchronize(), which has no bound.
inline cudaError_t cuda_stream_wait_bounded(cudaStream_t stream)
{
  if (cuda_sync_blocking_enabled()) {
    return cudaStreamSynchronize(stream);
  }
  const auto  deadline = std::chrono::steady_clock::now() + phy_acceleration_gpu_wait_timeout();
  cudaError_t err;
  while ((err = cudaStreamQuery(stream)) == cudaErrorNotReady) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return cudaErrorTimeout;
    }
    cuda_sync_poll_pause();
  }
  return err;
}

/// \brief Waits for \c event, for at most the GPU wait timeout, while yielding the CPU.
/// \return The event status, or cudaErrorTimeout if the event did not complete in time.
/// \note The experiment knob OCUDU_CUDA_SYNC_BLOCKING=1 waits in cudaEventSynchronize(), which has no bound.
inline cudaError_t cuda_event_wait_bounded(cudaEvent_t event)
{
  if (cuda_sync_blocking_enabled()) {
    return cudaEventSynchronize(event);
  }
  const auto  deadline = std::chrono::steady_clock::now() + phy_acceleration_gpu_wait_timeout();
  cudaError_t err;
  while ((err = cudaEventQuery(event)) == cudaErrorNotReady) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return cudaErrorTimeout;
    }
    cuda_sync_poll_pause();
  }
  return err;
}

/// \brief Keeps a GPU component off the GPU after one of its waits timed out, until the work queued at that time has
/// finished.
///
/// A timed-out wait leaves its GPU work queued, and that work may still write buffers that newer requests use. The
/// component reports each timeout with the stream it waited on; an event recorded there completes only after everything
/// queued on that stream before it. Before using the GPU, the component polls without waiting. If the CUDA context has
/// failed, or the event cannot be recorded, the work is never known to have finished and the component stays off the
/// GPU.
class cuda_gpu_wait_quarantine
{
public:
  /// Result of a poll.
  enum class state {
    /// No timed-out work is pending: the component may use the GPU.
    on_gpu,
    /// Timed-out work is still pending: the component must not use the GPU.
    off_gpu,
    /// The last pending timed-out work has just finished: the component may use the GPU again.
    back_on_gpu
  };

  cuda_gpu_wait_quarantine() = default;

  ~cuda_gpu_wait_quarantine()
  {
    for (const pending_work& work : pending) {
      cudaEventDestroy(work.event);
    }
  }

  cuda_gpu_wait_quarantine(const cuda_gpu_wait_quarantine&)            = delete;
  cuda_gpu_wait_quarantine& operator=(const cuda_gpu_wait_quarantine&) = delete;

  /// \brief Takes the component off the GPU after a wait on \c stream timed out.
  /// \return The number of timeouts reported so far, including this one.
  unsigned on_wait_timeout(cudaStream_t stream)
  {
    std::lock_guard<std::mutex> lock(mutex);
    ++nof_timeouts;
    off_gpu = true;

    // Reuse the event of the same stream, so that repeated timeouts on one stream take a single entry.
    unsigned index = 0;
    while ((index != pending.size()) && (pending[index].stream != stream)) {
      ++index;
    }
    if (index == pending.size()) {
      cudaEvent_t event = nullptr;
      if (cudaEventCreateWithFlags(&event, cudaEventDisableTiming) != cudaSuccess) {
        static_cast<void>(cudaGetLastError());
        never_drains = true;
        return nof_timeouts;
      }
      pending.push_back({stream, event});
    }
    if (cudaEventRecord(pending[index].event, stream) != cudaSuccess) {
      static_cast<void>(cudaGetLastError());
      never_drains = true;
    }
    return nof_timeouts;
  }

  /// Tells, without waiting, whether the component may use the GPU.
  state poll()
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!off_gpu) {
      return state::on_gpu;
    }
    if (never_drains) {
      return state::off_gpu;
    }
    for (const pending_work& work : pending) {
      cudaError_t status = cudaEventQuery(work.event);
      if (status == cudaErrorNotReady) {
        return state::off_gpu;
      }
      if (status != cudaSuccess) {
        // The context has failed: the queued work will never be known to have finished.
        static_cast<void>(cudaGetLastError());
        never_drains = true;
        return state::off_gpu;
      }
    }
    // The events stay allocated for later timeouts on the same streams.
    off_gpu = false;
    return state::back_on_gpu;
  }

  /// Gets the number of timeouts reported so far.
  unsigned get_nof_timeouts() const
  {
    std::lock_guard<std::mutex> lock(mutex);
    return nof_timeouts;
  }

private:
  /// Timed-out work queued on a stream, marked by an event recorded behind it.
  struct pending_work {
    cudaStream_t stream;
    cudaEvent_t  event;
  };

  mutable std::mutex        mutex;
  std::vector<pending_work> pending;
  unsigned                  nof_timeouts = 0;
  bool                      off_gpu      = false;
  bool                      never_drains = false;
};

/// Tells whether a timeout or drop count is worth logging: the first occurrence and whenever the count doubles.
inline bool is_gpu_failure_count_logged(unsigned count)
{
  return (count != 0) && ((count & (count - 1)) == 0);
}

} // namespace ocudu
