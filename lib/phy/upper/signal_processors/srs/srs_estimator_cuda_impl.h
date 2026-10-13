// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/adt/tensor.h"
#include "ocudu/phy/upper/sequence_generators/low_papr_sequence_generator.h"
#include "ocudu/phy/upper/signal_processors/srs/doa_estimator.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator.h"
#include <memory>
#include <mutex>
#include <srs_estimator.h>
#include <vector>

namespace ocudu {

class pusch_device_grid_reader_cuda;

/// GPU-backed SRS estimator using CUDA kernels with software fallback.
class srs_estimator_cuda_impl : public srs_estimator
{
public:
  /// \brief Creates the estimator.
  ///
  /// \param[in] sequence_generator_  SRS sequence generator.
  /// \param[in] fallback_            Software estimator used when the GPU cannot produce an estimate.
  /// \param[in] direction_estimator_ Direction-of-arrival estimator, or \c nullptr if it is not estimated. It takes the
  ///                                 received SRS of transmit port 0, which the GPU gathers from the grid.
  /// \param[in] max_nof_prb_         Maximum number of PRB.
  /// \param[in] force_gpu_path_      Drops the estimates the GPU cannot produce instead of using the fallback.
  /// \param[in] cuda_device_id_      CUDA device.
  srs_estimator_cuda_impl(std::unique_ptr<low_papr_sequence_generator> sequence_generator_,
                          std::unique_ptr<srs_estimator>               fallback_,
                          std::unique_ptr<doa_estimator>               direction_estimator_,
                          unsigned                                     max_nof_prb_,
                          bool                                         force_gpu_path_ = false,
                          int                                          cuda_device_id_ = 0);

  ~srs_estimator_cuda_impl() override;

  srs_estimator_cuda_impl(const srs_estimator_cuda_impl&)            = delete;
  srs_estimator_cuda_impl& operator=(const srs_estimator_cuda_impl&) = delete;
  srs_estimator_cuda_impl(srs_estimator_cuda_impl&&)                 = delete;
  srs_estimator_cuda_impl& operator=(srs_estimator_cuda_impl&&)      = delete;

  srs_estimator_result estimate(const resource_grid_reader& grid, const srs_estimator_configuration& config) override;

  bool is_gpu_available() const { return gpu_available; }

  /// Gets the number of estimates handed to the software estimator instead of the GPU.
  unsigned get_nof_host_fallbacks() const { return nof_host_fallbacks; }

  /// Gets the number of estimates the forced GPU path dropped because the GPU could not produce them.
  unsigned get_nof_dropped_estimates() const { return nof_dropped_estimates; }

private:
  /// \brief Produces the estimate when the GPU does not.
  ///
  /// Hands the estimate to the software estimator, or, on the forced GPU path, logs the failure and returns an empty
  /// result. \c reason describes the failure for the log; it is \c nullptr when the GPU was skipped on purpose.
  srs_estimator_result
  estimate_without_gpu(const resource_grid_reader& grid, const srs_estimator_configuration& config, const char* reason);

  std::unique_ptr<low_papr_sequence_generator> sequence_generator;
  std::unique_ptr<srs_estimator>               fallback;
  std::unique_ptr<doa_estimator>               direction_estimator;
  unsigned                                     max_nof_prb;

  bool                   gpu_available  = false;
  bool                   force_gpu_path = false;
  int                    device_id      = 0;
  void*                  stream         = nullptr;
  srs_estimator_handle_t handle         = nullptr;
  std::mutex             estimator_mutex;

  const resource_grid_reader*                    staged_grid_source = nullptr;
  std::unique_ptr<pusch_device_grid_reader_cuda> staged_grid_reader;

  bool                            gpu_configured = false;
  std::vector<int>                last_config_signature;
  std::vector<srs_estimator_cf_t> sequences;

  /// Received SRS of transmit port 0, as the GPU returns it: by receive port, then SRS symbol, then sequence element.
  std::vector<srs_estimator_cf_t> received_port0;
  /// Direction-of-arrival estimator input: the received SRS of transmit port 0, one column per receive port.
  static_tensor<2, cf_t, SRS_ESTIMATOR_MAX_SEQUENCE_LENGTH * SRS_ESTIMATOR_MAX_SYMBOLS * SRS_ESTIMATOR_MAX_RX_PORTS>
      doa_data;

  unsigned nof_host_fallbacks    = 0;
  unsigned nof_dropped_estimates = 0;
};

/// Returns true when the CUDA SRS estimator backend can be instantiated.
bool is_srs_estimator_cuda_available();

} // namespace ocudu
