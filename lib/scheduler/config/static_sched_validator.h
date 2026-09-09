// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/adt/expected.h"
#include "ocudu/scheduler/config/ran_cell_config.h"

namespace ocudu {

/// \brief Checks that the cell's static resources (SSB, CSI-RS, DL-PRS, etc.) do not collide with each other in
/// time and frequency.
error_type<std::string> check_static_resource_collisions(const ran_cell_config& ran);

} // namespace ocudu
