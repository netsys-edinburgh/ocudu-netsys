// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/adt/complex.h"
#include "ocudu/adt/span.h"
#include "ocudu/ofh/serdes/ofh_uplane_message_properties.h"
#include "ocudu/support/units.h"

namespace ocudu {
class resource_grid_reader;

namespace ofh {

/// \brief Open Fronthaul User-Plane message builder interface.
///
/// Builds a User Plane message following the O-RAN Open Fronthaul specification.
class uplane_message_builder
{
public:
  /// Default destructor.
  virtual ~uplane_message_builder() = default;

  /// Returns the Open Fronthaul User-Plane header size in bytes.
  virtual units::bytes get_header_size(const ru_compression_params& params) const = 0;

  /// \brief Builds a User Plane message given the \c params parameters, placing the result in \c buffer.
  ///
  /// \param[out] buffer Buffer where the message will be built.
  /// \param[in] iq_data IQ samples.
  /// \param[in] params  User plane message parameters.
  /// \return Number of bytes serialized in the buffer.
  virtual unsigned
  build_message(span<uint8_t> buffer, span<const cbf16_t> iq_data, const uplane_message_params& params) = 0;

  /// \brief Builds a User Plane message directly from a device-visible resource-grid symbol when supported.
  ///
  /// Implementations that cannot consume the resource grid without staging through a host IQ span return false.
  ///
  /// \param[out] buffer        Buffer where the message will be built.
  /// \param[in]  grid          Resource-grid reader exposing the source symbol.
  /// \param[in]  port          Resource-grid port index.
  /// \param[in]  params        User plane message parameters.
  /// \param[out] bytes_written Number of bytes serialized in the buffer on success.
  /// \return True on success, false when this path is unsupported or cannot be used.
  virtual bool build_message(span<uint8_t>                buffer,
                             const resource_grid_reader&  grid,
                             unsigned                     port,
                             const uplane_message_params& params,
                             unsigned&                    bytes_written)
  {
    (void)buffer;
    (void)grid;
    (void)port;
    (void)params;
    bytes_written = 0;
    return false;
  }

  /// \brief Builds multiple User Plane messages directly from contiguous device-visible resource-grid ports.
  ///
  /// Implementations that cannot batch the resource grid without staging through host IQ spans return false.
  ///
  /// \param[out] buffers       Buffers where each message will be built.
  /// \param[in]  grid          Resource-grid reader exposing the source symbol.
  /// \param[in]  first_port    First resource-grid port index.
  /// \param[in]  params        User plane message parameters shared by all ports.
  /// \param[out] bytes_written Number of bytes serialized in each buffer on success.
  /// \return True on success, false when this path is unsupported or cannot be used.
  virtual bool build_messages(span<span<uint8_t>>          buffers,
                              const resource_grid_reader&  grid,
                              unsigned                     first_port,
                              const uplane_message_params& params,
                              span<unsigned>               bytes_written)
  {
    (void)buffers;
    (void)grid;
    (void)first_port;
    (void)params;
    for (unsigned& value : bytes_written) {
      value = 0;
    }
    return false;
  }

  /// \brief Builds multiple User Plane messages for a symbol and port batch from a device-visible resource grid.
  ///
  /// Buffers are ordered by symbol first and port second.
  ///
  /// Implementations that cannot batch the resource grid without staging through host IQ spans return false.
  ///
  /// \param[out] buffers       Buffers where each message will be built.
  /// \param[in]  grid          Resource-grid reader exposing the source symbols.
  /// \param[in]  first_port    First resource-grid port index.
  /// \param[in]  first_symbol  First OFDM symbol index.
  /// \param[in]  nof_symbols   Number of consecutive OFDM symbols.
  /// \param[in]  params        User plane message parameters shared by all messages except symbol id.
  /// \param[out] bytes_written Number of bytes serialized in each buffer on success.
  /// \return True on success, false when this path is unsupported or cannot be used.
  virtual bool build_symbol_batch_messages(span<span<uint8_t>>          buffers,
                                           const resource_grid_reader&  grid,
                                           unsigned                     first_port,
                                           unsigned                     first_symbol,
                                           unsigned                     nof_symbols,
                                           const uplane_message_params& params,
                                           span<unsigned>               bytes_written)
  {
    (void)buffers;
    (void)grid;
    (void)first_port;
    (void)first_symbol;
    (void)nof_symbols;
    (void)params;
    for (unsigned& value : bytes_written) {
      value = 0;
    }
    return false;
  }

  /// \brief Builds the headers of the messages of a symbol and port batch, leaving their IQ payload to the caller.
  ///
  /// Same messages as build_symbol_batch_messages() (buffers ordered by symbol first and port second), but only the
  /// headers are serialized: \c payloads receives, per message, the span its compressed PRBs go to, so that the caller
  /// can compress several batches (e.g. every fragment of a slot) at once.
  ///
  /// \param[out] buffers       Buffers where each message is built.
  /// \param[in]  first_symbol  First OFDM symbol index.
  /// \param[in]  nof_symbols   Number of consecutive OFDM symbols.
  /// \param[in]  params        User plane message parameters shared by all messages except symbol id.
  /// \param[out] payloads      Payload span of each message, params.nof_prb compressed PRBs long.
  /// \param[out] bytes_written Size of each complete message (headers and payload).
  /// \return True on success, false when this path is unsupported or the arguments are inconsistent.
  virtual bool build_symbol_batch_message_headers(span<span<uint8_t>>          buffers,
                                                  unsigned                     first_symbol,
                                                  unsigned                     nof_symbols,
                                                  const uplane_message_params& params,
                                                  span<span<uint8_t>>          payloads,
                                                  span<unsigned>               bytes_written)
  {
    (void)buffers;
    (void)first_symbol;
    (void)nof_symbols;
    (void)params;
    (void)payloads;
    for (unsigned& value : bytes_written) {
      value = 0;
    }
    return false;
  }
};

} // namespace ofh
} // namespace ocudu
