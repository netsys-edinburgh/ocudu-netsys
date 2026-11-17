// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "mobility_manager_metrics_aggregator.h"

using namespace ocudu;
using namespace ocucp;

void mobility_manager_metrics_aggregator::aggregate_requested_handover_preparation()
{
  ++aggregated_mobility_manager_metrics.nof_handover_preparations_requested;
}

void mobility_manager_metrics_aggregator::aggregate_successful_handover_preparation()
{
  ++aggregated_mobility_manager_metrics.nof_successful_handover_preparations;
}

void mobility_manager_metrics_aggregator::aggregate_requested_inter_gnb_handover_resource_allocation()
{
  ++aggregated_mobility_manager_metrics.nof_inter_gnb_handover_resource_allocations_requested;
}

void mobility_manager_metrics_aggregator::aggregate_successful_inter_gnb_handover_resource_allocation()
{
  ++aggregated_mobility_manager_metrics.nof_successful_inter_gnb_handover_resource_allocations;
}

void mobility_manager_metrics_aggregator::aggregate_requested_inter_gnb_handover_execution()
{
  ++aggregated_mobility_manager_metrics.nof_inter_gnb_handover_executions_requested;
}

void mobility_manager_metrics_aggregator::aggregate_successful_inter_gnb_handover_execution()
{
  ++aggregated_mobility_manager_metrics.nof_successful_inter_gnb_handover_executions;
}

void mobility_manager_metrics_aggregator::aggregate_requested_intra_gnb_handover_preparation()
{
  ++aggregated_mobility_manager_metrics.nof_intra_gnb_handover_preparations_requested;
}

void mobility_manager_metrics_aggregator::aggregate_successful_intra_gnb_handover_preparation()
{
  ++aggregated_mobility_manager_metrics.nof_successful_intra_gnb_handover_preparations;
}

void mobility_manager_metrics_aggregator::aggregate_requested_handover_execution(gnb_du_id_t source_du_id)
{
  ++aggregated_mobility_manager_metrics.nof_handover_executions_requested;
  aggregate_requested_handover_execution_on_du(source_du_id);
}

void mobility_manager_metrics_aggregator::aggregate_requested_handover_execution_on_du(gnb_du_id_t source_du_id)
{
  if (source_du_id != gnb_du_id_t::invalid) {
    ++aggregated_mobility_manager_metrics.per_du_executions[source_du_id].nof_handover_executions_requested;
  }
}

void mobility_manager_metrics_aggregator::aggregate_successful_handover_execution(gnb_du_id_t source_du_id)
{
  ++aggregated_mobility_manager_metrics.nof_successful_handover_executions;
  aggregate_successful_handover_execution_on_du(source_du_id);
}

void mobility_manager_metrics_aggregator::aggregate_successful_handover_execution_on_du(gnb_du_id_t source_du_id)
{
  if (source_du_id != gnb_du_id_t::invalid) {
    ++aggregated_mobility_manager_metrics.per_du_executions[source_du_id].nof_successful_handover_executions;
  }
}

void mobility_manager_metrics_aggregator::aggregate_intra_gnb_handover_execution_time(
    std::chrono::milliseconds execution_time)
{
  intra_gnb_handover_execution_time_ms.update(execution_time.count());
}

void mobility_manager_metrics_aggregator::aggregate_ue_configured_with_cho(bool has_intra_gnb_candidate,
                                                                           bool has_inter_gnb_candidate)
{
  if (has_intra_gnb_candidate) {
    ++aggregated_mobility_manager_metrics.nof_ues_configured_with_intra_gnb_cho;
  }
  if (has_inter_gnb_candidate) {
    ++aggregated_mobility_manager_metrics.nof_ues_configured_with_inter_gnb_cho;
  }
}

void mobility_manager_metrics_aggregator::aggregate_successful_intra_gnb_cho_execution()
{
  ++aggregated_mobility_manager_metrics.nof_successful_intra_gnb_cho_executions;
}

mobility_management_metrics mobility_manager_metrics_aggregator::request_metrics_report()
{
  mobility_management_metrics metrics = aggregated_mobility_manager_metrics;
  if (intra_gnb_handover_execution_time_ms.get_nof_observations() != 0) {
    metrics.mean_intra_gnb_handover_execution_time = std::chrono::milliseconds{
        static_cast<std::chrono::milliseconds::rep>(std::round(intra_gnb_handover_execution_time_ms.get_mean()))};
    metrics.max_intra_gnb_handover_execution_time =
        std::chrono::milliseconds{intra_gnb_handover_execution_time_ms.get_max()};
  }
  intra_gnb_handover_execution_time_ms.reset();
  // netsys: every counter covers one reporting period (the campus5g collectors expect per-period values).
  aggregated_mobility_manager_metrics = {};
  return metrics;
}
