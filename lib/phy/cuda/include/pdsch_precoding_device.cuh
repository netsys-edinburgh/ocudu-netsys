// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "pdsch_fused.h"
#include <cstdint>

/**
 * Rounds a float to BF16 bits, to nearest even, as the host channel precoder
 * stores its outputs.
 */
__device__ __forceinline__ uint16_t pdsch_precoding_float_to_bf16_bits(float value)
{
    uint32_t as_u32 = __float_as_uint(value);
    as_u32 += 0x7fffU + ((as_u32 >> 16U) & 1U);
    return static_cast<uint16_t>(as_u32 >> 16U);
}

/**
 * Precodes the layer symbols of one RE and writes every port of the BF16 grid.
 *
 * Each complex product is formed as the host SIMD precoders form it,
 * real = fma(x_re, w_re, -(x_im * w_im)) and imag = fma(x_im, w_re, x_re * w_im),
 * and the layers are added in order, so that the rounded BF16 result matches
 * the host. The intrinsics keep the compiler from contracting or reordering.
 */
__device__ __forceinline__ void pdsch_precode_re_to_bf16_grid(const float* __restrict__ layer_re,
                                                              const float* __restrict__ layer_im,
                                                              const pdsch_precoding_weights_t& weights,
                                                              uint32_t re_offset,
                                                              int nof_grid_re_per_port,
                                                              uint16_t* __restrict__ d_grid_bf16)
{
    for (int i_port = 0; i_port != weights.nof_ports; ++i_port) {
        float sum_re = 0.0F;
        float sum_im = 0.0F;
        for (int i_layer = 0; i_layer != weights.nof_layers; ++i_layer) {
            float w_re = weights.re[i_port][i_layer];
            float w_im = weights.im[i_port][i_layer];
            float x_re = layer_re[i_layer];
            float x_im = layer_im[i_layer];
            float p_re = __fmaf_rn(x_re, w_re, -__fmul_rn(x_im, w_im));
            float p_im = __fmaf_rn(x_im, w_re, __fmul_rn(x_re, w_im));
            if (i_layer == 0) {
                sum_re = p_re;
                sum_im = p_im;
            } else {
                sum_re = __fadd_rn(sum_re, p_re);
                sum_im = __fadd_rn(sum_im, p_im);
            }
        }
        int out_index = 2 * (i_port * nof_grid_re_per_port + static_cast<int>(re_offset));
        d_grid_bf16[out_index] = pdsch_precoding_float_to_bf16_bits(sum_re);
        d_grid_bf16[out_index + 1] = pdsch_precoding_float_to_bf16_bits(sum_im);
    }
}

/**
 * Checks a precoding matrix can be mapped on the device: one to
 * PDSCH_PRECODING_MAX_LAYERS layers, no more layers than ports, and at most
 * PDSCH_PRECODING_MAX_PORTS ports.
 */
inline bool pdsch_precoding_weights_are_valid(const pdsch_precoding_weights_t* weights)
{
    return (weights != nullptr) && (weights->nof_layers >= 1) &&
           (weights->nof_layers <= PDSCH_PRECODING_MAX_LAYERS) && (weights->nof_ports >= weights->nof_layers) &&
           (weights->nof_ports <= PDSCH_PRECODING_MAX_PORTS);
}
