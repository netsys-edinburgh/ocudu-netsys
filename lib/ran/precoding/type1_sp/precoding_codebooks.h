// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once
#include "ocudu/ran/precoding/precoding_matrix_indicator.h"
#include "ocudu/ran/precoding_beamforming_composite.h"

namespace ocudu {

/// \brief Calculates the MIMO precoding matrix and its beam list for a Type I Single-Panel PMI.
///
/// \param[in] pmi        Type I Single-Panel Precoding Matrix Indicator (PMI).
/// \param[in] nof_layers Number of transmission layers, one to four.
/// \return The MIMO precoding matrix and beam list described by the PMI.
precoding_beamforming_composite calculate_mimo_matrix(const pmi_typeI_single_panel& pmi, unsigned nof_layers);

} // namespace ocudu
