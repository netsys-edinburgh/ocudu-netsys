// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/adt/complex.h"
#include "ocudu/adt/span.h"
#include "ocudu/ofh/compression/compression_params.h"
#include "ocudu/ofh/compression/iq_decompression_result.h"

namespace ocudu {

class prach_buffer;
class resource_grid_writer;

namespace ofh {

/// \brief Describes the IQ data decompressor.
///
/// Deserializes compression parameters and decompresses received PRBs with compressed IQ data according to compression
/// methods specified in WG4.CUS.0 document.
class iq_decompressor
{
public:
  /// Default destructor.
  virtual ~iq_decompressor() = default;

  /// \brief Decompress received compressed PRBs.
  ///
  /// Decompresses compressed PRBs from the input buffer according to received compression parameters and puts the
  /// results into an array of brain floating point IQ samples.
  ///
  /// \param[out] iq_data  Resulting IQ samples after decompression, unspecified when this returns false.
  /// \param[in]  compressed_data A span containing received compressed IQ data and compression parameters.
  /// \param[in]  params  Compression parameters.
  /// \return True on success, false if the IQ data could not be decompressed.
  virtual bool
  decompress(span<cbf16_t> iq_data, span<const uint8_t> compressed_data, const ru_compression_params& params) = 0;

  /// Returns true if the decompressor can write directly into a device-capable resource grid.
  virtual bool supports_resource_grid_decompression() const { return false; }

  /// Returns true if the decompressor can write the given compression format directly into a device-capable resource
  /// grid. Selectors override this method so that support is checked for the compression format carried by each
  /// section rather than inferred from an unrelated registered decompressor.
  virtual bool supports_resource_grid_decompression(const ru_compression_params& params) const
  {
    (void)params;
    return supports_resource_grid_decompression();
  }

  /// Returns true when direct resource-grid decompression benefits from receiving a complete OFDM symbol per call.
  virtual bool prefers_full_symbol_decompression(const ru_compression_params& params) const
  {
    (void)params;
    return false;
  }

  /// Returns true if the decompressor can write directly into a device-capable PRACH buffer.
  virtual bool supports_prach_buffer_decompression() const { return false; }

  /// Returns true if the decompressor can write the given compression format directly into a device-capable PRACH
  /// buffer.
  virtual bool supports_prach_buffer_decompression(const ru_compression_params& params) const
  {
    (void)params;
    return supports_prach_buffer_decompression();
  }

  /// \brief Decompress received compressed PRBs directly into a resource grid.
  ///
  /// The default implementation reports that direct decompression is unsupported and leaves the host path unchanged.
  ///
  /// \param[out] grid            Destination resource grid.
  /// \param[in]  port            Resource-grid port index.
  /// \param[in]  symbol          OFDM symbol index.
  /// \param[in]  start_prb       First destination PRB.
  /// \param[in]  nof_prbs        Number of PRBs to decompress.
  /// \param[in]  compressed_data Received compressed IQ payload and compression parameters.
  /// \param[in]  params          Compression parameters.
  /// \return Structured result describing whether the operation completed and, on failure, why it did not.
  virtual iq_decompression_result decompress_to_resource_grid(resource_grid_writer&        grid,
                                                              unsigned                     port,
                                                              unsigned                     symbol,
                                                              unsigned                     start_prb,
                                                              unsigned                     nof_prbs,
                                                              span<const uint8_t>          compressed_data,
                                                              const ru_compression_params& params)
  {
    (void)grid;
    (void)port;
    (void)symbol;
    (void)start_prb;
    (void)nof_prbs;
    (void)compressed_data;
    (void)params;
    return iq_decompression_result::unsupported;
  }

  /// \brief Queues the decompression of received compressed PRBs into a resource grid.
  ///
  /// Same contract as \ref decompress_to_resource_grid(), except that an implementation may defer the device work and
  /// complete it in \ref flush_resource_grid_decompressions(), so that consecutive writes of one OFDM symbol (all its
  /// ports and fragments) take one device submission. The caller must flush before a consumer reads the written REs.
  /// The compressed payload is copied before returning. The default implementation decompresses immediately.
  virtual iq_decompression_result enqueue_resource_grid_decompression(resource_grid_writer&        grid,
                                                                      unsigned                     port,
                                                                      unsigned                     symbol,
                                                                      unsigned                     start_prb,
                                                                      unsigned                     nof_prbs,
                                                                      span<const uint8_t>          compressed_data,
                                                                      const ru_compression_params& params)
  {
    return decompress_to_resource_grid(grid, port, symbol, start_prb, nof_prbs, compressed_data, params);
  }

  /// \brief Completes the submission of every decompression queued by \ref enqueue_resource_grid_decompression().
  ///
  /// After it returns, the device work is ordered before every later consumer of the destination grids, exactly as
  /// after \ref decompress_to_resource_grid(). Returns a failure when queued REs could not be submitted (they are then
  /// left unwritten). The default implementation has nothing queued.
  virtual iq_decompression_result flush_resource_grid_decompressions() { return iq_decompression_result::success; }

  /// \brief Decompress received compressed PRBs directly into a PRACH buffer.
  ///
  /// \param[out] buffer          Destination PRACH buffer.
  /// \param[in]  port            PRACH receive port index.
  /// \param[in]  td_occasion     PRACH time-domain occasion index.
  /// \param[in]  fd_occasion     PRACH frequency-domain occasion index.
  /// \param[in]  symbol          PRACH symbol index within the occasion.
  /// \param[in]  start_re        First destination RE in the PRACH symbol.
  /// \param[in]  input_start_re  First compressed-payload RE to consume.
  /// \param[in]  nof_re          Number of RE to decompress.
  /// \param[in]  nof_prbs        Number of PRBs represented by the compressed payload.
  /// \param[in]  compressed_data Received compressed IQ payload and compression parameters.
  /// \param[in]  params          Compression parameters.
  /// \return Structured result describing whether the operation completed and, on failure, why it did not.
  virtual iq_decompression_result decompress_to_prach_buffer(prach_buffer&                buffer,
                                                             unsigned                     port,
                                                             unsigned                     td_occasion,
                                                             unsigned                     fd_occasion,
                                                             unsigned                     symbol,
                                                             unsigned                     start_re,
                                                             unsigned                     input_start_re,
                                                             unsigned                     nof_re,
                                                             unsigned                     nof_prbs,
                                                             span<const uint8_t>          compressed_data,
                                                             const ru_compression_params& params)
  {
    (void)buffer;
    (void)port;
    (void)td_occasion;
    (void)fd_occasion;
    (void)symbol;
    (void)start_re;
    (void)input_start_re;
    (void)nof_re;
    (void)nof_prbs;
    (void)compressed_data;
    (void)params;
    return iq_decompression_result::unsupported;
  }
};

} // namespace ofh
} // namespace ocudu
