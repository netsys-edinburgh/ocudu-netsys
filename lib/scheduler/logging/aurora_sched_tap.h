#pragma once

#include "ocudu/ran/slot_point.h"
#include <chrono>

namespace ocudu {

struct sched_result;
struct csi_report_data;
struct csi_meas_config;
class cell_configuration;

/// Feeds the AURORA metrics framework (Groups 5-8, 11, 12) with the result of one cell scheduler slot decision.
/// Independent of the scheduler metrics configuration (cell_metrics_handler may be disabled).
void aurora_report_slot_result(const cell_configuration&  cell_cfg,
                               slot_point                 sl_tx,
                               const sched_result&        result,
                               std::chrono::microseconds decision_latency);

/// Feeds the AURORA metrics framework with a decoded UE CSI report (CQI, RI, PMI i1/i2, L1-RSRP).
void aurora_report_csi(unsigned ue_index, const csi_meas_config* csi_meas_cfg, const csi_report_data& csi);

} // namespace ocudu
