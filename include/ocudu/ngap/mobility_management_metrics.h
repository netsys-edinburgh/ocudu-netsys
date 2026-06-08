// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/ran/gnb_du_id.h"
#include <map>

namespace ocudu {

// Per-DU intra-gNB handover execution counters, see TS 28.552 section 5.1.1.6.2.
struct du_handover_execution_metrics {
  unsigned nof_handover_executions_requested  = 0;
  unsigned nof_successful_handover_executions = 0;
};

// Mobility Management metrics, see TS 28.552 section 5.1.1.6.
struct mobility_management_metrics {
  // Section 5.1.1.6.1: Inter-gNB handovers.
  unsigned nof_handover_preparations_requested  = 0;
  unsigned nof_successful_handover_preparations = 0;

  // Section 5.1.1.6.2: Intra-gNB handover executions tracked per source DU.
  std::map<gnb_du_id_t, du_handover_execution_metrics> per_du_executions;
};

} // namespace ocudu
