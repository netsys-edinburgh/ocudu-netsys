// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ocudu_ofh_compression_handle ocudu_ofh_compression_handle_t;

/// Details of the most recent CUDA runtime failure observed by a compression handle.
///
/// The string pointers are owned by the CUDA runtime or the compression backend and remain valid for the lifetime of
/// the process. A successful operation clears the error code and reports operation "none".
typedef struct {
  int         cuda_error_code;
  const char* operation;
  const char* error_name;
  const char* error_description;
} ocudu_ofh_compression_error_info_t;

enum { OCUDU_OFH_COMPRESSION_TYPE_NONE = 0, OCUDU_OFH_COMPRESSION_TYPE_BFP = 1 };

int ocudu_ofh_compression_available(void);
int ocudu_ofh_compression_create(ocudu_ofh_compression_handle_t** handle);
/// Preallocates device and pinned-host output workspaces used by device-grid compression to host buffers.
int ocudu_ofh_compression_reserve_host_buffer_compression(ocudu_ofh_compression_handle_t* handle,
                                                          unsigned                        max_output_bytes);
/// Preallocates the buffers and events used by asynchronous direct decompression.
int   ocudu_ofh_compression_reserve_direct_decompression(ocudu_ofh_compression_handle_t* handle,
                                                         unsigned                        max_input_bytes,
                                                         unsigned                        max_output_bytes);
void  ocudu_ofh_compression_destroy(ocudu_ofh_compression_handle_t* handle);
void* ocudu_ofh_compression_get_stream(ocudu_ofh_compression_handle_t* handle);
/// Returns 1 when all work enqueued on the handle has completed, 0 while work is pending and -1 on a CUDA error.
int ocudu_ofh_compression_query_completion(ocudu_ofh_compression_handle_t* handle);
int ocudu_ofh_compression_synchronize(ocudu_ofh_compression_handle_t* handle);
int ocudu_ofh_compression_get_last_error(const ocudu_ofh_compression_handle_t* handle,
                                         ocudu_ofh_compression_error_info_t*   error_info);

int ocudu_ofh_compress(ocudu_ofh_compression_handle_t* handle,
                       int                             compression_type,
                       void*                           output_bytes,
                       const void*                     input_cbf16,
                       unsigned                        nof_prbs,
                       unsigned                        data_width,
                       float                           iq_scaling);

int ocudu_ofh_decompress(ocudu_ofh_compression_handle_t* handle,
                         int                             compression_type,
                         void*                           output_cbf16,
                         const void*                     input_bytes,
                         unsigned                        nof_prbs,
                         unsigned                        data_width);

int ocudu_ofh_compress_device_grid(ocudu_ofh_compression_handle_t* handle,
                                   int                             compression_type,
                                   void*                           output_bytes,
                                   const void*                     input_grid_cbf16,
                                   unsigned                        nof_symbols,
                                   unsigned                        nof_subc,
                                   unsigned                        port,
                                   unsigned                        symbol,
                                   unsigned                        start_prb,
                                   unsigned                        nof_prbs,
                                   unsigned                        data_width,
                                   float                           iq_scaling);

int ocudu_ofh_decompress_to_device_grid(ocudu_ofh_compression_handle_t* handle,
                                        int                             compression_type,
                                        void*                           output_grid_cbf16,
                                        const void*                     input_bytes,
                                        unsigned                        nof_symbols,
                                        unsigned                        nof_subc,
                                        unsigned                        port,
                                        unsigned                        symbol,
                                        unsigned                        start_prb,
                                        unsigned                        nof_prbs,
                                        unsigned                        data_width);

int ocudu_ofh_decompress_to_device_grid_async(ocudu_ofh_compression_handle_t* handle,
                                              int                             compression_type,
                                              void*                           output_grid_cbf16,
                                              const void*                     input_bytes,
                                              unsigned                        nof_symbols,
                                              unsigned                        nof_subc,
                                              unsigned                        port,
                                              unsigned                        symbol,
                                              unsigned                        start_prb,
                                              unsigned                        nof_prbs,
                                              unsigned                        data_width);

