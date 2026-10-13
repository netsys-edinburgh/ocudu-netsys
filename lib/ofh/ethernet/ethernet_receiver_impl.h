// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ethernet_rx_buffer_pool.h"
#include "ethernet_rx_metrics_collector_impl.h"
#include "ocudu/ocudulog/logger.h"
#include "ocudu/ofh/ethernet/ethernet_controller.h"
#include "ocudu/ofh/ethernet/ethernet_receiver.h"
#include "ocudu/ofh/ethernet/ethernet_receiver_config.h"
#include "ocudu/support/synchronization/stop_event.h"

namespace ocudu {

class task_executor;

namespace ether {

/// Implementation for the Ethernet receiver.
class receiver_impl : public receiver, private receiver_operation_controller
{
  static constexpr unsigned BUFFER_SIZE = 9600;
  /// \brief Maximum number of frames read in one receive wake-up.
  ///
  /// Reading the queued frames in one wake-up keeps up with bursts of U-Plane frames (a 2-port 40 MHz uplink slot is
  /// tens of frames within a few hundred microseconds), where one frame per wake-up let the socket queue overflow.
  /// The bound keeps the transmit tasks that share the executor on time: at a few microseconds per frame it delays
  /// them by a few tens of microseconds at most, well inside a downlink U-Plane transmission window.
  static constexpr unsigned MAX_FRAMES_PER_WAKEUP = 8;
  /// \brief Requested socket receive buffer size in bytes.
  ///
  /// Absorbs a scheduling stall of the receiver (the default net.core.rmem_default, typically 208 KiB, holds a few
  /// dozen jumbo frames). Set with SO_RCVBUFFORCE when permitted (CAP_NET_ADMIN), SO_RCVBUF otherwise.
  static constexpr int SOCKET_RECEIVE_BUFFER_SIZE = 16 * 1024 * 1024;

public:
  receiver_impl(const receiver_config& config, task_executor& executor_, ocudulog::basic_logger& logger_);

  ~receiver_impl() override;

  // See interface for documentation.
  receiver_operation_controller& get_operation_controller() override { return *this; }

  // See interface for documentation.
  receiver_metrics_collector* get_metrics_collector() override;

private:
  // See interface for documentation.
  void start(frame_notifier& notifier_) override;

  // See interface for documentation.
  void stop() override;

  /// Main receiving loop.
  void receive_loop();

  /// Receives new Ethernet frames from the socket.
  ///
  /// \note This function will block until new frames become available.
  void receive();

  /// Reads one frame from the socket without blocking and notifies it. Returns false if no frame was read.
  bool receive_frame();

  /// Reports the frames the kernel dropped on this socket since the previous call.
  void report_socket_drops();

private:
  ocudulog::basic_logger&         logger;
  task_executor&                  executor;
  frame_notifier*                 notifier;
  int                             socket_fd = -1;
  rt_stop_event_source            stop_manager;
  ethernet_rx_buffer_pool         buffer_pool;
  receiver_metrics_collector_impl metrics_collector;
  uint64_t                        nof_consecutive_buffer_exhaustions = 0;
  /// Monotonic time of the previous receive poll, in nanoseconds.
  uint64_t last_poll_ns = 0;
  /// Monotonic time of the previous socket drop report, in nanoseconds.
  uint64_t last_socket_drops_report_ns = 0;
};

} // namespace ether
} // namespace ocudu
