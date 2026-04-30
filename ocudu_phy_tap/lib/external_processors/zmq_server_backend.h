// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/support/error_handling.h"
#include <string>
#include <zmq.h>

namespace ocudu {

/// \brief ZeroMQ based server backend for sending single-precision samples.
///
/// This class implements a server backend that uses ZeroMQ in PUSH mode to send single-precision floating-point samples
/// to connected clients. The server expects clients to connect using ZMQ_PULL mode for receiving the data.
class zmq_server_backend
{
public:
  /// \brief Constructs a ZMQ server backend and binds it to the specified address.
  ///
  /// Creates a ZeroMQ context and socket in PUSH mode, and binds the socket to the provided address. The socket is
  /// configured with no send timeout.
  ///
  /// \param address_ The ZeroMQ address to bind the socket to (e.g., "tcp://*:5555")
  explicit zmq_server_backend(const std::string& address_) : address(address_)
  {
    context = ::zmq_ctx_new();
    report_fatal_error_if_not(context != nullptr, "Failed to create ZMQ context: {}", ::zmq_strerror(::zmq_errno()));

    socket = ::zmq_socket(context, ZMQ_PUSH);
    report_fatal_error_if_not(socket != nullptr, "Failed to create ZMQ socket: {}", ::zmq_strerror(::zmq_errno()));

    int send_timeout = 0;
    report_fatal_error_if_not(::zmq_setsockopt(socket, ZMQ_SNDTIMEO, &send_timeout, sizeof(send_timeout)) == 0,
                              "Failed to configure socket: {}",
                              ::zmq_strerror(::zmq_errno()));

    report_fatal_error_if_not(
        ::zmq_bind(socket, address.c_str()) == 0, "Failed to bind socket: {}", ::zmq_strerror(::zmq_errno()));
  }

  /// \brief Default destructor - ensures safe closing of ZMQ components.
  ///
  /// Unbinds the socket, closes the socket, and destroys the ZeroMQ context.
  ~zmq_server_backend()
  {
    ::zmq_unbind(socket, address.c_str());
    ::zmq_close(socket);
    ::zmq_ctx_destroy(context);
  }

  /// \brief Sends a buffer of single-precision samples.
  ///
  /// Transmits the specified buffer of single-precision floating-point values through the ZeroMQ PUSH socket.
  ///
  /// \param buffer The span of float values to send
  void send_buffer(span<const float> buffer) { ::zmq_send(socket, buffer.data(), sizeof(float) * buffer.size(), 0); }

private:
  /// ZeroMQ bind address.
  std::string address;
  /// ZeroMQ context pointer.
  void* context = nullptr;
  /// ZeroMQ socket pointer.
  void* socket = nullptr;
};

} // namespace ocudu