/// Maximum number of grid regions of one ocudu_ofh_decompress_regions_to_device_grid_async() call.
enum { OCUDU_OFH_MAX_GRID_REGIONS = 32 };

/// One destination region of a batched decompression: nof_prbs compressed PRBs read from input_offset (bytes) of the
/// handle's batch input and written to grid_offset_re (REs from the start of the grid).
typedef struct {
  unsigned grid_offset_re;
  unsigned input_offset;
  unsigned nof_prbs;
} ocudu_ofh_grid_region_t;

/// \brief Returns the host address of the handle's batch input, a host buffer the GPU reads in place (zero copy).
///
/// The buffer holds at least \c nof_bytes; it may be reallocated, so call it only while no work using it is queued on
/// the handle. Returns NULL when the device cannot read mapped host memory coherently (the batch path is then not
/// available) or the allocation fails.
void* ocudu_ofh_batch_input_reserve(ocudu_ofh_compression_handle_t* handle, size_t nof_bytes);

/// \brief Decompresses the regions of the handle's batch input into a device-visible grid, with one kernel launch.
///
/// The batch input must have been filled through ocudu_ofh_batch_input_reserve(). The completion is reported like
/// ocudu_ofh_decompress_to_device_grid_async(). Every region uses the same compression type and data width. Returns
/// zero on failure, including an unsupported data width (nothing is then queued).
int ocudu_ofh_decompress_regions_to_device_grid_async(ocudu_ofh_compression_handle_t* handle,
                                                      int                             compression_type,
                                                      void*                           output_grid_cbf16,
                                                      const ocudu_ofh_grid_region_t*  regions,
                                                      unsigned                        nof_regions,
                                                      unsigned                        data_width);

int ocudu_ofh_decompress_to_device_prach_buffer_async(ocudu_ofh_compression_handle_t* handle,
                                                      int                             compression_type,
                                                      void*                           output_prach_cbf16,
                                                      unsigned                        output_offset_re,
                                                      const void*                     input_bytes,
                                                      unsigned                        input_start_re,
                                                      unsigned                        nof_re,
                                                      unsigned                        nof_prbs,
                                                      unsigned                        data_width);

int ocudu_ofh_compress_device_grid_ports(ocudu_ofh_compression_handle_t* handle,
                                         int                             compression_type,
                                         void*                           output_bytes,
                                         unsigned                        output_port_stride_bytes,
                                         const void*                     input_grid_cbf16,
                                         unsigned                        nof_symbols,
                                         unsigned                        nof_subc,
                                         unsigned                        first_port,
                                         unsigned                        nof_ports,
                                         unsigned                        symbol,
                                         unsigned                        start_prb,
                                         unsigned                        nof_prbs,
                                         unsigned                        data_width,
                                         float                           iq_scaling);

int ocudu_ofh_compress_device_grid_ports_to_host_buffers(ocudu_ofh_compression_handle_t* handle,
                                                         int                             compression_type,
                                                         void* const*                    output_host_buffers,
                                                         unsigned                        nof_output_buffers,
                                                         unsigned                        output_buffer_size_bytes,
                                                         const void*                     input_grid_cbf16,
                                                         unsigned                        nof_symbols,
                                                         unsigned                        nof_subc,
                                                         unsigned                        first_port,
                                                         unsigned                        nof_ports,
                                                         unsigned                        symbol,
                                                         unsigned                        start_prb,
                                                         unsigned                        nof_prbs,
                                                         unsigned                        data_width,
                                                         float                           iq_scaling);

int ocudu_ofh_compress_device_grid_ports_to_device(ocudu_ofh_compression_handle_t* handle,
                                                   int                             compression_type,
                                                   void*                           output_device_bytes,
                                                   unsigned                        output_port_stride_bytes,
                                                   const void*                     input_grid_cbf16,
                                                   unsigned                        nof_symbols,
                                                   unsigned                        nof_subc,
                                                   unsigned                        first_port,
                                                   unsigned                        nof_ports,
                                                   unsigned                        symbol,
                                                   unsigned                        start_prb,
                                                   unsigned                        nof_prbs,
                                                   unsigned                        data_width,
                                                   float                           iq_scaling);

