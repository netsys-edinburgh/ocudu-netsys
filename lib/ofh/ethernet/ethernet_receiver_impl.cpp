// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ethernet_receiver_impl.h"
#include "../support/ofh_ul_rx_diagnostics.h"
#include "ethernet_rx_buffer_impl.h"
#include "ocudu/instrumentation/traces/ofh_traces.h"
#include "ocudu/ofh/ethernet/ethernet_frame_notifier.h"
#include "ocudu/ofh/ethernet/ethernet_properties.h"
#include "ocudu/support/error_handling.h"
#include "ocudu/support/executors/task_executor.h"
#include "ocudu/support/synchronization/sync_event.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <linux/if_packet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace ocudu;
using namespace ether;

namespace {

class dummy_frame_notifier : public frame_notifier
{
  // See interface for documentation.
  void on_new_frame(ether::unique_rx_buffer buffer) override {}
};

} // namespace

/// This dummy object is passed to the constructor of the receiver implementation as a placeholder for the
/// actual frame notifier, which will be later set up through the \ref start() method.
static dummy_frame_notifier dummy_notifier;

/// Returns the given clock in nanoseconds.
static uint64_t clock_now_ns(clockid_t clock)
{
  ::timespec ts = {};
  ::clock_gettime(clock, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

receiver_impl::receiver_impl(const receiver_config& config, task_executor& executor_, ocudulog::basic_logger& logger_) :
  logger(logger_),
  executor(executor_),
  notifier(&dummy_notifier),
  buffer_pool(BUFFER_SIZE),
  metrics_collector(config.are_metrics_enabled)
{
  socket_fd = ::socket(AF_PACKET, SOCK_RAW, htons(ECPRI_ETH_TYPE));
  if (socket_fd < 0) {
    report_error("Unable to open raw socket for Ethernet receiver: {}", ::strerror(errno));
  }

  if (config.interface.size() > (IFNAMSIZ - 1)) {
    report_error("The Ethernet receiver interface name '{}' exceeds the maximum allowed length", config.interface);
  }

  if (config.is_promiscuous_mode_enabled) {
    // Set interface to promiscuous mode.
    ::ifreq if_opts;
    ::strncpy(if_opts.ifr_name, config.interface.c_str(), IFNAMSIZ - 1);
    if (::ioctl(socket_fd, SIOCGIFFLAGS, &if_opts) < 0) {
      report_error("Unable to get flags for NIC interface '{}' in the Ethernet receiver", config.interface);
    }
    if_opts.ifr_flags |= IFF_PROMISC;
    if (::ioctl(socket_fd, SIOCSIFFLAGS, &if_opts) < 0) {
      report_error("Unable to set flags for NIC interface '{}' in the Ethernet receiver", config.interface);
    }
  }

  // Bind to device.
  if (::setsockopt(socket_fd, SOL_SOCKET, SO_BINDTODEVICE, config.interface.c_str(), config.interface.size()) == -1) {
    report_error("Unable to bind socket to the NIC interface '{}' in Ethernet receiver", config.interface);
  }

  // Size the receive queue for bursts (see SOCKET_RECEIVE_BUFFER_SIZE).
  if (::setsockopt(
          socket_fd, SOL_SOCKET, SO_RCVBUFFORCE, &SOCKET_RECEIVE_BUFFER_SIZE, sizeof(SOCKET_RECEIVE_BUFFER_SIZE)) < 0) {
    ::setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &SOCKET_RECEIVE_BUFFER_SIZE, sizeof(SOCKET_RECEIVE_BUFFER_SIZE));
  }
  int       receive_buffer_size = 0;
  socklen_t optlen              = sizeof(receive_buffer_size);
  ::getsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer_size, &optlen);

  // Kernel receive timestamps (CLOCK_REALTIME) let the reception window be evaluated at the time the frame arrived
  // rather than when it is processed.
  const int enable_timestamps = 1;
  if (::setsockopt(socket_fd, SOL_SOCKET, SO_TIMESTAMPNS, &enable_timestamps, sizeof(enable_timestamps)) < 0) {
    logger.warning("Could not enable kernel receive timestamps on the Ethernet receiver socket: {}", ::strerror(errno));
  }

  logger.info("Opened successfully the NIC interface '{}' (fd = '{}', receive buffer = {} bytes) used by the Ethernet "
              "receiver",
              config.interface,
              socket_fd,
              receive_buffer_size);
}

receiver_impl::~receiver_impl()
{
  ::close(socket_fd);
}

void receiver_impl::start(frame_notifier& notifier_)
{
  logger.info("Starting the ethernet frame receiver");

  stop_manager.reset();

  notifier = &notifier_;

  sync_event wait_event;
  if (!executor.defer([this, token = wait_event.get_token()] { receive_loop(); })) {
    report_error("Unable to start the ethernet frame receiver, fd = '{}'", socket_fd);
  }

  // Block waiting for receiver executor to start.
  wait_event.wait();

  logger.info("Started the ethernet frame receiver with fd = '{}'", socket_fd);
}

void receiver_impl::stop()
{
  logger.info("Requesting stop of the ethernet frame receiver with fd = '{}'", socket_fd);
  stop_manager.stop();
  logger.info("Stopped the ethernet frame receiver with fd = '{}'", socket_fd);
}

