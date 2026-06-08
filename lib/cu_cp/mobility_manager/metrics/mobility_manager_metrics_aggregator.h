// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/ngap/mobility_management_metrics.h"
#include "ocudu/ran/gnb_du_id.h"

namespace ocudu::ocucp {

class mobility_manager_metrics_aggregator
{
public:
  /// \brief Aggregates the metrics for the requested handover preparation.
  void aggregate_requested_handover_preparation();

  /// \brief Aggregates the metrics for the successful handover preparation.
  void aggregate_successful_handover_preparation();

  /// \brief Aggregates the metrics for the requested handover execution on the given source DU.
  void aggregate_requested_handover_execution(gnb_du_id_t source_du_id);

  /// \brief Aggregates the metrics for the successful handover execution on the given source DU.
  void aggregate_successful_handover_execution(gnb_du_id_t source_du_id);

  mobility_management_metrics request_metrics_report() const;

private:
  mutable mobility_management_metrics aggregated_mobility_manager_metrics;
};

} // namespace ocudu::ocucp