int ocudu_ofh_compress_device_grid_ports_to_device_async(ocudu_ofh_compression_handle_t* handle,
                                                         int                             compression_type,
                                                         void*                           output_device_bytes,
                                                         unsigned                        output_port_stride_bytes,
                                                         const void*                     input_grid_cbf16,
                                                         unsigned                        nof_symbols,
                                                         unsigned                        nof_subc,
                                                         unsigned                        first_port,
                                                         unsigned                        nof_ports,
                                                         unsigned                        symbol,
                                                         unsigned                        start_prb,
                                                         unsigned                        nof_prbs,
                                                         unsigned                        data_width,
                                                         float                           iq_scaling);

int ocudu_ofh_compress_device_grid_symbol_batch(ocudu_ofh_compression_handle_t* handle,
                                                int                             compression_type,
                                                void*                           output_bytes,
                                                unsigned                        output_symbol_stride_bytes,
                                                unsigned                        output_port_stride_bytes,
                                                const void*                     input_grid_cbf16,
                                                unsigned                        nof_grid_symbols,
                                                unsigned                        nof_subc,
                                                unsigned                        first_port,
                                                unsigned                        nof_ports,
                                                unsigned                        first_symbol,
                                                unsigned                        nof_symbols,
                                                unsigned                        start_prb,
                                                unsigned                        nof_prbs,
                                                unsigned                        data_width,
                                                float                           iq_scaling);

int ocudu_ofh_compress_device_grid_symbol_batch_to_device(ocudu_ofh_compression_handle_t* handle,
                                                          int                             compression_type,
                                                          void*                           output_device_bytes,
                                                          unsigned                        output_symbol_stride_bytes,
                                                          unsigned                        output_port_stride_bytes,
                                                          const void*                     input_grid_cbf16,
                                                          unsigned                        nof_grid_symbols,
                                                          unsigned                        nof_subc,
                                                          unsigned                        first_port,
                                                          unsigned                        nof_ports,
                                                          unsigned                        first_symbol,
                                                          unsigned                        nof_symbols,
                                                          unsigned                        start_prb,
                                                          unsigned                        nof_prbs,
                                                          unsigned                        data_width,
                                                          float                           iq_scaling);

/// One region of a device-visible resource grid to compress into its own destination.
typedef struct {
  /// Destination of the region's compressed PRBs (nof_prbs PRBs, back to back). It must be writable by the device:
  /// device memory, or host memory when ocudu_ofh_compression_device_writes_host_memory() returns 1.
  uint8_t* output;
  uint16_t port;
  uint16_t symbol;
  uint16_t start_prb;
  uint16_t nof_prbs;
} ocudu_ofh_compression_region_t;

/// \brief Returns 1 if the compression kernels can write host memory directly (pageable memory access, e.g. GH200
/// and GB10 over NVLink-C2C), else 0.
int ocudu_ofh_compression_device_writes_host_memory(void);

/// \brief Compresses regions of a device-visible grid straight into their destinations, on the handle stream.
///
/// The grid layout is the one of ocudu_ofh_compress_device_grid_symbol_batch_to_host_buffers(). Regions are processed
/// in launches of up to 128; nothing is copied through staging buffers and the call does not wait: the caller
/// synchronises the handle (ocudu_ofh_compression_synchronize()) once for all the regions it enqueued.
/// Supported data widths: 8, 9, 10, 12, 14 and 16 (none and BFP).
/// \return 1 if every region was enqueued; 0 on invalid arguments, an unsupported width or a launch error.
int ocudu_ofh_compress_device_grid_regions_async(ocudu_ofh_compression_handle_t*       handle,
                                                 int                                   compression_type,
                                                 const void*                           input_grid_cbf16,
                                                 unsigned                              nof_grid_ports,
                                                 unsigned                              nof_grid_symbols,
                                                 unsigned                              nof_subc,
                                                 const ocudu_ofh_compression_region_t* regions,
                                                 unsigned                              nof_regions,
                                                 unsigned                              data_width,
                                                 float                                 iq_scaling);

