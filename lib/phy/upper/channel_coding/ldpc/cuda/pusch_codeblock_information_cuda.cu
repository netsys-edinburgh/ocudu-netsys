// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "pusch_codeblock_information_cuda.h"
#include <cstdint>

namespace {

/// One block per codeblock: any soft bit other than +0 or -0 marks the codeblock as carrying information.
__global__ void flag_codeblocks_with_information_half_kernel(const uint16_t* __restrict__ llrs_bits,
                                                             const uint2* __restrict__ cb_ranges,
                                                             int* __restrict__ has_info)
{
  const uint2 range = cb_ranges[blockIdx.x];

  int found = 0;
  for (unsigned i = threadIdx.x; (i < range.y) && (found == 0); i += blockDim.x) {
    found = ((llrs_bits[range.x + i] & 0x7fffU) != 0) ? 1 : 0;
  }

  found = __syncthreads_or(found);
  if (threadIdx.x == 0) {
    has_info[blockIdx.x] = found;
  }
}

} // namespace

bool pusch_flag_codeblocks_with_information_half(const void*  d_llrs_half,
                                                 const uint2* d_cb_ranges,
                                                 unsigned     nof_cbs,
                                                 int*         d_has_info,
                                                 cudaStream_t stream)
{
  if ((d_llrs_half == nullptr) || (d_cb_ranges == nullptr) || (d_has_info == nullptr) || (nof_cbs == 0)) {
    return false;
  }

  static constexpr unsigned block_size = 256;
  flag_codeblocks_with_information_half_kernel<<<nof_cbs, block_size, 0, stream>>>(
      static_cast<const uint16_t*>(d_llrs_half), d_cb_ranges, d_has_info);
  return cudaGetLastError() == cudaSuccess;
}
