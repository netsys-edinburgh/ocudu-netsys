#pragma once

#include "ocudu/ran/slot_point.h"
#include <chrono>

namespace ocudu {

struct sched_result;
class cell_configuration;

/// Feeds the AURORA metrics framework (Groups 5-8, 11, 12) with the result of one cell scheduler slot decision.
/// Independent of the scheduler metrics configuration (cell_metrics_handler may be disabled).
void aurora_report_slot_result(const cell_configuration&  cell_cfg,
                               slot_point                 sl_tx,
                               const sched_result&        result,
                               std::chrono::microseconds decision_latency);

} // namespace ocudu
