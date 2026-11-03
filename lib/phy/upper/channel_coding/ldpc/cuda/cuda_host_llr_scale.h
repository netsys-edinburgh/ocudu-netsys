// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

namespace ocudu {

/// \brief Gets the scale from device soft bits, in natural LLR units, to host INT8 soft bits.
///
/// The host soft bits must have the scale of the CPU demodulation mapper, which quantizes natural LLRs in the range
/// +/-24 (BPSK and QPSK) or +/-20 (the QAM orders) to +/-120. The PUSCH demodulator and the decoder's soft-bit export
/// share this scale, so that soft bits from both are combined on the same scale.
///
/// \param[in] bits_per_symbol Modulation order.
inline float get_cuda_host_llr_scale(unsigned bits_per_symbol)
{
  return (bits_per_symbol <= 2) ? (120.0F / 24.0F) : (120.0F / 20.0F);
}

} // namespace ocudu