void receiver_impl::receive_loop()
{
  auto token = stop_manager.get_token();
  if (OCUDU_UNLIKELY(token.is_stop_requested())) {
    return;
  }

  receive();

  // Retry the task deferring when it fails.
  // Note: token is captured by copy so that a failed defer (which destroys the passed task and its token copy) does not
  // leave it in a moved-from state for the retries.
  while (!executor.defer([this, token]() { receive_loop(); })) {
    std::this_thread::sleep_for(std::chrono::microseconds(10));
  }
}

/// Blocking function that waits for incoming data over the socket or until the specified timeout expires.
static bool wait_for_data(int socket, std::chrono::microseconds timeout)
{
  fd_set read_fs;
  FD_ZERO(&read_fs);
  FD_SET(socket, &read_fs);
  timeval tv = {0, static_cast<__suseconds_t>(timeout.count())};

  return (::select(socket + 1, &read_fs, nullptr, nullptr, &tv) > 0);
}

void receiver_impl::receive()
{
  ofh::ul_rx_diagnostics& diagnostics = ofh::ul_rx_diagnostics::get();
  const uint64_t          now_ns      = clock_now_ns(CLOCK_MONOTONIC);
  const uint64_t          poll_gap_ns = (last_poll_ns != 0) ? (now_ns - last_poll_ns) : 0;
  last_poll_ns                        = now_ns;
  if (diagnostics.enabled() && (now_ns - last_socket_drops_report_ns >= 1000000000ULL)) {
    last_socket_drops_report_ns = now_ns;
    report_socket_drops();
    diagnostics.report_if_due(logger);
  }

  if (!wait_for_data(socket_fd, std::chrono::microseconds(5))) {
    diagnostics.on_rx_wakeup(0, poll_gap_ns);
    return;
  }

  unsigned nof_frames = 0;
  while ((nof_frames != MAX_FRAMES_PER_WAKEUP) && receive_frame()) {
    ++nof_frames;
  }
  diagnostics.on_rx_wakeup(nof_frames, poll_gap_ns);
}

bool receiver_impl::receive_frame()
{
  auto        meas = metrics_collector.create_time_execution_measurer();
  trace_point tp   = ofh_tracer.now();

  auto exp_buffer = buffer_pool.reserve();
  if (!exp_buffer) {
    ++nof_consecutive_buffer_exhaustions;
    // Log the first failure and then at exponentially increasing intervals. Logging every failed attempt makes the
    // receiver spin on a readable socket and can turn a short executor stall into a persistent logger overload.
    if ((nof_consecutive_buffer_exhaustions & (nof_consecutive_buffer_exhaustions - 1U)) == 0U) {
      logger.warning("No buffer is available for receiving an Ethernet packet on the port bound to fd = '{}'. "
                     "Consecutive failed receive attempts: '{}'",
                     socket_fd,
                     nof_consecutive_buffer_exhaustions);
    }
    return false;
  }
  if (nof_consecutive_buffer_exhaustions != 0) {
    logger.warning("Ethernet packet reception on the port bound to fd = '{}' recovered after '{}' failed buffer "
                   "reservation attempts",
                   socket_fd,
                   nof_consecutive_buffer_exhaustions);
    nof_consecutive_buffer_exhaustions = 0;
  }
  ethernet_rx_buffer_impl buffer    = std::move(*exp_buffer);
  span<uint8_t>           data_span = buffer.storage();

  ::iovec                    iov = {data_span.data(), BUFFER_SIZE};
  alignas(::cmsghdr) uint8_t control[CMSG_SPACE(sizeof(::timespec))];
  ::msghdr                   msg = {};
  msg.msg_iov                    = &iov;
  msg.msg_iovlen                 = 1;
  msg.msg_control                = control;
  msg.msg_controllen             = sizeof(control);

  auto nof_bytes = ::recvmsg(socket_fd, &msg, MSG_DONTWAIT);
  if (nof_bytes < 0) {
    if ((errno != EAGAIN) && (errno != EWOULDBLOCK)) {
      logger.warning("Ethernet receiver call to recvmsg failed, fd = '{}': {}", socket_fd, ::strerror(errno));
      metrics_collector.update_stats(meas.stop());
    }
    return false;
  }
  const uint64_t read_ns = clock_now_ns(CLOCK_REALTIME);

  uint64_t kernel_rx_ns = 0;
  for (::cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
    if ((cmsg->cmsg_level == SOL_SOCKET) && (cmsg->cmsg_type == SCM_TIMESTAMPNS)) {
      ::timespec ts;
      std::memcpy(&ts, CMSG_DATA(cmsg), sizeof(ts));
      kernel_rx_ns = static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
    }
  }

  buffer.resize(nof_bytes);
  buffer.set_timestamps(kernel_rx_ns, read_ns);

  metrics_collector.update_stats(meas.stop(), nof_bytes);
  ofh::ul_rx_diagnostics::get().on_rx_frame((kernel_rx_ns != 0) ? static_cast<int64_t>(read_ns - kernel_rx_ns) : -1);

  notifier->on_new_frame(unique_rx_buffer(std::move(buffer)));
  ofh_tracer << trace_event("ofh_receiver", tp);
  return true;
}

void receiver_impl::report_socket_drops()
{
  // The kernel resets the counters on every read.
  ::tpacket_stats stats  = {};
  socklen_t       optlen = sizeof(stats);
  if (::getsockopt(socket_fd, SOL_PACKET, PACKET_STATISTICS, &stats, &optlen) == 0) {
    ofh::ul_rx_diagnostics::get().on_socket_drops(stats.tp_drops);
  }
}

receiver_metrics_collector* receiver_impl::get_metrics_collector()
{
  return metrics_collector.disabled() ? nullptr : &metrics_collector;
}
