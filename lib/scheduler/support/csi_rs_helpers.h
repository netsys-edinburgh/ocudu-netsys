// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "../config/cell_configuration.h"
#include "ocudu/adt/span.h"
#include "ocudu/ran/resource_allocation/ofdm_symbol_range.h"
#include "ocudu/ran/resource_allocation/rb_interval.h"
#include "ocudu/scheduler/result/csi_rs_info.h"

namespace ocudu {
namespace csi_helper {

/// \brief Helpers that checks if the slot is a candidate for CSI RS signalling.
bool is_csi_rs_slot(const cell_configuration& cell_cfg, slot_point sl_tx);

/// \brief Counts the resource elements that the CSI-RS of a slot occupy within a PDSCH allocation.
///
/// The PDSCH is rate-matched around the resource elements of all the CSI-RS transmitted in its slot, NZP and ZP (TS
/// 38.214, clause 5.1.4.1). As in the PHY, a resource element occupied by several CSI-RS or CSI-RS ports is counted
/// once.
///
/// \param[in] csi_rs_list CSI-RS transmitted in the slot of the PDSCH.
/// \param[in] symbols     OFDM symbols of the PDSCH within the slot.
/// \param[in] crbs        Common resource blocks of the PDSCH.
/// \return The number of resource elements of the PDSCH allocation reserved for CSI-RS.
unsigned get_nof_csi_rs_re(span<const csi_rs_info> csi_rs_list, ofdm_symbol_range symbols, crb_interval crbs);

} // namespace csi_helper
} // namespace ocudu
