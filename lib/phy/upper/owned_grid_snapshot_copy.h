// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief Device-to-device copy of a CUDA-visible resource grid into an owned snapshot buffer.

#pragma once

#include <cstdint>

namespace ocudu::detail {

/// \brief Copies a CUDA-visible resource grid into an owned device buffer on the given stream.
///
/// The grid is copied as \c n_u32 32-bit words, one per CBF16 resource element. The copy must be enqueued on the stream
/// that produced the source grid, right after it, so that it orders after the producer.
///
/// \return False if an argument is invalid or the kernel could not be launched.
bool launch_owned_grid_snapshot_copy(const void* src_cbf16, void* dst_cbf16, uint64_t n_u32, void* stream) noexcept;

} // namespace ocudu::detail
