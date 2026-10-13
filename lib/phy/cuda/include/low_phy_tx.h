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

typedef struct ocudu_lowphy_tx_handle ocudu_lowphy_tx_handle_t;

enum { OCUDU_LOWPHY_TX_MAX_SYMBOLS = 28, OCUDU_LOWPHY_TX_MAX_PORTS = 8, OCUDU_LOWPHY_TX_MAX_BEAMS = 16 };

typedef struct {
  int   dft_size;
  int   rg_size;
  int   nof_ports;
  int   nof_symbols;
  int   nof_samples;
  int   cp_lengths[OCUDU_LOWPHY_TX_MAX_SYMBOLS];
  int   symbol_offsets[OCUDU_LOWPHY_TX_MAX_SYMBOLS];
  float phase_re[OCUDU_LOWPHY_TX_MAX_SYMBOLS];
  float phase_im[OCUDU_LOWPHY_TX_MAX_SYMBOLS];
  float ofdm_scale;
  float amplitude_gain;
  float clipping_ceiling;
  int   clipping_enabled;
  /* Beamforming. The frequency-domain signal of antenna port a is the sum over the beams b of the weight (a, b) times
     the resource grid of beam b, as in the software OFDM modulator.

     When nof_beams is zero, the resource grid has one port per antenna port and port a feeds antenna port a unchanged;
     the other beamforming fields are ignored. Otherwise, nof_beams is at most OCUDU_LOWPHY_TX_MAX_BEAMS. */
  int nof_beams;
  /* Resource grid port of each beam, used by ocudu_lowphy_tx_process(). */
  int beam_grid_ports[OCUDU_LOWPHY_TX_MAX_BEAMS];
  /* Beamforming weights, antenna major: the weight of beam b on antenna port a is at a * nof_beams + b. */
  float beam_weights_re[OCUDU_LOWPHY_TX_MAX_PORTS * OCUDU_LOWPHY_TX_MAX_BEAMS];
  float beam_weights_im[OCUDU_LOWPHY_TX_MAX_PORTS * OCUDU_LOWPHY_TX_MAX_BEAMS];
} ocudu_lowphy_tx_config_t;

int  ocudu_lowphy_tx_create(const ocudu_lowphy_tx_config_t* config, ocudu_lowphy_tx_handle_t** handle);
void ocudu_lowphy_tx_destroy(ocudu_lowphy_tx_handle_t* handle);
int  ocudu_lowphy_tx_update_config(ocudu_lowphy_tx_handle_t* handle, const ocudu_lowphy_tx_config_t* config);
int  ocudu_lowphy_tx_register_host_output(ocudu_lowphy_tx_handle_t* handle, void* ptr, size_t bytes);

/* Modulates a slot from a resource grid in device memory, laid out as [port][symbol][subcarrier] with nof_symbols
   symbols of rg_size subcarriers. The beams read the ports given by beam_grid_ports. */
int ocudu_lowphy_tx_process(ocudu_lowphy_tx_handle_t* handle,
                            const void*               d_grid_cbf16,
                            void* const*              h_output_ports,
                            void*                     external_stream);

/* Modulates a slot from a host resource grid that holds the beams contiguously, in beam order, each laid out as
   [symbol][subcarrier]. With nof_beams equal to zero, it holds the antenna ports in order. beam_grid_ports is ignored.
 */
int ocudu_lowphy_tx_process_host_grid(ocudu_lowphy_tx_handle_t* handle,
                                      const void*               h_grid_cbf16,
                                      void* const*              h_output_ports,
                                      void*                     external_stream);

/* Like ocudu_lowphy_tx_process_host_grid(), but takes the host resource grid of each beam separately. h_beam_grids has
   one pointer per beam, or per antenna port when nof_beams is zero, to nof_symbols * rg_size packed elements. */
int ocudu_lowphy_tx_process_host_beams(ocudu_lowphy_tx_handle_t* handle,
                                       const void* const*        h_beam_grids,
                                       void* const*              h_output_ports,
                                       void*                     external_stream);

int   ocudu_lowphy_tx_synchronize(ocudu_lowphy_tx_handle_t* handle);
void* ocudu_lowphy_tx_get_completion_event(ocudu_lowphy_tx_handle_t* handle);
void* ocudu_lowphy_tx_get_stream(ocudu_lowphy_tx_handle_t* handle);

#ifdef __cplusplus
}
#endif
