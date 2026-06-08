// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/ngap/mobility_management_metrics.h"
#include "ocudu/ngap/ngap_metrics.h"
#include "ocudu/ran/gnb_du_id.h"
#include "ocudu/ran/pci.h"
#include "ocudu/ran/rnti.h"
#include "ocudu/rrc/rrc_metrics.h"
#include "ocudu/rrc/rrc_ue.h"
#include "ocudu/support/format/fmt_to_c_str.h"
#include <optional>
#include <vector>

namespace ocudu {

/// UE measurement report — per-UE serving + neighbor cell measurements.
struct cu_cp_ue_meas_report {
  rnti_t rnti;
  pci_t  serving_pci;

  struct cell_meas {
    std::optional<float> rsrp_dbm; ///< SS-RSRP in dBm
    std::optional<float> rsrq_db;  ///< SS-RSRQ in dB
    std::optional<float> sinr_db;  ///< SS-SINR in dB
  };

  cell_meas serving;

  struct neighbor {
    pci_t     pci;
    cell_meas meas;
  };
  std::vector<neighbor> neighbors;
};

/// CU-CP Metrics report.
struct cu_cp_metrics_report {
  struct ue_info {
    /// Current C-RNTI of the UE.
    rnti_t rnti;
    /// ID of the DU the UE is connected to.
    gnb_du_id_t du_id;
    /// PCI of the UE's PCell.
    pci_t pci;
    /// RRC connection state of the UE.
    ocucp::rrc_state rrc_connection_state;
    /// Masked IMEISV of the UE. Present when known from NGAP (initial attach or inter-CU handover);
    /// preserved across intra-CU handovers so the RNTI→IMEISV mapping is maintained.
    std::optional<uint64_t> masked_imeisv;
  };
  struct cell_info {
    nr_cell_global_id_t cgi;
    pci_t               pci;
  };

  struct du_info {
    /// ID of the DU connected to the CU-CP.
    gnb_du_id_t            id;
    std::vector<cell_info> cells;

    rrc_du_metrics                rrc_metrics;
    du_handover_execution_metrics ho_metrics;
  };

  std::vector<ue_info>              ues;
  std::vector<du_info>              dus;
  std::vector<ngap_info>            ngaps;
  mobility_management_metrics       mobility;
  std::vector<cu_cp_ue_meas_report> ue_measurements; ///< Latest measurement reports from UEs
};

/// Interface used by the CU-CP to report metrics.
class cu_cp_metrics_report_notifier
{
public:
  virtual ~cu_cp_metrics_report_notifier() = default;

