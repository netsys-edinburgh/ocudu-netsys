// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include <cuda_runtime.h>

/// \brief Flags the codeblocks whose soft bits carry information, on device.
///
/// A soft bit of zero is an erasure. A codeblock made only of erasures carries no information, so it must not be
/// reported as decoded even if the decoder converges: the all-zero codeword satisfies every parity check, and an
/// all-zero transport block satisfies its CRC.
///
/// \param[in]  d_llrs_half Device FP16 soft bits of all codeblocks, back to back.
/// \param[in]  d_cb_ranges Device array with the offset (x) and number (y) of soft bits of each codeblock.
/// \param[in]  nof_cbs     Number of codeblocks.
/// \param[out] d_has_info  Device array set to 1 for each codeblock with a non-zero soft bit, and to 0 otherwise.
/// \param[in]  stream      CUDA stream.
/// \return True if the launch was accepted.
bool pusch_flag_codeblocks_with_information_half(const void*  d_llrs_half,
                                                 const uint2* d_cb_ranges,
                                                 unsigned     nof_cbs,
                                                 int*         d_has_info,
                                                 cudaStream_t stream);