int ocudu_ofh_compress_device_grid_symbol_batch_to_host_buffers(ocudu_ofh_compression_handle_t* handle,
                                                                int                             compression_type,
                                                                void* const*                    output_host_buffers,
                                                                unsigned                        nof_output_buffers,
                                                                unsigned    output_buffer_size_bytes,
                                                                const void* input_grid_cbf16,
                                                                unsigned    nof_grid_symbols,
                                                                unsigned    nof_subc,
                                                                unsigned    first_port,
                                                                unsigned    nof_ports,
                                                                unsigned    first_symbol,
                                                                unsigned    nof_symbols,
                                                                unsigned    start_prb,
                                                                unsigned    nof_prbs,
                                                                unsigned    data_width,
                                                                float       iq_scaling);

int ocudu_ofh_compress_device_grid_symbol_batch_to_device_async(ocudu_ofh_compression_handle_t* handle,
                                                                int                             compression_type,
                                                                void*                           output_device_bytes,
                                                                unsigned    output_symbol_stride_bytes,
                                                                unsigned    output_port_stride_bytes,
                                                                const void* input_grid_cbf16,
                                                                unsigned    nof_grid_symbols,
                                                                unsigned    nof_subc,
                                                                unsigned    first_port,
                                                                unsigned    nof_ports,
                                                                unsigned    first_symbol,
                                                                unsigned    nof_symbols,
                                                                unsigned    start_prb,
                                                                unsigned    nof_prbs,
                                                                unsigned    data_width,
                                                                float       iq_scaling);

int ocudu_ofh_decompress_to_device_grid_ports(ocudu_ofh_compression_handle_t* handle,
                                              int                             compression_type,
                                              void*                           output_grid_cbf16,
                                              const void*                     input_bytes,
                                              unsigned                        input_port_stride_bytes,
                                              unsigned                        nof_symbols,
                                              unsigned                        nof_subc,
                                              unsigned                        first_port,
                                              unsigned                        nof_ports,
                                              unsigned                        symbol,
                                              unsigned                        start_prb,
                                              unsigned                        nof_prbs,
                                              unsigned                        data_width);

int ocudu_ofh_decompress_device_bytes_to_device_grid_ports(ocudu_ofh_compression_handle_t* handle,
                                                           int                             compression_type,
                                                           void*                           output_grid_cbf16,
                                                           const void*                     input_device_bytes,
                                                           unsigned                        input_port_stride_bytes,
                                                           unsigned                        nof_symbols,
                                                           unsigned                        nof_subc,
                                                           unsigned                        first_port,
                                                           unsigned                        nof_ports,
                                                           unsigned                        symbol,
                                                           unsigned                        start_prb,
                                                           unsigned                        nof_prbs,
                                                           unsigned                        data_width);

int ocudu_ofh_decompress_to_device_grid_symbol_batch(ocudu_ofh_compression_handle_t* handle,
                                                     int                             compression_type,
                                                     void*                           output_grid_cbf16,
                                                     const void*                     input_bytes,
                                                     unsigned                        input_symbol_stride_bytes,
                                                     unsigned                        input_port_stride_bytes,
                                                     unsigned                        nof_grid_symbols,
                                                     unsigned                        nof_subc,
                                                     unsigned                        first_port,
                                                     unsigned                        nof_ports,
                                                     unsigned                        first_symbol,
                                                     unsigned                        nof_symbols,
                                                     unsigned                        start_prb,
                                                     unsigned                        nof_prbs,
                                                     unsigned                        data_width);

int ocudu_ofh_decompress_device_bytes_to_device_grid_symbol_batch(ocudu_ofh_compression_handle_t* handle,
                                                                  int                             compression_type,
                                                                  void*                           output_grid_cbf16,
                                                                  const void*                     input_device_bytes,
                                                                  unsigned input_symbol_stride_bytes,
                                                                  unsigned input_port_stride_bytes,
                                                                  unsigned nof_grid_symbols,
                                                                  unsigned nof_subc,
                                                                  unsigned first_port,
                                                                  unsigned nof_ports,
                                                                  unsigned first_symbol,
                                                                  unsigned nof_symbols,
                                                                  unsigned start_prb,
                                                                  unsigned nof_prbs,
                                                                  unsigned data_width);

#ifdef __cplusplus
}
#endif