  /// \brief Used to notify a new metrics report.
  virtual void notify_metrics_report_request(const cu_cp_metrics_report& report) = 0;
};

inline std::string format_rrc_metrics(const std::vector<cu_cp_metrics_report::du_info>& report)
{
  fmt::memory_buffer buffer;

  for (const auto& du_info : report) {
    // log RRC metrics
    fmt::format_to(std::back_inserter(buffer), "[");
    fmt::format_to(std::back_inserter(buffer), " gnb_du_id={}", gnb_du_id_to_int(du_info.id));

    fmt::format_to(std::back_inserter(buffer),
                   " mean_nof_rrc_connections={} max_nof_rrc_connections={} mean_nof_inactive_rrc_connections={} "
                   "max_nof_inactive_rrc_connections={}",
                   du_info.rrc_metrics.mean_nof_rrc_connections,
                   du_info.rrc_metrics.max_nof_rrc_connections,
                   du_info.rrc_metrics.mean_nof_inactive_rrc_connections,
                   du_info.rrc_metrics.max_nof_inactive_rrc_connections);

    fmt::format_to(std::back_inserter(buffer), " attempted_rrc_connection_establishments=[");
    unsigned cause_index = 0;
    for (const auto& cause_count : du_info.rrc_metrics.attempted_rrc_connection_establishments) {
      fmt::format_to(std::back_inserter(buffer),
                     " {}={}",
                     du_info.rrc_metrics.attempted_rrc_connection_establishments.get_cause(cause_index),
                     cause_count);

      ++cause_index;
    }
    fmt::format_to(std::back_inserter(buffer), " ]");

    fmt::format_to(std::back_inserter(buffer), " successful_rrc_connection_establishments=[");
    cause_index = 0;
    for (const auto& cause_count : du_info.rrc_metrics.successful_rrc_connection_establishments) {
      fmt::format_to(std::back_inserter(buffer),
                     " {}={}",
                     du_info.rrc_metrics.successful_rrc_connection_establishments.get_cause(cause_index),
                     cause_count);
      ++cause_index;
    }
    fmt::format_to(std::back_inserter(buffer), " ]");

    fmt::format_to(std::back_inserter(buffer), " failed_rrc_connection_establishments=[");
    cause_index = 0;
    for (const auto& cause_count : du_info.rrc_metrics.failed_rrc_connection_establishments) {
      fmt::format_to(std::back_inserter(buffer),
                     " {}={}",
                     du_info.rrc_metrics.failed_rrc_connection_establishments.get_cause(cause_index),
                     cause_count);
      ++cause_index;
    }
    fmt::format_to(std::back_inserter(buffer), " ]");

    fmt::format_to(
        std::back_inserter(buffer),
        " attempted_rrc_connection_reestablishments={} successful_rrc_connection_reestablishments_with_ue_context={} "
        "successful_rrc_connection_reestablishments_without_ue_context={}",
        du_info.rrc_metrics.attempted_rrc_connection_reestablishments,
        du_info.rrc_metrics.successful_rrc_connection_reestablishments_with_ue_context,
        du_info.rrc_metrics.successful_rrc_connection_reestablishments_without_ue_context);

    fmt::format_to(std::back_inserter(buffer), " attempted_rrc_connection_resumes=[");
    cause_index = 0;
    for (const auto& cause_count : du_info.rrc_metrics.attempted_rrc_connection_resumes) {
      fmt::format_to(std::back_inserter(buffer),
                     " {}={}",
                     du_info.rrc_metrics.attempted_rrc_connection_resumes.get_cause(cause_index),
                     cause_count);

      ++cause_index;
    }
    fmt::format_to(std::back_inserter(buffer), " ]");

    fmt::format_to(std::back_inserter(buffer), " successful_rrc_connection_resumes=[");
    cause_index = 0;
    for (const auto& cause_count : du_info.rrc_metrics.successful_rrc_connection_resumes) {
      fmt::format_to(std::back_inserter(buffer),
                     " {}={}",
                     du_info.rrc_metrics.successful_rrc_connection_resumes.get_cause(cause_index),
                     cause_count);
      ++cause_index;
    }
    fmt::format_to(std::back_inserter(buffer), " ]");

    fmt::format_to(std::back_inserter(buffer), " successful_rrc_connection_resumes_with_fallback=[");
    cause_index = 0;
    for (const auto& cause_count : du_info.rrc_metrics.successful_rrc_connection_resumes_with_fallback) {
      fmt::format_to(std::back_inserter(buffer),
                     " {}={}",
                     du_info.rrc_metrics.successful_rrc_connection_resumes_with_fallback.get_cause(cause_index),
                     cause_count);
      ++cause_index;
    }
    fmt::format_to(std::back_inserter(buffer), " ]");

    fmt::format_to(std::back_inserter(buffer), " rrc_connection_resumes_followed_by_network_release=[");
    cause_index = 0;
    for (const auto& cause_count : du_info.rrc_metrics.rrc_connection_resumes_followed_by_network_release) {
      fmt::format_to(std::back_inserter(buffer),
                     " {}={}",
                     du_info.rrc_metrics.rrc_connection_resumes_followed_by_network_release.get_cause(cause_index),
                     cause_count);
      ++cause_index;
    }
    fmt::format_to(std::back_inserter(buffer), " ]");

    fmt::format_to(std::back_inserter(buffer), " attempted_rrc_connection_resumes_followed_by_rrc_setup=[");
    cause_index = 0;
    for (const auto& cause_count : du_info.rrc_metrics.attempted_rrc_connection_resumes_followed_by_rrc_setup) {
      fmt::format_to(std::back_inserter(buffer),
                     " {}={}",
                     du_info.rrc_metrics.attempted_rrc_connection_resumes_followed_by_rrc_setup.get_cause(cause_index),
                     cause_count);
      ++cause_index;
    }
    fmt::format_to(std::back_inserter(buffer), " ]");

    fmt::format_to(std::back_inserter(buffer),
                   " nof_handover_executions_requested={} nof_successful_handover_executions={}",
                   du_info.ho_metrics.nof_handover_executions_requested,
                   du_info.ho_metrics.nof_successful_handover_executions);

    fmt::format_to(std::back_inserter(buffer), " ],");
  }

  return to_c_str(buffer);
}

} // namespace ocudu
