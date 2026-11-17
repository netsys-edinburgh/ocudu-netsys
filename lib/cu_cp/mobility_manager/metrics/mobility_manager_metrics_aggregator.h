// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/ngap/mobility_management_metrics.h"
#include "ocudu/ran/gnb_du_id.h"
#include "ocudu/support/math/stats.h"

namespace ocudu::ocucp {

class mobility_manager_metrics_aggregator
{
public:
  /// \brief Aggregates the metrics for the requested handover preparation.
  void aggregate_requested_handover_preparation();

  /// \brief Aggregates the metrics for the successful handover preparation.
  void aggregate_successful_handover_preparation();

  /// \brief Aggregates the metrics for the requested inter-gNB handover resource allocation.
  void aggregate_requested_inter_gnb_handover_resource_allocation();

  /// \brief Aggregates the metrics for the successful inter-gNB handover resource allocation.
  void aggregate_successful_inter_gnb_handover_resource_allocation();

  /// \brief Aggregates the metrics for the requested inter-gNB handover execution.
  void aggregate_requested_inter_gnb_handover_execution();

  /// \brief Aggregates the metrics for the successful inter-gNB handover execution.
  void aggregate_successful_inter_gnb_handover_execution();

  /// \brief Aggregates the metrics for the requested intra-gNB handover preparation.
  void aggregate_requested_intra_gnb_handover_preparation();

  /// \brief Aggregates the metrics for the successful intra-gNB handover preparation.
  void aggregate_successful_intra_gnb_handover_preparation();

  /// \brief Aggregates the metrics for the requested handover execution, also on the given source DU.
  /// \param[in] source_du_id Source DU of the handover; gnb_du_id_t::invalid counts only the DU-agnostic metric.
  void aggregate_requested_handover_execution(gnb_du_id_t source_du_id);

  /// \brief Aggregates only the per-DU metric of a requested handover execution (conditional handover, which the
  /// DU-agnostic intra-gNB handover execution metric does not count).
  void aggregate_requested_handover_execution_on_du(gnb_du_id_t source_du_id);

  /// \brief Aggregates the metrics for the successful handover execution, also on the given source DU.
  /// \param[in] source_du_id Source DU of the handover; gnb_du_id_t::invalid counts only the DU-agnostic metric.
  void aggregate_successful_handover_execution(gnb_du_id_t source_du_id);

  /// \brief Aggregates only the per-DU metric of a successful handover execution (conditional handover).
  void aggregate_successful_handover_execution_on_du(gnb_du_id_t source_du_id);

  /// \brief Aggregates the execution time of a successful intra-gNB handover.
  void aggregate_intra_gnb_handover_execution_time(std::chrono::milliseconds execution_time);

  /// \brief Aggregates the metrics for a UE configured with conditional handover.
  /// \param[in] has_intra_gnb_candidate True if the UE has at least one candidate cell in this gNB.
  /// \param[in] has_inter_gnb_candidate True if the UE has at least one candidate cell in another gNB.
  void aggregate_ue_configured_with_cho(bool has_intra_gnb_candidate, bool has_inter_gnb_candidate);

  /// \brief Aggregates the metrics for the successful intra-gNB conditional handover execution.
  void aggregate_successful_intra_gnb_cho_execution();

  /// \brief Returns the mobility manager metrics of the reporting period and starts a new one (all counters reset).
  mobility_management_metrics request_metrics_report();

private:
  mobility_management_metrics aggregated_mobility_manager_metrics;
  /// Intra-gNB handover execution times in milliseconds of the current reporting period.
  sample_statistics<unsigned> intra_gnb_handover_execution_time_ms;
};

} // namespace ocudu::ocucp
